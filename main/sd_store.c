#include "sd_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "twai.h"

#define TAG "sd_store"

#define SD_MOUNT_POINT "/sdcard"
#define SD_QUEUE_LEN 16384u
#define SD_WRITE_BATCH 128u
#define SD_FILE_MAX_BYTES (32u * 1024u * 1024u)
#define SD_FILE_MAX_MS (10u * 60u * 1000u)
#define SD_KEEP_FILES 8u
#define SD_FREE_STOP_KB (16u * 1024u)
#define SD_FREE_PRUNE_KB (64u * 1024u)
#define SD_RETRY_MS 5000u
/* Worst candump line: "(" + 7-digit sec + "." + 6-digit usec + ") can0 " +
 * 8-digit ID + "#" + 16 hex data + "\n" = 48 bytes. */
#define SD_LINE_MAX 56u

typedef struct {
    char reason[12];
    uint32_t id;
    uint16_t seq;
    uint32_t ts_ms;
} sd_mark_t;

static QueueHandle_t s_queue;
static QueueHandle_t s_marks;
static SemaphoreHandle_t s_lock;
static linsight_can_record_t *s_batch; /* PSRAM: SD_WRITE_BATCH records */
static char *s_lines;                  /* PSRAM: SD_WRITE_BATCH * SD_LINE_MAX */
static sdmmc_card_t *s_card;
static sd_status_t s_status;
static volatile bool s_mounted;
static volatile uint32_t s_drops;
static uint32_t s_next_index = 1;
static uint32_t s_file_count;
static uint32_t s_file_index;
static uint32_t s_file_bytes;
static uint32_t s_file_start_ms;
static uint32_t s_last_sync_ms;
static uint32_t s_open_retry_ms; /* backoff after open_new() fails (card full/IO) */
static int s_fd = -1;
static char s_current[13]; /* "rec00001.log" */
static bool s_dirty;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void status_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void status_unlock(void)
{
    xSemaphoreGive(s_lock);
}

static bool write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        const ssize_t n = write(fd, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

/* FATFS with LFN disabled reports 8.3 names; depending on the volume's NT case
 * flags readdir() may return them upper-case. Parse "recNNNNN.log" with any
 * case so the retained-file scan and pruning work on real cards. */
static bool parse_rec_name(const char *name, uint32_t *index)
{
    if (strlen(name) != 12 || strncasecmp(name, "rec", 3) != 0 ||
        strcasecmp(name + 8, ".log") != 0) {
        return false;
    }
    uint32_t value = 0;
    for (int i = 3; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        value = value * 10u + (uint32_t)(name[i] - '0');
    }
    if (index) {
        *index = value;
    }
    return true;
}

/* Scan the card for retained .log files and pick the next free number. */
static void scan_files(void)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    uint32_t max_index = 0;
    uint32_t count = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            uint32_t idx = 0;
            if (parse_rec_name(e->d_name, &idx)) {
                count++;
                if (idx > max_index) {
                    max_index = idx;
                }
            }
        }
        closedir(d);
    }
    s_file_count = count;
    s_next_index = max_index + 1u;
    status_lock();
    s_status.files = count;
    status_unlock();
}

/* Rewrite the card guide on every mount: the format changed from the older
 * binary .lsc layout, and a stale guide is worse than an extra 400-byte write. */
static void write_readme(void)
{
    static const char text[] =
        "LinSight SD black box\r\n"
        "\r\n"
        "recNNNNN.log  candump text; open in SavvyCAN or can-utils-compatible tools\r\n"
        "recNNNNN.trg  mark sidecar, one JSON line per manual mark\r\n"
        "Timestamps are device uptime (no RTC); rotation 32 MiB / 10 min,\r\n"
        "newest 8 recordings kept.\r\n";
    const char *path = SD_MOUNT_POINT "/README.TXT";
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return; /* read-only card */
    }
    if (!write_all(fd, text, sizeof(text) - 1u)) {
        close(fd);
        unlink(path);
        return;
    }
    fsync(fd);
    close(fd);
}

static uint64_t free_kb(void)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        return 0;
    }
    return free_bytes / 1024u;
}

/* Remove the oldest file triple. Never touches the file currently open. */
static bool prune_oldest(void)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        return false;
    }
    uint32_t oldest = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t idx = 0;
        if (!parse_rec_name(e->d_name, &idx)) {
            continue;
        }
        if (strcasecmp(e->d_name, s_current) == 0) {
            continue;
        }
        if (oldest == 0 || idx < oldest) {
            oldest = idx;
        }
    }
    closedir(d);
    if (oldest == 0) {
        return false;
    }

    /* Remove the oldest log and its optional mark sidecar. */
    char path[40];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.log", (unsigned)oldest);
    unlink(path);
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.trg", (unsigned)oldest);
    unlink(path);
    if (s_file_count > 0) {
        s_file_count--;
    }
    status_lock();
    s_status.files = s_file_count;
    status_unlock();
    ESP_LOGI(TAG, "pruned rec%05u.log", (unsigned)oldest);
    return true;
}

static void close_current(void)
{
    if (s_fd >= 0) {
        fsync(s_fd);
        close(s_fd);
        s_fd = -1;
    }
    s_file_index = 0;
    s_file_bytes = 0;
    s_file_start_ms = 0;
    s_dirty = false;
    status_lock();
    s_status.recording = false;
    status_unlock();
}

static bool open_new(void)
{
    uint64_t kb = free_kb();
    if (kb < SD_FREE_STOP_KB) {
        status_lock();
        s_status.free_kb = kb;
        status_unlock();
        twai_event_push("sd_full");
        ESP_LOGW(TAG, "card nearly full (< %u KB), recording stopped",
                 (unsigned)SD_FREE_STOP_KB);
        return false;
    }
    while (s_file_count >= SD_KEEP_FILES) {
        if (!prune_oldest()) {
            break;
        }
    }
    kb = free_kb();
    if (kb < SD_FREE_PRUNE_KB) {
        while (kb < SD_FREE_PRUNE_KB && prune_oldest()) {
            kb = free_kb();
        }
        if (kb < SD_FREE_STOP_KB) {
            twai_event_push("sd_full");
            return false;
        }
    }

    uint32_t index = s_next_index++;
    if (index == 0 || index > 99999u) {
        index = 1;
        s_next_index = 2;
    }
    char path[40];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.log", (unsigned)index);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s: %s", path, strerror(errno));
        twai_event_push("sd_io");
        return false;
    }

    s_fd = fd;
    s_file_index = index;
    s_file_bytes = 0;
    s_file_start_ms = now_ms();
    s_last_sync_ms = s_file_start_ms;
    s_dirty = false;
    s_file_count++;
    snprintf(s_current, sizeof(s_current), "rec%05u.log", (unsigned)index);
    status_lock();
    s_status.recording = true;
    s_status.files = s_file_count;
    status_unlock();
    twai_event_push("sd_record");
    ESP_LOGI(TAG, "recording %s", path);
    return true;
}

static void io_fail(const char *what)
{
    ESP_LOGE(TAG, "%s failed: %s", what, strerror(errno));
    close_current();
    twai_event_push("sd_io");

    /* The card may have been removed. Stop new pushes first, then drain any
     * records queued by a producer that passed the mounted check earlier. */
    s_mounted = false;
    if (s_queue) {
        linsight_can_record_t dummy;
        while (xQueueReceive(s_queue, &dummy, 0) == pdTRUE) {
        }
    }
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    status_lock();
    s_status.mounted = false;
    s_status.recording = false;
    status_unlock();
}

/* "(<sec>.<usec>) can0 <id>#<hex|R>\n" — timestamps are device uptime because
 * the board has no RTC. Returns the line length; always < SD_LINE_MAX. */
static int format_line(const linsight_can_record_t *r, char *out)
{
    static const char HEX[] = "0123456789ABCDEF";
    const bool ext = (r->flags & LINSIGHT_REC_FLAG_EXT) != 0;
    const uint8_t dlc = r->dlc > LINSIGHT_DLC_MAX ? LINSIGHT_DLC_MAX : r->dlc;
    int n = snprintf(out, SD_LINE_MAX, "(%u.%06u) can0 %0*X#",
                     (unsigned)(r->ts_ms / 1000u),
                     (unsigned)((r->ts_ms % 1000u) * 1000u), ext ? 8 : 3,
                     (unsigned)(r->id & 0x1FFFFFFFu));
    if (r->flags & LINSIGHT_REC_FLAG_RTR) {
        out[n++] = 'R';
    } else {
        for (uint8_t i = 0; i < dlc; i++) {
            const uint8_t b = r->data[i];
            out[n++] = HEX[b >> 4];
            out[n++] = HEX[b & 0x0Fu];
        }
    }
    out[n++] = '\n';
    return n;
}

static void write_batch(const linsight_can_record_t *recs, uint32_t n, uint32_t now)
{
    if (!s_mounted || n == 0) {
        return;
    }

    size_t len = 0;
    for (uint32_t i = 0; i < n; i++) {
        len += (size_t)format_line(&recs[i], s_lines + len);
    }

    if (s_fd < 0) {
        /* A full card or a failing open must not be retried for every batch:
         * that would spam sd_full events and the log at the capture rate. */
        if (s_open_retry_ms != 0 && (int32_t)(now - s_open_retry_ms) < 0) {
            return;
        }
        if (!open_new()) {
            s_open_retry_ms = now + SD_RETRY_MS;
            return;
        }
        s_open_retry_ms = 0;
    }
    if (s_file_bytes > 0 &&
        (s_file_bytes + len > SD_FILE_MAX_BYTES ||
         (s_file_start_ms != 0 && now - s_file_start_ms >= SD_FILE_MAX_MS))) {
        close_current();
        twai_event_push("sd_rotate");
        if (!open_new()) {
            s_open_retry_ms = now + SD_RETRY_MS;
            return;
        }
    }

    if (!write_all(s_fd, s_lines, len)) {
        io_fail("write");
        return;
    }
    s_file_bytes += (uint32_t)len;
    s_dirty = true;

    if (now - s_last_sync_ms >= 1000u) {
        if (fsync(s_fd) != 0) {
            io_fail("sync");
            return;
        }
        s_last_sync_ms = now;
        s_dirty = false;
    }
}

static void write_mark(const sd_mark_t *m)
{
    if (s_fd < 0 || s_file_index == 0) {
        return;
    }
    char path[40];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.trg", (unsigned)s_file_index);
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) {
        return;
    }
    char line[96];
    const int n = snprintf(line, sizeof(line),
                           "{\"ts_ms\":%u,\"id\":%u,\"seq\":%u,\"reason\":\"%s\"}\n",
                           (unsigned)m->ts_ms, (unsigned)m->id, (unsigned)m->seq,
                           m->reason);
    if (n > 0) {
        (void)write_all(fd, line, (size_t)n);
        (void)fsync(fd);
    }
    close(fd);
}

static bool mount_card(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = 20000;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = GPIO_NUM_47;
    slot.cmd = GPIO_NUM_48;
    slot.d0 = GPIO_NUM_21;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        host.max_freq_khz = 10000; /* slower retry for marginal wiring/cards */
        err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &cfg, &card);
    }
    if (err != ESP_OK) {
        return false;
    }

    s_card = card;
    s_mounted = true;
    s_open_retry_ms = 0;
    scan_files();
    write_readme();
    status_lock();
    s_status.mounted = true;
    s_status.free_kb = free_kb();
    status_unlock();
    twai_event_push("sd_mount");
    ESP_LOGI(TAG, "mounted, %u log files, next rec%05u.log", (unsigned)s_file_count,
             (unsigned)s_next_index);
    return true;
}

static void sd_task(void *arg)
{
    (void)arg;
    uint32_t last_mount = 0;
    uint32_t last_mount_log = 0;
    uint32_t last_free = 0;

    for (;;) {
        const uint32_t now = now_ms();
        if (!s_mounted) {
            if (now - last_mount >= SD_RETRY_MS) {
                last_mount = now;
                if (!mount_card() && (now - last_mount_log >= 30000u)) {
                    last_mount_log = now;
                    ESP_LOGW(TAG, "no SD card (retrying every %u s)",
                             (unsigned)(SD_RETRY_MS / 1000u));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        sd_mark_t mark;
        while (xQueueReceive(s_marks, &mark, 0) == pdTRUE) {
            write_mark(&mark);
        }

        UBaseType_t n = 0;
        if (xQueueReceive(s_queue, &s_batch[0], pdMS_TO_TICKS(100)) == pdTRUE) {
            n = 1;
            while (n < SD_WRITE_BATCH && xQueueReceive(s_queue, &s_batch[n], 0) == pdTRUE) {
                n++;
            }
            write_batch(s_batch, n, now_ms());
        } else if (s_dirty && s_fd >= 0 && now_ms() - s_last_sync_ms >= 1000u) {
            if (fsync(s_fd) == 0) {
                s_last_sync_ms = now_ms();
                s_dirty = false;
            }
        }

        /* esp_vfs_fat_info() walks the FAT; the UI/host only need a coarse
         * value, so refresh it every couple of seconds instead of each loop. */
        const uint32_t t = now_ms();
        if (t - last_free >= 2000u) {
            last_free = t;
            status_lock();
            s_status.free_kb = free_kb();
            status_unlock();
        }
    }
}

void sd_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_marks = xQueueCreate(8, sizeof(sd_mark_t));
    s_queue = xQueueCreateWithCaps(SD_QUEUE_LEN, sizeof(linsight_can_record_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_batch = heap_caps_malloc(SD_WRITE_BATCH * sizeof(*s_batch),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lines = heap_caps_malloc(SD_WRITE_BATCH * SD_LINE_MAX,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_lock || !s_marks || !s_queue || !s_batch || !s_lines) {
        ESP_LOGE(TAG, "SD subsystem init failed; recording disabled");
        return; /* never block capture/network on storage */
    }

    if (xTaskCreatePinnedToCore(sd_task, "sd_store", 8192, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "SD task create failed");
    }
}

void sd_store_push(const linsight_can_record_t *rec)
{
    if (!s_mounted || !s_queue) {
        return;
    }
    if (xQueueSend(s_queue, rec, 0) != pdTRUE) {
        __atomic_add_fetch(&s_drops, 1u, __ATOMIC_RELAXED);
    }
}

void sd_store_mark(const char *reason, uint32_t id, uint16_t seq)
{
    if (!s_marks) {
        return;
    }
    sd_mark_t mark = {
        .id = id,
        .seq = seq,
        .ts_ms = now_ms(),
    };
    snprintf(mark.reason, sizeof(mark.reason), "%s", reason ? reason : "mark");
    (void)xQueueSend(s_marks, &mark, 0); /* a dropped mark never blocks TX */
}

void sd_store_get_status(sd_status_t *out)
{
    if (!s_lock) {
        return;
    }
    status_lock();
    *out = s_status;
    out->dropped = __atomic_load_n(&s_drops, __ATOMIC_RELAXED);
    status_unlock();
}
