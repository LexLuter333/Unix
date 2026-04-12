#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <sched.h>

volatile sig_atomic_t stop_flag = 0;
long lock_count = 0;
char target_path[256] = {0};
char lck_path[260] = {0};
const char *stats_file = "stats.txt";

void sigint_handler(int sig) { (void)sig; stop_flag = 1; }

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "f:")) != -1) {
        switch (opt) {
            case 'f':
                if (strlen(optarg) >= sizeof(target_path)) {
                    fprintf(stderr, "Error: filename too long\n");
                    exit(EXIT_FAILURE);
                }
                strcpy(target_path, optarg);
                break;
            default:
                fprintf(stderr, "Usage: %s -f <file>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    if (target_path[0] == '\0') {
        fprintf(stderr, "Error: target file not specified (-f)\n");
        exit(EXIT_FAILURE);
    }

    snprintf(lck_path, sizeof(lck_path), "%s.lck", target_path);

    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) { perror("sigaction"); exit(EXIT_FAILURE); }

    srand(getpid() ^ (time(NULL) << 8) ^ (clock() & 0xFFFF));

    int tgt_fd = open(target_path, O_CREAT | O_RDWR, 0644);
    if (tgt_fd == -1) { perror("open target"); exit(EXIT_FAILURE); }
    close(tgt_fd);

    while (!stop_flag) {
        int lck_fd = -1;
        while (1) {
            usleep(rand() % 80000 + 20000);
            sched_yield();
            lck_fd = open(lck_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (lck_fd >= 0) break;
            if (errno == EEXIST) {
                continue;
            } else {
                perror("open lock file");
                exit(EXIT_FAILURE);
            }
        }

        char pid_buf[32];
        int len = snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
        if (write(lck_fd, pid_buf, len) != len) {
            perror("write pid to lock"); close(lck_fd); exit(EXIT_FAILURE);
        }
        close(lck_fd);

        tgt_fd = open(target_path, O_RDWR);
        if (tgt_fd >= 0) {
            const char *msg = "working_data\n";
            write(tgt_fd, msg, strlen(msg));
            lseek(tgt_fd, 0, SEEK_SET);
            char rbuf[256];
            read(tgt_fd, rbuf, sizeof(rbuf));
            close(tgt_fd);
        }
        sleep(1);

        lck_fd = open(lck_path, O_RDONLY);
        if (lck_fd == -1) {
            fprintf(stderr, "PID %d: Lock file vanished during critical section!\n", getpid());
            exit(EXIT_FAILURE);
        }

        char rbuf[32] = {0};
        if (read(lck_fd, rbuf, sizeof(rbuf) - 1) <= 0) {
            fprintf(stderr, "PID %d: Lock file empty or corrupted!\n", getpid());
            close(lck_fd); exit(EXIT_FAILURE);
        }
        close(lck_fd);

        if (atoi(rbuf) != getpid()) {
            fprintf(stderr, "PID %d: Lock corrupted! Expected %d, got %s\n", getpid(), getpid(), rbuf);
            exit(EXIT_FAILURE);
        }

        if (unlink(lck_path) != 0 && errno != ENOENT) {
            perror("unlink lock"); exit(EXIT_FAILURE);
        }

        lock_count++;
    }

    int st_fd = open(stats_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (st_fd == -1) { perror("open stats file"); exit(EXIT_FAILURE); }
    char sline[64];
    int slen = snprintf(sline, sizeof(sline), "PID %d: %ld locks\n", getpid(), lock_count);
    if (write(st_fd, sline, slen) != slen) perror("write stats");
    close(st_fd);

    printf("PID %d: Graceful exit. Total successful locks: %ld\n", getpid(), lock_count);
    return 0;
}
