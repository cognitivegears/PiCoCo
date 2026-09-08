/* picoco-host: TCP-socket DriveWire server for bring-up/testing on the Mac.
 * Wraps the platform-independent dw_server in a Becker-port-style TCP
 * listener (default port 65504) so dwtest.py, XRoar, etc. can talk to it. */
#include "console.h"
#include "dw.h"
#include "dw_store.h"
#include "log.h"
#include "plat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void send_sock(void *ctx, const uint8_t *buf, size_t n) {
    int fd = *(int *)ctx;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, buf + sent, n - sent, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return; /* peer gone; dw_server has no error channel for this */
        }
        sent += (size_t)w;
    }
}

static void print_stdout(void *ctx, const char *s) {
    (void)ctx;
    fputs(s, stdout);
}

static void drain_log(void) {
    char buf[4096];
    size_t n = log_drain(buf, sizeof(buf) - 1);
    if (n) {
        buf[n] = '\0';
        fputs(buf, stderr);
    }
}

static void print_stats(const dw_stats *st) {
    printf("dw stats: reads=%u writes=%u read_err=%u write_err=%u crc_err=%u "
           "timeouts=%u unknown_op=%u notrdy=%u\n",
           st->reads, st->writes, st->read_err, st->write_err, st->crc_err,
           st->timeouts, st->unknown_op, st->notrdy);
    for (int i = 0; i < 256; i++) {
        if (st->ops[i]) printf("  op %#04x: %u\n", i, st->ops[i]);
    }
}

/* Parses "N=FILE" or "N=FILE,ro" -> drive, name, read_only. Returns 0 ok. */
static int parse_mount(char *arg, int *drive, const char **name, bool *read_only) {
    char *eq = strchr(arg, '=');
    if (!eq) return -1;
    *eq = '\0';
    *drive = atoi(arg);
    char *file = eq + 1;
    char *comma = strchr(file, ',');
    *read_only = false;
    if (comma) {
        if (strcmp(comma + 1, "ro") == 0) *read_only = true;
        *comma = '\0';
    }
    *name = file;
    return 0;
}

/* Replays a "dw capture" file: chunks of dir (0 rx / 1 tx), len_lo, len_hi,
 * bytes (see tests/fixtures/README.md). Only dir-0 (rx) chunks are fed back
 * in; dir-1 (tx) chunks are what the server sent when the capture was made
 * and are ignored here. */
static int run_replay(dw_server *srv, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "picoco-host: can't open --replay %s\n", path); return 1; }
    uint8_t hdr[3];
    uint8_t chunk[264];
    while (1) {
        size_t got = fread(hdr, 1, sizeof(hdr), f);
        if (got == 0) break;
        if (got != sizeof(hdr)) {
            fprintf(stderr, "picoco-host: --replay %s: truncated chunk header\n", path);
            fclose(f);
            return 1;
        }
        int dir = hdr[0];
        uint16_t len = (uint16_t)(hdr[1] | (hdr[2] << 8));
        if (len > sizeof(chunk)) {
            fprintf(stderr, "picoco-host: --replay %s: chunk too large (%u)\n", path, (unsigned)len);
            fclose(f);
            return 1;
        }
        if (fread(chunk, 1, len, f) != len) {
            fprintf(stderr, "picoco-host: --replay %s: truncated chunk body\n", path);
            fclose(f);
            return 1;
        }
        if (dir == 0) {
            dw_feed(srv, chunk, len, plat_now_ms());
            dw_tick(srv, plat_now_ms());
        }
    }
    fclose(f);
    print_stats(&srv->stats);
    return 0;
}

static int run_server(dw_server *srv, int port) {
    signal(SIGPIPE, SIG_IGN); /* macOS has no MSG_NOSIGNAL; ignore instead */

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 1) < 0) { perror("listen"); close(listen_fd); return 1; }
    printf("picoco-host: listening on port %d\n", port);

    int client_fd = -1;
    bool stdin_open = true;
    while (!g_stop) {
        struct pollfd pfds[3];
        int npfd = 0;
        pfds[npfd].fd = listen_fd; pfds[npfd].events = POLLIN; npfd++;
        int client_idx = -1;
        if (client_fd >= 0) { client_idx = npfd; pfds[npfd].fd = client_fd; pfds[npfd].events = POLLIN; npfd++; }
        int stdin_idx = -1;
        if (stdin_open) { stdin_idx = npfd; pfds[npfd].fd = STDIN_FILENO; pfds[npfd].events = POLLIN; npfd++; }

        int pr = poll(pfds, (nfds_t)npfd, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (client_fd < 0 && (pfds[0].revents & POLLIN)) {
            int fd = accept(listen_fd, NULL, NULL);
            if (fd >= 0) {
                client_fd = fd;
                srv->send_ctx = &client_fd;
                printf("picoco-host: client connected\n");
            }
        }
        if (client_idx >= 0 && (pfds[client_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t buf[512];
            ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                dw_feed(srv, buf, (size_t)n, plat_now_ms());
            } else {
                printf("picoco-host: client disconnected\n");
                close(client_fd);
                client_fd = -1;
                srv->state = DW_IDLE;
            }
        }
        if (stdin_idx >= 0 && (pfds[stdin_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint8_t buf[256];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                console_feed(buf, (size_t)n);
            } else {
                stdin_open = false; /* EOF: stop reading stdin, but keep serving */
                g_stop = 1;         /* treat stdin EOF as quit, per manual-check contract */
            }
        }

        dw_tick(srv, plat_now_ms());
        drain_log();
    }

    if (client_fd >= 0) close(client_fd);
    close(listen_fd);
    print_stats(&srv->stats);
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = ".";
    int port = 65504;
    const char *replay = NULL;
    bool hdbdos_set = false, hdbdos = false;
    struct { int drive; const char *name; bool read_only; } mounts[DW_MAX_DRIVES];
    int nmounts = 0;

    static struct option longopts[] = {
        {"dir", required_argument, 0, 'd'},
        {"port", required_argument, 0, 'p'},
        {"mount", required_argument, 0, 'm'},
        {"hdbdos", required_argument, 0, 'H'},
        {"replay", required_argument, 0, 'r'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "d:p:m:H:r:", longopts, NULL)) != -1) {
        switch (c) {
            case 'd': dir = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'm': {
                if (nmounts >= DW_MAX_DRIVES) {
                    fprintf(stderr, "picoco-host: too many --mount options\n");
                    return 1;
                }
                int drive; const char *name; bool ro;
                if (parse_mount(optarg, &drive, &name, &ro) != 0) {
                    fprintf(stderr, "picoco-host: bad --mount %s (want N=FILE[,ro])\n", optarg);
                    return 1;
                }
                mounts[nmounts].drive = drive;
                mounts[nmounts].name = name;
                mounts[nmounts].read_only = ro;
                nmounts++;
                break;
            }
            case 'H':
                hdbdos_set = true;
                hdbdos = strcmp(optarg, "on") == 0;
                if (!hdbdos && strcmp(optarg, "off") != 0) {
                    fprintf(stderr, "picoco-host: --hdbdos wants on|off\n");
                    return 1;
                }
                break;
            case 'r': replay = optarg; break;
            default:
                fprintf(stderr, "usage: %s [--dir DIR] [--port N] [--mount N=FILE[,ro]]... "
                                "[--hdbdos on|off] [--replay FILE]\n", argv[0]);
                return 1;
        }
    }

    plat_host_set_dir(dir);
    dw_store store;
    dw_store_posix_init(&store, dir);

    dw_server srv;
    int dummy_fd = -1;
    dw_init(&srv, &store, send_sock, &dummy_fd);
    if (hdbdos_set) srv.hdbdos = hdbdos;
    printf("picoco-host: hdbdos=%s\n", srv.hdbdos ? "on" : "off");

    for (int i = 0; i < nmounts; i++) {
        int r = dw_mount(&srv, mounts[i].drive, mounts[i].name, mounts[i].read_only);
        if (r != 0) {
            fprintf(stderr, "picoco-host: mount drive %d = %s failed (%d)\n",
                    mounts[i].drive, mounts[i].name, r);
            return 1;
        }
        printf("picoco-host: drive %d = %s%s\n", mounts[i].drive, mounts[i].name,
               mounts[i].read_only ? " (ro)" : "");
    }

    console_init(print_stdout, NULL, &srv, &store);
    int cfg_lines = console_run_config();
    if (cfg_lines >= 0) printf("picoco-host: config ran %d lines\n", cfg_lines);

    if (replay) return run_replay(&srv, replay);

    signal(SIGINT, on_sigint);
    return run_server(&srv, port);
}
