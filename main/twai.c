#include "twai.h"

#include <string.h>

#include "can_ring.h"
#include "driver/twai.h"
#include "esp_check.h"
#include "esp_log.h"
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
#define TWAI_CORE 0
#define TWAI_RX_QUEUE_LEN 200
#define EVENT_QUEUE_LEN 8

/* Wire bits per frame, ignoring stuffing: standard ~47+8*dlc, extended ~67+8*dlc. */
#define STD_FRAME_OVERHEAD_BITS 47u
#define EXT_FRAME_OVERHEAD_BITS 67u

typedef struct {
    can_bitrate_t bitrate;
    can_mode_t mode;
    volatile bool paused;
    volatile bool running;
    TaskHandle_t task;
    SemaphoreHandle_t lock;
    QueueHandle_t events;
    const char *state; /* "RUNNING" / "BUS_OFF" / "RECOVERING" / "STOPPED" */

    can_stats_t stats; /* counters; single writer (twai task) + UI/net readers */
    uint32_t bits_window;
    uint32_t frames_window;
} can_state_t;

static can_state_t s_can = {
    .state = "STOPPED",
};

static uint32_t bitrate_hz(can_bitrate_t br)
{
    switch (br) {
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

/* IDF's TWAI_*CONFIG_*() macros expand to struct initializers, so they can
 * only initialize objects -- hence this table instead of returning &{...}. */
static const twai_timing_config_t TIMING_125K = TWAI_TIMING_CONFIG_125KBITS();
static const twai_timing_config_t TIMING_250K = TWAI_TIMING_CONFIG_250KBITS();
static const twai_timing_config_t TIMING_500K = TWAI_TIMING_CONFIG_500KBITS();
static const twai_timing_config_t TIMING_1M = TWAI_TIMING_CONFIG_1MBITS();
static const twai_filter_config_t FILTER_ACCEPT_ALL = TWAI_FILTER_CONFIG_ACCEPT_ALL();

static const twai_timing_config_t *timing_for(can_bitrate_t br)
{
    switch (br) {
    case CAN_BITRATE_125K:
        return &TIMING_125K;
    case CAN_BITRATE_250K:
        return &TIMING_250K;
    case CAN_BITRATE_1M:
        return &TIMING_1M;
    default:
        return &TIMING_500K;
    }
}

const char *can_bitrate_name(can_bitrate_t br)
{
    switch (br) {
    case CAN_BITRATE_125K:
        return "125k";
    case CAN_BITRATE_250K:
        return "250k";
    case CAN_BITRATE_1M:
        return "1M";
    default:
        return "500k";
    }
}

const char *can_mode_name(can_mode_t mode)
{
    switch (mode) {
    case CAN_MODE_NORMAL:
        return "NORMAL";
    case CAN_MODE_SELFTEST:
        return "SELFTEST";
    default:
        return "LISTEN";
    }
}

can_bitrate_t twai_current_bitrate(void)
{
    return s_can.bitrate;
}

can_mode_t twai_current_mode(void)
{
    return s_can.mode;
}

static void stats_lock(void)
{
    xSemaphoreTake(s_can.lock, portMAX_DELAY);
}

static void stats_unlock(void)
{
    xSemaphoreGive(s_can.lock);
}

static void set_state(const char *state)
{
    stats_lock();
    s_can.state = state;
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

static void record_frame(const twai_message_t *msg)
{
    linsight_can_record_t rec = {
        .ts_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .id = msg->identifier,
        .flags = (msg->extd ? LINSIGHT_REC_FLAG_EXT : 0) |
                 (msg->rtr ? LINSIGHT_REC_FLAG_RTR : 0),
        .dlc = msg->data_length_code,
    };
    memcpy(rec.data, msg->data, LINSIGHT_DLC_MAX);
    can_ring_push(&rec);

    /* Single-writer counters; readers copy them under the lock. */
    s_can.frames_window++;
    s_can.stats.rx_total++;
    s_can.bits_window += (msg->extd ? EXT_FRAME_OVERHEAD_BITS : STD_FRAME_OVERHEAD_BITS) +
                         8u * msg->data_length_code;
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
        s_can.state = "RECOVERING";
        stats_unlock();
        push_event("bus_off");
        twai_initiate_recovery();
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
        ESP_LOGI(TAG, "bus recovered, restarting");
        set_state("RUNNING");
        twai_start();
    }
    if (alerts & TWAI_ALERT_ERR_PASS) {
        stats_lock();
        s_can.stats.err_passive++;
        stats_unlock();
        push_event("err_passive");
    }
    if (alerts & TWAI_ALERT_BUS_ERROR) {
        stats_lock();
        s_can.stats.bus_errors++;
        stats_unlock();
    }
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
        stats_lock();
        s_can.stats.rx_queue_full++;
        stats_unlock();
        push_event("rx_overflow");
    }
}

static const char *driver_state_name(twai_state_t state)
{
    switch (state) {
    case TWAI_STATE_RUNNING:
        return "RUNNING";
    case TWAI_STATE_BUS_OFF:
        return "BUS_OFF";
    case TWAI_STATE_RECOVERING:
        return "RECOVERING";
    default:
        return "STOPPED";
    }
}

static void twai_task(void *arg)
{
    (void)arg;
    uint64_t window_start = esp_timer_get_time();

    while (s_can.running) {
        twai_message_t msg;
        const esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(100));

        if (err == ESP_OK && !s_can.paused) {
            record_frame(&msg);
        } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "twai_receive: %s", esp_err_to_name(err));
        }

        handle_alerts();

        const uint64_t now = esp_timer_get_time();
        if (now - window_start < 1000000) {
            continue;
        }

        /* 1 Hz window: fps, bus load and driver-maintained counters. */
        const uint32_t seconds = (uint32_t)((now - window_start) / 1000000u);
        twai_status_info_t status;
        twai_get_status_info(&status);

        stats_lock();
        s_can.stats.fps = s_can.frames_window / seconds;
        s_can.stats.load_permille =
            (uint32_t)((uint64_t)s_can.bits_window * 1000u / bitrate_hz(s_can.bitrate) / seconds);
        s_can.stats.rx_missed = status.rx_missed_count;
        s_can.stats.tec = status.tx_error_counter;
        s_can.stats.rec = status.rx_error_counter;
        s_can.stats.paused = s_can.paused;
        if (!s_can.paused) {
            s_can.state = driver_state_name(status.state);
        }
        stats_unlock();

        can_ring_info_t ring;
        can_ring_get_info(&ring);
        ESP_LOGI(TAG, "%s %s%s fps=%u load=%u.%u%% total=%u drop=%u missed=%u rxq_full=%u "
                      "bus_err=%u ring=%u/%u msgs_to_rx=%u state=%s",
                 can_bitrate_name(s_can.bitrate), can_mode_name(s_can.mode),
                 s_can.paused ? " PAUSED" : "",
                 (unsigned)s_can.stats.fps, (unsigned)(s_can.stats.load_permille / 10),
                 (unsigned)(s_can.stats.load_permille % 10),
                 (unsigned)s_can.stats.rx_total, (unsigned)ring.dropped,
                 (unsigned)s_can.stats.rx_missed, (unsigned)s_can.stats.rx_queue_full,
                 (unsigned)s_can.stats.bus_errors, (unsigned)ring.used, (unsigned)ring.cap,
                 (unsigned)status.msgs_to_rx, s_can.state);

        s_can.frames_window = 0;
        s_can.bits_window = 0;
        window_start = now;
    }

    twai_stop();
    twai_driver_uninstall();
    s_can.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t twai_capture_init(void)
{
    if (s_can.lock && s_can.events) {
        return ESP_OK;
    }
    s_can.lock = xSemaphoreCreateMutex();
    s_can.events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(can_event_t));
    return (s_can.lock && s_can.events) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t twai_capture_start(can_bitrate_t bitrate, can_mode_t mode)
{
    if (s_can.task) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_can.lock || !s_can.events) {
        ESP_RETURN_ON_ERROR(twai_capture_init(), TAG, "twai init");
    }

    s_can.bitrate = bitrate;
    s_can.mode = mode;
    s_can.paused = false;
    s_can.state = "RUNNING";

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_GPIO, TWAI_RX_GPIO,
                                                               TWAI_MODE_LISTEN_ONLY);
    switch (mode) {
    case CAN_MODE_NORMAL:
        general.mode = TWAI_MODE_NORMAL;
        break;
    case CAN_MODE_SELFTEST:
        general.mode = TWAI_MODE_NO_ACK;
        break;
    default:
        general.mode = TWAI_MODE_LISTEN_ONLY;
        break;
    }
    general.rx_queue_len = TWAI_RX_QUEUE_LEN;
    /* No TWAI_ALERT_RX_DATA: receive is polled; alerts are error conditions only. */
    general.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED |
                             TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR |
                             TWAI_ALERT_RX_QUEUE_FULL;

    ESP_RETURN_ON_ERROR(twai_driver_install(&general, timing_for(bitrate), &FILTER_ACCEPT_ALL),
                        TAG, "twai install");
    ESP_RETURN_ON_ERROR(twai_start(), TAG, "twai start");

    s_can.running = true;
    const BaseType_t ok = xTaskCreatePinnedToCore(twai_task, "twai", TWAI_TASK_STACK, NULL,
                                                  TWAI_TASK_PRIO, &s_can.task, TWAI_CORE);
    if (ok != pdPASS) {
        s_can.running = false;
        twai_stop();
        twai_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "TWAI %s %s, TX=GPIO%d RX=GPIO%d", can_bitrate_name(bitrate),
             can_mode_name(mode), TWAI_TX_GPIO, TWAI_RX_GPIO);
    return ESP_OK;
}

esp_err_t twai_capture_restart(can_bitrate_t bitrate, can_mode_t mode)
{
    if (!s_can.task) {
        return twai_capture_start(bitrate, mode);
    }

    s_can.running = false; /* the task stops, uninstalls the driver, clears .task */
    for (int i = 0; i < 20 && s_can.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_can.task) {
        return ESP_ERR_INVALID_STATE;
    }
    return twai_capture_start(bitrate, mode);
}

void twai_capture_set_paused(bool paused)
{
    s_can.paused = paused;
    ESP_LOGI(TAG, "capture %s", paused ? "PAUSED" : "RESUMED");
}

void twai_stats_snapshot(can_stats_t *out)
{
    can_ring_info_t ring;
    can_ring_get_info(&ring);

    stats_lock();
    *out = s_can.stats;
    out->state = s_can.state;
    out->paused = s_can.paused;
    stats_unlock();
    out->dropped = ring.dropped;
}

bool twai_event_pop(can_event_t *out)
{
    return xQueueReceive(s_can.events, out, 0) == pdTRUE;
}

void twai_stats_clear(void)
{
    stats_lock();
    memset(&s_can.stats, 0, sizeof(s_can.stats));
    stats_unlock();
    ESP_LOGI(TAG, "stats cleared");
}
