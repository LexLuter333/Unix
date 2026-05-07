#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>

#define MAX_LINE 4096
#define MAX_PROCS 256
#define LOG_FILE "/tmp/myinit.log"
#define PID_FILE "/tmp/myinit.pid"

typedef struct {
    char *cmd;
    char *stdin_file;
    char *stdout_file;
    pid_t pid;
} ProcessConfig;

static ProcessConfig procs[MAX_PROCS];
static int proc_count = 0;
static volatile sig_atomic_t reload_config = 0;
static int log_fd = -1;
static char *config_path = NULL;

void log_message(const char *msg) {
    if (log_fd >= 0) {
        write(log_fd, msg, strlen(msg));
    }
}

void sig_handler(int sig) {
    if (sig == SIGHUP) {
        reload_config = 1;
    }
}

int is_absolute_path(const char *path) {
    return path != NULL && path[0] == '/';
}

void close_all_fds(void) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        exit(1);
    }
    for (int fd = 0; fd < (int)rl.rlim_max; fd++) {
        close(fd);
    }
}

void daemonize(void) {
    if (getppid() != 1) {
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        pid_t pid = fork();
        if (pid < 0) exit(1);
        if (pid > 0) exit(0);
        if (setsid() < 0) exit(1);
    }
    if (chdir("/") != 0) exit(1);
    close_all_fds();
    log_fd = open(LOG_FILE, O_CREAT | O_APPEND | O_WRONLY, 0600);
    if (log_fd < 0) exit(1);
    int pid_fd = open(PID_FILE, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (pid_fd >= 0) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%d\n", getpid());
        write(pid_fd, buf, len);
        close(pid_fd);
    }
}

int parse_config_line(char *line, char **cmd, char **stdin_file, char **stdout_file) {
    line[strcspn(line, "\n\r")] = 0;
    char *ptr = line;
    while (*ptr == ' ' || *ptr == '\t') ptr++;
    if (*ptr == '\0' || *ptr == '#') return 0;
    char *tokens[100];
    int n = 0;
    char *saveptr;
    char *token = strtok_r(ptr, " \t", &saveptr);
    while (token && n < 99) {
        tokens[n++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }
    if (n < 3) return 0;
    *stdout_file = strdup(tokens[n-1]);
    *stdin_file = strdup(tokens[n-2]);
    size_t cmd_len = 1;
    for (int i = 0; i < n-2; i++) cmd_len += strlen(tokens[i]) + 1;
    *cmd = malloc(cmd_len);
    if (!*cmd) return 0;
    (*cmd)[0] = '\0';
    for (int i = 0; i < n-2; i++) {
        if (i > 0) strcat(*cmd, " ");
        strcat(*cmd, tokens[i]);
    }
    return 1;
}

int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    proc_count = 0;
    while (fgets(line, sizeof(line), f) && proc_count < MAX_PROCS) {
        char *cmd, *in, *out;
        if (parse_config_line(line, &cmd, &in, &out)) {
            if (!is_absolute_path(cmd) || !is_absolute_path(in) || !is_absolute_path(out)) {
                free(cmd); free(in); free(out);
                continue;
            }
            procs[proc_count].cmd = cmd;
            procs[proc_count].stdin_file = in;
            procs[proc_count].stdout_file = out;
            procs[proc_count].pid = 0;
            proc_count++;
        }
    }
    fclose(f);
    return 0;
}

void start_process(int idx) {
    if (idx < 0 || idx >= proc_count) return;
    pid_t pid = fork();
    if (pid < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Ошибка fork: %s\n", strerror(errno));
        log_message(msg);
        return;
    }
    if (pid == 0) {
        int infd = open(procs[idx].stdin_file, O_RDONLY);
        if (infd >= 0) {
            dup2(infd, STDIN_FILENO);
            close(infd);
        } else {
            infd = open("/dev/null", O_RDONLY);
            if (infd >= 0) { dup2(infd, STDIN_FILENO); close(infd); }
        }
        int outfd = open(procs[idx].stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (outfd >= 0) {
            dup2(outfd, STDOUT_FILENO);
            dup2(outfd, STDERR_FILENO);
            close(outfd);
        }
        char *cmd_copy = strdup(procs[idx].cmd);
        if (!cmd_copy) exit(1);
        char *argv[100];
        int argc = 0;
        char *saveptr;
        char *token = strtok_r(cmd_copy, " \t", &saveptr);
        while (token && argc < 99) {
            argv[argc++] = token;
            token = strtok_r(NULL, " \t", &saveptr);
        }
        argv[argc] = NULL;
        if (argc > 0) execvp(argv[0], argv);
        exit(127);
    }
    procs[idx].pid = pid;
    char msg[256];
    snprintf(msg, sizeof(msg), "Запущен процесс %d: %s (PID: %d)\n", idx, procs[idx].cmd, pid);
    log_message(msg);
}

void stop_process(int idx) {
    if (idx < 0 || idx >= proc_count || procs[idx].pid <= 0) return;
    kill(procs[idx].pid, SIGTERM);
    waitpid(procs[idx].pid, NULL, 0);
    char msg[256];
    snprintf(msg, sizeof(msg), "Завершён процесс %d: %s (PID: %d)\n", idx, procs[idx].cmd, procs[idx].pid);
    log_message(msg);
    procs[idx].pid = 0;
}

void reload_all(void) {
    for (int i = 0; i < proc_count; i++) {
        if (procs[i].pid > 0) stop_process(i);
    }
    int old_count = proc_count;
    if (load_config(config_path) == 0) {
        for (int i = 0; i < proc_count; i++) start_process(i);
    }
    for (int i = proc_count; i < old_count; i++) {
        free(procs[i].cmd);
        free(procs[i].stdin_file);
        free(procs[i].stdout_file);
    }
}

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        if (opt == 'c') config_path = optarg;
    }
    if (!config_path) {
        fprintf(stderr, "Использование: %s -c конфиг_файл\n", argv[0]);
        exit(1);
    }
    daemonize();
    log_message("myinit запущен\n");
    signal(SIGHUP, sig_handler);
    if (load_config(config_path) != 0) {
        log_message("Ошибка загрузки конфигурации\n");
        exit(1);
    }
    for (int i = 0; i < proc_count; i++) start_process(i);
    while (1) {
        if (reload_config) {
            reload_config = 0;
            log_message("Получен SIGHUP, перезагрузка конфигурации\n");
            reload_all();
        }
        int status;
        pid_t cpid;
        while ((cpid = waitpid(-1, &status, WNOHANG)) > 0) {
            for (int i = 0; i < proc_count; i++) {
                if (procs[i].pid == cpid) {
                    char msg[256];
                    if (WIFEXITED(status)) {
                        snprintf(msg, sizeof(msg), "Процесс %d завершён с кодом %d (PID: %d)\n", i, WEXITSTATUS(status), cpid);
                    } else if (WIFSIGNALED(status)) {
                        snprintf(msg, sizeof(msg), "Процесс %d завершён сигналом %d (PID: %d)\n", i, WTERMSIG(status), cpid);
                    } else {
                        snprintf(msg, sizeof(msg), "Процесс %d завершён (PID: %d)\n", i, cpid);
                    }
                    log_message(msg);
                    procs[i].pid = 0;
                    start_process(i);
                    break;
                }
            }
        }
        sleep(1);
    }
    return 0;
}
