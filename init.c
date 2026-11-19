/*
 * ALXA OS - init + shell
 * Original code by alxa, MIT licensed. Runs as PID 1.
 *
 * Shell syntax: cmd args | cmd2 > file >> file < file ; cmd2 & cmd3
 *               'single quotes'  "double quotes"  $VAR  $?  NAME=value  *.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include "editor.h"
#include "cmds.h"

#define VERSION  "0.5"
#define MAXLINE  512
#define MAXARGS  64
#define MAXTOK   128
#define MAXPIPE  8

static const char *pathdirs[] = { "/bin", "/usr/bin", "/usr/local/bin", "/sbin", NULL };
static int last_status;
static volatile sig_atomic_t in_fg;   /* 1 while we wait for a foreground command */

static void setup_system(void) {
    mkdir("/dev", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);   /* may already be mounted */
    if (fcntl(0, F_GETFD) == -1) {
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
        }
    }
    const char *dirs[] = { "/bin", "/sbin", "/usr", "/usr/bin", "/usr/local",
                           "/usr/local/bin", "/tmp", "/etc", "/home", "/proc", NULL };
    for (int i = 0; dirs[i]; i++) mkdir(dirs[i], 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    setenv("PATH", "/bin:/usr/bin:/usr/local/bin:/sbin", 1);
    setenv("HOME", "/home", 1);
    if (!getenv("TERM")) setenv("TERM", "linux", 1);
}

static void banner(void) {
    puts("");
    puts("  ALXA OS " VERSION " - made by alxa");
    puts("  Linux kernel + alxa shell. Type 'help' for commands.");
    puts("");
}

/* ---------- command lookup ---------- */

static int find_in_path(const char *name, char *out, size_t size) {
    for (int i = 0; pathdirs[i]; i++) {
        snprintf(out, size, "%s/%s", pathdirs[i], name);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0) return 1;
    }
    return 0;
}

/* ---------- core builtins ---------- */

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("View / search:   ls dir cat more head tail find grep wc sort uniq cut tr rev tac nl tee hexdump");
    puts("Files / folders: cp mv rm(del) mkdir rmdir touch ln chmod stat du df basename dirname cd pwd");
    puts("Edit:            edit FILE   (Ctrl+O save, Ctrl+X exit)");
    puts("System:          mem free uptime date uname hostname id whoami dmesg ps kill sleep seq");
    puts("                 sync mount umount env export unset reboot halt poweroff");
    puts("Other:           echo clear true false ver commands which install help");
    puts("");
    puts("Shell syntax:    cmd1 | cmd2     pipe        cmd > file     write to file");
    puts("                 cmd >> file     append      cmd < file     read from file");
    puts("                 cmd1 ; cmd2     run both    cmd &          run in background");
    puts("                 \"quotes\" 'quotes'   $VAR   $?   NAME=value   *.txt  file?.c");
    puts("");
    puts("Programs in /bin, /usr/bin, /usr/local/bin or /sbin also run by name.");
    return 0;
}

static int cmd_ver(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("ALXA OS " VERSION " - made by alxa");
    return 0;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    fputs("\033[2J\033[H", stdout);
    return 0;
}

static int cmd_edit(int argc, char **argv) {
    if (argc < 2) { fputs("usage: edit FILE\n", stderr); return 1; }
    return editor_run(argv[1]);
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    sync();
    reboot(RB_AUTOBOOT);
    fputs("reboot: failed\n", stderr);
    return 1;
}

static int cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    sync();
    reboot(RB_POWER_OFF);
    fputs("poweroff: not supported by this kernel build (close the emulator instead)\n", stderr);
    return 1;
}

static int cmd_commands(int argc, char **argv);
static int cmd_which(int argc, char **argv);
static int cmd_install(int argc, char **argv);

static const struct builtin core_builtins[] = {
    {"help", cmd_help, 0}, {"ver", cmd_ver, 0}, {"clear", cmd_clear, 0}, {"edit", cmd_edit, 0},
    {"commands", cmd_commands, 0}, {"which", cmd_which, 0}, {"install", cmd_install, 0},
    {"reboot", cmd_reboot, 0}, {"poweroff", cmd_poweroff, 0},
    {NULL, NULL, 0}
};

static const struct builtin *find_builtin(const char *name) {
    for (const struct builtin *b = core_builtins; b->name; b++)
        if (!strcmp(name, b->name)) return b;
    for (const struct builtin *b = extra_builtins; b->name; b++)
        if (!strcmp(name, b->name)) return b;
    return NULL;
}

static int cmd_commands(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("Built-in commands:");
    int col = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (const struct builtin *b = pass ? extra_builtins : core_builtins; b->name; b++) {
            printf("%s%-10s", col == 0 ? "  " : "", b->name);
            if (++col == 7) { putchar('\n'); col = 0; }
        }
    }
    if (col) putchar('\n');
    for (int i = 0; pathdirs[i]; i++) {
        DIR *d = opendir(pathdirs[i]);
        if (!d) continue;
        struct dirent *e;
        int header = 0;
        while ((e = readdir(d)) != NULL) {
            char full[512];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            snprintf(full, sizeof full, "%s/%s", pathdirs[i], e->d_name);
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || access(full, X_OK) != 0) continue;
            if (!header) { printf("Programs in %s:\n", pathdirs[i]); header = 1; }
            printf("  %s\n", e->d_name);
        }
        closedir(d);
    }
    return 0;
}

static int cmd_which(int argc, char **argv) {
    if (argc < 2) { fputs("usage: which NAME\n", stderr); return 1; }
    char path[256];
    if (find_builtin(argv[1])) { printf("%s: built-in\n", argv[1]); return 0; }
    if (find_in_path(argv[1], path, sizeof path)) { puts(path); return 0; }
    printf("%s: not found\n", argv[1]);
    return 1;
}

static int cmd_install(int argc, char **argv) {
    if (argc < 2) { fputs("usage: install FILE [NAME]\n", stderr); return 1; }
    struct stat st;
    if (stat(argv[1], &st) < 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "install: %s: not a regular file\n", argv[1]); return 1;
    }
    const char *name = argc > 2 ? argv[2] : base_name(argv[1]);
    if (find_builtin(name)) { fprintf(stderr, "install: '%s' is a built-in command\n", name); return 1; }

    unsigned char magic[4] = {0};
    int fd = open(argv[1], O_RDONLY);
    if (fd >= 0) { if (read(fd, magic, 4) < 0) {} close(fd); }
    if (!(magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F'))
        fputs("install: warning: not a compiled (ELF) program, it may not run\n", stderr);

    char dst[300];
    mkdir("/bin", 0755);
    snprintf(dst, sizeof dst, "/bin/%s", name);
    if (copy_file(argv[1], dst, 0755) < 0) { fprintf(stderr, "install: %s\n", strerror(errno)); return 1; }
    printf("installed: type '%s' to run it\n", name);
    return 0;
}

/* ---------- parsing ---------- */

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APP, T_SEMI };
struct tok { int type; char *text; int glob; };

struct cmd {
    char *argv[MAXARGS];
    int argc;
    char *in, *out;
    int append;
};

static char arena[16384];
static size_t apos;

static char *arena_dup(const char *s, size_t n) {
    if (apos + n + 1 > sizeof arena) return NULL;
    char *p = arena + apos;
    memcpy(p, s, n);
    p[n] = 0;
    apos += n + 1;
    return p;
}

static void add_str(char *w, size_t *wl, size_t max, const char *v) {
    while (*v && *wl < max - 1) w[(*wl)++] = *v++;
}

static void expand_var(const char **sp, char *w, size_t *wl, size_t max) {
    const char *s = *sp + 1;
    char name[64];
    size_t nl = 0;
    if (*s == '?') {
        char b[16];
        snprintf(b, sizeof b, "%d", last_status);
        add_str(w, wl, max, b);
        s++;
    } else if (isalpha((unsigned char)*s) || *s == '_') {
        while ((isalnum((unsigned char)*s) || *s == '_') && nl < sizeof name - 1) name[nl++] = *s++;
        name[nl] = 0;
        const char *v = getenv(name);
        if (v) add_str(w, wl, max, v);
    } else if (*wl < max - 1) w[(*wl)++] = '$';
    *sp = s;
}

static int tokenize(const char *s, struct tok *t, int max) {
    int n = 0;
    apos = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s) break;
        if (n >= max - 1) return -1;
        if (*s == '|') { t[n++] = (struct tok){T_PIPE, NULL, 0}; s++; continue; }
        if (*s == ';') { t[n++] = (struct tok){T_SEMI, NULL, 0}; s++; continue; }
        if (*s == '<') { t[n++] = (struct tok){T_IN, NULL, 0}; s++; continue; }
        if (*s == '>') {
            if (s[1] == '>') { t[n++] = (struct tok){T_APP, NULL, 0}; s += 2; }
            else { t[n++] = (struct tok){T_OUT, NULL, 0}; s++; }
            continue;
        }
        char w[1024];
        size_t wl = 0;
        int gl = 0;                       /* saw an unquoted * ? [ */
        while (*s && !strchr(" \t\r\n|;<>", *s)) {
            if (*s == '\'') {
                s++;
                while (*s && *s != '\'') { if (wl < sizeof w - 1) w[wl++] = *s; s++; }
                if (*s) s++;
            } else if (*s == '"') {
                s++;
                while (*s && *s != '"') {
                    if (*s == '$') expand_var(&s, w, &wl, sizeof w);
                    else if (*s == '\\' && s[1] && strchr("\"\\$", s[1])) { s++; if (wl < sizeof w - 1) w[wl++] = *s++; }
                    else { if (wl < sizeof w - 1) w[wl++] = *s; s++; }
                }
                if (*s) s++;
            } else if (*s == '\\' && s[1]) {
                s++;
                if (wl < sizeof w - 1) w[wl++] = *s++;
            } else if (*s == '$') {
                expand_var(&s, w, &wl, sizeof w);
            } else {
                if (*s == '*' || *s == '?' || *s == '[') gl = 1;
                if (wl < sizeof w - 1) w[wl++] = *s;
                s++;
            }
        }
        w[wl] = 0;
        char *d = arena_dup(w, wl);
        if (!d) return -1;
        t[n++] = (struct tok){T_WORD, d, gl};
    }
    return n;
}

/* Copies the next command (up to an unquoted ; or &) into seg.
 * Returns 1 if it ended with '&' (background), else 0.
 * Sets *bad if it met '&&' (not supported). */
static int next_segment(const char **sp, char *seg, size_t max, int *bad) {
    const char *s = *sp;
    size_t n = 0;
    int bg = 0;
    char q = 0;
#define PUT(ch) do { if (n < max - 1) seg[n++] = (ch); } while (0)
    for (; *s; s++) {
        char c = *s;
        if (q) {
            if (c == q) q = 0;
            else if (q == '"' && c == '\\' && s[1]) { PUT(c); c = *++s; }
        } else if (c == '\'' || c == '"') q = c;
        else if (c == '\\' && s[1]) { PUT(c); c = *++s; }
        else if (c == ';') { s++; break; }
        else if (c == '&') { if (s[1] == '&') *bad = 1; bg = 1; s++; break; }
        PUT(c);
    }
#undef PUT
    seg[n] = 0;
    *sp = s;
    return bg;
}

/* ---------- running commands ---------- */

static int wait_for(pid_t pid) {
    if (pid <= 0) return 1;
    int st;
    if (waitpid(pid, &st, 0) < 0) return 1;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

static int apply_redirs(const struct cmd *c) {
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "alxa: %s: %s\n", c->in, strerror(errno)); return -1; }
        dup2(fd, 0); close(fd);
    }
    if (c->out) {
        int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { fprintf(stderr, "alxa: %s: %s\n", c->out, strerror(errno)); return -1; }
        dup2(fd, 1); close(fd);
    }
    return 0;
}

/* Runs in a child process: never returns. */
static void child_exec(const struct cmd *c, const struct builtin *b) {
    if (b) {
        int rc = b->fn(c->argc, (char **)c->argv);
        fflush(NULL);
        _exit(rc & 0xff);
    }
    char path[300];
    if (strchr(c->argv[0], '/')) snprintf(path, sizeof path, "%s", c->argv[0]);
    else if (!find_in_path(c->argv[0], path, sizeof path)) {
        fprintf(stderr, "%s: command not found\n", c->argv[0]);
        _exit(127);
    }
    execv(path, (char **)c->argv);
    fprintf(stderr, "%s: cannot run: %s\n", c->argv[0], strerror(errno));
    _exit(126);
}

static int run_pipeline(struct cmd *c, int n, int bg) {
    /* cd, export, unset change the shell itself, so they run in-process */
    if (n == 1 && !bg) {
        const struct builtin *b = find_builtin(c[0].argv[0]);
        if (b && b->in_parent) {
            int s0 = dup(0), s1 = dup(1), rc;
            if (apply_redirs(&c[0]) < 0) rc = 1;
            else { rc = b->fn(c[0].argc, c[0].argv); fflush(stdout); }
            dup2(s0, 0); dup2(s1, 1);
            close(s0); close(s1);
            return rc;
        }
    }

    fflush(NULL);
    pid_t pids[MAXPIPE];
    for (int i = 0; i < MAXPIPE; i++) pids[i] = -1;
    int prev = -1;
    pid_t pgid = 0;
    for (int i = 0; i < n; i++) {
        int fds[2] = {-1, -1};
        if (i < n - 1 && pipe(fds) < 0) { fputs("alxa: pipe failed\n", stderr); break; }
        pid_t pid = fork();
        if (pid < 0) {
            fputs("alxa: fork failed\n", stderr);
            if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
            break;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            if (bg) {
                setpgid(0, pgid);              /* own group: Ctrl+C won't hit it */
                if (i == 0 && !c[i].in) {
                    int nf = open("/dev/null", O_RDONLY);
                    if (nf >= 0) { dup2(nf, 0); close(nf); }
                }
            }
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); close(fds[0]); }
            if (apply_redirs(&c[i]) < 0) _exit(1);
            child_exec(&c[i], find_builtin(c[i].argv[0]));
        }
        pids[i] = pid;
        if (bg) { if (!pgid) pgid = pid; setpgid(pid, pgid); }
        if (prev >= 0) close(prev);
        if (fds[1] >= 0) { close(fds[1]); prev = fds[0]; } else prev = -1;
    }
    if (prev >= 0) close(prev);

    if (bg) {
        if (pids[n - 1] > 0) printf("[%d]\n", (int)pids[n - 1]);
        return 0;
    }
    int st = 1;
    for (int i = 0; i < n; i++) st = wait_for(pids[i]);
    return st;
}

static int is_assign(const char *s) {
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    while (isalnum((unsigned char)*s) || *s == '_') s++;
    return *s == '=';
}

static void add_word(struct cmd *c, const struct tok *t) {
    if (t->glob) {
        glob_t g;
        if (glob(t->text, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc && c->argc < MAXARGS - 1; k++) {
                char *d = arena_dup(g.gl_pathv[k], strlen(g.gl_pathv[k]));
                if (d) c->argv[c->argc++] = d;
            }
            globfree(&g);
            return;
        }
        /* no match: keep the pattern as typed */
    }
    if (c->argc < MAXARGS - 1) c->argv[c->argc++] = t->text;
}

static void run_segment(const char *text, int bg) {
    struct tok t[MAXTOK];
    int n = tokenize(text, t, MAXTOK);
    if (n < 0) { fputs("alxa: line too complex\n", stderr); last_status = 2; return; }

    struct cmd cmds[MAXPIPE];
    memset(cmds, 0, sizeof cmds);
    int nc = 0, bad = 0, assigned = 0;
    struct cmd *c = &cmds[0];
    for (int i = 0; i < n; i++) {
        switch (t[i].type) {
        case T_WORD:
            if (c->argc == 0 && nc == 0 && is_assign(t[i].text)) {
                char *eq = strchr(t[i].text, '=');
                *eq = 0;
                setenv(t[i].text, eq + 1, 1);
                assigned = 1;
            } else add_word(c, &t[i]);
            break;
        case T_PIPE:
            if (c->argc == 0 || nc + 1 >= MAXPIPE) bad = 1;
            else c = &cmds[++nc];
            break;
        case T_SEMI:
            break;
        default:
            if (i + 1 >= n || t[i + 1].type != T_WORD) bad = 1;
            else {
                char *f = t[++i].text;
                if (t[i - 1].type == T_IN) c->in = f;
                else { c->out = f; c->append = (t[i - 1].type == T_APP); }
            }
        }
    }
    if (nc == 0 && c->argc == 0 && !c->in && !c->out && !bad) {   /* empty or assignment only */
        if (assigned) last_status = 0;
        return;
    }
    int total = nc + 1;
    for (int k = 0; k < total; k++) if (cmds[k].argc == 0) bad = 1;
    if (bad) { fputs("alxa: syntax error\n", stderr); last_status = 2; return; }

    in_fg = !bg;
    last_status = run_pipeline(cmds, total, bg);
    in_fg = 0;
}

static void run_line(const char *line) {
    const char *p = line;
    while (*p) {
        char seg[MAXLINE];
        int bad = 0;
        int bg = next_segment(&p, seg, sizeof seg, &bad);
        if (bad) { fputs("alxa: '&&' is not supported yet\n", stderr); last_status = 2; return; }
        run_segment(seg, bg);
    }
}

/* ---------- main loop ---------- */

static void on_sigchld(int sig) {
    (void)sig;
    if (in_fg) return;                 /* wait_for() needs that exit status */
    int e = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) ;
    errno = e;
}

static void prompt(void) {
    char cwd[128];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    if (isatty(1)) printf("\033[1;36malxa\033[0m:\033[1;34m%s\033[0m> ", cwd);
    else printf("alxa:%s> ", cwd);
    fflush(stdout);
}

int main(void) {
    char line[MAXLINE];

    if (getpid() == 1) {
        setup_system();
        setsid();                      /* own session, so the console can be our terminal */
        ioctl(0, TIOCSCTTY, 0);        /* needed for Ctrl+C to reach programs */
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGINT, SIG_IGN);           /* Ctrl+C must not kill the shell */
    signal(SIGQUIT, SIG_IGN);

    banner();

    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) ;   /* reap orphans (PID 1 duty) */
        prompt();
        if (!fgets(line, sizeof line, stdin)) {
            clearerr(stdin);
            pause();                    /* no input available: wait, never exit */
            continue;
        }
        size_t len = strlen(line);
        if (len == sizeof line - 1 && line[len - 1] != '\n') {
            int ch;                     /* line too long: throw away the rest of it */
            while ((ch = getchar()) != '\n' && ch != EOF) ;
            fputs("alxa: line too long (max 511 characters)\n", stderr);
            last_status = 2;
            continue;
        }
        run_line(line);
    }
}