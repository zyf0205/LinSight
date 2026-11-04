/*
 * linsight-cli — control / inspect a running linsightd.
 *
 *   linsight-cli stats                     one JSON status line from the daemon
 *   linsight-cli live                      stream candump-format frames
 *   linsight-cli pause | resume | clear    forward CONTROL to the device, print ACK
 *   linsight-cli export asc|candump F.lsc [-o OUT]
 *                                          convert an .lsc recording
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "canlog.h"

#define DEFAULT_CTL "/tmp/linsight.sock"
#define REPLY_MAX 4096

static int ctl_connect(const char *path)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Sends cmd, prints the reply until EOF (or one line for commands that close). */
static int ctl_cmd(const char *path, const char *cmd)
{
    const int fd = ctl_connect(path);
    if (fd < 0) {
        fprintf(stderr, "linsight-cli: cannot connect to %s: %s\n", path,
                strerror(errno));
        fprintf(stderr, "(is linsightd running?)\n");
        return 1;
    }

    char msg[64];
    const int n = snprintf(msg, sizeof(msg), "%s\n", cmd);
    if (write(fd, msg, (size_t)n) != n) {
        fprintf(stderr, "linsight-cli: send failed\n");
        close(fd);
        return 1;
    }

    static char buf[REPLY_MAX];
    ssize_t got;
    for (;;) {
        got = read(fd, buf, sizeof(buf));
        if (got > 0) {
            fwrite(buf, 1, (size_t)got, stdout);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && errno != 0) {
            fprintf(stderr, "linsight-cli: read failed: %s\n", strerror(errno));
        }
        break;
    }
    close(fd);
    return 0;
}

static int cmd_export(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: linsight-cli export asc|candump FILE.lsc [-o OUT]\n");
        return 1;
    }
    const char *fmt = argv[2];
    const char *lsc = argv[3];
    const char *out = "-";
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        }
    }

    long n;
    if (strcmp(fmt, "asc") == 0) {
        n = canlog_export_asc(lsc, out);
    } else if (strcmp(fmt, "candump") == 0) {
        n = canlog_export_candump(lsc, out);
    } else {
        fprintf(stderr, "linsight-cli: unknown format '%s' (asc|candump)\n", fmt);
        return 1;
    }
    if (n < 0) {
        fprintf(stderr, "linsight-cli: export failed: %s\n", strerror(errno));
        return 1;
    }
    if (strcmp(out, "-") != 0) {
        printf("%s: %ld frames -> %s\n", fmt, n, out);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: linsight-cli stats|live|pause|resume|clear [--ctl PATH]\n"
                "       linsight-cli export asc|candump FILE.lsc [-o OUT]\n");
        return 1;
    }

    const char *ctl = DEFAULT_CTL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctl") == 0 && i + 1 < argc) {
            ctl = argv[++i];
        }
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "export") == 0) {
        return cmd_export(argc, argv);
    }
    if (strcmp(cmd, "stats") == 0 || strcmp(cmd, "live") == 0 ||
        strcmp(cmd, "pause") == 0 || strcmp(cmd, "resume") == 0 ||
        strcmp(cmd, "clear") == 0) {
        return ctl_cmd(ctl, cmd);
    }

    fprintf(stderr, "linsight-cli: unknown command '%s'\n", cmd);
    return 1;
}
