#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#define MAX_CLIENTS 256
#define INBUF_SIZE 8192
#define OUTBUF_SIZE 8192

struct client {
    int fd;
    bool active;
    char inbuf[INBUF_SIZE];
    size_t inlen;
    char outbuf[OUTBUF_SIZE];
    size_t outlen;
    size_t outpos;
};

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int parse_config(const char *path, char *socket_path, size_t maxlen) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "SOCKET_PATH=", 12) == 0) {
            size_t len = strlen(line + 12);
            if (len > 0 && line[12 + len - 1] == '\n') len--;
            if (len >= maxlen) len = maxlen - 1;
            memcpy(socket_path, line + 12, len);
            socket_path[len] = '\0';
            found = true;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static void log_msg(FILE *log, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log, fmt, ap);
    va_end(ap);
    fprintf(log, "\n");
    fflush(log);
}

static void close_client(struct client *clients, struct pollfd *pfds, int index, int *nfds, FILE *log) {
    int fd = clients[index].fd;
    log_msg(log, "DISCONNECT fd=%d", fd);
    close(fd);
    int last = (*nfds) - 2;
    if (index < last) {
        clients[index] = clients[last];
        pfds[index + 1] = pfds[last + 1];
    }
    (*nfds)--;
}

int main(int argc, char **argv) {
    char config_path[256] = "config";
    char log_path[256] = "server.log";
    int opt;

    while ((opt = getopt(argc, argv, "c:l:")) != -1) {
        switch (opt) {
            case 'c': strncpy(config_path, optarg, sizeof(config_path) - 1); break;
            case 'l': strncpy(log_path, optarg, sizeof(log_path) - 1); break;
            default: die("Usage: %s [-c config] [-l log]", argv[0]);
        }
    }

    char socket_path[256];
    if (parse_config(config_path, socket_path, sizeof(socket_path)) < 0) {
        die("Unable to read config file '%s'", config_path);
    }

    FILE *log = fopen(log_path, "a");
    if (!log) die("Cannot open log file '%s'", log_path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket: %s", strerror(errno));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t len = strlen(socket_path);
    if (len >= sizeof(addr.sun_path)) len = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, socket_path, len);
    addr.sun_path[len] = '\0';
    unlink(socket_path);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        die("bind(%s): %s", socket_path, strerror(errno));
    }
    if (listen(listen_fd, 128) < 0) die("listen: %s", strerror(errno));
    if (set_nonblocking(listen_fd) < 0) die("fcntl: %s", strerror(errno));

    struct client clients[MAX_CLIENTS];
    struct pollfd pfds[MAX_CLIENTS + 1];
    int nfds = 1;
    long state = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i].active = false;
        clients[i].fd = -1;
    }

    pfds[0].fd = listen_fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;

    log_msg(log, "SERVER_START socket=%s", socket_path);
    while (1) {
        int ready = poll(pfds, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            die("poll: %s", strerror(errno));
        }

        if (pfds[0].revents & POLLIN) {
            int client_fd = accept(listen_fd, NULL, NULL);
            if (client_fd >= 0) {
                if (set_nonblocking(client_fd) < 0) {
                    close(client_fd);
                } else if (nfds > MAX_CLIENTS) {
                    close(client_fd);
                    log_msg(log, "REFUSE too many connections");
                } else {
                    long brk = (long)sbrk(0);
                    int index = nfds - 1;
                    clients[index].fd = client_fd;
                    clients[index].active = true;
                    clients[index].inlen = 0;
                    clients[index].outlen = 0;
                    clients[index].outpos = 0;
                    pfds[nfds].fd = client_fd;
                    pfds[nfds].events = POLLIN;
                    pfds[nfds].revents = 0;
                    nfds++;
                    log_msg(log, "CONNECT fd=%d sbrk=%ld", client_fd, brk);
                }
            }
        }

        for (int i = 0; i < nfds - 1; ++i) {
            struct client *c = &clients[i];
            int idx = i + 1;
            if (!c->active) continue;
            if (pfds[idx].revents & (POLLHUP | POLLERR)) {
                close_client(clients, pfds, i, &nfds, log);
                i--;
                continue;
            }

            if ((pfds[idx].revents & POLLOUT) && c->outpos < c->outlen) {
                ssize_t sent = write(c->fd, c->outbuf + c->outpos, c->outlen - c->outpos);
                if (sent > 0) {
                    c->outpos += sent;
                    if (c->outpos == c->outlen) {
                        c->outpos = c->outlen = 0;
                        pfds[idx].events &= ~POLLOUT;
                    }
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    close_client(clients, pfds, i, &nfds, log);
                    i--;
                    continue;
                }
            }

            if (pfds[idx].revents & POLLIN) {
                ssize_t received = read(c->fd, c->inbuf + c->inlen, sizeof(c->inbuf) - c->inlen);
                if (received <= 0) {
                    if (received == 0) {
                        close_client(clients, pfds, i, &nfds, log);
                        i--;
                        continue;
                    }
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        close_client(clients, pfds, i, &nfds, log);
                        i--;
                        continue;
                    }
                } else {
                    c->inlen += received;
                    size_t processed = 0;
                    while (processed < c->inlen) {
                        char *newline = memchr(c->inbuf + processed, '\n', c->inlen - processed);
                        if (!newline) break;
                        size_t line_len = newline - (c->inbuf + processed);
                        char line[128] = {0};
                        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
                        memcpy(line, c->inbuf + processed, line_len);
                        line[line_len] = '\0';
                        processed += line_len + 1;
                        long value = 0;
                        bool valid = true;
                        char *endptr = NULL;
                        if (line_len == 0) valid = false;
                        else {
                            errno = 0;
                            value = strtol(line, &endptr, 10);
                            if (errno != 0 || endptr == line || *endptr != '\0') valid = false;
                        }
                        long new_state = state;
                        char reply[128];
                        if (valid) {
                            new_state += value;
                            state = new_state;
                            snprintf(reply, sizeof(reply), "%ld\n", state);
                        } else {
                            snprintf(reply, sizeof(reply), "ERR\n");
                        }
                        size_t reply_len = strlen(reply);
                        if (c->outlen + reply_len >= sizeof(c->outbuf)) {
                            close_client(clients, pfds, i, &nfds, log);
                            i--;
                            break;
                        }
                        memcpy(c->outbuf + c->outlen, reply, reply_len);
                        c->outlen += reply_len;
                        pfds[idx].events |= POLLOUT;
                        log_msg(log, "RECV fd=%d line='%s' state=%ld reply='%s'", c->fd, line, state, reply);
                    }
                    if (processed > 0) {
                        memmove(c->inbuf, c->inbuf + processed, c->inlen - processed);
                        c->inlen -= processed;
                    }
                }
            }
        }
    }

    fclose(log);
    return 0;
}
