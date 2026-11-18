#include "twai.h"

#include <string.h>

#include "can_ring.h"
#include "driver/twai.h"
#include "frame.h"
#include "sd_store.h"
#include "settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "twai"

#define TWAI_TX_GPIO GPIO_NUM_10
#define TWAI_RX_GPIO GPIO_NUM_11
#define TWAI_TASK_PRIO 10
#define TWAI_TASK_STACK 4096
#define TWAI_CORE 1
#define TWAI_RX_QUEUE_LEN 200
#define EVENT_QUEUE_LEN 8

/* Wire bits per frame, ignoring stuffing: standard ~47+8*dlc, extended ~67+8*dlc. */
#define STD_FRAME_OVERHEAD_BITS 47u
#define EXT_FRAME_OVERHEAD_BITS 67u

typedef struct {
    can_bitrate_t bitrate;
    can_mode_t mode;
    volatile bool running;
    TaskHandle_t volatile task;
    SemaphoreHandle_t lock;
    QueueHandle_t events;

    can_stats_t stats; /* counters; single writer (twai task) + UI/net readers */
    uint32_t rx_missed_base;
    volatile uint32_t rx_total;
} twai_ctx_t;

static twai_ctx_t s_can;

/* Serializes start/restart/recover and bus-off alert handling. The restart
 * path releases this lock while waiting for the TWAI task to exit, so the
 * task's alert handler can take it and observe running == false. */
static SemaphoreHandle_t s_restart_lock;

/* IDF's TWAI_*CONFIG_*() macros expand to struct initializers, so they can
 * only appear in static initializers -- hence this table. */
typedef struct {
    uint32_t hz;
    const char *name;
    twai_timing_config_t timing;
} bitrate_info_t;

static const bitrate_info_t BITRATES[] = {
    [CAN_BITRATE_125K] = {125000u, "125k", TWAI_TIMING_CONFIG_125KBITS()},
    [CAN_BITRATE_250K] = {250000u, "250k", TWAI_TIMING_CONFIG_250KBITS()},
    [CAN_BITRATE_500K] = {500000u, "500k", TWAI_TIMING_CONFIG_500KBITS()},
    [CAN_BITRATE_1M] = {1000000u, "1M", TWAI_TIMING_CONFIG_1MBITS()},
};
static const twai_filter_config_t FILTER_ACCEPT_ALL = TWAI_FILTER_CONFIG_ACCEPT_ALL();

static const bitrate_info_t *bitrate_info(can_bitrate_t br)
{
    if ((unsigned)br > (unsigned)CAN_BITRATE_1M) {
        br = CAN_BITRATE_500K;
    }
    return &BITRATES[br];
}

const char *can_bitrate_name(can_bitrate_t br)
{
    return bitrate_info(br)->name;
}

const char *can_mode_name(can_mode_t mode)
{
    return mode == CAN_MODE_NORMAL ? "NORMAL" : "LISTEN";
}

const char *can_state_name(can_state_t state)
{
    switch (state) {
    case CAN_STATE_RUNNING:
        return "RUNNING";
    case CAN_STATE_BUS_OFF:
        return "BUS_OFF";
    case CAN_STATE_RECOVERING:
        return "RECOVERING";
    default:
        return "STOPPED";
    }
}

can_bitrate_t twai_current_bitrate(void)
{
    return (can_bitrate_t)__atomic_load_n(&s_can.bitrate, __ATOMIC_RELAXED);
}

can_mode_t twai_current_mode(void)
{
    return (can_mode_t)__atomic_load_n(&s_can.mode, __ATOMIC_RELAXED);
}

static void stats_lock(void)
{
    xSemaphoreTake(s_can.lock, portMAX_DELAY);
}

static void stats_unlock(void)
{
    xSemaphoreGive(s_can.lock);
}

static void set_state(can_state_t state)
{
    stats_lock();
    s_can.stats.state = state;
    stats_unlock();
}

static void push_event(const char *type)
{
    const can_event_t ev = {
        .ts_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .type = type,
    };
    if (xQueueSend(s_can.events, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped %s", type);
    }
}

void twai_event_push(const char *type)
{
    push_event(type);
}

static uint32_t record_frame(const twai_message_t *msg)
{
    const uint8_t dlc = msg->data_length_code > LINSIGHT_DLC_MAX
                            ? LINSIGHT_DLC_MAX
                            : msg->data_length_code;
    linsight_can_record_t rec = {
        .ts_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .id = msg->identifier,
        .flags = (msg->extd ? LINSIGHT_REC_FLAG_EXT : 0) |
                 (msg->rtr ? LINSIGHT_REC_FLAG_RTR : 0),
        .dlc = dlc,
    };
    memcpy(rec.data, msg->data, LINSIGHT_DLC_MAX);
    can_ring_push(&rec); /* assigns rec.seq */
    sd_store_push(&rec); /* independent SD fan-out; never blocks */
    __atomic_add_fetch(&s_can.rx_total, 1u, __ATOMIC_RELAXED);

    return (msg->extd ? EXT_FRAME_OVERHEAD_BITS : STD_FRAME_OVERHEAD_BITS) +
           (msg->rtr ? 0u : 8u * dlc);
}

static void handle_alerts(void)
{
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) != ESP_OK || alerts == 0) {
        return;
    }

    if (alerts & TWAI_ALERT_BUS_OFF) {
        ESP_LOGW(TAG, "BUS-OFF, starting recovery");
        stats_lock();
        s_can.stats.bus_off++;
        s_can.stats.state = CAN_STATE_RECOVERING;
        stats_unlock();
        push_event("bus_off");
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGI(TAG, "bus recovered, restarting");
    }

    /* The alert handler runs on the TWAI task, which twai_capture_restart()
     * stops by clearing running and waiting outside this lock. Serialize the
     * actual driver recovery with manual recover/restart. */
    if ((alerts & (TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED)) && s_restart_lock) {
        xSemaphoreTake(s_restart_lock, portMAX_DELAY);
        if (s_can.running) {
            if (alerts & TWAI_ALERT_BUS_OFF) {
                const esp_err_t err = twai_initiate_recovery();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "initiate recovery: %s", esp_err_to_name(err));
                }
            }
            if (alerts & TWAI_ALERT_BUS_RECOVERED) {
                const esp_err_t err = twai_start();
                if (err == ESP_OK) {
                    set_state(CAN_STATE_RUNNING);
                } else {
                    ESP_LOGE(TAG, "bus recovery start: %s", esp_err_to_name(err));
                }
            }
        }
        xSemaphoreGive(s_restart_lock);
    }
    if (alerts & TWAI_ALERT_ERR_PASS) {
        push_event("err_passive");
    }
    if (alerts & TWAI_ALERT_BUS_ERROR) {
        stats_lock();
        s_can.stats.bus_errors++;
        stats_unlock();
    }
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
        push_event("rx_overflow");
    }
}

static can_state_t driver_state(twai_state_t state)
{
    switch (state) {
    case TWAI_STATE_RUNNING:
        return CAN_STATE_RUNNING;
    case TWAI_STATE_BUS_OFF:
        return CAN_STATE_BUS_OFF;
    case TWAI_STATE_RECOVERING:
        return CAN_STATE_RECOVERING;
    default:
        return CAN_STATE_STOPPED;
    }
}

static void twai_task(void *arg)
{
    (void)arg;
    uint64_t window_start = esp_timer_get_time();
    uint32_t frames_window = 0;
    uint32_t bits_window = 0;

    while (s_can.running) {
        twai_message_t msg;
        const esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(50));

        if (err == ESP_OK) {
            bits_window += record_frame(&msg);
            frames_window++;
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "twai_receive: %s", esp_err_to_name(err));
        }

        handle_alerts();

        const uint64_t now = esp_timer_get_time();
        if (now - window_start < 1000000) {
            continue;
        }

        /* 1 Hz window: fps, bus load and driver-maintained counters. Use the
         * real elapsed time instead of an integer second count: a window that
         * fires 100 ms late would otherwise over-report by 10% at low rates. */
        const uint64_t elapsed_us = now - window_start;
        const can_bitrate_t br = twai_current_bitrate();
        const can_mode_t mode = twai_current_mode();
        twai_status_info_t status = {0};
        const bool status_ok = twai_get_status_info(&status) == ESP_OK;

        stats_lock();
        s_can.stats.fps = (uint32_t)((uint64_t)frames_window * 1000000u / elapsed_us);
        uint32_t permille = (uint32_t)((uint64_t)bits_window * 1000000000ull /
                                       ((uint64_t)bitrate_info(br)->hz * elapsed_us));
        s_can.stats.load_permille = permille > 1000u ? 1000u : permille;
        if (status_ok) {
            if (status.rx_missed_count < s_can.rx_missed_base) {
                s_can.rx_missed_base = 0;
            }
            s_can.stats.rx_missed = status.rx_missed_count - s_can.rx_missed_base;
            s_can.stats.tec = status.tx_error_counter;
            s_can.stats.rec = status.rx_error_counter;
            s_can.stats.state = driver_state(status.state);
        }
        const can_stats_t snap = s_can.stats;
        stats_unlock();

        frames_window = 0;
        bits_window = 0;

        can_ring_info_t ring;
        can_ring_get_info(&ring);
        const uint32_t rx_total = __atomic_load_n(&s_can.rx_total, __ATOMIC_RELAXED);
        ESP_LOGI(TAG, "%s %s fps=%u load=%u.%u%% total=%u drop=%u missed=%u bus_err=%u "
                      "ring=%u/%u state=%s heap=%u psram=%u",
                 can_bitrate_name(br), can_mode_name(mode),
                 (unsigned)snap.fps, (unsigned)(snap.load_permille / 10),
                 (unsigned)(snap.load_permille % 10),
                 (unsigned)rx_total, (unsigned)ring.dropped, (unsigned)snap.rx_missed,
                 (unsigned)snap.bus_errors, (unsigned)ring.used, (unsigned)ring.cap,
                 can_state_name(snap.state),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        window_start = now;
    }

    stats_lock();
    s_can.stats.fps = 0;
    s_can.stats.load_permille = 0;
    s_can.stats.state = CAN_STATE_STOPPED;
    stats_unlock();
    twai_stop();
    twai_driver_uninstall();
    s_can.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t twai_capture_init(void)
{
    /* Called once from app_main before the UI and capture task start. */
    s_can.lock = xSemaphoreCreateMutex();
    s_can.events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(can_event_t));
    s_restart_lock = xSemaphoreCreateMutex();
    return (s_can.lock && s_can.events && s_restart_lock) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t twai_capture_start(can_bitrate_t bitrate, can_mode_t mode)
{
    if (s_can.task) {
        return ESP_ERR_INVALID_STATE;
    }

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_GPIO, TWAI_RX_GPIO,
                                                               TWAI_MODE_LISTEN_ONLY);
    if (mode == CAN_MODE_NORMAL) {
        general.mode = TWAI_MODE_NORMAL;
    }
    general.rx_queue_len = TWAI_RX_QUEUE_LEN;
    /* No TWAI_ALERT_RX_DATA: receive is polled; alerts are error conditions only. */
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                             TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR |
                             TWAI_ALERT_RX_QUEUE_FULL;

    const esp_err_t install_err = twai_driver_install(&general, &bitrate_info(bitrate)->timing,
                                                      &FILTER_ACCEPT_ALL);
    if (install_err != ESP_OK) {
        set_state(CAN_STATE_STOPPED);
        ESP_LOGE(TAG, "twai install: %s", esp_err_to_name(install_err));
        return install_err;
    }
    const esp_err_t start_err = twai_start();
    if (start_err != ESP_OK) {
        twai_driver_uninstall();
        set_state(CAN_STATE_STOPPED);
        ESP_LOGE(TAG, "twai start: %s", esp_err_to_name(start_err));
        return start_err;
    }

    /* Commit only after the driver is live: a failed install/start must not
     * leave the UI advertising a configuration that never took effect. */
    stats_lock();
    __atomic_store_n(&s_can.bitrate, bitrate, __ATOMIC_RELAXED);
    __atomic_store_n(&s_can.mode, mode, __ATOMIC_RELAXED);
    s_can.rx_missed_base = 0;
    s_can.stats.state = CAN_STATE_RUNNING;
    stats_unlock();

    s_can.running = true;
    TaskHandle_t handle = NULL;
    const BaseType_t ok = xTaskCreatePinnedToCore(twai_task, "twai", TWAI_TASK_STACK, NULL,
                                                  TWAI_TASK_PRIO, &handle, TWAI_CORE);
    s_can.task = handle;
    if (ok != pdPASS) {
        s_can.running = false;
        twai_stop();
        twai_driver_uninstall();
        set_state(CAN_STATE_STOPPED);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TWAI %s %s, TX=GPIO%d RX=GPIO%d", can_bitrate_name(bitrate),
             can_mode_name(mode), TWAI_TX_GPIO, TWAI_RX_GPIO);
    return ESP_OK;
}

esp_err_t twai_capture_restart(can_bitrate_t bitrate, can_mode_t mode)
{
    xSemaphoreTake(s_restart_lock, portMAX_DELAY);

    esp_err_t err;
    if (!s_can.task) {
        err = twai_capture_start(bitrate, mode);
        xSemaphoreGive(s_restart_lock);
        goto persist;
    }

    s_can.running = false; /* the task stops, uninstalls the driver, clears .task */
    xSemaphoreGive(s_restart_lock);

    /* Wait outside the lock: the TWAI task's alert handler needs it to see
     * running == false and leave the driver alone while the task exits. */
    for (int i = 0; i < 50 && s_can.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    xSemaphoreTake(s_restart_lock, portMAX_DELAY);
    if (s_can.task) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        err = twai_capture_start(bitrate, mode);
    }
    xSemaphoreGive(s_restart_lock);

persist:
    if (err == ESP_OK) {
        const esp_err_t save = settings_save_can(bitrate, mode);
        if (save != ESP_OK) {
            ESP_LOGW(TAG, "persist CAN config: %s", esp_err_to_name(save));
        }
    }
    return err;
}

void twai_stats_snapshot(can_stats_t *out)
{
    if (!out) {
        return;
    }
    stats_lock();
    *out = s_can.stats;
    out->rx_total = __atomic_load_n(&s_can.rx_total, __ATOMIC_RELAXED);
    stats_unlock();
}

bool twai_event_pop(can_event_t *out)
{
    return xQueueReceive(s_can.events, out, 0) == pdTRUE;
}

void twai_stats_clear(void)
{
    twai_status_info_t status = {0};
    const bool status_ok = twai_get_status_info(&status) == ESP_OK;

    stats_lock();
    const can_state_t state = s_can.stats.state;
    const uint32_t tec = s_can.stats.tec;
    const uint32_t rec = s_can.stats.rec;
    memset(&s_can.stats, 0, sizeof(s_can.stats));
    s_can.stats.state = state;
    s_can.stats.tec = tec;
    s_can.stats.rec = rec;
    if (status_ok) {
        s_can.rx_missed_base = status.rx_missed_count;
    }
    __atomic_store_n(&s_can.rx_total, 0u, __ATOMIC_RELAXED);
    stats_unlock();
    ESP_LOGI(TAG, "stats cleared");
}
