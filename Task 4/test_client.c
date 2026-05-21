#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
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

static void write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("write: %s", strerror(errno));
        }
        left -= n;
        p += n;
    }
}

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1000000.0;
}

int main(int argc, char **argv) {
    char config_path[256] = "config";
    char input_file[256] = "";
    char log_file[256] = "client.log";
    double delay = 0.0;
    int opt;

    while ((opt = getopt(argc, argv, "c:f:d:l:")) != -1) {
        switch (opt) {
            case 'c': strncpy(config_path, optarg, sizeof(config_path) - 1); break;
            case 'f': strncpy(input_file, optarg, sizeof(input_file) - 1); break;
            case 'd': delay = atof(optarg); break;
            case 'l': strncpy(log_file, optarg, sizeof(log_file) - 1); break;
            default: die("Usage: %s -f file [-c config] [-d delay] [-l log]", argv[0]);
        }
    }
    if (input_file[0] == '\0') die("Input file must be specified with -f");

    char socket_path[256];
    if (parse_config(config_path, socket_path, sizeof(socket_path)) < 0) {
        die("Cannot read config %s", config_path);
    }

    int sock = open_socket(socket_path);
    if (sock < 0) die("Cannot connect to socket %s", socket_path);

    FILE *input = fopen(input_file, "r");
    if (!input) die("Cannot open input file %s", input_file);

    FILE *log = fopen(log_file, "w");
    if (!log) die("Cannot open log file %s", log_file);

    srandom((unsigned int)(time(NULL) ^ getpid()));
    double t_start = now_seconds();
    double total_delay = 0.0;
    int line_count = 0;
    char buffer[8192];
    size_t buffer_len = 0;
    int bytes_since = 0;
    int next_delay_bytes = (random() % 255) + 1;

    while (1) {
        int c = fgetc(input);
        if (c == EOF) break;
        buffer[buffer_len++] = (char)c;
        if (c == '\n') line_count++;
        if (buffer_len == sizeof(buffer)) {
            write_all(sock, buffer, buffer_len);
            buffer_len = 0;
        }
        bytes_since++;
        if (bytes_since >= next_delay_bytes) {
            if (buffer_len > 0) {
                write_all(sock, buffer, buffer_len);
                buffer_len = 0;
            }
            double pause = ((double)random() / RAND_MAX) * delay;
            if (pause > 0) {
                total_delay += pause;
                usleep((useconds_t)(pause * 1000000.0));
            }
            bytes_since = 0;
            next_delay_bytes = (random() % 255) + 1;
        }
    }
    if (buffer_len > 0) {
        write_all(sock, buffer, buffer_len);
    }
    shutdown(sock, SHUT_WR);

    int expected_responses = line_count;
    int received_responses = 0;
    char recv_buf[8192];
    char reply[16384];
    size_t reply_len = 0;

    while (received_responses < expected_responses) {
        ssize_t n = read(sock, recv_buf, sizeof(recv_buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            die("read: %s", strerror(errno));
        }
        if (n == 0) break;
        size_t pos = 0;
        while (pos < (size_t)n) {
            char ch = recv_buf[pos++];
            reply[reply_len++] = ch;
            if (ch == '\n') {
                received_responses++;
                reply_len = 0;
            }
            if (reply_len >= sizeof(reply)) reply_len = 0;
        }
    }

    double t_end = now_seconds();
    fprintf(log, "START=%.6f\n", t_start);
    fprintf(log, "END=%.6f\n", t_end);
    fprintf(log, "DELAY=%.6f\n", total_delay);
    fprintf(log, "LINES=%d\n", line_count);
    fclose(log);
    fclose(input);
    close(sock);
    return 0;
}
