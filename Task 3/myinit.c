#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#define MAX_LINE 4096
#define MAX_PROCS 256
#define MAX_ARGS 64
#define LOG_FILE "/tmp/myinit.log"
#define LOG_MODE (O_CREAT | O_APPEND | O_WRONLY)
#define LOG_PERM 0644

typedef struct {
    char *cmd;
    char *stdin_file;
    char *stdout_file;
} proc_cfg_t;

static int g_log_fd = -1;
static volatile sig_atomic_t g_reload = 0;
static pid_t g_children[MAX_PROCS];
static proc_cfg_t *g_configs[MAX_PROCS];
static int g_child_cnt = 0;

static void log_write(const char *s) {
    if (g_log_fd >= 0 && s) {
        size_t len = strlen(s);
        ssize_t written = 0;
        while (written < (ssize_t)len) {
            ssize_t n = write(g_log_fd, s + written, len - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            written += n;
        }
        fsync(g_log_fd);
    }
}

static int is_abs_path(const char *p) { return p && *p == '/'; }

static char *trim_end(char *s) {
    if (!s) return s;
    char *e = s + strlen(s) - 1;
    while (e >= s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static char *skip_spaces(const char *s) {
    while (s && *s == ' ') s++;
    return (char *)s;
}

static int parse_line(char *line, proc_cfg_t *cfg) {
    if (!line || !cfg) return -1;
    char *p = trim_end(line);
    p = skip_spaces(p);
    if (!*p || *p == '#') return 0;
    
    char *last = strrchr(p, ' ');
    if (!last) return -1;
    char *stdout_f = last + 1; *last = '\0';
    
    char *prev = strrchr(p, ' ');
    if (!prev) return -1;
    char *stdin_f = prev + 1; *prev = '\0';
    
    char *cmd_start = skip_spaces(p);
    if (!*cmd_start) return -1;
    
    if (!is_abs_path(cmd_start) || !is_abs_path(stdin_f) || !is_abs_path(stdout_f))
        return -2;
    
    struct stat st;
    if (stat(cmd_start, &st) != 0 || !(st.st_mode & S_IXUSR))
        return -3;
    
    cfg->cmd = strdup(cmd_start);
    cfg->stdin_file = strdup(stdin_f);
    cfg->stdout_file = strdup(stdout_f);
    
    if (!cfg->cmd || !cfg->stdin_file || !cfg->stdout_file) {
        free(cfg->cmd); free(cfg->stdin_file); free(cfg->stdout_file);
        return -1;
    }
    return 1;
}

static int build_argv(const char *cmd, char **argv, int max_argc) {
    if (!cmd || !argv || max_argc < 2) return -1;
    char *buf = strdup(cmd); if (!buf) return -1;
    int argc = 0; argv[argc++] = strdup(buf);
    char *tok = strtok(buf, " \t");
    while (tok && argc < max_argc - 1) {
        tok = strtok(NULL, " \t");
        if (tok) argv[argc++] = strdup(tok);
    }
    argv[argc] = NULL; free(buf); return argc;
}

static pid_t spawn_child(const proc_cfg_t *cfg) {
    if (!cfg) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
        for (long fd = 3; fd < (long)rl.rlim_max; fd++) close((int)fd);
        
        int fd_in = open(cfg->stdin_file, O_RDONLY);
        if (fd_in < 0) fd_in = open(cfg->stdin_file, O_CREAT | O_RDONLY, 0644);
        if (fd_in >= 0) { dup2(fd_in, STDIN_FILENO); close(fd_in); }
        
        int fd_out = open(cfg->stdout_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out >= 0) { dup2(fd_out, STDOUT_FILENO); dup2(fd_out, STDERR_FILENO); close(fd_out); }
        
        char *argv[MAX_ARGS];
        if (build_argv(cfg->cmd, argv, MAX_ARGS) < 0) _exit(127);
        execv(argv[0], argv); _exit(127);
    }
    return pid;
}

static void terminate_child(pid_t pid) {
    if (pid > 1) { kill(pid, SIGTERM); usleep(100000); kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
}

static void stop_all_children(void) {
    for (int i = 0; i < g_child_cnt; i++) {
        if (g_children[i] > 0) terminate_child(g_children[i]);
        if (g_configs[i]) {
            free(g_configs[i]->cmd); free(g_configs[i]->stdin_file);
            free(g_configs[i]->stdout_file); free(g_configs[i]); g_configs[i] = NULL;
        }
        g_children[i] = 0;
    }
    g_child_cnt = 0;
}

static int find_child(pid_t pid) {
    for (int i = 0; i < g_child_cnt; i++) if (g_children[i] == pid) return i;
    return -1;
}

static void sighup_handler(int sig) { (void)sig; g_reload = 1; }
static void child_handler(int sig) { (void)sig; }

static int load_procs(const char *cfg_path) {
    if (!cfg_path) return -1;
    FILE *fp = fopen(cfg_path, "r");
    if (!fp) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ERROR: cannot open config '%s': %s\n", cfg_path, strerror(errno));
        log_write(msg); return -1;
    }
    char line[MAX_LINE]; int started = 0;
    while (fgets(line, sizeof(line), fp)) {
        proc_cfg_t cfg = {0}; int rc = parse_line(line, &cfg);
        if (rc == -2) { char msg[256]; snprintf(msg, sizeof(msg), "ERROR: relative path: %s", line); log_write(msg); continue; }
        if (rc == -3) { char msg[256]; snprintf(msg, sizeof(msg), "ERROR: not executable: %s", line); log_write(msg); continue; }
        if (rc <= 0) continue;
        
        pid_t pid = spawn_child(&cfg);
        if (pid > 0 && g_child_cnt < MAX_PROCS) {
            g_children[g_child_cnt] = pid;
            proc_cfg_t *saved = malloc(sizeof(proc_cfg_t));
            if (saved) { saved->cmd = cfg.cmd; saved->stdin_file = cfg.stdin_file; saved->stdout_file = cfg.stdout_file; g_configs[g_child_cnt] = saved; }
            char msg[512];
            snprintf(msg, sizeof(msg), "START: pid=%d cmd=\"%s\" stdin=%s stdout=%s\n", pid, cfg.cmd, cfg.stdin_file, cfg.stdout_file);
            log_write(msg); g_child_cnt++; started++;
        } else {
            free(cfg.cmd); free(cfg.stdin_file); free(cfg.stdout_file);
        }
    }
    fclose(fp); return started;
}

static void handle_sighup_reload(const char *cfg_path) {
    log_write("SIGHUP: reloading configuration\n");
    stop_all_children();
    log_write("SIGHUP: all children terminated\n");
    load_procs(cfg_path);
    log_write("SIGHUP: new processes started\n");
}

static void handle_child_exit(void) {
    pid_t cpid; int status;
    while ((cpid = waitpid(-1, &status, WNOHANG)) > 0) {
        int idx = find_child(cpid);
        if (idx >= 0) {
            char msg[256];
            if (WIFEXITED(status)) snprintf(msg, sizeof(msg), "EXIT: pid=%d index=%d exited with code %d\n", cpid, idx, WEXITSTATUS(status));
            else if (WIFSIGNALED(status)) snprintf(msg, sizeof(msg), "EXIT: pid=%d index=%d killed by signal %d\n", cpid, idx, WTERMSIG(status));
            else snprintf(msg, sizeof(msg), "EXIT: pid=%d index=%d status=%d\n", cpid, idx, status);
            log_write(msg);
            
            // 🔑 КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Перезапуск процесса
            proc_cfg_t *cfg = g_configs[idx];
            pid_t new_pid = -1;
            if (cfg) {
                new_pid = spawn_child(cfg);
                if (new_pid > 0) {
                    char rmsg[256];
                    snprintf(rmsg, sizeof(rmsg), "RESTART: pid=%d cmd=\"%s\"\n", new_pid, cfg->cmd);
                    log_write(rmsg);
                    g_children[idx] = new_pid; // Обновляем PID в массиве
                }
            }
            
            // Если рестарт не удался, освобождаем слот
            if (new_pid <= 0) {
                if (cfg) {
                    free(cfg->cmd); free(cfg->stdin_file); free(cfg->stdout_file); free(cfg);
                    g_configs[idx] = NULL;
                }
                g_children[idx] = 0;
            }
        }
    }
}

static int daemonize(void) {
    if (getppid() != 1) { signal(SIGTTOU, SIG_IGN); signal(SIGTTIN, SIG_IGN); signal(SIGTSTP, SIG_IGN); }
    pid_t pid = fork(); if (pid < 0) return -1; if (pid > 0) _exit(0);
    if (setsid() < 0) return -1;
    pid = fork(); if (pid < 0) return -1; if (pid > 0) _exit(0);
    if (chdir("/") < 0) return -1;
    umask(0);
    struct rlimit rl; if (getrlimit(RLIMIT_NOFILE, &rl) == 0) { for (long fd = 0; fd < (long)rl.rlim_max; fd++) close((int)fd); }
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) { dup2(null_fd, STDIN_FILENO); dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); if (null_fd > 2) close(null_fd); }
    return 0;
}

static int open_log(void) {
    g_log_fd = open(LOG_FILE, LOG_MODE, LOG_PERM);
    if (g_log_fd < 0) g_log_fd = open("/tmp/myinit.log", LOG_MODE, LOG_PERM);
    if (g_log_fd >= 0) { char msg[128]; snprintf(msg, sizeof(msg), "=== myinit pid=%d started ===\n", getpid()); log_write(msg); return 0; }
    return -1;
}

static void cleanup(void) { stop_all_children(); if (g_log_fd >= 0) { log_write("=== myinit exiting ===\n"); close(g_log_fd); g_log_fd = -1; } }

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s <config_file>\n", argv[0]); return 1; }
    const char *cfg_path = argv[1];
    if (!is_abs_path(cfg_path)) { char abs_path[PATH_MAX]; if (realpath(cfg_path, abs_path)) cfg_path = abs_path; }
    if (daemonize() < 0) { fprintf(stderr, "daemonize failed: %s\n", strerror(errno)); return 1; }
    if (open_log() < 0) return 1;
    
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = sighup_handler; sigemptyset(&sa.sa_mask); sa.sa_flags = 0; sigaction(SIGHUP, &sa, NULL);
    memset(&sa, 0, sizeof(sa)); sa.sa_handler = child_handler; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_NOCLDSTOP | SA_RESTART; sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); signal(SIGUSR1, SIG_IGN); signal(SIGUSR2, SIG_IGN);
    atexit(cleanup);
    
    if (load_procs(cfg_path) <= 0) log_write("WARNING: no processes started from config\n");
    
    while (1) {
        if (g_reload) { g_reload = 0; handle_sighup_reload(cfg_path); }
        handle_child_exit();
        pause();
    }
    return 0;
}
