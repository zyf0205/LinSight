#include "sd_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "device_info.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "settings.h"
#include "twai.h"

#define TAG "sd_store"

#define SD_MOUNT_POINT "/sdcard"
#define SD_QUEUE_LEN 16384u
#define SD_FALLBACK_QUEUE_LEN 4096u
#define SD_WRITE_BATCH 128u
#define SD_FILE_MAX_BYTES (32u * 1024u * 1024u)
#define SD_FILE_MAX_MS (10u * 60u * 1000u)
#define SD_KEEP_FILES 8u
#define SD_FREE_STOP_KB (16u * 1024u)
#define SD_FREE_PRUNE_KB (64u * 1024u)
#define SD_RETRY_MS 5000u

#define LSC_HEADER_SIZE 32u
#define LSC_FLAG_TS_UPTIME_ONLY 0x1u
#define LSC_FLAG_BOOT_VALID 0x4u

typedef struct {
    char reason[12];
    uint32_t id;
    uint16_t seq;
    uint32_t ts_ms;
} sd_mark_t;

static QueueHandle_t s_queue;
static QueueHandle_t s_marks;
static SemaphoreHandle_t s_lock;
static sdmmc_card_t *s_card;
static sd_status_t s_status;
static volatile bool s_mounted;
static volatile uint32_t s_drops;
static uint32_t s_boot;
static uint32_t s_next_index = 1;
static uint32_t s_file_count;
static uint32_t s_file_index;
static uint32_t s_file_bytes;
static uint32_t s_file_start_ms;
static uint32_t s_last_sync_ms;
static uint32_t s_open_retry_ms; /* backoff after open_new() fails (card full/IO) */
static int s_fd = -1;
static char s_current[13]; /* "rec00001.lsc" */
static bool s_dirty;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t bitrate_hz(void)
{
    switch (twai_current_bitrate()) {
    case CAN_BITRATE_125K:
        return 125000u;
    case CAN_BITRATE_250K:
        return 250000u;
    case CAN_BITRATE_1M:
        return 1000000u;
    default:
        return 500000u;
    }
}

static void status_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
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

static void build_header(uint8_t out[LSC_HEADER_SIZE])
{
    memset(out, 0, LSC_HEADER_SIZE);
    memcpy(out, "LSC1", 4);
    linsight_put_u16(out + 4, 1);
    linsight_put_u32(out + 6, bitrate_hz());
    linsight_put_u32(out + 10, LSC_FLAG_TS_UPTIME_ONLY | LSC_FLAG_BOOT_VALID);
    /* start_wall (16..23) and anchor_ms (28..31) stay zero: this file is
     * explicitly uptime-only until tools/lsc.py import re-anchors it. */
    linsight_put_u32(out + 24, s_boot);
}

/* FATFS with LFN disabled reports 8.3 names; depending on the volume's NT case
 * flags readdir() may return them upper-case. Parse "recNNNNN.lsc" with any
 * case so the retained-file scan and pruning work on real cards. */
static bool parse_rec_name(const char *name, uint32_t *index)
{
    if (!name || strlen(name) != 12) {
        return false;
    }
    if (tolower((unsigned char)name[0]) != 'r' ||
        tolower((unsigned char)name[1]) != 'e' ||
        tolower((unsigned char)name[2]) != 'c' || name[8] != '.' ||
        tolower((unsigned char)name[9]) != 'l' ||
        tolower((unsigned char)name[10]) != 's' ||
        tolower((unsigned char)name[11]) != 'c') {
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

/* Scan the card for retained .lsc files and pick the next free number. */
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
    if (s_next_index == 0) {
        s_next_index = 1;
    }
    status_lock();
    s_status.files = count;
    status_unlock();
}

/* Drop a plain-text guide on the card so a PC user immediately understands
 * that the .lsc files are binary and how to convert them. */
static void write_readme(void)
{
    static const char text[] =
        "LinSight SD black box\r\n"
        "\r\n"
        "recNNNNN.lsc  binary CAN log (32-byte header + 20-byte records)\r\n"
        "recNNNNN.met  JSON metadata (device, bitrate, mode, boot)\r\n"
        "recNNNNN.trg  trigger/mark sidecar\r\n"
        "\r\n"
        "The .lsc files are binary, not text. Copy them out of the card first\r\n"
        "(the card is FAT32), then on the PC (WSL host repo):\r\n"
        "  python3 tools/lsc.py export csv rec00001.lsc -o rec00001.csv\r\n"
        "  python3 tools/lsc.py export candump rec00001.lsc -o rec00001.log\r\n"
        "  python3 tools/lsc.py verify rec00001.lsc\r\n"
        "Open the csv in a spreadsheet/PlotJuggler, the log in SavvyCAN/can-utils.\r\n";
    const char *path = SD_MOUNT_POINT "/README.TXT";
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        return; /* already there (or the card is read-only) */
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

    char path[40];
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.lsc", (unsigned)oldest);
    unlink(path);
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.met", (unsigned)oldest);
    unlink(path);
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.trg", (unsigned)oldest);
    unlink(path);
    if (s_file_count > 0) {
        s_file_count--;
    }
    status_lock();
    s_status.files = s_file_count;
    status_unlock();
    ESP_LOGI(TAG, "pruned rec%05u.lsc", (unsigned)oldest);
    return true;
}

static void write_meta(uint32_t index, uint32_t start_ms, bool closed)
{
    char json[256];
    const int n = snprintf(
        json, sizeof(json),
        "{\"file\":\"rec%05u.lsc\",\"device_id\":\"%s\",\"fw\":\"%s\","
        "\"bitrate\":%u,\"mode\":\"%s\",\"boot\":%u,\"start_uptime_ms\":%u,"
        "\"closed\":%s}\n",
        (unsigned)index, linsight_device_id(), LINSIGHT_FW_VERSION, (unsigned)bitrate_hz(),
        can_mode_name(twai_current_mode()), (unsigned)s_boot, (unsigned)start_ms,
        closed ? "true" : "false");

    char tmp[40];
    char met[40];
    snprintf(tmp, sizeof(tmp), SD_MOUNT_POINT "/rec%05u.tmp", (unsigned)index);
    snprintf(met, sizeof(met), SD_MOUNT_POINT "/rec%05u.met", (unsigned)index);
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return;
    }
    const bool ok = n > 0 && write_all(fd, json, (size_t)n) && fsync(fd) == 0;
    close(fd);
    if (ok && rename(tmp, met) == 0) {
        return;
    }
    unlink(tmp);
}

static void close_current(bool write_meta_file)
{
    if (s_fd >= 0) {
        fsync(s_fd);
        close(s_fd);
        s_fd = -1;
    }
    if (write_meta_file && s_file_index > 0) {
        write_meta(s_file_index, s_file_start_ms, true);
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
    if (free_kb() < SD_FREE_STOP_KB) {
        status_lock();
        s_status.free_kb = free_kb();
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
    if (free_kb() < SD_FREE_PRUNE_KB) {
        while (free_kb() < SD_FREE_PRUNE_KB && prune_oldest()) {
        }
        if (free_kb() < SD_FREE_STOP_KB) {
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
    snprintf(path, sizeof(path), SD_MOUNT_POINT "/rec%05u.lsc", (unsigned)index);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s: %s", path, strerror(errno));
        twai_event_push("sd_io");
        return false;
    }
    uint8_t header[LSC_HEADER_SIZE];
    build_header(header);
    if (!write_all(fd, header, sizeof(header)) || fsync(fd) != 0) {
        ESP_LOGE(TAG, "header write %s: %s", path, strerror(errno));
        close(fd);
        unlink(path);
        twai_event_push("sd_io");
        return false;
    }

    s_fd = fd;
    s_file_index = index;
    s_file_bytes = sizeof(header);
    s_file_start_ms = now_ms();
    s_last_sync_ms = s_file_start_ms;
    s_dirty = false;
    s_file_count++;
    snprintf(s_current, sizeof(s_current), "rec%05u.lsc", (unsigned)index);
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
    close_current(true);
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

static void write_batch(const linsight_can_record_t *recs, uint32_t n, uint32_t now)
{
    if (!s_mounted || n == 0) {
        return;
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
    if (s_file_bytes + n * LINSIGHT_REC_SIZE > SD_FILE_MAX_BYTES ||
        (s_file_start_ms != 0 && now - s_file_start_ms >= SD_FILE_MAX_MS)) {
        if (s_file_bytes > LSC_HEADER_SIZE) {
            close_current(true);
            twai_event_push("sd_rotate");
            if (!open_new()) {
                s_open_retry_ms = now + SD_RETRY_MS;
                return;
            }
        }
    }

    static uint8_t packed[SD_WRITE_BATCH * LINSIGHT_REC_SIZE];
    for (uint32_t i = 0; i < n; i++) {
        linsight_record_pack(&recs[i], packed + (size_t)i * LINSIGHT_REC_SIZE);
    }
    const size_t len = (size_t)n * LINSIGHT_REC_SIZE;
    if (!write_all(s_fd, packed, len)) {
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
    s_status.present = true;
    s_status.mounted = true;
    s_status.free_kb = free_kb();
    status_unlock();
    twai_event_push("sd_mount");
    ESP_LOGI(TAG, "mounted, %u lsc files, next rec%05u.lsc", (unsigned)s_file_count,
             (unsigned)s_next_index);
    return true;
}

static void sd_task(void *arg)
{
    (void)arg;
    linsight_can_record_t batch[SD_WRITE_BATCH];
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
        if (xQueueReceive(s_queue, &batch[0], pdMS_TO_TICKS(100)) == pdTRUE) {
            n = 1;
            while (n < SD_WRITE_BATCH && xQueueReceive(s_queue, &batch[n], 0) == pdTRUE) {
                n++;
            }
            write_batch(batch, n, now_ms());
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

esp_err_t sd_store_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_marks = xQueueCreate(8, sizeof(sd_mark_t));
    s_queue = xQueueCreateWithCaps(SD_QUEUE_LEN, sizeof(linsight_can_record_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue) {
        ESP_LOGW(TAG, "PSRAM queue failed, using internal RAM");
        s_queue = xQueueCreateWithCaps(SD_FALLBACK_QUEUE_LEN,
                                       sizeof(linsight_can_record_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_lock || !s_marks || !s_queue) {
        ESP_LOGE(TAG, "SD subsystem init failed; recording disabled");
        return ESP_OK; /* never block capture/network on storage */
    }

    s_boot = linsight_boot_count();
    if (xTaskCreatePinnedToCore(sd_task, "sd_store", 8192, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "SD task create failed");
    }
    return ESP_OK;
}

void sd_store_push(const linsight_can_record_t *rec)
{
    if (!s_mounted || !s_queue || !rec) {
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
    if (!out) {
        return;
    }
    status_lock();
    *out = s_status;
    out->dropped = __atomic_load_n(&s_drops, __ATOMIC_RELAXED);
    status_unlock();
}
