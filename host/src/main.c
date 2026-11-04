/*
 * linsightd — LinSight CAN host daemon (M2).
 *
 * epoll main loop: TCP listener (the device), Unix-socket listener (the CLI),
 * signalfd (SIGTERM/SIGINT graceful exit) and a 1 Hz timerfd (stats).
 * One device session at a time; CAN_BATCH records flow into a bounded ring
 * drained by a writer thread that appends to per-session .lsc files.
 *
 * CLI commands (newline-terminated over the control socket):
 *   stats   -> one JSON status line, socket closes
 *   live    -> candump lines streamed until the client disconnects
 *   pause / resume / clear -> forwarded to the device as CONTROL, ACK echoed
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <time.h>
#include <unistd.h>

#include "canlog.h"
#include "frame.h"

#define TAG "linsightd"

#define WRING_CAP 16384u /* records; ~320 KB */
#define CLI_MAX 8
#define RX_BUF_SIZE 4096
#define IDLE_TIMEOUT_MS 30000u

static const char *g_port = "8888";
static const char *g_ctl_path = "/tmp/linsight.sock";
static const char *g_log_dir = ".";

/* ------------------------------------------------------------ write ring */

typedef struct {
    linsight_can_record_t rec[WRING_CAP];
    uint32_t head; /* next write index */
    uint32_t count;
    uint64_t dropped;
    uint64_t written;

    bool open_req;
    char open_path[512];
    bool close_req;

    bool done;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} wring_t;

static wring_t g_ring = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static void wring_push(const linsight_can_record_t *recs, uint32_t n)
{
    pthread_mutex_lock(&g_ring.lock);
    for (uint32_t i = 0; i < n; i++) {
        if (g_ring.count == WRING_CAP) {
            g_ring.head = (g_ring.head + 1) % WRING_CAP; /* drop oldest */
            g_ring.count--;
            g_ring.dropped++;
        }
        g_ring.rec[(g_ring.head + g_ring.count) % WRING_CAP] = recs[i];
        g_ring.count++;
    }
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
}

static void *writer_thread(void *arg)
{
    (void)arg;
    lsc_writer_t w = {0};
    bool open = false;

    for (;;) {
        pthread_mutex_lock(&g_ring.lock);
        while (g_ring.count == 0 && !g_ring.open_req && !g_ring.close_req && !g_ring.done) {
            pthread_cond_wait(&g_ring.cond, &g_ring.lock);
        }
        const bool finish = g_ring.done;
        const bool open_now = g_ring.open_req;
        const bool close_now = g_ring.close_req;
        char path[512] = {0};
        if (open_now) {
            memcpy(path, g_ring.open_path, sizeof(path));
            g_ring.open_req = false;
        }
        g_ring.close_req = false;

        uint32_t n = g_ring.count;
        static linsight_can_record_t batch[512];
        if (n > 512) {
            n = 512;
        }
        for (uint32_t i = 0; i < n; i++) {
            batch[i] = g_ring.rec[g_ring.head];
            g_ring.head = (g_ring.head + 1) % WRING_CAP;
        }
        g_ring.count -= n;
        pthread_mutex_unlock(&g_ring.lock);

        if (open_now) {
            if (open) {
                lsc_writer_close(&w);
            }
            const uint32_t bitrate = 500000;
            open = lsc_writer_open(&w, path, bitrate, (uint64_t)time(NULL)) == 0;
            if (!open) {
                fprintf(stderr, TAG ": cannot open %s: %s\n", path, strerror(errno));
            }
        }
        if (close_now && open) {
            lsc_writer_close(&w);
            open = false;
        }

        for (uint32_t i = 0; i < n; i++) {
            if (lsc_writer_write(&w, &batch[i]) != 0) {
                fprintf(stderr, TAG ": log write failed: %s\n", strerror(errno));
                break;
            }
            g_ring.written++;
        }
        if (n > 0 && open) {
            lsc_writer_flush(&w);
        }

        if (finish) {
            pthread_mutex_lock(&g_ring.lock);
            const bool empty = g_ring.count == 0;
            pthread_mutex_unlock(&g_ring.lock);
            if (empty) {
                if (open) {
                    lsc_writer_close(&w);
                }
                break;
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------- session */

typedef struct {
    int fd;
    linsight_parser_t parser;
    uint8_t payload[LINSIGHT_MAX_CAN_BATCH];
    char peer[64];
    uint64_t recs;
    uint64_t batches;
    char stats[512]; /* latest STATS JSON from the device */
    time_t last_rx;
} device_t;

typedef struct {
    int fd;
    uint8_t buf[128];
    int len;
    bool live;
    uint16_t pending_id; /* CONTROL id awaiting ACK, 0 = none */
} cli_conn_t;

static device_t g_dev = {.fd = -1};
static cli_conn_t g_cli[CLI_MAX];
static int g_epoll = -1;
static volatile sig_atomic_t g_stop;
static uint16_t g_ctrl_id;
static char g_current_log[512];
static uint64_t g_session_start;

static void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t left = len;
    while (left) {
        const ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        left -= (size_t)n;
    }
    return true;
}

static void send_frame(int fd, uint8_t type, const void *payload, uint16_t len)
{
    uint8_t hdr[LINSIGHT_FRAME_HEADER_SIZE];
    linsight_frame_write_header(hdr, type, len);
    if (!send_all(fd, hdr, sizeof(hdr)) ||
        (len && !send_all(fd, payload, len))) {
        fprintf(stderr, TAG ": send_frame type %u failed: %s\n", type, strerror(errno));
    }
}

static bool send_control(int fd, const char *action)
{
    char json[64];
    const int n = snprintf(json, sizeof(json), "{\"id\":%u,\"action\":\"%s\"}",
                           (unsigned)++g_ctrl_id, action);
    if (n <= 0) {
        return false;
    }
    send_frame(fd, LINSIGHT_FRAME_CONTROL, json, (uint16_t)n);
    return true;
}

/* ------------------------------------------------------------- frames */

static void deliver_live(const linsight_can_record_t *recs, uint32_t n)
{
    char line[128];
    for (uint32_t i = 0; i < n; i++) {
        for (int c = 0; c < CLI_MAX; c++) {
            if (g_cli[c].fd >= 0 && g_cli[c].live) {
                const int len = canlog_format_candump(&recs[i], g_session_start, line,
                                                      sizeof(line));
                if (len > 0 && !send_all(g_cli[c].fd, line, (size_t)len)) {
                    close(g_cli[c].fd);
                    g_cli[c].fd = -1;
                }
            }
        }
    }
}

static void handle_batch(const uint8_t *payload, uint16_t len)
{
    if (g_dev.fd < 0 || len < 2) {
        return;
    }
    const uint16_t count = linsight_get_u16(payload);
    if (count == 0 || len < (uint16_t)(2u + (uint32_t)count * LINSIGHT_REC_SIZE)) {
        fprintf(stderr, TAG ": bad batch len %u count %u\n", (unsigned)len,
                (unsigned)count);
        return;
    }

    static linsight_can_record_t recs[LINSIGHT_BATCH_MAX_RECORDS];
    uint32_t valid = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = payload + 2 + (size_t)i * LINSIGHT_REC_SIZE;
        linsight_can_record_t r;
        if (linsight_record_unpack(p, &r)) {
            recs[valid++] = r;
        }
    }
    if (valid < count) {
        fprintf(stderr, TAG ": batch had %u invalid records\n", (unsigned)(count - valid));
    }

    g_dev.recs += valid;
    g_dev.batches++;
    if (valid) {
        wring_push(recs, valid);
        deliver_live(recs, valid);
    }
}

static void handle_frame(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    (void)ctx;
    switch (type) {
    case LINSIGHT_FRAME_HELLO:
        fprintf(stderr, TAG ": device hello: %.*s\n", (int)len, (const char *)payload);
        break;
    case LINSIGHT_FRAME_CAN_BATCH:
        handle_batch(payload, len);
        break;
    case LINSIGHT_FRAME_STATS:
        if (len < sizeof(g_dev.stats)) {
            memcpy(g_dev.stats, payload, len);
            g_dev.stats[len] = '\0';
        }
        break;
    case LINSIGHT_FRAME_EVENT:
        fprintf(stderr, TAG ": device event: %.*s\n", (int)len, (const char *)payload);
        break;
    case LINSIGHT_FRAME_PING:
        send_frame(g_dev.fd, LINSIGHT_FRAME_PONG, payload, len);
        break;
    case LINSIGHT_FRAME_PONG:
        fprintf(stderr, TAG ": device pong (%u bytes)\n", (unsigned)len);
        break;
    case LINSIGHT_FRAME_ACK: {
        char ack[64] = {0};
        memcpy(ack, payload, len < sizeof(ack) - 1 ? len : sizeof(ack) - 1);
        for (int c = 0; c < CLI_MAX; c++) {
            if (g_cli[c].fd >= 0 && g_cli[c].pending_id != 0) {
                g_cli[c].pending_id = 0;
                send_all(g_cli[c].fd, ack, strlen(ack));
                send_all(g_cli[c].fd, "\n", 1);
            }
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------- sessions */

static void device_close(void)
{
    if (g_dev.fd < 0) {
        return;
    }
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, g_dev.fd, NULL);
    close(g_dev.fd);
    g_dev.fd = -1;
    fprintf(stderr, TAG ": device session ended (%llu records)\n",
            (unsigned long long)g_dev.recs);
    pthread_mutex_lock(&g_ring.lock);
    g_ring.close_req = true;
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
    g_current_log[0] = '\0';
}

static void device_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }
    if (g_dev.fd >= 0) {
        fprintf(stderr, TAG ": replacing existing device session\n");
        device_close();
    }

    set_nonblock(fd);
    static uint8_t one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    g_dev.fd = fd;
    g_dev.recs = 0;
    g_dev.batches = 0;
    g_dev.last_rx = time(NULL);
    g_dev.stats[0] = '\0';
    linsight_parser_reset(&g_dev.parser);
    g_session_start = (uint64_t)time(NULL);

    snprintf(g_dev.peer, sizeof(g_dev.peer), "device");
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 0}; /* u32 0 = device */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);

    pthread_mutex_lock(&g_ring.lock);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(g_ring.open_path, sizeof(g_ring.open_path),
             "%s/can-%04d%02d%02d-%02d%02d%02d.lsc", g_log_dir, tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    snprintf(g_current_log, sizeof(g_current_log), "%s", g_ring.open_path);
    g_ring.open_req = true;
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);

    fprintf(stderr, TAG ": device connected, logging to %s\n", g_current_log);
}

static void cli_close(cli_conn_t *c)
{
    epoll_ctl(g_epoll, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    c->fd = -1;
    c->live = false;
    c->pending_id = 0;
    c->len = 0;
}

static void cli_dispatch(cli_conn_t *c)
{
    char cmd[64] = {0};
    memcpy(cmd, c->buf, c->len < (int)sizeof(cmd) - 1 ? (size_t)c->len : sizeof(cmd) - 1);
    c->len = 0;
    for (char *p = cmd; *p; p++) {
        if (*p == '\n' || *p == '\r') {
            *p = '\0';
        }
    }

    if (strcmp(cmd, "stats") == 0) {
        char out[1024];
        pthread_mutex_lock(&g_ring.lock);
        const uint64_t written = g_ring.written;
        const uint64_t dropped = g_ring.dropped;
        pthread_mutex_unlock(&g_ring.lock);
        const int n = snprintf(
            out, sizeof(out),
            "{\"device\":%s,\"records\":%llu,\"written\":%llu,\"dropped\":%llu,"
            "\"log\":\"%s\",\"stats\":%s}\n",
            g_dev.fd >= 0 ? "true" : "false", (unsigned long long)g_dev.recs,
            (unsigned long long)written, (unsigned long long)dropped,
            g_current_log[0] ? g_current_log : "",
            g_dev.stats[0] ? g_dev.stats : "{}");
        send_all(c->fd, out, (size_t)n);
        cli_close(c);
    } else if (strcmp(cmd, "live") == 0) {
        c->live = true; /* records stream until the client goes away */
    } else if (strcmp(cmd, "pause") == 0 || strcmp(cmd, "resume") == 0 ||
               strcmp(cmd, "clear") == 0) {
        if (g_dev.fd < 0) {
            send_all(c->fd, "ERR no device\n", 14);
            cli_close(c);
            return;
        }
        /* Device protocol names the action "clear_stats". */
        send_control(g_dev.fd, strcmp(cmd, "clear") == 0 ? "clear_stats" : cmd);
        c->pending_id = g_ctrl_id;
    } else {
        send_all(c->fd, "ERR unknown command\n", 21);
        cli_close(c);
    }
}

static void cli_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }

    cli_conn_t *slot = NULL;
    for (int i = 0; i < CLI_MAX; i++) {
        if (g_cli[i].fd < 0) {
            slot = &g_cli[i];
            break;
        }
    }
    if (!slot) {
        close(fd);
        return;
    }

    set_nonblock(fd);
    memset(slot, 0, sizeof(*slot));
    slot->fd = fd;
    /* Use u64 tags everywhere; .ptr/u32 union mixes are ambiguous on 64-bit. */
    struct epoll_event ev = {.events = EPOLLIN, .data.u64 = 0x100u + (uint64_t)(slot - g_cli)};
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, fd, &ev);
}

/* ---------------------------------------------------------------- main */

static int make_listener(const char *port, bool unix_socket)
{
    int fd;
    if (unix_socket) {
        unlink(g_ctl_path);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_ctl_path);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(fd, 4) < 0) {
            close(fd);
            return -1;
        }
        chmod(g_ctl_path, 0666);
    } else {
        fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            return -1;
        }
        static int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr = {.sin_family = AF_INET};
        addr.sin_port = htons((uint16_t)atoi(port));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
            close(fd);
            return -1;
        }
    }
    set_nonblock(fd);
    return fd;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_port = argv[++i];
        } else if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            g_ctl_path = argv[++i];
        } else if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            g_log_dir = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: linsightd [--port N] [--ctl PATH] [--log-dir DIR]\n");
            return 1;
        }
    }
    mkdir(g_log_dir, 0755);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    const int sigfd = signalfd(-1, &mask, 0);

    const int tcp_fd = make_listener(g_port, false);
    const int unix_fd = make_listener(g_ctl_path, true);
    if (tcp_fd < 0 || unix_fd < 0 || sigfd < 0) {
        fprintf(stderr, TAG ": listener setup failed: %s\n", strerror(errno));
        return 1;
    }

    g_epoll = epoll_create1(0);
    for (int i = 0; i < CLI_MAX; i++) {
        g_cli[i].fd = -1;
    }
    linsight_parser_init(&g_dev.parser, g_dev.payload, LINSIGHT_MAX_CAN_BATCH);

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u32 = 1; /* tcp listener */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, tcp_fd, &ev);
    ev.data.u32 = 2; /* unix listener */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, unix_fd, &ev);
    ev.data.u32 = 3; /* signalfd */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, sigfd, &ev);

    const int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = {
        .it_interval = {.tv_sec = 1},
        .it_value = {.tv_sec = 1},
    };
    timerfd_settime(tfd, 0, &its, NULL);
    ev.data.u32 = 4; /* timer */
    epoll_ctl(g_epoll, EPOLL_CTL_ADD, tfd, &ev);

    pthread_t writer;
    pthread_create(&writer, NULL, writer_thread, NULL);

    fprintf(stderr, TAG ": listening on :%s, ctl %s, logs %s\n", g_port, g_ctl_path,
            g_log_dir);

    uint64_t last_recs = 0;
    while (!g_stop) {
        struct epoll_event events[8];
        const int n = epoll_wait(g_epoll, events, 8, -1);
        for (int i = 0; i < n; i++) {
            const uint64_t tag = events[i].data.u64;
            cli_conn_t *cli = NULL;
            if (tag >= 0x100u && tag < 0x100u + CLI_MAX) {
                cli = &g_cli[tag - 0x100u];
            }

            if (tag == 3) { /* signal */
                struct signalfd_siginfo si;
                while (read(sigfd, &si, sizeof(si)) == sizeof(si)) {
                    g_stop = 1;
                }
            } else if (tag == 4) { /* 1 Hz stats */
                uint64_t misses;
                while (read(tfd, &misses, sizeof(misses)) == sizeof(misses)) {
                }
                pthread_mutex_lock(&g_ring.lock);
                const uint64_t written = g_ring.written;
                const uint64_t dropped = g_ring.dropped;
                pthread_mutex_unlock(&g_ring.lock);
                fprintf(stderr, TAG ": recs=%llu (+%llu) written=%llu dropped=%llu\n",
                        (unsigned long long)g_dev.recs,
                        (unsigned long long)(g_dev.recs - last_recs),
                        (unsigned long long)written, (unsigned long long)dropped);
                last_recs = g_dev.recs;
            } else if (tag == 1) {
                device_accept(tcp_fd);
            } else if (tag == 2) {
                cli_accept(unix_fd);
            } else if (tag == 0) { /* device socket */
                uint8_t rx[RX_BUF_SIZE];
                const ssize_t got = recv(g_dev.fd, rx, sizeof(rx), 0);
                if (got <= 0) {
                    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        continue;
                    }
                    device_close();
                    continue;
                }
                g_dev.last_rx = time(NULL);
                if (linsight_parser_feed(&g_dev.parser, rx, (size_t)got, handle_frame,
                                         NULL) != LINSIGHT_PARSE_OK) {
                    fprintf(stderr, TAG ": protocol error, closing device\n");
                    device_close();
                }
            } else { /* cli conn */
                const ssize_t got =
                    recv(cli->fd, cli->buf + cli->len, sizeof(cli->buf) - (size_t)cli->len, 0);
                if (got <= 0) {
                    cli_close(cli);
                    continue;
                }
                cli->len += (int)got;
                if (memchr(cli->buf, '\n', (size_t)cli->len)) {
                    cli_dispatch(cli);
                }
            }
        }
    }

    device_close();
    for (int i = 0; i < CLI_MAX; i++) {
        if (g_cli[i].fd >= 0) {
            cli_close(&g_cli[i]);
        }
    }
    pthread_mutex_lock(&g_ring.lock);
    g_ring.done = true;
    pthread_cond_signal(&g_ring.cond);
    pthread_mutex_unlock(&g_ring.lock);
    pthread_join(writer, NULL);
    close(tcp_fd);
    close(unix_fd);
    unlink(g_ctl_path);
    fprintf(stderr, TAG ": exited cleanly\n");
    return 0;
}
