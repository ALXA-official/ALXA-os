/*
 * ALXA OS - init + shell
 * Original code by alxa, MIT licensed. Runs as PID 1.
 *
 * Shell syntax: cmd args | cmd2 > file >> file < file ; cmd2
 *               'single quotes'  "double quotes"  $VAR  $?
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
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include "editor.h"
#include "cmds.h"

#define VERSION  "0.4"
#define MAXLINE  512
#define MAXARGS  64
#define MAXTOK   128
#define MAXPIPE  8

/* Folders searched for commands, in order. Drop a program in one of them
 * (or use the 'install' command) and it becomes a command. */
static const char *pathdirs[] = { "/bin", "/usr/bin", "/usr/local/bin", "/sbin", NULL };
static int last_status;

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
    mount("proc", "/proc", "proc", 0, NULL);          /* needed by ps, mount */
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
    puts("                 cmd1 ; cmd2     run both    \"quotes\" 'quotes'   $VAR   $?");
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
struct tok { int type; char *text; };

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
        if (*s == '|') { t[n++] = (struct tok){T_PIPE, NULL}; s++; continue; }
        if (*s == ';') { t[n++] = (struct tok){T_SEMI, NULL}; s++; continue; }
        if (*s == '<') { t[n++] = (struct tok){T_IN, NULL}; s++; continue; }
        if (*s == '>') {
            if (s[1] == '>') { t[n++] = (struct tok){T_APP, NULL}; s += 2; }
            else { t[n++] = (struct tok){T_OUT, NULL}; s++; }
            continue;
        }
        char w[1024];
        size_t wl = 0;
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
                if (wl < sizeof w - 1) w[wl++] = *s;
                s++;
            }
        }
        w[wl] = 0;
        char *d = arena_dup(w, wl);
        if (!d) return -1;
        t[n++] = (struct tok){T_WORD, d};
    }
    return n;
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

static int run_single(struct cmd *c) {
    const struct builtin *b = find_builtin(c->argv[0]);
    if (b && b->in_parent) {                 /* cd, export, unset: change the shell itself */
        int s0 = dup(0), s1 = dup(1), rc;
        if (apply_redirs(c) < 0) rc = 1;
        else { rc = b->fn(c->argc, c->argv); fflush(stdout); }
        dup2(s0, 0); dup2(s1, 1);
        close(s0); close(s1);
        return rc;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { fputs("alxa: fork failed\n", stderr); return 1; }
    if (pid == 0) {
        if (apply_redirs(c) < 0) _exit(1);
        child_exec(c, b);
    }
    return wait_for(pid);
}

static int run_pipeline(struct cmd *c, int n) {
    if (n == 1) return run_single(&c[0]);
    fflush(NULL);
    pid_t pids[MAXPIPE];
    int prev = -1;
    for (int i = 0; i < n; i++) {
        int fds[2] = {-1, -1};
        pids[i] = -1;
        if (i < n - 1 && pipe(fds) < 0) { fputs("alxa: pipe failed\n", stderr); break; }
        pid_t pid = fork();
        if (pid == 0) {
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); close(fds[0]); }
            if (apply_redirs(&c[i]) < 0) _exit(1);
            child_exec(&c[i], find_builtin(c[i].argv[0]));
        }
        pids[i] = pid;
        if (prev >= 0) close(prev);
        if (fds[1] >= 0) { close(fds[1]); prev = fds[0]; } else prev = -1;
    }
    if (prev >= 0) close(prev);
    int st = 1;
    for (int i = 0; i < n; i++) st = wait_for(pids[i]);
    return st;
}

static void run_line(const char *line) {
    struct tok t[MAXTOK];
    int n = tokenize(line, t, MAXTOK);
    if (n < 0) { fputs("alxa: line too complex\n", stderr); last_status = 2; return; }
    int i = 0;
    while (i < n) {
        struct cmd cmds[MAXPIPE];
        memset(cmds, 0, sizeof cmds);
        int nc = 0, bad = 0;
        struct cmd *c = &cmds[0];
        for (; i < n && t[i].type != T_SEMI; i++) {
            switch (t[i].type) {
            case T_WORD:
                if (c->argc < MAXARGS - 1) c->argv[c->argc++] = t[i].text;
                break;
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
        i++;                                   /* skip the ';' */
        int total = nc + 1;
        if (total == 1 && c->argc == 0 && !c->in && !c->out && !bad) continue;   /* empty piece */
        for (int k = 0; k < total; k++) if (cmds[k].argc == 0) bad = 1;
        if (bad) { fputs("alxa: syntax error\n", stderr); last_status = 2; continue; }
        last_status = run_pipeline(cmds, total);
    }
}

/* ---------- main loop ---------- */

static void prompt(void) {
    char cwd[128];
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    if (isatty(1)) printf("\033[1;36malxa\033[0m:\033[1;34m%s\033[0m> ", cwd);
    else printf("alxa:%s> ", cwd);
    fflush(stdout);
}

int main(void) {
    char line[MAXLINE];

    if (getpid() == 1) setup_system();
    banner();

    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) ;   /* reap orphans (PID 1 duty) */
        prompt();
        if (!fgets(line, sizeof line, stdin)) {
            clearerr(stdin);
            pause();                    /* no input available: wait, never exit */
            continue;
        }
        run_line(line);
    }
}
