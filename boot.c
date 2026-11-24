/*
 * Magnesium OS - init (PID 1): system setup, disk, login and sessions.
 * Original code by alxa, MIT licensed.
 *
 * This one program is several things, chosen by the name it is started as:
 *   /init            the init system and login prompt
 *   /sbin/passwd     the setuid helper that lets users change their password
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <grp.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include "cmds.h"
#include "users.h"
#include "shell.h"
#include "version.h"
#include "registry.h"

#ifndef MG_ETC_DIR
#define MG_ETC_DIR "/etc"
#endif
#ifndef MG_HOME_DIR
#define MG_HOME_DIR "/home"
#endif

static int persistent;           /* 1 when the data disk is mounted on /data */

static void mount_disk(void) {
    mkdir("/data", 0755);
    const char *devs[] = { "/dev/vda", "/dev/sda", "/dev/vdb", NULL };
    int seen = 0, err = 0;
    for (int attempt = 0; attempt < 5 && !persistent; attempt++) {
        for (int i = 0; devs[i] && !persistent; i++) {
            if (access(devs[i], F_OK) != 0) continue;
            seen = 1;
            if (mount(devs[i], "/data", "ext2", MS_NOATIME | MS_NODEV, NULL) == 0) {
                persistent = 1;
                puts("Disk mounted on /data (your files and accounts are saved).");
            } else err = errno;
        }
        if (seen) break;
        usleep(100000);
    }
    if (seen && !persistent)
        printf("A disk was found but could not be mounted as ext2 (%s). Is it formatted?\n", strerror(err));
    if (persistent) {
        if (chown("/data", 0, 0) < 0) { /* keep going */ }
        chmod("/data", 0755);
        mkdir("/data/etc", 0755);
        mkdir("/data/home", 0755);
        mkdir("/data/bin", 0755);
    }
}

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
    chmod("/tmp", 01777);                            /* everyone can write, only owners can delete */
    mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount_disk();
    setenv("PATH", "/bin:/usr/bin:/usr/local/bin:/sbin:/data/bin", 1);
    if (!getenv("TERM")) setenv("TERM", "linux", 1);
}

static void reset_console(void) {
    struct termios t;
    if (tcgetattr(0, &t) < 0) return;
    t.c_iflag |= ICRNL;
    t.c_oflag |= OPOST | ONLCR;
    t.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;
    tcsetattr(0, TCSAFLUSH, &t);
}

/* ---------- first start ---------- */

static void first_start(void) {
    char name[64], home[96];
    puts("\nFirst start: no accounts exist yet.");
    puts("Create the administrator account (called 'root'). Passwords need 8+ characters.\n");
    char pw[128];
    if (ask_new_password(pw, sizeof pw) < 0) { sleep(2); return; }
    snprintf(home, sizeof home, "%s/root", users_homebase());
    if (user_add("root", pw, 0, 0, home) < 0) { puts("Could not create the account."); sleep(2); return; }
    user_make_home(home, 0, 0);
    memset(pw, 0, sizeof pw);

    puts("\nRecommended: also create a regular user for everyday work.");
    if (read_line("Username (press Enter to skip): ", name, sizeof name, 1) > 0) {
        if (!user_valid_name(name)) puts("Invalid name (use lowercase letters, digits, - or _); skipped.");
        else if (ask_new_password(pw, sizeof pw) == 0) {
            uid_t uid = users_next_uid();
            snprintf(home, sizeof home, "%s/%s", users_homebase(), name);
            if (user_add(name, pw, uid, uid, home) == 0) user_make_home(home, uid, uid);
        }
        memset(pw, 0, sizeof pw);
    }
    if (users_save() < 0) puts("Warning: could not save the accounts.");
    else if (!persistent) puts("\nNote: no disk is attached, so accounts are lost when you reboot (see README).");
    puts("");
}

/* ---------- login and sessions ---------- */

static void run_session(const struct user *u) {
    struct user me = *u;                  /* copy: the table may be reloaded */
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { puts("fork failed"); return; }
    if (pid == 0) {
        setsid();
        ioctl(0, TIOCSCTTY, 1);           /* make the console this session's terminal */
        if (setgroups(0, NULL) < 0 && geteuid() == 0) _exit(1);
        if (setgid(me.gid) < 0 || setuid(me.uid) < 0) _exit(1);
        if (getuid() != me.uid || geteuid() != me.uid) _exit(1);   /* never keep privileges by mistake */
        const char *term = getenv("TERM");
        char termcopy[64] = "linux";
        if (term) snprintf(termcopy, sizeof termcopy, "%s", term);
        clearenv();
        setenv("PATH", "/bin:/usr/bin:/usr/local/bin:/sbin:/data/bin", 1);
        setenv("HOME", me.home, 1);
        setenv("USER", me.name, 1);
        setenv("LOGNAME", me.name, 1);
        setenv("TERM", termcopy, 1);
        { char up[200]; snprintf(up, sizeof up, "%s/.mg-registry", me.home); reg_set_paths(NULL, up); }
        reg_apply_env();
        umask(022);
        if (chdir(me.home) < 0 && chdir("/") < 0) _exit(1);
        printf("\nWelcome, %s. Type 'help' for commands.\n\n", me.name);
        shell_session();
        _exit(0);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) ;
    reset_console();
}

static void login_cycle(void) {
    static int failures;
    while (waitpid(-1, NULL, WNOHANG) > 0) ;         /* reap orphans (PID 1 duty) */
    users_load();
    reg_refresh();                                   /* settings may have changed in a session */
    if (!users_exist()) { first_start(); return; }
    char name[64], pw[128];
    if (read_line(OS_SHORT " login: ", name, sizeof name, 1) < 0) { sleep(1); return; }
    if (!name[0]) return;
    if (read_password("Password: ", pw, sizeof pw) < 0) { sleep(1); return; }
    const struct user *u = user_find(name);
    int ok = user_verify(u, pw);
    memset(pw, 0, sizeof pw);
    if (!ok) {
        failures++;
        int base = atoi(reg_get(HIVE_SYSTEM, "Security/LoginDelay", "2"));
        if (base < 0) base = 0;
        if (base > 30) base = 30;
        sleep((unsigned)(failures < 3 ? base : base * 2 + 1));
        puts("Login incorrect\n");
        return;
    }
    failures = 0;
    run_session(u);
}

static void init_loop(void) {
    users_set_dir(persistent ? "/data/etc" : MG_ETC_DIR);
    users_set_homebase(persistent ? "/data/home" : MG_HOME_DIR);
    {
        char rp[200];
        snprintf(rp, sizeof rp, "%s/registry", persistent ? "/data/etc" : MG_ETC_DIR);
        reg_set_paths(rp, "");               /* system settings and variables */
    }
    puts("");
    puts("  " OS_NAME " " OS_VERSION_LONG);
    puts("  Made by " OS_AUTHOR " - Linux kernel + Magnesium shell");
    puts("");
    for (;;) login_cycle();
}

int main(int argc, char **argv) {
    const char *prog = base_name(argv[0]);
    if (geteuid() != getuid()) {                     /* started setuid: only passwd is allowed */
        if (strcmp(prog, "passwd")) { fputs("not permitted\n", stderr); return 1; }
        return passwd_main(argc, argv);
    }
    if (!strcmp(prog, "passwd")) return passwd_main(argc, argv);

    int pid1 = getpid() == 1;
    if (pid1) setup_system();
    if (pid1 || (argc > 1 && !strcmp(argv[1], "--init"))) {
        init_loop();
        return 0;
    }
    shell_session();                                 /* development mode: no login */
    return 0;
}
