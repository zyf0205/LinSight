#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "can_ring.h"
#include "esp_check.h"
#include "esp_event.h"
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
#include "sdkconfig.h"
#include "settings.h"
#include "twai.h"
#include "ui.h"

#define TAG "net"

/* Bench endpoint comes from Kconfig (LinSight menu), so production builds can
 * override it without editing this file. */

#define WIFI_CONNECTED_BIT BIT0
#define NET_TASK_STACK 8192
#define NET_TASK_PRIO 5
#define NET_CORE 0

#define RX_BUF_SIZE 1024
#define PARSER_BUF_SIZE 256u
#define PING_INTERVAL_MS 5000u
#define HOST_TIMEOUT_MS 15000u
#define RECONNECT_DELAY_MIN_MS 1000u
#define RECONNECT_DELAY_MAX_MS 5000u
/* Batches (max 819 records each) sent per session-loop iteration. */
#define MAX_BATCHES_PER_TICK 2

#define DISCOVERY_PROBE "LINSIGHT?1"
#define DISCOVERY_PROBE_LEN 10u
#define DISCOVERY_REPLY_PREFIX "LINSIGHT!1 "
#define DISCOVERY_REPLY_PREFIX_LEN 11u
#define DISCOVERY_TIMEOUT_MS 300
#define DISCOVERY_ATTEMPTS 2
#define DISCOVERY_RETRY_FAILURES 3u

static int tcp_connect_addr(uint32_t ip_be, uint16_t port, int timeout_ms, int *err_out);
static bool discover_host(uint32_t *ip_be, uint16_t *port);
static esp_err_t wifi_init(void);

static EventGroupHandle_t s_wifi_events;

static linsight_parser_t s_parser;
static uint8_t *s_tx_buf;                 /* PSRAM: header + one CAN_BATCH payload */
static linsight_can_record_t *s_pop;      /* PSRAM: LINSIGHT_BATCH_MAX_RECORDS */
static size_t s_pending_len;              /* packed frame bytes awaiting send; survives reconnect */

static uint32_t s_ping_value;
static uint64_t s_ping_sent_us;

/* Host discovery state. Only net_task mutates the endpoint; the IP event
 * handler publishes the subnet broadcast and a dirty flag. */
static esp_ip4_addr_t s_bcast_addr;
static bool s_bcast_valid;
static bool s_need_discovery = true;
static uint32_t s_host_ip;      /* network byte order */
static uint16_t s_host_port;    /* host byte order */
static bool s_host_valid;

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

/* Small session frames (<= 256 B payload: HELLO/EVENT/PING/PONG) built on the
 * stack. */
static bool send_frame(int fd, uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[LINSIGHT_FRAME_HEADER_SIZE + LINSIGHT_MAX_HELLO];

    linsight_frame_write_header(frame, type, len);
    if (len) {
        memcpy(frame + LINSIGHT_FRAME_HEADER_SIZE, payload, len);
    }
    return send_all(fd, frame, LINSIGHT_FRAME_HEADER_SIZE + len);
}

/* ------------------------------------------------------------- batch path */

/* Send up to MAX_BATCHES_PER_TICK batches; returns false on socket error.
 * A popped batch stays packed in s_tx_buf until its whole frame is sent, so a
 * failed send/reconnect does not consume ring records. The host dedupes a
 * possible partial resend by ts_ms/seq. */
static bool send_batches(int fd)
{
    for (int i = 0; i < MAX_BATCHES_PER_TICK; i++) {
        if (s_pending_len == 0) {
            const uint16_t count =
                (uint16_t)can_ring_pop(s_pop, LINSIGHT_BATCH_MAX_RECORDS);
            if (count == 0) {
                return true;
            }
            const size_t payload = linsight_batch_pack(
                s_tx_buf + LINSIGHT_FRAME_HEADER_SIZE, LINSIGHT_MAX_CAN_BATCH,
                s_pop, count);
            linsight_frame_write_header(s_tx_buf, LINSIGHT_FRAME_CAN_BATCH,
                                        (uint16_t)payload);
            s_pending_len = LINSIGHT_FRAME_HEADER_SIZE + payload;
        }

        if (!send_all(fd, s_tx_buf, s_pending_len)) {
            return false;
        }
        s_pending_len = 0;
    }
    return true;
}

/* --------------------------------------------------------------- session */

static bool send_hello(int fd)
{
    char json[LINSIGHT_MAX_HELLO];
    const int n = snprintf(
        json, sizeof(json),
        "{\"proto\":%u,\"fw\":\"%s\",\"device_id\":\"%s\",\"boot\":%u,\"uptime_ms\":%u,"
        "\"bitrate\":\"%s\",\"mode\":\"%s\"}",
        (unsigned)LINSIGHT_PROTO_VERSION, LINSIGHT_FW_VERSION, linsight_device_id(),
        (unsigned)linsight_boot_count(), (unsigned)(esp_timer_get_time() / 1000),
        can_bitrate_name(twai_current_bitrate()), can_mode_name(twai_current_mode()));
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return false;
    }
    return send_frame(fd, LINSIGHT_FRAME_HELLO, (const uint8_t *)json, (uint16_t)n);
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
        if (n <= 0 || (size_t)n >= sizeof(json)) {
            return false;
        }
        if (!send_frame(fd, LINSIGHT_FRAME_EVENT, (const uint8_t *)json, (uint16_t)n)) {
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
        send_frame(*fd, LINSIGHT_FRAME_PONG, payload, len);
        break;

    case LINSIGHT_FRAME_PONG:
        if (len == LINSIGHT_MAX_PONG && s_ping_sent_us &&
            linsight_get_u32(payload) == s_ping_value) {
            const uint32_t rtt =
                (uint32_t)((esp_timer_get_time() - s_ping_sent_us) / 1000);
            linsight_ui_set_conn(UI_CONN_ONLINE, rtt);
            s_ping_sent_us = 0;
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

    linsight_parser_reset(&s_parser);

    if (!send_hello(fd) || !send_ping(fd)) {
        return;
    }
    last_ping = last_rx_ms;
    linsight_ui_set_conn(UI_CONN_ONLINE, 0);

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

    /* Pending batch stays packed in s_tx_buf for the next session. */
    linsight_ui_set_conn(UI_CONN_RECONNECT, 0);
    ESP_LOGI(TAG, "TCP session ended");
}

static void net_task(void *arg)
{
    (void)arg;

    linsight_ui_set_conn(UI_CONN_WIFI, 0);

    /* Wi-Fi is initialized here, not in net_start(), so the task stack is
     * allocated before the driver claims ~30 KB of internal RAM. */
    const esp_err_t wifi_err = wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(wifi_err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "connecting Wi-Fi SSID=\"%s\"", CONFIG_LINSIGHT_WIFI_SSID);

    uint32_t backoff_ms = RECONNECT_DELAY_MIN_MS;
    uint32_t fail_streak = 0;

    for (;;) {
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            linsight_ui_set_conn(UI_CONN_WIFI, 0);
            esp_wifi_connect();
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        if (__atomic_load_n(&s_need_discovery, __ATOMIC_ACQUIRE) || !s_host_valid) {
            uint32_t ip = 0;
            uint16_t port = 0;
            if (discover_host(&ip, &port)) {
                s_host_ip = ip;
                s_host_port = port;
                s_host_valid = true;
                linsight_ui_set_host_endpoint(ip, port);
                esp_ip4_addr_t ep = {.addr = ip};
                ESP_LOGI(TAG, "discovered host " IPSTR ":%u", IP2STR(&ep), (unsigned)port);
            } else if (!s_host_valid) {
                struct in_addr fallback;
                if (inet_pton(AF_INET, CONFIG_LINSIGHT_HOST_IP, &fallback) == 1) {
                    s_host_ip = fallback.s_addr;
                    s_host_port = CONFIG_LINSIGHT_HOST_PORT;
                    s_host_valid = true;
                    linsight_ui_set_host_endpoint(s_host_ip, s_host_port);
                    ESP_LOGI(TAG, "using fallback host %s:%u",
                             CONFIG_LINSIGHT_HOST_IP, (unsigned)CONFIG_LINSIGHT_HOST_PORT);
                } else {
                    ESP_LOGE(TAG, "bad fallback host address: %s", CONFIG_LINSIGHT_HOST_IP);
                }
            }
            __atomic_store_n(&s_need_discovery, false, __ATOMIC_RELEASE);
        }

        if (!s_host_valid) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MAX_MS));
            continue;
        }

        linsight_ui_set_conn(UI_CONN_HOST, 0);
        int err = 0;
        const int fd = tcp_connect_addr(s_host_ip, s_host_port, 3000, &err);
        esp_ip4_addr_t ep = {.addr = s_host_ip};
        if (fd < 0) {
            fail_streak++;
            if (fail_streak == 1u || fail_streak % 10u == 0u) {
                ESP_LOGW(TAG, "host " IPSTR ":%u unreachable (attempt %u, errno %d)",
                         IP2STR(&ep), (unsigned)s_host_port, (unsigned)fail_streak, err);
            }
            if (fail_streak % DISCOVERY_RETRY_FAILURES == 0u || err == EHOSTUNREACH ||
                err == ENETUNREACH) {
                __atomic_store_n(&s_need_discovery, true, __ATOMIC_RELEASE);
            }
            linsight_ui_set_conn(UI_CONN_RECONNECT, 0);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = backoff_ms < RECONNECT_DELAY_MAX_MS ? backoff_ms * 2u
                                                             : RECONNECT_DELAY_MAX_MS;
            continue;
        }

        backoff_ms = RECONNECT_DELAY_MIN_MS;
        fail_streak = 0;
        ESP_LOGI(TAG, "TCP session started with " IPSTR ":%u", IP2STR(&ep),
                 (unsigned)s_host_port);
        session_loop(fd);
        close(fd);
        linsight_ui_set_conn(UI_CONN_RECONNECT, 0);
        /* The host may have moved while the session was up; probe once before
         * the next connect. */
        __atomic_store_n(&s_need_discovery, true, __ATOMIC_RELEASE);
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MIN_MS));
    }
}

/* --------------------------------------------------------------- transport */

static int tcp_connect_addr(uint32_t ip_be, uint16_t port, int timeout_ms, int *err_out)
{
    int err = 0;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        err = errno;
        ESP_LOGW(TAG, "socket failed: errno %d", err);
        goto fail;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = ip_be;

    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&dest, sizeof(dest));
    if (rc < 0 && errno != EINPROGRESS) {
        /* Per-attempt failures are routine (host offline) — the caller owns
         * the visible summary. */
        err = errno;
        ESP_LOGD(TAG, "connect failed: errno %d", err);
        goto fail;
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
            err = rc == 0 ? ETIMEDOUT : errno;
            ESP_LOGD(TAG, "connect timeout");
            goto fail;
        }
        int so_error = 0;
        socklen_t optlen = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0 ||
            so_error != 0) {
            err = so_error ? so_error : errno;
            ESP_LOGD(TAG, "connect error: %d", err);
            goto fail;
        }
    }

    fcntl(fd, F_SETFL, flags);
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* recv() also paces streaming: batches are only flushed after it returns.
     * 50 ms keeps end-to-end latency low (max ~1.3 MB/s through 2x16 KB sends)
     * while still yielding to PING handling. A stuck host costs one bounded
     * 500 ms send and a reconnect; the ring keeps the backlog. */
    struct timeval rcv = {.tv_sec = 0, .tv_usec = 50000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    struct timeval snd = {.tv_sec = 0, .tv_usec = 500000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    if (err_out) {
        *err_out = 0;
    }
    return fd;

fail:
    if (err_out) {
        *err_out = err;
    }
    if (fd >= 0) {
        close(fd);
    }
    return -1;
}

static bool discover_host(uint32_t *ip_be, uint16_t *port)
{
#if CONFIG_LINSIGHT_DISCOVERY
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGW(TAG, "discovery socket failed: errno %d", errno);
        return false;
    }

    /* IP_SOF_BROADCAST is 0 in this IDF build, so this is best-effort for
     * forward compatibility. */
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGW(TAG, "discovery bind failed: errno %d", errno);
        close(fd);
        return false;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in targets[2] = {0};
    int target_count = 1;
    targets[0].sin_family = AF_INET;
    targets[0].sin_port = htons(CONFIG_LINSIGHT_DISCOVERY_PORT);
    targets[0].sin_addr.s_addr = htonl(INADDR_BROADCAST);

    const bool bcast_valid = __atomic_load_n(&s_bcast_valid, __ATOMIC_ACQUIRE);
    const uint32_t bcast = __atomic_load_n(&s_bcast_addr.addr, __ATOMIC_ACQUIRE);
    if (bcast_valid && bcast != htonl(INADDR_BROADCAST)) {
        targets[target_count].sin_family = AF_INET;
        targets[target_count].sin_port = htons(CONFIG_LINSIGHT_DISCOVERY_PORT);
        targets[target_count].sin_addr.s_addr = bcast;
        target_count++;
    }

    for (int attempt = 0; attempt < DISCOVERY_ATTEMPTS; attempt++) {
        bool sent = false;
        for (int i = 0; i < target_count; i++) {
            if (sendto(fd, DISCOVERY_PROBE, DISCOVERY_PROBE_LEN, 0,
                       (struct sockaddr *)&targets[i], sizeof(targets[i])) >= 0) {
                sent = true;
            } else {
                ESP_LOGD(TAG, "discovery send failed: errno %d", errno);
            }
        }
        if (!sent) {
            break;
        }

        const int64_t deadline = esp_timer_get_time() / 1000 + DISCOVERY_TIMEOUT_MS;
        while (true) {
            const int64_t now = esp_timer_get_time() / 1000;
            if (now >= deadline) {
                break;
            }
            const int64_t left = deadline - now;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);
            struct timeval tv = {
                .tv_sec = (time_t)(left / 1000),
                .tv_usec = (suseconds_t)((left % 1000) * 1000),
            };
            const int rc = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (rc < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ESP_LOGD(TAG, "discovery select failed: errno %d", errno);
                break;
            }
            if (rc == 0) {
                break;
            }

            for (;;) {
                char reply[64];
                struct sockaddr_in from;
                socklen_t from_len = sizeof(from);
                const int n = recvfrom(fd, reply, sizeof(reply) - 1, 0,
                                       (struct sockaddr *)&from, &from_len);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    ESP_LOGD(TAG, "discovery recv failed: errno %d", errno);
                    break;
                }
                reply[n] = '\0';
                if (n <= (int)DISCOVERY_REPLY_PREFIX_LEN ||
                    strncmp(reply, DISCOVERY_REPLY_PREFIX,
                            DISCOVERY_REPLY_PREFIX_LEN) != 0) {
                    continue;
                }
                char *end = NULL;
                const unsigned long reply_port =
                    strtoul(reply + DISCOVERY_REPLY_PREFIX_LEN, &end, 10);
                if (end == reply + DISCOVERY_REPLY_PREFIX_LEN || reply_port == 0 ||
                    reply_port > 65535 || (*end != '\n' && *end != '\r' && *end != '\0')) {
                    continue;
                }
                const uint32_t src = from.sin_addr.s_addr;
                if (src == 0 || src == htonl(INADDR_BROADCAST) ||
                    (ntohl(src) >> 28) == 0xEu) {
                    continue;
                }
                *ip_be = src;
                *port = (uint16_t)reply_port;
                close(fd);
                return true;
            }
        }
    }

    close(fd);
    return false;
#else
    (void)ip_be;
    (void)port;
    return false;
#endif
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
        const uint32_t mask = ev->ip_info.netmask.addr;
        const uint32_t bcast = (ev->ip_info.ip.addr & mask) | ~mask;
        esp_ip4_addr_t bcast_addr = {.addr = bcast};
        __atomic_store_n(&s_bcast_addr.addr, bcast, __ATOMIC_RELEASE);
        __atomic_store_n(&s_bcast_valid, mask != 0 && mask != 0xFFFFFFFFu,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_need_discovery, true, __ATOMIC_RELEASE);
        ESP_LOGI(TAG, "Wi-Fi got IP " IPSTR "/" IPSTR " gw " IPSTR " bcast " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
                 IP2STR(&ev->ip_info.gw), IP2STR(&bcast_addr));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        linsight_ui_set_conn(UI_CONN_HOST, 0);
    }
}

static esp_err_t wifi_init(void)
{
    /* NVS is initialized once by settings_init() before net_start(). */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_FAIL;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    const wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_country(&country), TAG, "wifi country");
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
    /* Parser scratch covers any host frame we read (PING today); larger
     * frames are skipped by the parser by design. */
    static uint8_t parser_scratch[PARSER_BUF_SIZE];
    linsight_parser_init(&s_parser, parser_scratch, PARSER_BUF_SIZE);

    s_tx_buf = heap_caps_malloc(LINSIGHT_FRAME_HEADER_SIZE + LINSIGHT_MAX_CAN_BATCH,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pop = heap_caps_malloc(LINSIGHT_BATCH_MAX_RECORDS * sizeof(*s_pop),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_buf || !s_pop) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_events = xEventGroupCreate();
    return s_wifi_events ? ESP_OK : ESP_ERR_NO_MEM;
}

static StackType_t *s_net_stack = NULL;
static StaticTask_t s_net_tcb;

esp_err_t net_start(void)
{
    ESP_RETURN_ON_ERROR(buffers_init(), TAG, "buffers");

    s_net_stack = heap_caps_malloc(NET_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_net_stack != NULL, ESP_ERR_NO_MEM, TAG, "net stack");

    TaskHandle_t th = xTaskCreateStaticPinnedToCore(net_task, "net", NET_TASK_STACK, NULL,
                                                    NET_TASK_PRIO, s_net_stack, &s_net_tcb,
                                                    NET_CORE);
    ESP_RETURN_ON_FALSE(th != NULL, ESP_ERR_NO_MEM, TAG, "net task");

#if CONFIG_LINSIGHT_DISCOVERY
    ESP_LOGI(TAG, "net started, discovery udp :%u, fallback %s:%u",
             (unsigned)CONFIG_LINSIGHT_DISCOVERY_PORT, CONFIG_LINSIGHT_HOST_IP,
             (unsigned)CONFIG_LINSIGHT_HOST_PORT);
#else
    ESP_LOGI(TAG, "net started, discovery disabled, host %s:%u", CONFIG_LINSIGHT_HOST_IP,
             (unsigned)CONFIG_LINSIGHT_HOST_PORT);
#endif
    return ESP_OK;
}
