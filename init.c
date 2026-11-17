/*
 * ALXA OS - init + shell (alxa shell, "ash")
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

#define VERSION  "0.2"
#define MAXLINE  256
#define MAXARGS  32

static void setup_console(void) {
    /* Make sure /dev exists and stdin/stdout/stderr are usable. */
    mkdir("/dev", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);   /* may already be mounted */
    if (fcntl(0, F_GETFD) == -1) {
        int fd = open("/dev/console", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
        }
    }
}

static void banner(void) {
    puts("");
    puts("  ALXA OS " VERSION " - made by alxa");
    puts("  Linux kernel + alxa shell. Type 'help' for commands.");
    puts("");
}

/* ---------- builtin commands ---------- */

static int cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("Commands:");
    puts("  help            show this help");
    puts("  ver             show ALXA OS version");
    puts("  echo TEXT       print text");
    puts("  clear           clear the screen");
    puts("  ls [DIR]        list a directory");
    puts("  cat FILE        print a file");
    puts("  mkdir DIR       create a directory");
    puts("  cd DIR / pwd    change / show directory");
    puts("  mem             show memory usage");
    puts("  uptime          show time since boot");
    puts("  reboot          restart the machine");
    puts("  poweroff        shut down (may need ACPI support)");
    puts("Any program placed in /bin can also be run by name.");
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

static int cmd_mkdir(int argc, char **argv) {
    if (argc < 2) { puts("usage: mkdir DIR"); return 1; }
    if (mkdir(argv[1], 0755) < 0) { printf("mkdir: %s: %s\n", argv[1], strerror(errno)); return 1; }
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
static const struct builtin builtins[] = {
    {"help", cmd_help}, {"ver", cmd_ver}, {"echo", cmd_echo},
    {"clear", cmd_clear}, {"ls", cmd_ls}, {"cat", cmd_cat},
    {"mkdir", cmd_mkdir}, {"cd", cmd_cd}, {"pwd", cmd_pwd},
    {"mem", cmd_mem}, {"uptime", cmd_uptime},
    {"reboot", cmd_reboot}, {"poweroff", cmd_poweroff},
    {NULL, NULL}
};

/* ---------- running external programs ---------- */

static void run_external(char **argv) {
    pid_t pid = fork();
    if (pid < 0) { puts("fork failed"); return; }
    if (pid == 0) {
        if (strchr(argv[0], '/')) {
            execv(argv[0], argv);
        } else {
            char path[128];
            snprintf(path, sizeof path, "/bin/%s", argv[0]);
            execv(path, argv);
        }
        printf("%s: command not found\n", argv[0]);
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

    if (getpid() == 1) setup_console();
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

        int handled = 0;
        for (const struct builtin *b = builtins; b->name; b++) {
            if (!strcmp(argv[0], b->name)) { b->fn(argc, argv); handled = 1; break; }
        }
        if (!handled) run_external(argv);
    }
}
