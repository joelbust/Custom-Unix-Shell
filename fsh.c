#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_LINE 4096
#define MAX_ARGS 256
#define MAX_CMDS 6

typedef struct {
    // stores entered command strings for history and !prefix lookup
    char **items;
    int size;
    int cap;
} History;

typedef struct Job {
    // tracks a child process group used by bg jobs and launch jobs
    pid_t pgid;
    int is_launch;
    char *desc;
    struct Job *next;
} Job;

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
} SimpleCmd;

typedef struct {
    SimpleCmd cmds[MAX_CMDS];
    int ncmd;
    int background;
} Parsed;

static History history_store = {NULL, 0, 0};
// linked list of active child job groups used by Ctrl-K and exit checks
static Job *job_list = NULL;

// current foreground process group used by Ctrl-C forwarding
static volatile sig_atomic_t fg_pgid = 0;
static volatile sig_atomic_t got_sigchld = 0;
static volatile sig_atomic_t launch_stop = 0;

static char *shell_path = NULL;
static char *prompt_template = NULL;
static int command_count = 0;
static int exit_requested = 0;

// log state and file handle used by log on/log off and output mirroring
static int log_enabled = 0;
static FILE *log_fp = NULL;
static char log_path[PATH_MAX];

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (p == NULL) {
        perror("malloc");
        exit(1);
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n;
    char *p;
    if (s == NULL) {
        return NULL;
    }
    n = strlen(s);
    p = (char *)xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static void history_push(const char *line) {
    // saves a non-empty command into history_store
    if (line == NULL || line[0] == '\0') {
        return;
    }
    if (history_store.size == history_store.cap) {
        int new_cap = history_store.cap == 0 ? 64 : history_store.cap * 2;
        char **tmp = realloc(history_store.items, sizeof(char *) * (size_t)new_cap);
        if (tmp == NULL) {
            perror("realloc");
            exit(1);
        }
        history_store.items = tmp;
        history_store.cap = new_cap;
    }
    history_store.items[history_store.size++] = xstrdup(line);
}

static const char *history_find_prefix(const char *prefix) {
    // finds the newest command that matches !prefix
    int i;
    size_t pn;
    if (prefix == NULL) {
        return NULL;
    }
    pn = strlen(prefix);
    for (i = history_store.size - 1; i >= 0; --i) {
        if (strncmp(history_store.items[i], prefix, pn) == 0) {
            return history_store.items[i];
        }
    }
    return NULL;
}

static void history_show_last(int n) {
    int start;
    int i;
    if (n < 0) {
        n = 0;
    }
    start = history_store.size - n;
    if (start < 0) {
        start = 0;
    }
    for (i = start; i < history_store.size; ++i) {
        printf("%d %s\n", i + 1, history_store.items[i]);
        if (log_enabled && log_fp != NULL) {
            fprintf(log_fp, "%d %s\n", i + 1, history_store.items[i]);
            fflush(log_fp);
        }
    }
}

static void add_job(pid_t pgid, int is_launch, const char *desc) {
    Job *j = (Job *)xmalloc(sizeof(Job));
    j->pgid = pgid;
    j->is_launch = is_launch;
    j->desc = xstrdup(desc == NULL ? "" : desc);
    j->next = job_list;
    job_list = j;
}

static void remove_dead_jobs(void) {
    Job **pp = &job_list;
    while (*pp != NULL) {
        Job *j = *pp;
        if (kill(-j->pgid, 0) == -1 && errno == ESRCH) {
            *pp = j->next;
            free(j->desc);
            free(j);
        } else {
            pp = &((*pp)->next);
        }
    }
}

static int jobs_running(void) {
    remove_dead_jobs();
    return job_list != NULL;
}

static void kill_all_children(void) {
    Job *j;
    for (j = job_list; j != NULL; j = j->next) {
        kill(-j->pgid, SIGKILL);
    }
}

static void sigint_handler(int signo) {
    (void)signo;
    if (fg_pgid > 0) {
        kill(-fg_pgid, SIGINT);
    }
}

static void sigchld_handler(int signo) {
    (void)signo;
    got_sigchld = 1;
}

static void launch_term_handler(int signo) {
    (void)signo;
    launch_stop = 1;
}

static void install_signal_handlers(void) {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
}

static void reap_children(void) {
    // collects finished children so the shell avoids zombie processes
    int status;
    pid_t p;
    if (!got_sigchld) {
        return;
    }
    got_sigchld = 0;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        (void)p;
    }
    remove_dead_jobs();
}

static void shell_log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (log_enabled && log_fp != NULL) {
        va_start(ap, fmt);
        vfprintf(log_fp, fmt, ap);
        va_end(ap);
        fflush(log_fp);
    }
}

static char *str_trim(char *s) {
    size_t len;
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        --len;
    }
    return s;
}

static char *normalize_specials(const char *line) {
    size_t i;
    size_t n;
    char *out;
    size_t w = 0;

    n = strlen(line);
    out = (char *)xmalloc(n * 3 + 1);

    for (i = 0; i < n; ++i) {
        char c = line[i];
        if (c == '<' || c == '>' || c == '|' || c == '&') {
            out[w++] = ' ';
            out[w++] = c;
            out[w++] = ' ';
        } else {
            out[w++] = c;
        }
    }
    out[w] = '\0';
    return out;
}

static int starts_with_set_assignment(const char *line) {
    const char *p = line;
    const char *eq;
    while (*p != '\0' && isspace((unsigned char)*p)) {
        ++p;
    }
    if (strncmp(p, "set", 3) != 0) {
        return 0;
    }
    p += 3;
    if (*p != '\0' && !isspace((unsigned char)*p)) {
        return 0;
    }
    eq = strchr(p, '=');
    return eq != NULL;
}

static char *join_path(const char *a, const char *b) {
    size_t na = strlen(a);
    size_t nb = strlen(b);
    char *p = (char *)xmalloc(na + nb + 2);
    memcpy(p, a, na);
    p[na] = '/';
    memcpy(p + na + 1, b, nb + 1);
    return p;
}

static char **split_plus_path(const char *val, int *count) {
    char *tmp;
    char *tok;
    char **arr = NULL;
    int n = 0;

    *count = 0;
    if (val == NULL || val[0] == '\0') {
        return NULL;
    }

    tmp = xstrdup(val);
    tok = strtok(tmp, "+");
    while (tok != NULL) {
        char **next = realloc(arr, sizeof(char *) * (size_t)(n + 1));
        if (next == NULL) {
            perror("realloc");
            free(tmp);
            exit(1);
        }
        arr = next;
        arr[n++] = xstrdup(tok);
        tok = strtok(NULL, "+");
    }
    free(tmp);
    *count = n;
    return arr;
}

static void free_split(char **arr, int n) {
    int i;
    for (i = 0; i < n; ++i) {
        free(arr[i]);
    }
    free(arr);
}

static char *resolve_executable(const char *cmd) {
    int i;
    int ndir;
    char **dirs;

    if (cmd == NULL || cmd[0] == '\0') {
        return NULL;
    }

    if (strchr(cmd, '/') != NULL) {
        if (access(cmd, X_OK) == 0) {
            return xstrdup(cmd);
        }
        return NULL;
    }

    dirs = split_plus_path(shell_path, &ndir);
    for (i = 0; i < ndir; ++i) {
        char *candidate = join_path(dirs[i], cmd);
        if (access(candidate, X_OK) == 0) {
            free_split(dirs, ndir);
            return candidate;
        }
        free(candidate);
    }
    free_split(dirs, ndir);
    return NULL;
}

static void cmd_free(Parsed *p) {
    int i;
    int j;
    for (i = 0; i < p->ncmd; ++i) {
        for (j = 0; j < p->cmds[i].argc; ++j) {
            free(p->cmds[i].argv[j]);
        }
        free(p->cmds[i].infile);
        free(p->cmds[i].outfile);
    }
}

static int parse_pipeline(const char *line, Parsed *out) {
    // tokenizes one command segment into argv, redirection, pipes, and &
    char *norm;
    char *work;
    char *tok;
    SimpleCmd *cur;

    memset(out, 0, sizeof(*out));
    out->ncmd = 1;

    if (starts_with_set_assignment(line)) {
        norm = xstrdup(line);
    } else {
        norm = normalize_specials(line);
    }
    work = norm;

    cur = &out->cmds[0];
    tok = strtok(work, " \t\r\n");
    while (tok != NULL) {
        if (strcmp(tok, "|") == 0) {
            if (out->ncmd >= MAX_CMDS) {
                free(norm);
                return -1;
            }
            out->ncmd++;
            cur = &out->cmds[out->ncmd - 1];
        } else if (strcmp(tok, "<") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok == NULL) {
                free(norm);
                return -1;
            }
            cur->infile = xstrdup(tok);
        } else if (strcmp(tok, ">") == 0) {
            tok = strtok(NULL, " \t\r\n");
            if (tok == NULL) {
                free(norm);
                return -1;
            }
            cur->outfile = xstrdup(tok);
        } else if (strcmp(tok, "&") == 0) {
            out->background = 1;
        } else {
            if (cur->argc + 1 >= MAX_ARGS) {
                free(norm);
                return -1;
            }
            cur->argv[cur->argc++] = xstrdup(tok);
            cur->argv[cur->argc] = NULL;
        }
        tok = strtok(NULL, " \t\r\n");
    }

    free(norm);

    if (out->cmds[0].argc == 0) {
        return 0;
    }
    return 1;
}

static void split_semicolons(char *line, char **parts, int *nparts) {
    char *tok;
    int n = 0;
    tok = strtok(line, ";");
    while (tok != NULL && n < 128) {
        parts[n++] = tok;
        tok = strtok(NULL, ";");
    }
    *nparts = n;
}

static void render_dir_relative(char *out, size_t out_sz) {
    char cwd[PATH_MAX];
    const char *home = getenv("HOME");

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        snprintf(out, out_sz, ".");
        return;
    }
    if (home != NULL && strncmp(cwd, home, strlen(home)) == 0) {
        const char *rest = cwd + strlen(home);
        if (rest[0] == '\0') {
            snprintf(out, out_sz, ".");
        } else if (rest[0] == '/') {
            snprintf(out, out_sz, "%s", rest + 1);
        } else {
            snprintf(out, out_sz, "%s", rest);
        }
    } else {
        snprintf(out, out_sz, "%s", cwd);
    }
}

static void build_prompt(char *out, size_t out_sz) {
    char host[256] = "";
    char dir[PATH_MAX];
    const char *src;
    size_t w = 0;

    if (getenv("HOST") != NULL) {
        snprintf(host, sizeof(host), "%s", getenv("HOST"));
    } else {
        gethostname(host, sizeof(host));
    }
    render_dir_relative(dir, sizeof(dir));

    src = prompt_template;
    while (*src != '\0' && w + 1 < out_sz) {
        if (*src == '$') {
            if (strncmp(src, "$c", 2) == 0) {
                char num[32];
                size_t ln;
                snprintf(num, sizeof(num), "%d", command_count);
                ln = strlen(num);
                if (w + ln < out_sz) {
                    memcpy(out + w, num, ln);
                    w += ln;
                }
                src += 2;
                continue;
            }
            if (strncmp(src, "$host", 5) == 0) {
                size_t ln = strlen(host);
                if (w + ln < out_sz) {
                    memcpy(out + w, host, ln);
                    w += ln;
                }
                src += 5;
                continue;
            }
            if (strncmp(src, "$dir", 4) == 0) {
                size_t ln = strlen(dir);
                if (w + ln < out_sz) {
                    memcpy(out + w, dir, ln);
                    w += ln;
                }
                src += 4;
                continue;
            }
        }
        out[w++] = *src++;
    }
    out[w] = '\0';
}

static void refresh_line(const char *prompt, const char *buf) {
    printf("\r\033[2K%s%s", prompt, buf);
    fflush(stdout);
}

static char *read_line_editor(const char *prompt) {
    // reads one interactive line and handles arrows, Ctrl-P/N, backspace, Ctrl-A, Ctrl-K
    struct termios oldt;
    struct termios raw;
    char buf[MAX_LINE];
    int len = 0;
    int hist_pos = history_store.size;

    if (tcgetattr(STDIN_FILENO, &oldt) == -1) {
        char fallback[MAX_LINE];
        if (fgets(fallback, sizeof(fallback), stdin) == NULL) {
            return NULL;
        }
        fallback[strcspn(fallback, "\n")] = '\0';
        return xstrdup(fallback);
    }

    raw = oldt;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    buf[0] = '\0';
    printf("%s", prompt);
    fflush(stdout);

    while (1) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) {
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return NULL;
        }

        if (c == '\n' || c == '\r') {
            printf("\n");
            fflush(stdout);
            break;
        }

        if (c == 3) {
            len = 0;
            buf[0] = '\0';
            printf("\n");
            fflush(stdout);
            break;
        }

        if ((unsigned char)c == 127 || c == 8 || c == 1) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
            }
            refresh_line(prompt, buf);
            continue;
        }

        if (c == 11) {
            kill_all_children();
            refresh_line(prompt, buf);
            continue;
        }

        if (c == 16 || c == 14 || (unsigned char)c == 27) {
            int up = 0;
            int down = 0;
            if (c == 16) {
                up = 1;
            } else if (c == 14) {
                down = 1;
            } else {
                char seq[2];
                if (read(STDIN_FILENO, seq, 2) == 2 && seq[0] == '[') {
                    if (seq[1] == 'A') {
                        up = 1;
                    } else if (seq[1] == 'B') {
                        down = 1;
                    }
                }
            }

            if (up && history_store.size > 0 && hist_pos > 0) {
                hist_pos--;
                snprintf(buf, sizeof(buf), "%s", history_store.items[hist_pos]);
                len = (int)strlen(buf);
                refresh_line(prompt, buf);
            } else if (down && history_store.size > 0) {
                if (hist_pos < history_store.size - 1) {
                    hist_pos++;
                    snprintf(buf, sizeof(buf), "%s", history_store.items[hist_pos]);
                } else {
                    hist_pos = history_store.size;
                    buf[0] = '\0';
                }
                len = (int)strlen(buf);
                refresh_line(prompt, buf);
            }
            continue;
        }

        if (isprint((unsigned char)c)) {
            if (len + 1 < MAX_LINE) {
                buf[len++] = c;
                buf[len] = '\0';
                refresh_line(prompt, buf);
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return xstrdup(buf);
}

static int redirect_file(const char *path, int fd, int flags, mode_t mode) {
    int f = open(path, flags, mode);
    if (f < 0) {
        perror(path);
        return -1;
    }
    if (dup2(f, fd) < 0) {
        perror("dup2");
        close(f);
        return -1;
    }
    close(f);
    return 0;
}

static int run_pipeline(Parsed *p, const char *cmdline) {
    // forks/execs commands, wires pipes/redirection, and runs fg or bg behavior
    int i;
    int pipes[MAX_CMDS - 1][2];
    pid_t pgid = 0;
    int capture[2] = {-1, -1};

    if (log_enabled && pipe(capture) < 0) {
        perror("pipe");
        capture[0] = capture[1] = -1;
    }

    for (i = 0; i < p->ncmd - 1; ++i) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return -1;
        }
    }

    for (i = 0; i < p->ncmd; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return -1;
        }
        if (pid == 0) {
            int j;
            char *exe;

            signal(SIGINT, SIG_DFL);
            setpgid(0, pgid == 0 ? 0 : pgid);

            if (p->background) {
                int devnull = open("/dev/null", O_RDONLY);
                if (devnull >= 0) {
                    dup2(devnull, STDIN_FILENO);
                    close(devnull);
                }
            }

            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            }
            if (i < p->ncmd - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            if (log_enabled && capture[1] != -1) {
                if (i == p->ncmd - 1) {
                    dup2(capture[1], STDOUT_FILENO);
                }
                dup2(capture[1], STDERR_FILENO);
            }

            for (j = 0; j < p->ncmd - 1; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            if (capture[0] != -1) {
                close(capture[0]);
                close(capture[1]);
            }

            if (p->cmds[i].infile != NULL) {
                if (redirect_file(p->cmds[i].infile, STDIN_FILENO, O_RDONLY, 0) != 0) {
                    _exit(1);
                }
            }
            if (p->cmds[i].outfile != NULL) {
                if (redirect_file(p->cmds[i].outfile, STDOUT_FILENO,
                                  O_WRONLY | O_CREAT | O_TRUNC, 0644) != 0) {
                    _exit(1);
                }
            }

            exe = resolve_executable(p->cmds[i].argv[0]);
            if (exe == NULL) {
                fprintf(stderr, "command not found: %s\n", p->cmds[i].argv[0]);
                _exit(127);
            }
            execv(exe, p->cmds[i].argv);
            perror("execv");
            free(exe);
            _exit(127);
        } else {
            if (pgid == 0) {
                pgid = pid;
            }
            setpgid(pid, pgid);
        }
    }

    for (i = 0; i < p->ncmd - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (capture[1] != -1) {
        close(capture[1]);
    }

    if (p->background) {
        add_job(pgid, 0, cmdline);
        shell_log_printf("[bg %d] %s\n", pgid, cmdline);

        if (capture[0] != -1) {
            pid_t lpid = fork();
            if (lpid == 0) {
                char buf[1024];
                ssize_t n;
                while ((n = read(capture[0], buf, sizeof(buf))) > 0) {
                    write(STDOUT_FILENO, buf, (size_t)n);
                    if (log_fp != NULL) {
                        fwrite(buf, 1, (size_t)n, log_fp);
                        fflush(log_fp);
                    }
                }
                close(capture[0]);
                _exit(0);
            }
            close(capture[0]);
        }
        return 0;
    }

    fg_pgid = pgid;

    if (capture[0] != -1) {
        char buf[1024];
        ssize_t n;
        while ((n = read(capture[0], buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, (size_t)n);
            if (log_fp != NULL) {
                fwrite(buf, 1, (size_t)n, log_fp);
                fflush(log_fp);
            }
        }
        close(capture[0]);
    }

    while (waitpid(-pgid, NULL, 0) > 0) {
    }
    fg_pgid = 0;
    return 0;
}

static int builtin_set(Parsed *p) {
    char value[MAX_LINE];
    int i;

    if (p->cmds[0].argc < 4 || strcmp(p->cmds[0].argv[2], "=") != 0) {
        shell_log_printf("usage: set VAR = value\n");
        return 1;
    }

    value[0] = '\0';
    for (i = 3; i < p->cmds[0].argc; ++i) {
        if (i > 3) {
            strncat(value, " ", sizeof(value) - strlen(value) - 1);
        }
        strncat(value, p->cmds[0].argv[i], sizeof(value) - strlen(value) - 1);
    }

    setenv(p->cmds[0].argv[1], value, 1);
    if (strcmp(p->cmds[0].argv[1], "FSH_PATH") == 0) {
        free(shell_path);
        shell_path = xstrdup(value);
    } else if (strcmp(p->cmds[0].argv[1], "prompt") == 0) {
        free(prompt_template);
        prompt_template = xstrdup(value);
    }
    return 1;
}

static int builtin_whereis(Parsed *p) {
    int ndir;
    char **dirs;
    int i;
    int found = 0;

    if (p->cmds[0].argc != 2) {
        shell_log_printf("usage: whereis cmd\n");
        return 1;
    }

    dirs = split_plus_path(shell_path, &ndir);
    for (i = 0; i < ndir; ++i) {
        char *candidate = join_path(dirs[i], p->cmds[0].argv[1]);
        if (access(candidate, X_OK) == 0) {
            shell_log_printf("%s\n", candidate);
            found = 1;
        }
        free(candidate);
    }
    if (!found) {
        shell_log_printf("command not found\n");
    }
    free_split(dirs, ndir);
    return 1;
}

static int builtin_which(Parsed *p) {
    char *path;
    if (p->cmds[0].argc != 2) {
        shell_log_printf("usage: which cmd\n");
        return 1;
    }
    path = resolve_executable(p->cmds[0].argv[1]);
    if (path == NULL) {
        shell_log_printf("command not found\n");
    } else {
        shell_log_printf("%s\n", path);
        free(path);
    }
    return 1;
}

static int launch_supervisor(char **argv) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        setsid();
        signal(SIGTERM, launch_term_handler);
        signal(SIGINT, SIG_IGN);
        while (!launch_stop) {
            pid_t cpid = fork();
            if (cpid < 0) {
                _exit(1);
            }
            if (cpid == 0) {
                char *exe = resolve_executable(argv[0]);
                signal(SIGINT, SIG_DFL);
                if (exe == NULL) {
                    fprintf(stderr, "command not found: %s\n", argv[0]);
                    _exit(127);
                }
                execv(exe, argv);
                perror("execv");
                _exit(127);
            }
            while (!launch_stop) {
                pid_t w = waitpid(cpid, NULL, 0);
                if (w == cpid) {
                    break;
                }
                if (w < 0 && errno == EINTR) {
                    continue;
                }
                break;
            }
            if (!launch_stop) {
                sleep(1);
            }
        }
        _exit(0);
    }

    setpgid(pid, pid);
    add_job(pid, 1, argv[0]);
    shell_log_printf("[launch %d] %s\n", pid, argv[0]);
    return 1;
}

static int run_builtin(Parsed *p) {
    // handles internal shell commands without exec (cd, set, history, log, launch, exit)
    char **a = p->cmds[0].argv;

    if (a[0] == NULL) {
        return 1;
    }
    if (strcmp(a[0], "show") == 0) {
        shell_log_printf("%s\n", shell_path);
        return 1;
    }
    if (strcmp(a[0], "history") == 0) {
        int n = 10;
        if (p->cmds[0].argc >= 2) {
            n = atoi(a[1]);
        }
        history_show_last(n);
        return 1;
    }
    if (strcmp(a[0], "quit") == 0 || strcmp(a[0], "exit") == 0) {
        exit_requested = 1;
        return 1;
    }
    if (strcmp(a[0], "cd") == 0) {
        const char *dir = p->cmds[0].argc >= 2 ? a[1] : getenv("HOME");
        char expanded[PATH_MAX];
        if (dir != NULL && dir[0] == '~') {
            const char *home = getenv("HOME");
            if (home != NULL) {
                if (dir[1] == '\0') {
                    snprintf(expanded, sizeof(expanded), "%s", home);
                    dir = expanded;
                } else if (dir[1] == '/') {
                    snprintf(expanded, sizeof(expanded), "%s%s", home, dir + 1);
                    dir = expanded;
                }
            }
        }
        if (dir == NULL || chdir(dir) != 0) {
            perror("cd");
        }
        return 1;
    }
    if (strcmp(a[0], "set") == 0) {
        return builtin_set(p);
    }
    if (strcmp(a[0], "whereis") == 0) {
        return builtin_whereis(p);
    }
    if (strcmp(a[0], "which") == 0) {
        return builtin_which(p);
    }
    if (strcmp(a[0], "log") == 0 && p->cmds[0].argc == 2) {
        if (strcmp(a[1], "on") == 0) {
            const char *home = getenv("HOME");
            if (home == NULL) {
                home = ".";
            }
            snprintf(log_path, sizeof(log_path), "%s/%s", home, "fsh_log000111.txt");
            log_fp = fopen(log_path, "a");
            if (log_fp == NULL) {
                perror("log file");
            } else {
                log_enabled = 1;
                shell_log_printf("logging to %s\n", log_path);
            }
        } else if (strcmp(a[1], "off") == 0) {
            if (log_fp != NULL) {
                fclose(log_fp);
                log_fp = NULL;
            }
            log_enabled = 0;
            printf("logging off\n");
        }
        return 1;
    }
    if (strcmp(a[0], "launch") == 0) {
        if (p->cmds[0].argc < 2) {
            shell_log_printf("usage: launch cmd [args]\n");
            return 1;
        }
        return launch_supervisor(&a[1]);
    }

    return 0;
}

static int process_command(char *cmd_raw) {
    // executes one ; separated command including !repeat, parse, builtins, or external run
    Parsed p;
    int rc;
    char *cmd;

    cmd = str_trim(cmd_raw);
    if (cmd[0] == '\0') {
        return 0;
    }

    if (cmd[0] == '!' && cmd[1] != '\0') {
        const char *old = history_find_prefix(cmd + 1);
        if (old == NULL) {
            shell_log_printf("no match for !%s\n", cmd + 1);
            return 0;
        }
        shell_log_printf("%s\n", old);
        cmd = xstrdup(old);
    } else {
        cmd = xstrdup(cmd);
    }

    history_push(cmd);
    if (log_enabled && log_fp != NULL) {
        fprintf(log_fp, "%s\n", cmd);
        fflush(log_fp);
    }

    rc = parse_pipeline(cmd, &p);
    if (rc < 0) {
        shell_log_printf("parse error\n");
        free(cmd);
        return -1;
    }
    if (rc == 0) {
        free(cmd);
        return 0;
    }

    command_count++;

    if (p.ncmd == 1 && run_builtin(&p)) {
        cmd_free(&p);
        free(cmd);
        return 0;
    }

    run_pipeline(&p, cmd);
    cmd_free(&p);
    free(cmd);
    return 0;
}

int main(void) {
    char *line;

    shell_path = xstrdup("/usr/bin+/bin");
    prompt_template = xstrdup("<fsh:$c> ");
    setenv("FSH_PATH", shell_path, 1);
    setenv("prompt", prompt_template, 1);

    install_signal_handlers();

    while (1) {
        char prompt[512];
        char *parts[128];
        int nparts;
        int i;

        reap_children();

        if (exit_requested) {
            if (!jobs_running() && fg_pgid == 0) {
                break;
            }
            shell_log_printf("fsh: child processes still running; use Ctrl-K to kill all children\n");
            exit_requested = 0;
        }

        build_prompt(prompt, sizeof(prompt));
        line = read_line_editor(prompt);
        if (line == NULL) {
            break;
        }

        {
            char *work = xstrdup(line);
            split_semicolons(work, parts, &nparts);
            for (i = 0; i < nparts; ++i) {
                process_command(parts[i]);
            }
            free(work);
        }

        free(line);
    }

    kill_all_children();
    while (waitpid(-1, NULL, 0) > 0) {
    }

    if (log_fp != NULL) {
        fclose(log_fp);
    }

    while (history_store.size > 0) {
        free(history_store.items[--history_store.size]);
    }
    free(history_store.items);
    free(shell_path);
    free(prompt_template);

    return 0;
}
