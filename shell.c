/*
 * Magnesium OS - the shell.
 * Original code by alxa, MIT licensed.
 *
 * Syntax: cmd args | cmd2   > file  >> file  < file   cmd1 ; cmd2
 *         cmd1 && cmd2   cmd1 || cmd2   cmd &   # comment
 *         'single'  "double"  $VAR  $?  ~  wildcards * ? [..]
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
#include <glob.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include "editor.h"
#include "cmds.h"
#include "users.h"
#include "line.h"
#include "gfx.h"
#include "gui.h"
#include "net.h"
#include "registry.h"
#include "shell.h"
#include "version.h"

#define MAXLINE  512
#define MAXARGS  64
#define MAXTOK   160
#define MAXPIPE  8
#define MAXMATCH 256

static const char *pathdirs[] = { "/programs", "/data/programs", "/bin", "/usr/bin", "/usr/local/bin", "/sbin", "/data/bin", NULL };
static int last_status;
static int job_ctl;            /* 1 when we own a terminal and can hand it to jobs */
static pid_t shell_pgid;
static int want_exit;
static int script_depth;

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
    puts("Files / folders: cp mv rm(del) mkdir rmdir touch ln chmod chown stat du df basename dirname cd pwd");
    puts("Edit:            edit FILE   (Ctrl+O save, Ctrl+X exit)");
    puts("System:          mem free uptime date uname hostname dmesg ps kill sleep seq sync mount umount");
    puts("                 env export unset reboot halt poweroff");
    puts("Accounts:        id whoami users passwd adduser deluser exit(logout)");
    puts("Shell:           echo clear true false history source sh ver commands which install help");
    puts("Programs:        files notes calc canvas sysmon netcenter mines   (in /programs)");
    puts("Network:         ip dhcp ping dns fetch netstat");
    puts("Drivers & UI:    drivers lspci display(gpu) theme desktop");
    puts("Registry:        reg list | reg get KEY | reg set KEY VALUE | reg del KEY");
    puts("");
    puts("Syntax:  a | b   a > f   a >> f   a < f   a ; b   a && b   a || b   a &   # comment");
    puts("         \"quotes\"  'quotes'  $VAR  $?  ~  wildcards (* ? [])");
    puts("Keys:    Up/Down = history   Tab = complete   Ctrl+C = cancel/stop   Ctrl+D = logout");
    puts("Programs in /programs, /data/programs, /bin, /usr/bin, /sbin or /data/bin run by name.");
    return 0;
}

static int cmd_ver(int argc, char **argv) { (void)argc; (void)argv; puts(OS_NAME " " OS_VERSION_LONG " - made by " OS_AUTHOR); return 0; }

static int cmd_clear(int argc, char **argv) { (void)argc; (void)argv; fputs("\033[2J\033[H", stdout); return 0; }

static int cmd_edit(int argc, char **argv) {
    if (argc < 2) { fputs("usage: edit FILE\n", stderr); return 1; }
    return editor_run(argv[1]);
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    sync();
    if (reboot(RB_AUTOBOOT) < 0) fprintf(stderr, "reboot: %s\n", strerror(errno));
    return 1;
}

static int cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    sync();
    if (reboot(RB_POWER_OFF) < 0) fprintf(stderr, "poweroff: %s\n", strerror(errno));
    else fputs("poweroff: not supported by this kernel build (close the emulator instead)\n", stderr);
    return 1;
}

static int cmd_exit(int argc, char **argv) { (void)argc; (void)argv; want_exit = 1; return 0; }

static int cmd_desktop(int argc, char **argv) {
    (void)argc; (void)argv;
    if (gui_run() == GUI_LOGOUT) want_exit = 1;
    return 0;
}

static int cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;
    for (int i = 0; i < line_history_count(); i++) printf("%5d  %s\n", i + 1, line_history_get(i));
    return 0;
}

static int cmd_source(int argc, char **argv) {
    if (argc < 2) { fputs("usage: source FILE\n", stderr); return 1; }
    if (script_depth >= 8) { fputs("source: too many nested scripts\n", stderr); return 1; }
    script_depth++;
    int rc = shell_run_script(argv[1]);
    script_depth--;
    return rc;
}

static int cmd_sh(int argc, char **argv) {
    if (argc < 2) { fputs("usage: sh FILE\n", stderr); return 1; }
    return shell_run_script(argv[1]);
}

static int cmd_commands(int argc, char **argv);
static int cmd_which(int argc, char **argv);
static int cmd_install(int argc, char **argv);

static const struct builtin core_builtins[] = {
    {"help", cmd_help, 0}, {"ver", cmd_ver, 0}, {"clear", cmd_clear, 0}, {"edit", cmd_edit, 0},
    {"commands", cmd_commands, 0}, {"which", cmd_which, 0}, {"install", cmd_install, 0},
    {"reboot", cmd_reboot, 0}, {"poweroff", cmd_poweroff, 0},
    {"exit", cmd_exit, 1}, {"logout", cmd_exit, 1}, {"history", cmd_history, 0},
    {"source", cmd_source, 1}, {"sh", cmd_sh, 0},
    {"desktop", cmd_desktop, 1}, {"display", cmd_display, 0}, {"gpu", cmd_display, 0},
    {"theme", cmd_theme, 0}, {"reg", cmd_reg, 0},
    {"ip", cmd_ip, 0}, {"ifconfig", cmd_ip, 0}, {"dhcp", cmd_dhcp, 0},
    {"ping", cmd_ping, 0}, {"dns", cmd_dns, 0}, {"fetch", cmd_fetch, 0}, {"netstat", cmd_netstat, 0},
    {"drivers", cmd_drivers, 0}, {"lspci", cmd_lspci, 0},
    {"adduser", cmd_adduser, 0}, {"deluser", cmd_deluser, 0}, {"users", cmd_users, 0},
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
    for (int pass = 0; pass < 2; pass++)
        for (const struct builtin *b = pass ? extra_builtins : core_builtins; b->name; b++) {
            printf("%s%-10s", col == 0 ? "  " : "", b->name);
            if (++col == 7) { putchar('\n'); col = 0; }
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
    if (find_builtin(name) || strchr(name, '/')) { fprintf(stderr, "install: '%s' is not allowed as a name\n", name); return 1; }

    unsigned char magic[4] = {0};
    int fd = open(argv[1], O_RDONLY);
    if (fd >= 0) { if (read(fd, magic, 4) < 0) {} close(fd); }
    if (!(magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') &&
        !(magic[0] == '#' && magic[1] == '!'))
        fputs("install: warning: not a compiled program or script, it may not run\n", stderr);

    /* Prefer the persistent programs directory when mounted, otherwise /programs. */
    const char *dir = access("/data/programs", W_OK) == 0 ? "/data/programs" :
                      access("/programs", W_OK) == 0 ? "/programs" :
                      access("/data/bin", W_OK) == 0 ? "/data/bin" : "/bin";
    char dst[300];
    snprintf(dst, sizeof dst, "%s/%s", dir, name);
    if (copy_file(argv[1], dst, 0755) < 0) {
        fprintf(stderr, "install: %s: %s%s\n", dst, strerror(errno),
                errno == EACCES ? " (only root can install programs)" : "");
        return 1;
    }
    printf("installed %s: type '%s' to run it%s\n", dst, name,
           strcmp(dir, "/bin") ? "" : " (not saved: no disk, lost at reboot)");
    return 0;
}

/* ---------- parsing ---------- */

enum { T_WORD, T_PIPE, T_IN, T_OUT, T_APP, T_SEMI, T_AND, T_OR, T_BG };
struct tok { int type; char *text; int glob; };

struct cmd {
    char *argv[MAXARGS];
    int argc;
    char *in, *out;
    int append;
};

struct lex { char arena[16384]; size_t apos; };

static char *lex_dup(struct lex *lx, const char *s, size_t n) {
    if (lx->apos + n + 1 > sizeof lx->arena) return NULL;
    char *p = lx->arena + lx->apos;
    memcpy(p, s, n);
    p[n] = 0;
    lx->apos += n + 1;
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

static int tokenize(struct lex *lx, const char *s, struct tok *t, int max) {
    int n = 0;
    lx->apos = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
        if (!*s || *s == '#') break;
        if (n >= max - 1) return -1;
        if (*s == '|') {
            if (s[1] == '|') { t[n++] = (struct tok){T_OR, NULL, 0}; s += 2; }
            else { t[n++] = (struct tok){T_PIPE, NULL, 0}; s++; }
            continue;
        }
        if (*s == '&') {
            if (s[1] == '&') { t[n++] = (struct tok){T_AND, NULL, 0}; s += 2; }
            else { t[n++] = (struct tok){T_BG, NULL, 0}; s++; }
            continue;
        }
        if (*s == ';') { t[n++] = (struct tok){T_SEMI, NULL, 0}; s++; continue; }
        if (*s == '<') { t[n++] = (struct tok){T_IN, NULL, 0}; s++; continue; }
        if (*s == '>') {
            if (s[1] == '>') { t[n++] = (struct tok){T_APP, NULL, 0}; s += 2; }
            else { t[n++] = (struct tok){T_OUT, NULL, 0}; s++; }
            continue;
        }
        char w[1024];
        size_t wl = 0;
        int has_glob = 0;
        if (*s == '~' && (s[1] == '/' || !s[1] || strchr(" \t\r\n|;<>&", s[1]))) {
            const char *h = getenv("HOME");
            if (h) add_str(w, &wl, sizeof w, h);
            s++;
        }
        while (*s && !strchr(" \t\r\n|;<>&", *s)) {
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
                if (strchr("*?[", *s)) has_glob = 1;
                if (wl < sizeof w - 1) w[wl++] = *s;
                s++;
            }
        }
        w[wl] = 0;
        char *d = lex_dup(lx, w, wl);
        if (!d) return -1;
        t[n++] = (struct tok){T_WORD, d, has_glob};
    }
    return n;
}

/* ---------- running commands ---------- */

static void reset_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
}

static void jobs_init(void) {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    if (isatty(0) && tcgetpgrp(0) == getpgrp()) { job_ctl = 1; shell_pgid = getpgrp(); }
}

static int wait_for(pid_t pid) {
    if (pid <= 0) return 1;
    int st;
    for (;;) {
        if (waitpid(pid, &st, WUNTRACED) < 0) { if (errno == EINTR) continue; return 1; }
        if (WIFSTOPPED(st)) { kill(pid, SIGKILL); continue; }
        break;
    }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

static int apply_redirs(const struct cmd *c) {
    if (c->in) {
        int fd = open(c->in, O_RDONLY);
        if (fd < 0) { fprintf(stderr, "mg: %s: %s\n", c->in, strerror(errno)); return -1; }
        dup2(fd, 0); close(fd);
    }
    if (c->out) {
        int fd = open(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { fprintf(stderr, "mg: %s: %s\n", c->out, strerror(errno)); return -1; }
        dup2(fd, 1); close(fd);
    }
    return 0;
}

static int is_script(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char m[2] = {0, 0};
    ssize_t r = read(fd, m, 2);
    close(fd);
    return r == 2 && m[0] == '#' && m[1] == '!';
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
    if (is_script(path)) {                      /* "#!" scripts run with the Magnesium shell */
        int rc = shell_run_script(path);
        fflush(NULL);
        _exit(rc & 0xff);
    }
    execv(path, (char **)c->argv);
    fprintf(stderr, "%s: cannot run: %s\n", c->argv[0], strerror(errno));
    _exit(126);
}

static void child_setup(pid_t pgid, int fg) {
    if (job_ctl) {
        setpgid(0, pgid);
        if (fg && pgid == 0) tcsetpgrp(0, getpid());   /* SIGTTOU is still ignored here */
    }
    reset_signals();
    job_ctl = 0;                    /* only the shell itself hands out the terminal */
}

static int run_in_parent(struct cmd *c, const struct builtin *b) {
    int s0 = dup(0), s1 = dup(1), rc;
    if (apply_redirs(c) < 0) rc = 1;
    else { rc = b->fn(c->argc, c->argv); fflush(stdout); }
    dup2(s0, 0); dup2(s1, 1);
    close(s0); close(s1);
    return rc;
}

static int run_pipeline(struct cmd *c, int n, int bg) {
    if (n == 1 && !bg) {
        const struct builtin *b = find_builtin(c[0].argv[0]);
        if (b && b->in_parent) return run_in_parent(&c[0], b);
    }
    fflush(NULL);
    pid_t pids[MAXPIPE], pgid = 0;
    int prev = -1;
    for (int i = 0; i < n; i++) {
        int fds[2] = {-1, -1};
        pids[i] = -1;
        if (i < n - 1 && pipe(fds) < 0) { fputs("mg: pipe failed\n", stderr); break; }
        pid_t pid = fork();
        if (pid < 0) { fputs("mg: fork failed\n", stderr); if (fds[0] >= 0) { close(fds[0]); close(fds[1]); } break; }
        if (pid == 0) {
            child_setup(pgid, !bg);
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            else if (bg && !c[i].in) { int z = open("/dev/null", O_RDONLY); if (z >= 0) { dup2(z, 0); close(z); } }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); close(fds[0]); }
            if (apply_redirs(&c[i]) < 0) _exit(1);
            child_exec(&c[i], find_builtin(c[i].argv[0]));
        }
        pids[i] = pid;
        if (i == 0) pgid = pid;
        if (job_ctl) { setpgid(pid, pgid); if (!bg && i == 0) tcsetpgrp(0, pgid); }
        if (prev >= 0) close(prev);
        if (fds[1] >= 0) { close(fds[1]); prev = fds[0]; } else prev = -1;
    }
    if (prev >= 0) close(prev);
    if (bg) { printf("[running in background: %d]\n", (int)pids[n - 1]); return 0; }
    int st = 1;
    for (int i = 0; i < n; i++) st = wait_for(pids[i]);
    if (job_ctl) tcsetpgrp(0, shell_pgid);
    return st;
}

static void add_arg(struct cmd *c, char *s) {
    if (c->argc < MAXARGS - 1) c->argv[c->argc++] = s;
}

static void add_word(struct lex *lx, struct cmd *c, const struct tok *t) {
    if (!t->glob) { add_arg(c, t->text); return; }
    glob_t g;
    if (glob(t->text, GLOB_NOCHECK, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            char *d = lex_dup(lx, g.gl_pathv[i], strlen(g.gl_pathv[i]));
            if (d) add_arg(c, d);
        }
        globfree(&g);
    } else add_arg(c, t->text);
}

/* Splits off the next command group (up to ; && || or &) without expanding
 * anything, so variables are expanded only when that group actually runs. */
static void next_segment(const char **sp, char *seg, size_t size, int *conn) {
    const char *s = *sp;
    size_t n = 0;
    *conn = T_SEMI;
    while (*s) {
        char c = *s;
        if (c == '\'' || c == '"') {
            if (n + 1 < size) seg[n++] = c;
            s++;
            while (*s && *s != c) {
                if (c == '"' && *s == '\\' && s[1]) {
                    if (n + 2 < size) { seg[n++] = s[0]; seg[n++] = s[1]; }
                    s += 2;
                    continue;
                }
                if (n + 1 < size) seg[n++] = *s;
                s++;
            }
            if (*s) { if (n + 1 < size) seg[n++] = *s; s++; }
            continue;
        }
        if (c == '\\' && s[1]) {
            if (n + 2 < size) { seg[n++] = s[0]; seg[n++] = s[1]; }
            s += 2;
            continue;
        }
        if (c == '#' && (n == 0 || seg[n - 1] == ' ' || seg[n - 1] == '\t')) { s += strlen(s); break; }
        if (c == ';') { s++; break; }
        if (c == '&') { if (s[1] == '&') { *conn = T_AND; s += 2; } else { *conn = T_BG; s++; } break; }
        if (c == '|' && s[1] == '|') { *conn = T_OR; s += 2; break; }
        if (n + 1 < size) seg[n++] = c;
        s++;
    }
    seg[n] = 0;
    *sp = s;
}

static void run_segment(const char *seg, int bg) {
    struct lex lx;
    struct tok t[MAXTOK];
    int n = tokenize(&lx, seg, t, MAXTOK);
    if (n < 0) { fputs("mg: line too complex\n", stderr); last_status = 2; return; }
    if (n == 0) return;
    struct cmd cmds[MAXPIPE];
    memset(cmds, 0, sizeof cmds);
    int nc = 0, bad = 0;
    struct cmd *c = &cmds[0];
    for (int i = 0; i < n; i++) {
        switch (t[i].type) {
        case T_WORD: add_word(&lx, c, &t[i]); break;
        case T_PIPE:
            if (c->argc == 0 || nc + 1 >= MAXPIPE) bad = 1;
            else c = &cmds[++nc];
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
    for (int k = 0; k <= nc; k++) if (cmds[k].argc == 0) bad = 1;
    if (bad) { fputs("mg: syntax error\n", stderr); last_status = 2; return; }
    last_status = run_pipeline(cmds, nc + 1, bg);
}

static void run_line(const char *line) {
    const char *p = line;
    int prev_conn = T_SEMI;
    while (*p && !want_exit) {
        char seg[MAXLINE * 2];
        int conn;
        next_segment(&p, seg, sizeof seg, &conn);
        int skip = (prev_conn == T_AND && last_status != 0) || (prev_conn == T_OR && last_status == 0);
        if (!skip) run_segment(seg, conn == T_BG);
        prev_conn = conn;
    }
}

int shell_run_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "sh: %s: %s\n", path, strerror(errno)); return 127; }
    char line[MAXLINE];
    int saved_exit = want_exit;
    want_exit = 0;
    while (fgets(line, sizeof line, f) && !want_exit) run_line(line);
    fclose(f);
    want_exit = saved_exit;
    return last_status;
}

/* ---------- tab completion ---------- */

static int add_match(char **v, int n, const char *s) {
    if (n >= MAXMATCH) return n;
    for (int i = 0; i < n; i++) if (!strcmp(v[i], s)) return n;
    v[n] = strdup(s);
    return v[n] ? n + 1 : n;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int complete(const char *word, int first, char ***outp) {
    char **v = malloc(MAXMATCH * sizeof(char *));
    int n = 0;
    if (!v) return 0;
    size_t wl = strlen(word);
    if (first && !strchr(word, '/')) {
        for (int pass = 0; pass < 2; pass++)
            for (const struct builtin *b = pass ? extra_builtins : core_builtins; b->name; b++)
                if (!strncmp(b->name, word, wl)) n = add_match(v, n, b->name);
        for (int i = 0; pathdirs[i]; i++) {
            DIR *d = opendir(pathdirs[i]);
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.' || strncmp(e->d_name, word, wl)) continue;
                char full[512];
                snprintf(full, sizeof full, "%s/%s", pathdirs[i], e->d_name);
                if (access(full, X_OK) == 0) n = add_match(v, n, e->d_name);
            }
            closedir(d);
        }
    } else {
        char dirpart[512] = "", base[256];
        const char *slash = strrchr(word, '/');
        if (slash) {
            size_t dl = (size_t)(slash - word) + 1;
            if (dl >= sizeof dirpart) { free(v); return 0; }
            memcpy(dirpart, word, dl);
            dirpart[dl] = 0;
            snprintf(base, sizeof base, "%s", slash + 1);
        } else snprintf(base, sizeof base, "%s", word);
        char opened[600] = "";
        const char *home = getenv("HOME");
        if (slash && !strncmp(dirpart, "~/", 2) && home) snprintf(opened, sizeof opened, "%s%s", home, dirpart + 1);
        else snprintf(opened, sizeof opened, "%s", slash ? dirpart : ".");
        DIR *d = opendir(opened);
        if (d) {
            struct dirent *e;
            size_t bl = strlen(base);
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                if (e->d_name[0] == '.' && base[0] != '.') continue;
                if (strncmp(e->d_name, base, bl)) continue;
                char cand[1100], full[1024];
                char real[1200];
                snprintf(full, sizeof full, "%s%s", slash ? dirpart : "", e->d_name);
                snprintf(real, sizeof real, "%s%s", slash ? opened : "", e->d_name);
                snprintf(cand, sizeof cand, "%s%s", full, is_dir(real) ? "/" : "");
                n = add_match(v, n, cand);
            }
            closedir(d);
        }
    }
    qsort(v, (size_t)n, sizeof(char *), cmp_str);
    *outp = v;
    return n;
}

/* ---------- main loop ---------- */

static void build_prompt(char *out, size_t size) {
    char cwd[256], pre[96] = "";
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    const char *user = getenv("USER");
    int tty = isatty(1);
    int root = getuid() == 0;
    if (reg_eff_int("Shell/ShowUser", 0)) {
        if (tty) snprintf(pre, sizeof pre, "\033[1;32m%s@" OS_SHORT "\033[0m ", user ? user : OS_SHORT);
        else snprintf(pre, sizeof pre, "%s@" OS_SHORT " ", user ? user : OS_SHORT);
    }
    int classic = !strcasecmp(reg_eff("Shell/Prompt", "Arrow"), "Classic");
    if (classic) {
        if (tty) snprintf(out, size, "%s\033[1;34m%s\033[0m %c ", pre, cwd, root ? '#' : '$');
        else snprintf(out, size, "%s%s %c ", pre, cwd, root ? '#' : '$');
    } else {
        /* /folder ---->   (the arrow is red for root, green for everyone else) */
        if (tty) snprintf(out, size, "%s\033[1;34m%s\033[0m %s----> \033[0m", pre, cwd, root ? "\033[1;31m" : "\033[1;32m");
        else snprintf(out, size, "%s%s ----> ", pre, cwd);
    }
}

static int read_command(const char *prompt, char *line, size_t size) {
    if (isatty(0)) {
        int r = line_edit(prompt, line, size);
        if (r >= 0) return r;
    }
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(line, (int)size, stdin)) return 0;
    return 1;
}

int shell_exec_line(const char *line) {
    run_line(line);
    return last_status;
}

void shell_session(void) {
    char line[MAXLINE], prompt[700];
    jobs_init();
    line_set_completer(complete);
    want_exit = 0;
    const char *home = getenv("HOME");
    if (home && *home) {
        char up[300];
        snprintf(up, sizeof up, "%s/.mg-registry", home);
        reg_set_paths(NULL, up);              /* this user's own settings */
    }
    reg_apply_env();                          /* Environment values become real variables */
    int def_auto = getenv("MG_GRAPHICAL_LOGIN") ? 1 : 0;
    if (!getenv("MG_IN_DESKTOP") && isatty(0) && reg_eff_int("Desktop/AutoStart", def_auto)) {
        setenv("MG_IN_DESKTOP", "1", 1);
        cmd_desktop(0, NULL);
        unsetenv("MG_IN_DESKTOP");
    }
    for (;;) {
        if (want_exit) break;
        reg_refresh();
        while (waitpid(-1, NULL, WNOHANG) > 0) ;     /* reap finished background jobs */
        build_prompt(prompt, sizeof prompt);
        if (!read_command(prompt, line, sizeof line)) { if (isatty(0)) puts("logout"); break; }
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (l && isatty(0)) line_history_add(line);
        run_line(line);
        if (want_exit) break;
    }
}
