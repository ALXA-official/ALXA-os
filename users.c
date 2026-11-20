/*
 * ALXA OS - accounts, password hashing and the passwd helper.
 * Original code by alxa, MIT licensed.
 *
 * Files:  <etc>/passwd  (public)   name:uid:gid:home
 *         <etc>/shadow  (root only) name:hash
 * Hashes are salted SHA-512 made by the C library's crypt().
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/random.h>
#include <crypt.h>
#include "users.h"
#include "cmds.h"

#ifndef ALXA_ETC_DIR
#define ALXA_ETC_DIR "/etc"
#endif
#ifndef ALXA_DATA_DIR
#define ALXA_DATA_DIR "/data"
#endif

static char etc_dir[160] = ALXA_ETC_DIR;
static char homebase[160] = "/home";
static struct user db[MAXUSERS];
static int ndb;

void users_set_dir(const char *d) { snprintf(etc_dir, sizeof etc_dir, "%s", d); }
void users_set_homebase(const char *d) { snprintf(homebase, sizeof homebase, "%s", d); }
const char *users_homebase(void) { return homebase; }

void users_autodetect_dir(void) {
    char p[256];
    snprintf(p, sizeof p, "%s/etc/passwd", ALXA_DATA_DIR);
    if (access(p, F_OK) == 0) {
        snprintf(etc_dir, sizeof etc_dir, "%s/etc", ALXA_DATA_DIR);
        snprintf(homebase, sizeof homebase, "%s/home", ALXA_DATA_DIR);
    }
}

static void fpath(char *out, size_t n, const char *file) { snprintf(out, n, "%s/%s", etc_dir, file); }

int users_load(void) {
    char p[256], line[512];
    ndb = 0;
    fpath(p, sizeof p, "passwd");
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f) && ndb < MAXUSERS) {
        unsigned uid, gid;
        char name[32], home[96];
        if (sscanf(line, "%31[^:]:%u:%u:%95[^:\n]", name, &uid, &gid, home) != 4) continue;
        struct user *u = &db[ndb++];
        memset(u, 0, sizeof *u);
        snprintf(u->name, sizeof u->name, "%s", name);
        snprintf(u->home, sizeof u->home, "%s", home);
        u->uid = uid; u->gid = gid;
    }
    fclose(f);
    fpath(p, sizeof p, "shadow");
    f = fopen(p, "r");                      /* only root can read this */
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char name[32], hash[128];
            if (sscanf(line, "%31[^:]:%127[^:\n]", name, hash) != 2) continue;
            for (int i = 0; i < ndb; i++)
                if (!strcmp(db[i].name, name)) snprintf(db[i].hash, sizeof db[i].hash, "%s", hash);
        }
        fclose(f);
    }
    return ndb;
}

static int save_file(const char *name, mode_t mode, int shadow) {
    char p[256], t[272];
    fpath(p, sizeof p, name);
    snprintf(t, sizeof t, "%s.tmp", p);
    int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    fchmod(fd, mode);
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return -1; }
    for (int i = 0; i < ndb; i++) {
        if (shadow) fprintf(f, "%s:%s\n", db[i].name, db[i].hash[0] ? db[i].hash : "!");
        else fprintf(f, "%s:%u:%u:%s\n", db[i].name, (unsigned)db[i].uid, (unsigned)db[i].gid, db[i].home);
    }
    fflush(f);
    fsync(fd);
    if (fclose(f) != 0) return -1;
    return rename(t, p);
}

int users_save(void) {
    mkdir(etc_dir, 0755);
    if (save_file("shadow", 0600, 1) < 0) return -1;
    return save_file("passwd", 0644, 0);
}

int users_exist(void) { return ndb > 0; }

const struct user *user_find(const char *name) {
    for (int i = 0; i < ndb; i++) if (!strcmp(db[i].name, name)) return &db[i];
    return NULL;
}
const struct user *user_by_uid(uid_t uid) {
    for (int i = 0; i < ndb; i++) if (db[i].uid == uid) return &db[i];
    return NULL;
}
const struct user *user_by_gid(gid_t gid) {
    for (int i = 0; i < ndb; i++) if (db[i].gid == gid) return &db[i];
    return NULL;
}

int user_valid_name(const char *n) {
    size_t l = strlen(n);
    if (l == 0 || l > 31) return 0;
    if (!(islower((unsigned char)n[0]) || n[0] == '_')) return 0;
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)n[i];
        if (!(islower(c) || isdigit(c) || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static int ct_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    unsigned char d = 0;
    for (size_t i = 0; i < la; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

static int random_bytes(unsigned char *buf, size_t n) {
    if (getrandom(buf, n, 0) == (ssize_t)n) return 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n);
    close(fd);
    return r == (ssize_t)n ? 0 : -1;
}

static int make_hash(const char *pw, char *out, size_t size) {
    static const char cs[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char r[16];
    if (random_bytes(r, sizeof r) < 0) return -1;
    char setting[32] = "$6$";
    for (int i = 0; i < 16; i++) setting[3 + i] = cs[r[i] & 63];
    setting[19] = '$'; setting[20] = 0;
    char *h = crypt(pw, setting);
    if (!h || h[0] == '*' || strlen(h) + 1 > size) return -1;
    snprintf(out, size, "%s", h);
    return 0;
}

int user_verify(const struct user *u, const char *pw) {
    static const char dummy[] = "$6$0000000000000000$";
    int real = u && u->hash[0] == '$';
    const char *hash = real ? u->hash : dummy;      /* unknown user: still do the work */
    char *h = crypt(pw, hash);
    if (!h || h[0] == '*') return 0;
    int same = ct_equal(h, hash);
    return real && same;
}

uid_t users_next_uid(void) {
    uid_t m = 999;
    for (int i = 0; i < ndb; i++) if (db[i].uid > m) m = db[i].uid;
    return m + 1;
}

int user_add(const char *name, const char *pw, uid_t uid, gid_t gid, const char *home) {
    if (ndb >= MAXUSERS || !user_valid_name(name) || user_find(name)) return -1;
    struct user *u = &db[ndb];
    memset(u, 0, sizeof *u);
    snprintf(u->name, sizeof u->name, "%s", name);
    snprintf(u->home, sizeof u->home, "%s", home);
    u->uid = uid; u->gid = gid;
    if (make_hash(pw, u->hash, sizeof u->hash) < 0) return -1;
    ndb++;
    return 0;
}

int user_set_password(const char *name, const char *pw) {
    for (int i = 0; i < ndb; i++)
        if (!strcmp(db[i].name, name)) return make_hash(pw, db[i].hash, sizeof db[i].hash);
    return -1;
}

int user_remove(const char *name) {
    for (int i = 0; i < ndb; i++)
        if (!strcmp(db[i].name, name)) {
            memmove(&db[i], &db[i + 1], (size_t)(ndb - i - 1) * sizeof db[0]);
            ndb--;
            return 0;
        }
    return -1;
}

int user_make_home(const char *home, uid_t uid, gid_t gid) {
    if (mkdir_p(home) < 0) return -1;
    if (chown(home, uid, gid) < 0) return -1;
    return chmod(home, 0700);
}

int password_policy_ok(const char *pw) {
    if (strlen(pw) < 8) { fputs("Password must be at least 8 characters.\n", stderr); return 0; }
    return 1;
}

/* ---------- terminal input ---------- */

int read_line(const char *prompt, char *buf, size_t size, int echo) {
    struct termios o, n;
    int tty = tcgetattr(0, &o) == 0;
    fputs(prompt, stdout);
    fflush(stdout);
    if (tty && !echo) { n = o; n.c_lflag &= ~(tcflag_t)ECHO; tcsetattr(0, TCSADRAIN, &n); }
    ssize_t r = read(0, buf, size - 1);
    if (tty && !echo) { tcsetattr(0, TCSADRAIN, &o); fputs("\n", stdout); fflush(stdout); }
    if (r <= 0) { buf[0] = 0; return -1; }
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r')) buf[--r] = 0;
    return (int)r;
}

int read_password(const char *prompt, char *buf, size_t size) {
    return read_line(prompt, buf, size, 0);
}

int ask_new_password(char *out, size_t size) {
    char a[128], b[128];
    for (int tries = 0; tries < 5; tries++) {
        if (read_password("New password: ", a, sizeof a) < 0) return -1;
        if (!password_policy_ok(a)) continue;
        if (read_password("Retype password: ", b, sizeof b) < 0) return -1;
        if (strcmp(a, b)) { fputs("Passwords do not match.\n", stderr); continue; }
        snprintf(out, size, "%s", a);
        memset(a, 0, sizeof a); memset(b, 0, sizeof b);
        return 0;
    }
    return -1;
}

/* ---------- passwd helper (runs setuid root as /sbin/passwd) ---------- */

int passwd_main(int argc, char **argv) {
    uid_t ruid = getuid();
    clearenv();                                 /* never trust the caller's environment */
    users_autodetect_dir();
    umask(077);
    if (users_load() == 0) { fputs("passwd: no accounts found\n", stderr); return 1; }
    const struct user *me = user_by_uid(ruid);
    const char *target;
    if (argc > 1) {
        if (ruid != 0 && (!me || strcmp(argv[1], me->name))) {
            fputs("passwd: you may only change your own password\n", stderr);
            return 1;
        }
        target = argv[1];
    } else {
        if (!me) { fputs("passwd: unknown user\n", stderr); return 1; }
        target = me->name;
    }
    const struct user *u = user_find(target);
    if (!u) { fprintf(stderr, "passwd: no such user: %s\n", target); return 1; }
    if (ruid != 0) {
        char old[128];
        if (read_password("Current password: ", old, sizeof old) < 0) return 1;
        int ok = user_verify(u, old);
        memset(old, 0, sizeof old);
        if (!ok) { sleep(2); fputs("passwd: incorrect password\n", stderr); return 1; }
    }
    char pw[128];
    if (ask_new_password(pw, sizeof pw) < 0) { fputs("passwd: password not changed\n", stderr); return 1; }
    int rc = user_set_password(target, pw) == 0 && users_save() == 0 ? 0 : 1;
    memset(pw, 0, sizeof pw);
    if (rc) fputs("passwd: could not save\n", stderr);
    else puts("Password updated.");
    return rc;
}

/* ---------- admin commands ---------- */

int cmd_adduser(int argc, char **argv) {
    if (getuid() != 0) { fputs("adduser: only root can do this\n", stderr); return 1; }
    if (argc != 2) { fputs("usage: adduser NAME\n", stderr); return 1; }
    users_load();
    if (!user_valid_name(argv[1])) { fputs("adduser: name must be lowercase letters, digits, - or _\n", stderr); return 1; }
    if (user_find(argv[1])) { fputs("adduser: that user already exists\n", stderr); return 1; }
    char pw[128], home[96];
    if ((size_t)snprintf(home, sizeof home, "%s/%s", users_homebase(), argv[1]) >= sizeof home) {
        fputs("adduser: home path too long\n", stderr);
        return 1;
    }
    if (ask_new_password(pw, sizeof pw) < 0) { fputs("adduser: cancelled\n", stderr); return 1; }
    uid_t uid = users_next_uid();
    if (user_add(argv[1], pw, uid, uid, home) < 0 || users_save() < 0) {
        memset(pw, 0, sizeof pw);
        fputs("adduser: failed\n", stderr);
        return 1;
    }
    memset(pw, 0, sizeof pw);
    if (user_make_home(home, uid, uid) < 0) fprintf(stderr, "adduser: warning: could not create %s\n", home);
    printf("User '%s' created (uid %u).\n", argv[1], (unsigned)uid);
    return 0;
}

int cmd_deluser(int argc, char **argv) {
    if (getuid() != 0) { fputs("deluser: only root can do this\n", stderr); return 1; }
    int rmhome = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-r")) { rmhome = 1; i++; }
    if (argc - i != 1) { fputs("usage: deluser [-r] NAME   (-r also deletes the home folder)\n", stderr); return 1; }
    users_load();
    const struct user *u = user_find(argv[i]);
    if (!u) { fputs("deluser: no such user\n", stderr); return 1; }
    if (u->uid == 0) { fputs("deluser: refusing to delete the root account\n", stderr); return 1; }
    char home[96];
    snprintf(home, sizeof home, "%s", u->home);
    if (user_remove(argv[i]) < 0 || users_save() < 0) { fputs("deluser: failed\n", stderr); return 1; }
    if (rmhome) remove_tree(home);
    puts("User deleted.");
    return 0;
}

int cmd_users(int argc, char **argv) {
    (void)argc; (void)argv;
    users_load();
    for (int i = 0; i < ndb; i++) printf("%-16s uid=%-6u home=%s\n", db[i].name, (unsigned)db[i].uid, db[i].home);
    return 0;
}
