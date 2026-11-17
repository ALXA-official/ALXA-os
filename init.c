/*
 * ALXA OS - init + shell
 * Original code by alxa, MIT licensed. Runs as PID 1.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include "editor.h"

#define VERSION  "0.3"
#define MAXLINE  256
#define MAXARGS  32

/* Folders searched for commands, in order. Drop a program in one of them
 * (or use the 'install' command) and it becomes a command. */
static const char *pathdirs[] = { "/bin", "/usr/bin", "/usr/local/bin", "/sbin", NULL };

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
                           "/usr/local/bin", "/tmp", "/etc", "/home", NULL };
    for (int i = 0; dirs[i]; i++) mkdir(dirs[i], 0755);
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

/* ---------- helpers ---------- */

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int find_in_path(const char *name, char *out, size_t size) {
    for (int i = 0; pathdirs[i]; i++) {
        snprintf(out, size, "%s/%s", pathdirs[i], name);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && access(out, X_OK) == 0) return 1;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) { close(in); return -1; }
    char buf[4096];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        char *p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w < 0) { rc = -1; goto done; }
            p += w; n -= w;
        }
    }
    if (n < 0) rc = -1;
done:
    close(in); close(out);
    return rc;
}

/* ---------- builtin commands ---------- */

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("Files and folders:");
    puts("  ls [DIR]          list names          dir [DIR]   list with sizes");
    puts("  cat FILE          print a file        edit FILE   open the text editor");
    puts("  mkdir DIR         make a folder       rmdir DIR   remove an empty folder");
    puts("  rm FILE (or del)  delete a file       cp SRC DST  copy a file");
    puts("  cd DIR / pwd      change / show folder");
    puts("System:");
    puts("  mem  uptime  ver  clear  echo TEXT  reboot  poweroff");
    puts("Programs and commands:");
    puts("  commands          list every available command");
    puts("  which NAME        show where a command comes from");
    puts("  install FILE [NAME]   add a program to /bin as a new command");
    puts("Any program in /bin, /usr/bin, /usr/local/bin or /sbin runs by name.");
    return 0;
}

static int cmd_ver(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("ALXA OS " VERSION " - made by alxa");
    return 0;
}

static int cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        fputs(argv[i], stdout);
    }
    putchar('\n');
    return 0;
}

static int cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    fputs("\033[2J\033[H", stdout);
    return 0;
}

static int cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) { printf("ls: %s: %s\n", path, strerror(errno)); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        printf("%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
    }
    closedir(d);
    return 0;
}

static int cmd_dir(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) { printf("dir: %s: %s\n", path, strerror(errno)); return 1; }
    struct dirent *e;
    int files = 0, dirs = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[512];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("  <DIR>      %s\n", e->d_name); dirs++;
        } else if (stat(full, &st) == 0) {
            printf("  %9lld  %s\n", (long long)st.st_size, e->d_name); files++;
        } else {
            printf("  ?          %s\n", e->d_name);
        }
    }
    closedir(d);
    printf("  %d file(s), %d folder(s)\n", files, dirs);
    return 0;
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) { puts("usage: cat FILE"); return 1; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { printf("cat: %s: %s\n", argv[1], strerror(errno)); return 1; }
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, stdout);
    close(fd);
    return 0;
}

static int cmd_edit(int argc, char **argv) {
    if (argc < 2) { puts("usage: edit FILE"); return 1; }
    return editor_run(argv[1]);
}

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { puts("usage: mkdir DIR"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (mkdir(argv[i], 0755) < 0) { printf("mkdir: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
    return rc;
}

static int cmd_rmdir(int argc, char **argv) {
    if (argc < 2) { puts("usage: rmdir DIR"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) { printf("rmdir: %s: %s\n", argv[i], strerror(errno)); rc = 1; }
    return rc;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s FILE...\n", argv[0]); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) < 0) {
            if (errno == EISDIR || (errno == EPERM && is_dir(argv[i])))
                printf("%s: %s is a folder (use rmdir)\n", argv[0], argv[i]);
            else
                printf("%s: %s: %s\n", argv[0], argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

static int cmd_cp(int argc, char **argv) {
    if (argc < 3) { puts("usage: cp SRC DST"); return 1; }
    struct stat st;
    if (stat(argv[1], &st) < 0 || !S_ISREG(st.st_mode)) {
        printf("cp: %s: not a regular file\n", argv[1]); return 1;
    }
    char dst[512];
    if (is_dir(argv[2])) snprintf(dst, sizeof dst, "%s/%s", argv[2], base_name(argv[1]));
    else snprintf(dst, sizeof dst, "%s", argv[2]);
    if (copy_file(argv[1], dst, st.st_mode & 0777) < 0) {
        printf("cp: %s\n", strerror(errno)); return 1;
    }
    return 0;
}

static int cmd_cd(int argc, char **argv) {
    const char *p = argc > 1 ? argv[1] : "/";
    if (chdir(p) < 0) { printf("cd: %s: %s\n", p, strerror(errno)); return 1; }
    return 0;
}

static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[256];
    if (getcwd(buf, sizeof buf)) puts(buf);
    return 0;
}

static int cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sysinfo si;
    if (sysinfo(&si) < 0) { puts("mem: failed"); return 1; }
    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    printf("total: %lu KB\nfree:  %lu KB\n",
           (si.totalram * unit) / 1024, (si.freeram * unit) / 1024);
    return 0;
}

static int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sysinfo si;
    if (sysinfo(&si) < 0) { puts("uptime: failed"); return 1; }
    printf("up %ld seconds\n", si.uptime);
    return 0;
}

static int cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    sync();
    reboot(RB_AUTOBOOT);
    puts("reboot: failed");
    return 1;
}

static int cmd_poweroff(int argc, char **argv) {
    (void)argc; (void)argv;
    sync();
    reboot(RB_POWER_OFF);
    puts("poweroff: not supported by this kernel build (close the emulator instead)");
    return 1;
}

struct builtin { const char *name; int (*fn)(int, char **); };
static int cmd_commands(int, char **);
static int cmd_which(int, char **);
static int cmd_install(int, char **);

static const struct builtin builtins[] = {
    {"help", cmd_help}, {"ver", cmd_ver}, {"echo", cmd_echo}, {"clear", cmd_clear},
    {"ls", cmd_ls}, {"dir", cmd_dir}, {"cat", cmd_cat}, {"edit", cmd_edit},
    {"mkdir", cmd_mkdir}, {"rmdir", cmd_rmdir}, {"rm", cmd_rm}, {"del", cmd_rm},
    {"cp", cmd_cp}, {"cd", cmd_cd}, {"pwd", cmd_pwd},
    {"mem", cmd_mem}, {"uptime", cmd_uptime},
    {"commands", cmd_commands}, {"which", cmd_which}, {"install", cmd_install},
    {"reboot", cmd_reboot}, {"poweroff", cmd_poweroff},
    {NULL, NULL}
};

static const struct builtin *find_builtin(const char *name) {
    for (const struct builtin *b = builtins; b->name; b++)
        if (!strcmp(name, b->name)) return b;
    return NULL;
}

/* ---------- command registry (PATH based) ---------- */

static int cmd_commands(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("Built-in commands:");
    for (const struct builtin *b = builtins; b->name; b++) printf("  %s\n", b->name);
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
    if (argc < 2) { puts("usage: which NAME"); return 1; }
    char path[256];
    if (find_builtin(argv[1])) { printf("%s: built-in\n", argv[1]); return 0; }
    if (find_in_path(argv[1], path, sizeof path)) { puts(path); return 0; }
    printf("%s: not found\n", argv[1]);
    return 1;
}

static int cmd_install(int argc, char **argv) {
    if (argc < 2) { puts("usage: install FILE [NAME]"); return 1; }
    struct stat st;
    if (stat(argv[1], &st) < 0 || !S_ISREG(st.st_mode)) {
        printf("install: %s: not a regular file\n", argv[1]); return 1;
    }
    const char *name = argc > 2 ? argv[2] : base_name(argv[1]);
    if (find_builtin(name)) { printf("install: '%s' is a built-in command\n", name); return 1; }

    unsigned char magic[4] = {0};
    int fd = open(argv[1], O_RDONLY);
    if (fd >= 0) { if (read(fd, magic, 4) < 0) {} close(fd); }
    if (!(magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F'))
        puts("install: warning: not a compiled (ELF) program, it may not run");

    char dst[300];
    mkdir("/bin", 0755);
    snprintf(dst, sizeof dst, "/bin/%s", name);
    if (copy_file(argv[1], dst, 0755) < 0) { printf("install: %s\n", strerror(errno)); return 1; }
    printf("installed: type '%s' to run it\n", name);
    return 0;
}

/* ---------- running external programs ---------- */

static void run_external(char **argv) {
    char path[300];
    if (strchr(argv[0], '/')) snprintf(path, sizeof path, "%s", argv[0]);
    else if (!find_in_path(argv[0], path, sizeof path)) {
        printf("%s: command not found\n", argv[0]);
        return;
    }
    pid_t pid = fork();
    if (pid < 0) { puts("fork failed"); return; }
    if (pid == 0) {
        execv(path, argv);
        printf("%s: cannot run: %s\n", argv[0], strerror(errno));
        fflush(stdout);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
}

/* ---------- main loop ---------- */

int main(void) {
    char line[MAXLINE];
    char *argv[MAXARGS];

    if (getpid() == 1) setup_system();
    banner();

    for (;;) {
        while (waitpid(-1, NULL, WNOHANG) > 0) ;   /* reap orphans (PID 1 duty) */

        fputs("alxa> ", stdout);
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) {
            clearerr(stdin);
            pause();                    /* no input available: wait, never exit */
            continue;
        }

        int argc = 0;
        for (char *t = strtok(line, " \t\r\n"); t && argc < MAXARGS - 1;
             t = strtok(NULL, " \t\r\n"))
            argv[argc++] = t;
        argv[argc] = NULL;
        if (argc == 0) continue;

        const struct builtin *b = find_builtin(argv[0]);
        if (b) b->fn(argc, argv);
        else run_external(argv);
    }
}
