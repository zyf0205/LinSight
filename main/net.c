#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "can_ring.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "frame.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "twai.h"
#include "ui.h"

#define TAG "net"

#define WIFI_CONNECTED_BIT BIT0
#define NET_TASK_STACK 8192
#define NET_TASK_PRIO 5
#define NET_CORE 0

#define RX_BUF_SIZE 1024
#define PARSER_BUF_SIZE 256u
#define PING_INTERVAL_MS 5000u
#define HOST_TIMEOUT_MS 15000u
#define RECONNECT_DELAY_MS 1000u
/* Batches (max 819 records each) sent per session-loop iteration. */
#define MAX_BATCHES_PER_TICK 4

static int tcp_connect(const char *host, uint16_t port, int timeout_ms);

static EventGroupHandle_t s_wifi_events;

static linsight_parser_t s_parser;
static uint8_t *s_parser_buf;                     /* internal, 256 B scratch */
static uint8_t *s_tx_buf;                         /* PSRAM, one CAN_BATCH frame */
static linsight_can_record_t s_pop[LINSIGHT_BATCH_MAX_RECORDS]; /* internal, 16 KB */

static uint32_t s_ping_value;
static uint64_t s_ping_sent_us;
static volatile bool s_stream_paused; /* FLOW from host: stop popping the ring */

/* --------------------------------------------------------------- utilities */

static bool send_all(int fd, const uint8_t *data, size_t len)
{
    size_t left = len;

    while (left > 0) {
        const int n = send(fd, data, left, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ESP_LOGW(TAG, "send failed: errno %d", errno);
            return false;
        }
        if (n == 0) {
            return false;
        }
        data += n;
        left -= (size_t)n;
    }
    return true;
}

/* Small control/stats frames (<= 512 B payload) built on the stack. */
static bool send_frame(int fd, uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[LINSIGHT_FRAME_HEADER_SIZE + 512];

    if (len > 512) {
        return false;
    }
    linsight_frame_write_header(frame, type, len);
    if (len) {
        memcpy(frame + LINSIGHT_FRAME_HEADER_SIZE, payload, len);
    }
    return send_all(fd, frame, LINSIGHT_FRAME_HEADER_SIZE + len);
}

/* ------------------------------------------------------------- batch path */

/* Send up to MAX_BATCHES_PER_TICK batches; returns false on socket error. */
static bool send_batches(int fd)
{
    if (s_stream_paused) {
        return true;
    }

    for (int i = 0; i < MAX_BATCHES_PER_TICK; i++) {
        const uint16_t count = (uint16_t)can_ring_pop(s_pop, LINSIGHT_BATCH_MAX_RECORDS);
        if (count == 0) {
            return true;
        }

        const size_t payload = linsight_batch_pack(s_tx_buf, LINSIGHT_MAX_CAN_BATCH, s_pop,
                                                   count);
        if (payload == 0) {
            ESP_LOGE(TAG, "batch pack failed for %u records", (unsigned)count);
            return false;
        }

        uint8_t hdr[LINSIGHT_FRAME_HEADER_SIZE];
        linsight_frame_write_header(hdr, LINSIGHT_FRAME_CAN_BATCH, (uint16_t)payload);
        if (!send_all(fd, hdr, sizeof(hdr)) ||
            !send_all(fd, s_tx_buf, payload)) {
            return false;
        }
    }
    return true;
}

/* --------------------------------------------------------------- controls */

static void send_ack(int fd, uint16_t id, uint8_t code)
{
    char json[32];
    const int n = snprintf(json, sizeof(json), "{\"id\":%u,\"code\":%u}", (unsigned)id,
                           (unsigned)code);
    if (n > 0 && !send_frame(fd, LINSIGHT_FRAME_ACK, (const uint8_t *)json, (uint16_t)n)) {
        ESP_LOGW(TAG, "ACK send failed");
    }
}

/* Handles one CONTROL frame. Returns false when the socket is broken. */
static bool handle_control(int fd, const uint8_t *payload, uint16_t len)
{
    char json[LINSIGHT_MAX_CONTROL + 1];

    if (len == 0 || len > LINSIGHT_MAX_CONTROL) {
        return true;
    }
    memcpy(json, payload, len);
    json[len] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "bad CONTROL JSON");
        return true;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    const uint16_t ctrl_id = cJSON_IsNumber(id) ? (uint16_t)id->valuedouble : 0;
    const char *act = cJSON_IsString(action) ? action->valuestring : "";
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;

    if (strcmp(act, LINSIGHT_CTRL_PAUSE) == 0) {
        twai_capture_set_paused(true);
        err = ESP_OK;
    } else if (strcmp(act, LINSIGHT_CTRL_RESUME) == 0) {
        twai_capture_set_paused(false);
        err = ESP_OK;
    } else if (strcmp(act, LINSIGHT_CTRL_CLEAR) == 0) {
        twai_stats_clear();
        err = ESP_OK;
    } else if (strcmp(act, LINSIGHT_CTRL_CONFIG) == 0) {
        const cJSON *bitrate = cJSON_GetObjectItemCaseSensitive(root, "bitrate");
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
        can_bitrate_t br;
        if (cJSON_IsNumber(bitrate) && bitrate->valuedouble == 125000.0) {
            br = CAN_BITRATE_125K;
        } else if (cJSON_IsNumber(bitrate) && bitrate->valuedouble == 250000.0) {
            br = CAN_BITRATE_250K;
        } else if (cJSON_IsNumber(bitrate) && bitrate->valuedouble == 1000000.0) {
            br = CAN_BITRATE_1M;
        } else {
            br = CAN_BITRATE_500K;
        }
        can_mode_t m = CAN_MODE_LISTEN_ONLY;
        if (cJSON_IsString(mode) && strcmp(mode->valuestring, "normal") == 0) {
            m = CAN_MODE_NORMAL;
        }
        err = twai_capture_restart(br, m);
    }

    cJSON_Delete(root);
    if (err == ESP_OK) {
        /* Host control was applied: mirror it on screen (settings nav badge). */
        can_stats_t st;
        twai_stats_snapshot(&st);
        uint16_t kbps;
        switch (twai_current_bitrate()) {
        case CAN_BITRATE_125K:
            kbps = 125;
            break;
        case CAN_BITRATE_250K:
            kbps = 250;
            break;
        case CAN_BITRATE_1M:
            kbps = 1000;
            break;
        default:
            kbps = 500;
            break;
        }
        linsight_ui_sync_control(st.paused ? 1 : 0, kbps, 0xFFFFFFFFu);
    }
    send_ack(fd, ctrl_id, err == ESP_OK     ? LINSIGHT_ACK_OK
                          : err == ESP_ERR_NOT_SUPPORTED ? LINSIGHT_ACK_BAD_ARG
                                                         : LINSIGHT_ACK_FAILED);
    ESP_LOGI(TAG, "CONTROL id=%u action=%s -> %s", (unsigned)ctrl_id, act,
             esp_err_to_name(err));
    return true;
}

/* --------------------------------------------------------------- session */

static bool send_hello(int fd)
{
    char json[192];
    const int n = snprintf(
        json, sizeof(json),
        "{\"proto\":%u,\"fw\":\"2.0.0\",\"device\":\"linsight-can\","
        "\"bitrate\":\"%s\",\"mode\":\"%s\"}",
        (unsigned)LINSIGHT_PROTO_VERSION, can_bitrate_name(twai_current_bitrate()),
        can_mode_name(twai_current_mode()));
    return n > 0 && send_frame(fd, LINSIGHT_FRAME_HELLO, (const uint8_t *)json, (uint16_t)n);
}

static bool send_stats(int fd)
{
    can_stats_t st;
    twai_stats_snapshot(&st);
    can_ring_info_t ring;
    can_ring_get_info(&ring);

    static const char *bitrate_json[] = {
        [CAN_BITRATE_125K] = "125000",
        [CAN_BITRATE_250K] = "250000",
        [CAN_BITRATE_500K] = "500000",
        [CAN_BITRATE_1M] = "1000000",
    };

    char json[LINSIGHT_MAX_STATS];
    const int n = snprintf(
        json, sizeof(json),
        "{\"fps\":%u,\"total\":%u,\"dropped\":%u,\"missed\":%u,\"rxq_full\":%u,"
        "\"bus_err\":%u,\"bus_off\":%u,\"err_pass\":%u,\"tec\":%u,\"rec\":%u,"
        "\"state\":\"%s\",\"bitrate\":%s,\"mode\":\"%s\",\"paused\":%d,"
        "\"ring_used\":%u,\"ring_cap\":%u,\"uptime_ms\":%u}",
        (unsigned)st.fps, (unsigned)st.rx_total, (unsigned)st.dropped,
        (unsigned)st.rx_missed, (unsigned)st.rx_queue_full, (unsigned)st.bus_errors,
        (unsigned)st.bus_off, (unsigned)st.err_passive, (unsigned)st.tec, (unsigned)st.rec,
        st.state, bitrate_json[twai_current_bitrate()], can_mode_name(twai_current_mode()),
        (int)st.paused, (unsigned)ring.used, (unsigned)ring.cap,
        (unsigned)(esp_timer_get_time() / 1000));
    if (n <= 0 || n >= (int)sizeof(json)) {
        return true; /* stats are best-effort, never break the session */
    }
    return send_frame(fd, LINSIGHT_FRAME_STATS, (const uint8_t *)json, (uint16_t)n);
}

static bool send_events(int fd)
{
    for (int i = 0; i < 4; i++) {
        can_event_t ev;
        if (!twai_event_pop(&ev)) {
            return true;
        }
        char json[96];
        const int n = snprintf(json, sizeof(json), "{\"type\":\"%s\",\"ts_ms\":%u}", ev.type,
                               (unsigned)ev.ts_ms);
        if (n > 0 &&
            !send_frame(fd, LINSIGHT_FRAME_EVENT, (const uint8_t *)json, (uint16_t)n)) {
            return false;
        }
    }
    return true;
}

static bool send_ping(int fd)
{
    uint8_t payload[LINSIGHT_MAX_PING];

    s_ping_value = (uint32_t)(esp_timer_get_time() / 1000);
    s_ping_sent_us = esp_timer_get_time();
    linsight_put_u32(payload, s_ping_value);
    return send_frame(fd, LINSIGHT_FRAME_PING, payload, sizeof(payload));
}

static void on_frame(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    int *fd = ctx;

    switch (type) {
    case LINSIGHT_FRAME_PING:
        if (len <= LINSIGHT_MAX_PING) {
            send_frame(*fd, LINSIGHT_FRAME_PONG, payload, len);
        }
        break;

    case LINSIGHT_FRAME_PONG:
        if (len == LINSIGHT_MAX_PONG && s_ping_sent_us &&
            linsight_get_u32(payload) == s_ping_value) {
            uint32_t rtt = (uint32_t)((esp_timer_get_time() - s_ping_sent_us) / 1000);
            if (rtt == 0) {
                rtt = 1;
            }
            linsight_ui_set_conn(UI_CONN_ONLINE, rtt);
            s_ping_sent_us = 0;
        }
        break;

    case LINSIGHT_FRAME_CONTROL:
        handle_control(*fd, payload, len);
        break;

    case LINSIGHT_FRAME_FLOW:
        if (len == LINSIGHT_MAX_FLOW) {
            s_stream_paused = payload[0] != 0;
            ESP_LOGI(TAG, "FLOW: streaming %s", s_stream_paused ? "paused" : "resumed");
        }
        break;

    default:
        break;
    }
}

static void session_loop(int fd)
{
    uint8_t rx[RX_BUF_SIZE];
    uint64_t last_rx_ms = esp_timer_get_time() / 1000;
    uint64_t last_ping = 0;
    uint64_t last_stats = 0;

    linsight_parser_reset(&s_parser);
    s_stream_paused = false;

    if (!send_hello(fd) || !send_ping(fd)) {
        return;
    }
    last_ping = last_rx_ms;
    linsight_ui_set_conn(UI_CONN_ONLINE, 0);
    ESP_LOGI(TAG, "TCP session started");

    while (true) {
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "Wi-Fi lost");
            break;
        }

        const int n = recv(fd, rx, sizeof(rx), 0);
        if (n > 0) {
            last_rx_ms = esp_timer_get_time() / 1000;
            if (linsight_parser_feed(&s_parser, rx, (size_t)n, on_frame, &fd) !=
                LINSIGHT_PARSE_OK) {
                ESP_LOGW(TAG, "protocol error, closing");
                break;
            }
        } else if (n == 0) {
            ESP_LOGI(TAG, "host closed the connection");
            break;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "recv failed: errno %d", errno);
            break;
        }

        if (!send_batches(fd) || !send_events(fd)) {
            break;
        }

        const uint64_t now = esp_timer_get_time() / 1000;
        if (now - last_stats >= 1000) {
            if (!send_stats(fd)) {
                break;
            }
            last_stats = now;
        }
        if (now - last_ping >= PING_INTERVAL_MS) {
            if (!send_ping(fd)) {
                break;
            }
            last_ping = now;
        }
        if (now - last_rx_ms > HOST_TIMEOUT_MS) {
            ESP_LOGW(TAG, "host silent for %u ms", (unsigned)HOST_TIMEOUT_MS);
            break;
        }
    }

    linsight_ui_set_conn(UI_CONN_RECONNECT, 0);
    ESP_LOGI(TAG, "TCP session ended");
}

static void net_task(void *arg)
{
    (void)arg;

    linsight_ui_set_conn(UI_CONN_WIFI, 0);
    ESP_LOGI(TAG, "connecting Wi-Fi SSID=\"%s\"", CONFIG_LINSIGHT_WIFI_SSID);

    for (;;) {
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            linsight_ui_set_conn(UI_CONN_WIFI, 0);
            esp_wifi_connect();
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        linsight_ui_set_conn(UI_CONN_HOST, 0);
        const int fd = tcp_connect(CONFIG_LINSIGHT_HOST, CONFIG_LINSIGHT_PORT, 3000);
        if (fd < 0) {
            static uint32_t attempts;
            if (++attempts % 10u == 1u) {
                ESP_LOGI(TAG, "host %s:%u unreachable (attempt %u)", CONFIG_LINSIGHT_HOST,
                         (unsigned)CONFIG_LINSIGHT_PORT, (unsigned)attempts);
            }
            linsight_ui_set_conn(UI_CONN_RECONNECT, 0);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        session_loop(fd);
        close(fd);
        linsight_ui_set_conn(UI_CONN_RECONNECT, 0);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

/* --------------------------------------------------------------- transport */

static int tcp_connect(const char *host, uint16_t port, int timeout_ms)
{
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "socket failed: errno %d", errno);
        return -1;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        ESP_LOGE(TAG, "bad host address: %s", host);
        close(fd);
        return -1;
    }

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&dest, sizeof(dest));
    if (rc < 0 && errno != EINPROGRESS) {
        /* Per-attempt failures are routine (host offline) — summary is logged
         * by the caller every 10th attempt. */
        ESP_LOGD(TAG, "connect failed: errno %d", errno);
        close(fd);
        return -1;
    }
    if (rc < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            ESP_LOGD(TAG, "connect timeout to %s:%u", host, (unsigned)port);
            close(fd);
            return -1;
        }
        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0 || so_error != 0) {
            ESP_LOGD(TAG, "connect error: %d", so_error);
            close(fd);
            return -1;
        }
    }

    fcntl(fd, F_SETFL, flags);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* recv() also paces streaming: batches are only flushed after it returns.
     * 50 ms keeps end-to-end latency low (max 1.3 MB/s through 4x16 KB sends)
     * while still yielding to CONTROL/PING handling. */
    struct timeval rcv = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    struct timeval snd = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    return fd;
}

/* ------------------------------------------------------------ bring-up */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason %u", (unsigned)ev->reason);
        linsight_ui_set_conn(UI_CONN_RECONNECT, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        linsight_ui_set_conn(UI_CONN_HOST, 0);
    }
}

static esp_err_t wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_FAIL;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "ip handler");

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, CONFIG_LINSIGHT_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_LINSIGHT_WIFI_PASSWORD,
            sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode =
        cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    const esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps != ESP_OK) {
        ESP_LOGW(TAG, "wifi set_ps: %s", esp_err_to_name(ps));
    }
    return ESP_OK;
}

static esp_err_t buffers_init(void)
{
    /* Parser scratch covers CONTROL (the largest frame we actually read);
     * anything bigger is skipped by the parser by design. */
    static uint8_t parser_scratch[PARSER_BUF_SIZE];
    s_parser_buf = parser_scratch;
    linsight_parser_init(&s_parser, s_parser_buf, PARSER_BUF_SIZE);

    s_tx_buf = heap_caps_malloc(LINSIGHT_MAX_CAN_BATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_buf) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_events = xEventGroupCreate();
    return s_wifi_events ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t net_start(void)
{
    ESP_RETURN_ON_ERROR(buffers_init(), TAG, "buffers");
    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "wifi");

    const BaseType_t ok = xTaskCreatePinnedToCore(net_task, "net", NET_TASK_STACK, NULL,
                                                  NET_TASK_PRIO, NULL, NET_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "net task");

    ESP_LOGI(TAG, "net started, host %s:%u", CONFIG_LINSIGHT_HOST,
             (unsigned)CONFIG_LINSIGHT_PORT);
    return ESP_OK;
}
