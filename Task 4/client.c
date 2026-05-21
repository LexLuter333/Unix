#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
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

static int open_socket(const char *socket_path) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t len = strlen(socket_path);
    if (len >= sizeof(addr.sun_path)) len = sizeof(addr.sun_path) - 1;
    memcpy(addr.sun_path, socket_path, len);
    addr.sun_path[len] = '\0';
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("write: %s", strerror(errno));
        }
        sent += n;
    }
}

static void read_response(int fd) {
    char buffer[4096];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, n, stdout);
    }
    if (n < 0) die("read: %s", strerror(errno));
}

int main(int argc, char **argv) {
    char config_path[256] = "config";
    char message[1024] = "";
    bool single = false;
    int opt;

    while ((opt = getopt(argc, argv, "c:m:")) != -1) {
        switch (opt) {
            case 'c': strncpy(config_path, optarg, sizeof(config_path) - 1); break;
            case 'm': strncpy(message, optarg, sizeof(message) - 1); single = true; break;
            default: die("Usage: %s [-c config] [-m message]", argv[0]);
        }
    }

    char socket_path[256];
    if (parse_config(config_path, socket_path, sizeof(socket_path)) < 0) {
        die("Unable to read config '%s'", config_path);
    }

    int sock = open_socket(socket_path);
    if (sock < 0) die("Unable to connect to socket '%s'", socket_path);

    if (single) {
        char buf[1024];
        size_t len = snprintf(buf, sizeof(buf), "%s\n", message);
        send_all(sock, buf, len);
        shutdown(sock, SHUT_WR);
        read_response(sock);
        close(sock);
        return 0;
    }

    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        send_all(sock, line, len);
        char resp[1024];
        ssize_t n = read(sock, resp, sizeof(resp) - 1);
        if (n < 0) die("read: %s", strerror(errno));
        if (n == 0) break;
        resp[n] = '\0';
        fputs(resp, stdout);
    }

    close(sock);
    return 0;
}
