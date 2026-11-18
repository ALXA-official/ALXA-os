/*
 * ALXA OS - command set (files, text, system tools).
 * Original implementations written by alxa, MIT licensed.
 * Not derived from BusyBox or any other GPL code.
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
#include <time.h>
#include <signal.h>
#include <regex.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/klog.h>
#include <sys/reboot.h>
#include "cmds.h"

extern char **environ;

#define C_BLUE  "\033[1;34m"
#define C_GREEN "\033[1;32m"
#define C_CYAN  "\033[36m"
#define C_RESET "\033[0m"

/* ================= shared helpers ================= */

const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void err(const char *cmd, const char *what, int e) {
    fprintf(stderr, "%s: %s: %s\n", cmd, what, strerror(e));
}

int copy_file(const char *src, const char *dst, mode_t mode) {
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

int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst, st.st_mode & 0777);
    size_t l = strlen(src);
    if (!strncmp(dst, src, l) && (dst[l] == '/' || dst[l] == 0)) { errno = EINVAL; return -1; }
    if (mkdir(dst, st.st_mode & 0777) < 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[1024], t[1024];
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        if (copy_tree(s, t) < 0) rc = -1;
    }
    closedir(d);
    return rc;
}

int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (remove_tree(full) < 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) < 0) rc = -1;
    return rc;
}

/* Run fn on each named file, or on stdin when no files are given. */
typedef int (*input_fn)(FILE *f, const char *name, void *ctx);
static int each_input(int argc, char **argv, int first, input_fn fn, void *ctx) {
    int rc = 0;
    if (first >= argc) return fn(stdin, "-", ctx);
    for (int i = first; i < argc; i++) {
        FILE *f = strcmp(argv[i], "-") ? fopen(argv[i], "r") : stdin;
        if (!f) { err(argv[0], argv[i], errno); rc = 1; continue; }
        if (fn(f, argv[i], ctx)) rc = 1;
        if (f != stdin) fclose(f);
    }
    return rc;
}

static void strip_nl(char *l) {
    size_t n = strlen(l);
    if (n && l[n - 1] == '\n') l[n - 1] = 0;
}

static void human(char *out, size_t sz, unsigned long long b) {
    const char *u = "BKMGT";
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && u[i + 1]) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, sz, "%llu", b);
    else snprintf(out, sz, "%.1f%c", v, u[i]);
}

/* ================= listing and basic files ================= */

struct ent { char *name; mode_t mode; off_t size; };

static int cmp_ent(const void *a, const void *b) {
    return strcmp(((const struct ent *)a)->name, ((const struct ent *)b)->name);
}

static void mode_str(mode_t m, char *s) {
    s[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) s[i + 1] = (m & (1 << (8 - i))) ? rwx[i] : '-';
    s[10] = 0;
}

static void print_ent(const struct ent *e, int lng, int color) {
    if (lng) {
        char ms[12];
        mode_str(e->mode, ms);
        printf("%s %9lld ", ms, (long long)e->size);
    }
    const char *col = NULL;
    if (color) {
        if (S_ISDIR(e->mode)) col = C_BLUE;
        else if (e->mode & 0111) col = C_GREEN;
    }
    if (col) fputs(col, stdout);
    fputs(e->name, stdout);
    if (col) fputs(C_RESET, stdout);
    if (S_ISDIR(e->mode)) putchar('/');
    putchar('\n');
}

static int ls_one(const char *cmd, const char *path, int all, int lng, int header) {
    struct stat st;
    int color = isatty(1);
    if (stat(path, &st) < 0) { err(cmd, path, errno); return 1; }
    if (!S_ISDIR(st.st_mode)) {
        struct ent e = { (char *)path, st.st_mode, st.st_size };
        print_ent(&e, lng, color);
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) { err(cmd, path, errno); return 1; }
    struct ent *v = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!all && de->d_name[0] == '.') continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; v = realloc(v, cap * sizeof *v); }
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, de->d_name);
        struct stat s2;
        if (stat(full, &s2) < 0) memset(&s2, 0, sizeof s2);
        v[n].name = strdup(de->d_name);
        v[n].mode = s2.st_mode;
        v[n].size = s2.st_size;
        n++;
    }
    closedir(d);
    qsort(v, n, sizeof *v, cmp_ent);
    if (header) printf("%s:\n", path);
    for (size_t i = 0; i < n; i++) { print_ent(&v[i], lng, color); free(v[i].name); }
    free(v);
    return 0;
}

static int do_ls(int argc, char **argv, int lng) {
    int all = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'a') all = 1;
            else if (*f == 'l') lng = 1;
            else if (*f == '1') ;
            else { fprintf(stderr, "%s: unknown option -%c\n", argv[0], *f); return 1; }
        }
    if (i >= argc) return ls_one(argv[0], ".", all, lng, 0);
    int rc = 0, multi = argc - i > 1;
    for (; i < argc; i++) {
        if (ls_one(argv[0], argv[i], all, lng, multi)) rc = 1;
        if (multi && i < argc - 1) putchar('\n');
    }
    return rc;
}
static int cmd_ls(int argc, char **argv) { return do_ls(argc, argv, 0); }

static int cmd_dir(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) { err("dir", path, errno); return 1; }
    struct dirent *e;
    int files = 0, dirs = 0;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1024];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) { printf("  <DIR>      %s\n", e->d_name); dirs++; }
        else if (stat(full, &st) == 0) { printf("  %9lld  %s\n", (long long)st.st_size, e->d_name); files++; }
        else printf("  ?          %s\n", e->d_name);
    }
    closedir(d);
    printf("  %d file(s), %d folder(s)\n", files, dirs);
    return 0;
}

struct catctx { int number; long line; };
static int cat_in(FILE *f, const char *name, void *c) {
    (void)name;
    struct catctx *x = c;
    if (!x->number) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
        return 0;
    }
    char *l = NULL; size_t cap = 0;
    while (getline(&l, &cap, f) > 0) printf("%6ld  %s", ++x->line, l);
    free(l);
    return 0;
}
static int cmd_cat(int argc, char **argv) {
    struct catctx x = {0, 0};
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-n")) { x.number = 1; i++; }
    return each_input(argc, argv, i, cat_in, &x);
}

static int cmd_echo(int argc, char **argv) {
    int nonl = 0, esc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *f = argv[i] + 1;
        while (*f == 'n' || *f == 'e') f++;
        if (*f) break;
        for (f = argv[i] + 1; *f; f++) { if (*f == 'n') nonl = 1; else esc = 1; }
    }
    for (; i < argc; i++) {
        const char *s = argv[i];
        for (; *s; s++) {
            if (esc && *s == '\\' && s[1]) {
                s++;
                switch (*s) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case 'e': putchar(27); break;
                case '\\': putchar('\\'); break;
                default: putchar('\\'); putchar(*s);
                }
            } else putchar(*s);
        }
        if (i < argc - 1) putchar(' ');
    }
    if (!nonl) putchar('\n');
    return 0;
}

static int mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int cmd_mkdir(int argc, char **argv) {
    int parents = 0, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-p")) { parents = 1; i++; }
    if (i >= argc) { fputs("usage: mkdir [-p] DIR...\n", stderr); return 1; }
    for (; i < argc; i++) {
        int r = parents ? mkdir_p(argv[i]) : mkdir(argv[i], 0755);
        if (r < 0) { err("mkdir", argv[i], errno); rc = 1; }
    }
    return rc;
}

static int cmd_rmdir(int argc, char **argv) {
    if (argc < 2) { fputs("usage: rmdir DIR...\n", stderr); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0) { err("rmdir", argv[i], errno); rc = 1; }
    return rc;
}

static int cmd_rm(int argc, char **argv) {
    int rec = 0, force = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R') rec = 1;
            else if (*f == 'f') force = 1;
            else { fprintf(stderr, "%s: unknown option -%c\n", argv[0], *f); return 1; }
        }
    if (i >= argc) { if (force) return 0; fprintf(stderr, "usage: %s [-rf] FILE...\n", argv[0]); return 1; }
    for (; i < argc; i++) {
        const char *p = argv[i];
        if (!strcmp(p, "/") || !strcmp(p, "/.") || !strcmp(p, "/..")) {
            fprintf(stderr, "%s: refusing to remove '/'\n", argv[0]); rc = 1; continue;
        }
        if (rec) {
            if (remove_tree(p) < 0 && !(force && errno == ENOENT)) { err(argv[0], p, errno); rc = 1; }
        } else if (unlink(p) < 0) {
            if (force && errno == ENOENT) continue;
            if (errno == EISDIR || (errno == EPERM && is_dir(p)))
                fprintf(stderr, "%s: %s is a folder (use rm -r or rmdir)\n", argv[0], p);
            else err(argv[0], p, errno);
            rc = 1;
        }
    }
    return rc;
}

static int cmd_cp(int argc, char **argv) {
    int rec = 0, i = 1, rc = 0;
    if (i < argc && (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-R"))) { rec = 1; i++; }
    if (argc - i < 2) { fputs("usage: cp [-r] SRC... DST\n", stderr); return 1; }
    const char *dst = argv[argc - 1];
    int dst_dir = is_dir(dst);
    if (argc - i > 2 && !dst_dir) { fprintf(stderr, "cp: %s is not a folder\n", dst); return 1; }
    for (; i < argc - 1; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) { err("cp", argv[i], errno); rc = 1; continue; }
        if (S_ISDIR(st.st_mode) && !rec) { fprintf(stderr, "cp: %s is a folder (use -r)\n", argv[i]); rc = 1; continue; }
        char target[1024];
        if (dst_dir) snprintf(target, sizeof target, "%s/%s", dst, base_name(argv[i]));
        else snprintf(target, sizeof target, "%s", dst);
        if (copy_tree(argv[i], target) < 0) { err("cp", argv[i], errno); rc = 1; }
    }
    return rc;
}

static int cmd_mv(int argc, char **argv) {
    if (argc < 3) { fputs("usage: mv SRC... DST\n", stderr); return 1; }
    const char *dst = argv[argc - 1];
    int dst_dir = is_dir(dst), rc = 0;
    if (argc > 3 && !dst_dir) { fprintf(stderr, "mv: %s is not a folder\n", dst); return 1; }
    for (int i = 1; i < argc - 1; i++) {
        char target[1024];
        if (dst_dir) snprintf(target, sizeof target, "%s/%s", dst, base_name(argv[i]));
        else snprintf(target, sizeof target, "%s", dst);
        if (rename(argv[i], target) < 0) {
            if (errno == EXDEV && copy_tree(argv[i], target) == 0 && remove_tree(argv[i]) == 0) continue;
            err("mv", argv[i], errno); rc = 1;
        }
    }
    return rc;
}

static int cmd_touch(int argc, char **argv) {
    if (argc < 2) { fputs("usage: touch FILE...\n", stderr); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (access(argv[i], F_OK) != 0) {
            int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
            if (fd < 0) { err("touch", argv[i], errno); rc = 1; } else close(fd);
        } else if (utimensat(AT_FDCWD, argv[i], NULL, 0) < 0) { err("touch", argv[i], errno); rc = 1; }
    }
    return rc;
}

static int cmd_ln(int argc, char **argv) {
    int sym = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-s")) { sym = 1; i++; }
    if (argc - i != 2) { fputs("usage: ln [-s] TARGET LINK\n", stderr); return 1; }
    if ((sym ? symlink(argv[i], argv[i + 1]) : link(argv[i], argv[i + 1])) < 0) { err("ln", argv[i + 1], errno); return 1; }
    return 0;
}

static int cmd_chmod(int argc, char **argv) {
    if (argc < 3) { fputs("usage: chmod MODE FILE...  (MODE: 755, +x, -w, u+x ...)\n", stderr); return 1; }
    const char *m = argv[1];
    int octal = 1;
    for (const char *p = m; *p; p++) if (*p < '0' || *p > '7') octal = 0;
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) { err("chmod", argv[i], errno); rc = 1; continue; }
        mode_t nm;
        if (octal) nm = (mode_t)strtol(m, NULL, 8);
        else {
            const char *p = m;
            int who = 0;
            for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++)
                who |= *p == 'u' ? 4 : *p == 'g' ? 2 : *p == 'o' ? 1 : 7;
            if (!who) who = 7;
            char op = *p;
            if (op != '+' && op != '-' && op != '=') { fprintf(stderr, "chmod: bad mode '%s'\n", m); return 1; }
            p++;
            int perm = 0;
            for (; *p; p++) {
                if (*p == 'r') perm |= 4; else if (*p == 'w') perm |= 2; else if (*p == 'x') perm |= 1;
                else { fprintf(stderr, "chmod: bad mode '%s'\n", m); return 1; }
            }
            mode_t mask = 0, whomask = 0;
            if (who & 4) { mask |= (mode_t)perm << 6; whomask |= 0700; }
            if (who & 2) { mask |= (mode_t)perm << 3; whomask |= 0070; }
            if (who & 1) { mask |= (mode_t)perm; whomask |= 0007; }
            mode_t old = st.st_mode & 07777;
            nm = op == '+' ? (old | mask) : op == '-' ? (old & ~mask) : ((old & ~whomask) | mask);
        }
        if (chmod(argv[i], nm) < 0) { err("chmod", argv[i], errno); rc = 1; }
    }
    return rc;
}

static int cmd_stat(int argc, char **argv) {
    if (argc < 2) { fputs("usage: stat FILE...\n", stderr); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) < 0) { err("stat", argv[i], errno); rc = 1; continue; }
        const char *type = S_ISDIR(st.st_mode) ? "folder" : S_ISLNK(st.st_mode) ? "symlink" :
                           S_ISREG(st.st_mode) ? "file" : S_ISCHR(st.st_mode) ? "char device" :
                           S_ISBLK(st.st_mode) ? "block device" : "other";
        char tb[64];
        time_t t = st.st_mtime;
        strftime(tb, sizeof tb, "%Y-%m-%d %H:%M:%S", localtime(&t));
        printf("  Name: %s\n  Type: %s\n  Size: %lld bytes\n  Mode: %04o\n  Owner: uid %u, gid %u\n  Links: %lu\n  Modified: %s\n",
               argv[i], type, (long long)st.st_size, st.st_mode & 07777, st.st_uid, st.st_gid,
               (unsigned long)st.st_nlink, tb);
    }
    return rc;
}

static long long du_walk(const char *path, int summary, int hum, int top) {
    struct stat st;
    if (lstat(path, &st) < 0) { err("du", path, errno); return 0; }
    long long total = st.st_size;
    int isdir = S_ISDIR(st.st_mode);
    if (isdir) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char full[1024];
                snprintf(full, sizeof full, "%s/%s", path, e->d_name);
                total += du_walk(full, summary, hum, 0);
            }
            closedir(d);
        }
    }
    if (top || (isdir && !summary)) {
        char hb[32];
        if (hum) { human(hb, sizeof hb, (unsigned long long)total); printf("%s\t%s\n", hb, path); }
        else printf("%lld\t%s\n", total, path);
    }
    return total;
}
static int cmd_du(int argc, char **argv) {
    int summary = 0, hum = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 's') summary = 1; else if (*f == 'h') hum = 1; }
    if (i >= argc) { du_walk(".", summary, hum, 1); return 0; }
    for (; i < argc; i++) du_walk(argv[i], summary, hum, 1);
    return 0;
}

static int cmd_df(int argc, char **argv) {
    printf("%-16s %10s %10s %10s %5s\n", "Mounted on", "Total KB", "Used KB", "Free KB", "Use%");
    int rc = 0;
    for (int i = (argc > 1 ? 1 : 0); i < (argc > 1 ? argc : 1); i++) {
        const char *p = argc > 1 ? argv[i] : "/";
        struct statvfs s;
        if (statvfs(p, &s) < 0) { err("df", p, errno); rc = 1; continue; }
        unsigned long long tot = (unsigned long long)s.f_blocks * s.f_frsize / 1024;
        unsigned long long fre = (unsigned long long)s.f_bfree * s.f_frsize / 1024;
        unsigned long long used = tot - fre;
        printf("%-16s %10llu %10llu %10llu %4llu%%\n", p, tot, used, fre, tot ? used * 100 / tot : 0);
    }
    return rc;
}

static int cmd_basename(int argc, char **argv) {
    if (argc < 2) { fputs("usage: basename PATH [SUFFIX]\n", stderr); return 1; }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", argv[1]);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    const char *b = n == 1 && tmp[0] == '/' ? "/" : base_name(tmp);
    char out[1024];
    snprintf(out, sizeof out, "%s", b);
    if (argc > 2) {
        size_t bl = strlen(out), sl = strlen(argv[2]);
        if (sl < bl && !strcmp(out + bl - sl, argv[2])) out[bl - sl] = 0;
    }
    puts(out);
    return 0;
}

static int cmd_dirname(int argc, char **argv) {
    if (argc < 2) { fputs("usage: dirname PATH\n", stderr); return 1; }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", argv[1]);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    char *s = strrchr(tmp, '/');
    if (!s) puts(".");
    else if (s == tmp) puts("/");
    else { *s = 0; puts(tmp); }
    return 0;
}

static int cmd_find(int argc, char **argv);

/* ================= text tools ================= */

struct hn { long n; int multi; int first; };

static int parse_n_opts(int argc, char **argv, long *n) {
    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { *n = atol(argv[i + 1]); i += 2; }
        else if (isdigit((unsigned char)argv[i][1])) { *n = atol(argv[i] + 1); i++; }
        else break;
    }
    if (*n < 0) *n = 0;
    if (*n > 100000) *n = 100000;
    return i;
}

static void file_header(struct hn *h, const char *name) {
    if (h->multi) { printf("%s==> %s <==\n", h->first ? "" : "\n", name); h->first = 0; }
}

static int head_in(FILE *f, const char *name, void *c) {
    struct hn *h = c;
    file_header(h, name);
    char *l = NULL; size_t cap = 0;
    long i = 0;
    while (i < h->n && getline(&l, &cap, f) > 0) { fputs(l, stdout); i++; }
    free(l);
    return 0;
}
static int cmd_head(int argc, char **argv) {
    struct hn h = {10, 0, 1};
    int i = parse_n_opts(argc, argv, &h.n);
    h.multi = argc - i > 1;
    return each_input(argc, argv, i, head_in, &h);
}

static int tail_in(FILE *f, const char *name, void *c) {
    struct hn *h = c;
    file_header(h, name);
    if (h->n == 0) return 0;
    char **ring = calloc((size_t)h->n, sizeof(char *));
    if (!ring) return 1;
    char *l = NULL; size_t cap = 0;
    long cnt = 0;
    while (getline(&l, &cap, f) > 0) {
        long idx = cnt % h->n;
        free(ring[idx]);
        ring[idx] = strdup(l);
        cnt++;
    }
    free(l);
    long start = cnt > h->n ? cnt - h->n : 0;
    for (long k = start; k < cnt; k++) fputs(ring[k % h->n], stdout);
    for (long k = 0; k < h->n; k++) free(ring[k]);
    free(ring);
    return 0;
}
static int cmd_tail(int argc, char **argv) {
    struct hn h = {10, 0, 1};
    int i = parse_n_opts(argc, argv, &h.n);
    h.multi = argc - i > 1;
    return each_input(argc, argv, i, tail_in, &h);
}

struct wcc { int l, w, c; long tl, tw, tc; };
static int wc_in(FILE *f, const char *name, void *cx) {
    struct wcc *x = cx;
    long L = 0, W = 0, C = 0;
    int inw = 0, ch;
    while ((ch = getc(f)) != EOF) {
        C++;
        if (ch == '\n') L++;
        if (isspace(ch)) inw = 0; else if (!inw) { inw = 1; W++; }
    }
    if (x->l) printf("%7ld", L);
    if (x->w) printf("%7ld", W);
    if (x->c) printf("%7ld", C);
    if (strcmp(name, "-")) printf(" %s", name);
    putchar('\n');
    x->tl += L; x->tw += W; x->tc += C;
    return 0;
}
static int cmd_wc(int argc, char **argv) {
    struct wcc x = {0, 0, 0, 0, 0, 0};
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'l') x.l = 1; else if (*f == 'w') x.w = 1; else if (*f == 'c') x.c = 1; }
    if (!x.l && !x.w && !x.c) x.l = x.w = x.c = 1;
    int rc = each_input(argc, argv, i, wc_in, &x);
    if (argc - i > 1) {
        if (x.l) printf("%7ld", x.tl);
        if (x.w) printf("%7ld", x.tw);
        if (x.c) printf("%7ld", x.tc);
        puts(" total");
    }
    return rc;
}

struct lines { char **v; size_t n, cap; };
static int collect_in(FILE *f, const char *name, void *c) {
    (void)name;
    struct lines *L = c;
    char *l = NULL; size_t cap = 0;
    while (getline(&l, &cap, f) > 0) {
        strip_nl(l);
        if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 64; L->v = realloc(L->v, L->cap * sizeof(char *)); }
        L->v[L->n++] = strdup(l);
    }
    free(l);
    return 0;
}
static void free_lines(struct lines *L) {
    for (size_t i = 0; i < L->n; i++) free(L->v[i]);
    free(L->v);
}

static int s_rev, s_num;
static int cmp_lines(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r;
    if (s_num) {
        double dx = strtod(x, NULL), dy = strtod(y, NULL);
        r = (dx > dy) - (dx < dy);
        if (!r) r = strcmp(x, y);
    } else r = strcmp(x, y);
    return s_rev ? -r : r;
}
static int cmd_sort(int argc, char **argv) {
    int uniq = 0, i = 1;
    s_rev = s_num = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'r') s_rev = 1; else if (*f == 'n') s_num = 1; else if (*f == 'u') uniq = 1; }
    struct lines L = {0};
    int rc = each_input(argc, argv, i, collect_in, &L);
    qsort(L.v, L.n, sizeof(char *), cmp_lines);
    for (size_t k = 0; k < L.n; k++)
        if (!uniq || k == 0 || strcmp(L.v[k], L.v[k - 1])) puts(L.v[k]);
    free_lines(&L);
    return rc;
}

static int cmd_uniq(int argc, char **argv) {
    int count = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-c")) { count = 1; i++; }
    struct lines L = {0};
    int rc = each_input(argc, argv, i, collect_in, &L);
    for (size_t k = 0; k < L.n;) {
        size_t j = k;
        while (j < L.n && !strcmp(L.v[j], L.v[k])) j++;
        if (count) printf("%7zu %s\n", j - k, L.v[k]); else puts(L.v[k]);
        k = j;
    }
    free_lines(&L);
    return rc;
}

static int cmd_tac(int argc, char **argv) {
    struct lines L = {0};
    int rc = each_input(argc, argv, 1, collect_in, &L);
    for (size_t k = L.n; k > 0; k--) puts(L.v[k - 1]);
    free_lines(&L);
    return rc;
}

static int rev_in(FILE *f, const char *name, void *c) {
    (void)name; (void)c;
    char *l = NULL; size_t cap = 0;
    while (getline(&l, &cap, f) > 0) {
        strip_nl(l);
        for (size_t a = 0, b = strlen(l); a + 1 < b; a++, b--) { char t = l[a]; l[a] = l[b - 1]; l[b - 1] = t; }
        puts(l);
    }
    free(l);
    return 0;
}
static int cmd_rev(int argc, char **argv) { return each_input(argc, argv, 1, rev_in, NULL); }

static int nl_in(FILE *f, const char *name, void *c) {
    (void)name;
    long *n = c;
    char *l = NULL; size_t cap = 0;
    while (getline(&l, &cap, f) > 0) printf("%6ld\t%s", ++*n, l);
    free(l);
    return 0;
}
static int cmd_nl(int argc, char **argv) { long n = 0; return each_input(argc, argv, 1, nl_in, &n); }

struct gctx { regex_t re; int inv, num, cnt, multi; long matched; };
static int grep_in(FILE *f, const char *name, void *c) {
    struct gctx *g = c;
    char *l = NULL; size_t cap = 0;
    long ln = 0, count = 0;
    while (getline(&l, &cap, f) > 0) {
        ln++;
        strip_nl(l);
        int hit = regexec(&g->re, l, 0, NULL, 0) == 0;
        if (hit == g->inv) continue;
        count++;
        if (g->cnt) continue;
        if (g->multi) printf("%s:", name);
        if (g->num) printf("%ld:", ln);
        puts(l);
    }
    free(l);
    if (g->cnt) { if (g->multi) printf("%s:", name); printf("%ld\n", count); }
    g->matched += count;
    return 0;
}
static int cmd_grep(int argc, char **argv) {
    struct gctx g;
    memset(&g, 0, sizeof g);
    int ic = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'i') ic = 1; else if (*f == 'v') g.inv = 1;
            else if (*f == 'n') g.num = 1; else if (*f == 'c') g.cnt = 1;
            else { fprintf(stderr, "grep: unknown option -%c\n", *f); return 2; }
        }
    if (i >= argc) { fputs("usage: grep [-ivnc] PATTERN [FILE...]\n", stderr); return 2; }
    int r = regcomp(&g.re, argv[i], REG_EXTENDED | REG_NOSUB | (ic ? REG_ICASE : 0));
    if (r) { char eb[128]; regerror(r, &g.re, eb, sizeof eb); fprintf(stderr, "grep: %s\n", eb); return 2; }
    i++;
    g.multi = argc - i > 1;
    int rc = each_input(argc, argv, i, grep_in, &g);
    regfree(&g.re);
    return rc ? 2 : (g.matched > 0 ? 0 : 1);
}

static int cut_lo[32], cut_hi[32], cut_n;
static int parse_list(const char *s) {
    cut_n = 0;
    char *d = strdup(s), *sp, *t;
    for (t = strtok_r(d, ",", &sp); t && cut_n < 32; t = strtok_r(NULL, ",", &sp)) {
        char *dash = strchr(t, '-');
        int lo, hi;
        if (!dash) lo = hi = atoi(t);
        else { *dash = 0; lo = *t ? atoi(t) : 1; hi = dash[1] ? atoi(dash + 1) : (1 << 30); }
        if (lo < 1) lo = 1;
        cut_lo[cut_n] = lo; cut_hi[cut_n] = hi; cut_n++;
    }
    free(d);
    return cut_n > 0;
}
static int in_list(int n) {
    for (int i = 0; i < cut_n; i++) if (n >= cut_lo[i] && n <= cut_hi[i]) return 1;
    return 0;
}
static char cut_delim = '\t';
static int cut_chars;
static int cut_in(FILE *f, const char *name, void *c) {
    (void)name; (void)c;
    char *l = NULL; size_t cap = 0;
    while (getline(&l, &cap, f) > 0) {
        strip_nl(l);
        if (cut_chars) {
            for (int k = 0; l[k]; k++) if (in_list(k + 1)) putchar(l[k]);
            putchar('\n');
            continue;
        }
        if (!strchr(l, cut_delim)) { puts(l); continue; }
        int field = 1, first = 1;
        char *p = l;
        for (;;) {
            char *e = strchr(p, cut_delim);
            if (in_list(field)) {
                if (!first) putchar(cut_delim);
                first = 0;
                if (e) fwrite(p, 1, (size_t)(e - p), stdout); else fputs(p, stdout);
            }
            if (!e) break;
            p = e + 1; field++;
        }
        putchar('\n');
    }
    free(l);
    return 0;
}
static int cmd_cut(int argc, char **argv) {
    int i = 1, have = 0;
    cut_delim = '\t'; cut_chars = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char o = argv[i][1];
        const char *val = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : NULL);
        if (!val) { fputs("cut: missing value\n", stderr); return 1; }
        if (o == 'd') cut_delim = val[0];
        else if (o == 'f') have = parse_list(val);
        else if (o == 'c') { cut_chars = 1; have = parse_list(val); }
        else { fprintf(stderr, "cut: unknown option -%c\n", o); return 1; }
    }
    if (!have) { fputs("usage: cut -f LIST [-d DELIM] | -c LIST  [FILE...]\n", stderr); return 1; }
    return each_input(argc, argv, i, cut_in, NULL);
}

static int expand_set(const char *s, unsigned char *out) {
    int n = 0;
    for (; *s && n < 256; s++) {
        int c = (unsigned char)*s;
        if (c == '\\' && s[1]) {
            s++;
            c = *s == 'n' ? '\n' : *s == 't' ? '\t' : (unsigned char)*s;
        }
        if (s[1] == '-' && s[2]) {
            int hi = (unsigned char)s[2];
            if (hi >= c) { for (int k = c; k <= hi && n < 256; k++) out[n++] = (unsigned char)k; s += 2; continue; }
        }
        out[n++] = (unsigned char)c;
    }
    return n;
}
static int cmd_tr(int argc, char **argv) {
    int del = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-d")) { del = 1; i++; }
    if (i >= argc || (!del && i + 1 >= argc)) { fputs("usage: tr SET1 SET2  |  tr -d SET1\n", stderr); return 1; }
    unsigned char s1[256], s2[256], map[256];
    char delete_[256];
    int n1 = expand_set(argv[i], s1), n2 = 0;
    for (int k = 0; k < 256; k++) { map[k] = (unsigned char)k; delete_[k] = 0; }
    if (del) for (int k = 0; k < n1; k++) delete_[s1[k]] = 1;
    else {
        n2 = expand_set(argv[i + 1], s2);
        if (n2 == 0) { fputs("tr: SET2 is empty\n", stderr); return 1; }
        for (int k = 0; k < n1; k++) map[s1[k]] = s2[k < n2 ? k : n2 - 1];
    }
    int c;
    while ((c = getchar()) != EOF) if (!delete_[c]) putchar(map[c]);
    return 0;
}

static int cmd_tee(int argc, char **argv) {
    int app = 0, i = 1, nf = 0;
    if (i < argc && !strcmp(argv[i], "-a")) { app = 1; i++; }
    int fds[16];
    for (; i < argc && nf < 16; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) err("tee", argv[i], errno); else fds[nf++] = fd;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        if (write(1, buf, (size_t)n) < 0) {}
        for (int k = 0; k < nf; k++) if (write(fds[k], buf, (size_t)n) < 0) {}
    }
    for (int k = 0; k < nf; k++) close(fds[k]);
    return 0;
}

static int more_in(FILE *f, const char *name, void *c) {
    (void)name; (void)c;
    char *l = NULL; size_t cap = 0;
    int shown = 0, page = 22;
    while (getline(&l, &cap, f) > 0) {
        fputs(l, stdout);
        if (++shown >= page) {
            fputs("\033[7m-- more (Enter = next page, q = quit) --\033[0m", stdout);
            fflush(stdout);
            char ch, first = 0;
            ssize_t r;
            while ((r = read(2, &ch, 1)) == 1) { if (!first) first = ch; if (ch == '\n') break; }
            fputs("\r\033[K", stdout);
            if (r != 1 || first == 'q' || first == 'Q') { free(l); return 0; }
            shown = 0;
        }
    }
    free(l);
    return 0;
}
static int cmd_more(int argc, char **argv) { return each_input(argc, argv, 1, more_in, NULL); }

static int hex_in(FILE *f, const char *name, void *c) {
    (void)name; (void)c;
    unsigned char b[16];
    size_t n;
    unsigned long off = 0;
    while ((n = fread(b, 1, 16, f)) > 0) {
        printf("%08lx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", b[i]); else fputs("   ", stdout);
            if (i == 7) putchar(' ');
        }
        fputs(" |", stdout);
        for (size_t i = 0; i < n; i++) putchar(isprint(b[i]) ? b[i] : '.');
        puts("|");
        off += n;
    }
    return 0;
}
static int cmd_hexdump(int argc, char **argv) { return each_input(argc, argv, 1, hex_in, NULL); }

/* ================= find ================= */

struct fctx { const char *name; char type; };
static void find_walk(const char *path, const struct fctx *c, int depth) {
    struct stat st;
    if (lstat(path, &st) < 0) return;
    int match = 1;
    if (c->type == 'f' && !S_ISREG(st.st_mode)) match = 0;
    if (c->type == 'd' && !S_ISDIR(st.st_mode)) match = 0;
    if (c->name && fnmatch(c->name, base_name(path), 0) != 0) match = 0;
    if (match) puts(path);
    if (!S_ISDIR(st.st_mode) || depth >= 40) return;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1024];
        size_t pl = strlen(path);
        snprintf(full, sizeof full, "%s%s%s", path, (pl && path[pl - 1] == '/') ? "" : "/", e->d_name);
        find_walk(full, c, depth + 1);
    }
    closedir(d);
}
static int cmd_find(int argc, char **argv) {
    struct fctx c = {NULL, 0};
    int i = 1, nroots = 0;
    int roots_start = 1;
    while (i < argc && argv[i][0] != '-') { i++; nroots++; }
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-name") && i + 1 < argc) c.name = argv[++i];
        else if (!strcmp(argv[i], "-type") && i + 1 < argc) c.type = argv[++i][0];
        else { fprintf(stderr, "find: unknown option %s\n", argv[i]); return 1; }
    }
    if (nroots == 0) find_walk(".", &c, 0);
    for (int k = 0; k < nroots; k++) find_walk(argv[roots_start + k], &c, 0);
    return 0;
}

/* ================= system tools ================= */

static int cmd_cd(int argc, char **argv) {
    const char *p = argc > 1 ? argv[1] : (getenv("HOME") ? getenv("HOME") : "/");
    if (chdir(p) < 0) { err("cd", p, errno); return 1; }
    return 0;
}
static int cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[512];
    if (!getcwd(buf, sizeof buf)) { err("pwd", ".", errno); return 1; }
    puts(buf);
    return 0;
}

static int cmd_mem(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sysinfo si;
    if (sysinfo(&si) < 0) { fputs("mem: failed\n", stderr); return 1; }
    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    printf("total: %lu KB\nfree:  %lu KB\n", (si.totalram * unit) / 1024, (si.freeram * unit) / 1024);
    return 0;
}
static int cmd_free(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sysinfo si;
    if (sysinfo(&si) < 0) { fputs("free: failed\n", stderr); return 1; }
    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    unsigned long tot = si.totalram * unit / 1024, fre = si.freeram * unit / 1024;
    printf("%-6s %10s %10s %10s\n", "", "total", "used", "free");
    printf("%-6s %10lu %10lu %10lu\n", "Mem:", tot, tot - fre, fre);
    return 0;
}
static int cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    struct sysinfo si;
    if (sysinfo(&si) < 0) { fputs("uptime: failed\n", stderr); return 1; }
    long u = si.uptime;
    printf("up %ld day(s) %02ld:%02ld:%02ld\n", u / 86400, (u / 3600) % 24, (u / 60) % 60, u % 60);
    return 0;
}

static int cmd_date(int argc, char **argv) {
    time_t t = time(NULL);
    char b[256];
    const char *fmt = (argc > 1 && argv[1][0] == '+') ? argv[1] + 1 : "%a %b %e %H:%M:%S %Z %Y";
    strftime(b, sizeof b, fmt, localtime(&t));
    puts(b);
    return 0;
}

static int cmd_id(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("uid=%u%s gid=%u%s\n", getuid(), getuid() == 0 ? "(root)" : "", getgid(), getgid() == 0 ? "(root)" : "");
    return 0;
}
static int cmd_whoami(int argc, char **argv) {
    (void)argc; (void)argv;
    if (getuid() == 0) puts("root"); else printf("uid%u\n", getuid());
    return 0;
}
static int cmd_hostname(int argc, char **argv) {
    char b[256];
    if (argc > 1) {
        if (sethostname(argv[1], strlen(argv[1])) < 0) { err("hostname", argv[1], errno); return 1; }
        return 0;
    }
    if (gethostname(b, sizeof b) < 0) { err("hostname", "", errno); return 1; }
    puts(b);
    return 0;
}
static int cmd_uname(int argc, char **argv) {
    struct utsname u;
    if (uname(&u) < 0) { err("uname", "", errno); return 1; }
    int s = 0, n = 0, r = 0, v = 0, m = 0;
    for (int i = 1; i < argc; i++)
        for (const char *f = argv[i] + (argv[i][0] == '-'); *f; f++) {
            if (*f == 'a') s = n = r = v = m = 1; else if (*f == 's') s = 1; else if (*f == 'n') n = 1;
            else if (*f == 'r') r = 1; else if (*f == 'v') v = 1; else if (*f == 'm') m = 1;
        }
    if (!s && !n && !r && !v && !m) s = 1;
    int first = 1;
#define PU(flag, val) if (flag) { printf("%s%s", first ? "" : " ", val); first = 0; }
    PU(s, u.sysname) PU(n, u.nodename) PU(r, u.release) PU(v, u.version) PU(m, u.machine)
#undef PU
    putchar('\n');
    return 0;
}

static int sig_from_name(const char *s) {
    static const struct { const char *n; int v; } t[] = {
        {"HUP", 1}, {"INT", 2}, {"QUIT", 3}, {"KILL", 9}, {"USR1", 10}, {"USR2", 12},
        {"TERM", 15}, {"CONT", 18}, {"STOP", 19}, {NULL, 0}};
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (int i = 0; t[i].n; i++) if (!strcmp(s, t[i].n)) return t[i].v;
    return -1;
}
static int cmd_kill(int argc, char **argv) {
    int sig = 15, i = 1, rc = 0;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        sig = isdigit((unsigned char)argv[i][1]) ? atoi(argv[i] + 1) : sig_from_name(argv[i] + 1);
        if (sig < 0) { fprintf(stderr, "kill: unknown signal %s\n", argv[i]); return 1; }
        i++;
    }
    if (i >= argc) { fputs("usage: kill [-SIGNAL] PID...\n", stderr); return 1; }
    for (; i < argc; i++)
        if (kill((pid_t)atoi(argv[i]), sig) < 0) { err("kill", argv[i], errno); rc = 1; }
    return rc;
}

static int cmd_sleep(int argc, char **argv) {
    if (argc < 2) { fputs("usage: sleep SECONDS\n", stderr); return 1; }
    double s = strtod(argv[1], NULL);
    if (s < 0) s = 0;
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
    return 0;
}
static int cmd_seq(int argc, char **argv) {
    long a = 1, inc = 1, b;
    if (argc == 2) b = atol(argv[1]);
    else if (argc == 3) { a = atol(argv[1]); b = atol(argv[2]); }
    else if (argc == 4) { a = atol(argv[1]); inc = atol(argv[2]); b = atol(argv[3]); }
    else { fputs("usage: seq [FIRST [STEP]] LAST\n", stderr); return 1; }
    if (inc == 0) { fputs("seq: step cannot be 0\n", stderr); return 1; }
    for (long k = a; inc > 0 ? k <= b : k >= b; k += inc) printf("%ld\n", k);
    return 0;
}
static int cmd_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int cmd_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }
static int cmd_sync(int argc, char **argv) { (void)argc; (void)argv; sync(); return 0; }
static int cmd_halt(int argc, char **argv) {
    (void)argc; (void)argv;
    sync();
    reboot(RB_HALT_SYSTEM);
    fputs("halt: failed\n", stderr);
    return 1;
}

static int cmd_dmesg(int argc, char **argv) {
    (void)argc; (void)argv;
    int size = klogctl(10, NULL, 0);
    if (size <= 0) size = 65536;
    char *buf = malloc((size_t)size + 1);
    if (!buf) return 1;
    int n = klogctl(3, buf, size);
    if (n < 0) { err("dmesg", "kernel log", errno); free(buf); return 1; }
    fwrite(buf, 1, (size_t)n, stdout);
    free(buf);
    return 0;
}

static int cmd_mount(int argc, char **argv) {
    const char *type = NULL;
    int i = 1;
    if (argc == 1) {
        FILE *f = fopen("/proc/mounts", "r");
        if (!f) { err("mount", "/proc/mounts", errno); return 1; }
        char buf[512];
        while (fgets(buf, sizeof buf, f)) fputs(buf, stdout);
        fclose(f);
        return 0;
    }
    if (i + 1 < argc && !strcmp(argv[i], "-t")) { type = argv[i + 1]; i += 2; }
    if (argc - i != 2 || !type) { fputs("usage: mount -t TYPE SOURCE TARGET\n", stderr); return 1; }
    if (mount(argv[i], argv[i + 1], type, 0, NULL) < 0) { err("mount", argv[i + 1], errno); return 1; }
    return 0;
}
static int cmd_umount(int argc, char **argv) {
    if (argc != 2) { fputs("usage: umount TARGET\n", stderr); return 1; }
    if (umount(argv[1]) < 0) { err("umount", argv[1], errno); return 1; }
    return 0;
}

static int cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    DIR *d = opendir("/proc");
    if (!d) { fputs("ps: /proc is not available\n", stderr); return 1; }
    printf("%6s %6s %c  %s\n", "PID", "PPID", 'S', "COMMAND");
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char p[300], buf[512];
        snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (fgets(buf, sizeof buf, f)) {
            int pid, ppid; char comm[256], st;
            if (sscanf(buf, "%d (%255[^)]) %c %d", &pid, comm, &st, &ppid) == 4)
                printf("%6d %6d %c  %s\n", pid, ppid, st, comm);
        }
        fclose(f);
    }
    closedir(d);
    return 0;
}

static int cmd_env(int argc, char **argv) {
    (void)argc; (void)argv;
    for (char **e = environ; e && *e; e++) puts(*e);
    return 0;
}
static int cmd_export(int argc, char **argv) {
    if (argc == 1) return cmd_env(argc, argv);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq || eq == argv[i]) { fprintf(stderr, "export: use NAME=VALUE (got '%s')\n", argv[i]); rc = 1; continue; }
        *eq = 0;
        setenv(argv[i], eq + 1, 1);
    }
    return rc;
}
static int cmd_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) unsetenv(argv[i]);
    return 0;
}

const struct builtin extra_builtins[] = {
    {"ls", cmd_ls, 0}, {"dir", cmd_dir, 0}, {"cat", cmd_cat, 0}, {"echo", cmd_echo, 0},
    {"mkdir", cmd_mkdir, 0}, {"rmdir", cmd_rmdir, 0}, {"rm", cmd_rm, 0}, {"del", cmd_rm, 0},
    {"cp", cmd_cp, 0}, {"mv", cmd_mv, 0}, {"touch", cmd_touch, 0}, {"ln", cmd_ln, 0},
    {"chmod", cmd_chmod, 0}, {"stat", cmd_stat, 0}, {"du", cmd_du, 0}, {"df", cmd_df, 0},
    {"basename", cmd_basename, 0}, {"dirname", cmd_dirname, 0}, {"find", cmd_find, 0},
    {"head", cmd_head, 0}, {"tail", cmd_tail, 0}, {"wc", cmd_wc, 0}, {"sort", cmd_sort, 0},
    {"uniq", cmd_uniq, 0}, {"tac", cmd_tac, 0}, {"rev", cmd_rev, 0}, {"nl", cmd_nl, 0},
    {"grep", cmd_grep, 0}, {"cut", cmd_cut, 0}, {"tr", cmd_tr, 0}, {"tee", cmd_tee, 0},
    {"more", cmd_more, 0}, {"hexdump", cmd_hexdump, 0},
    {"cd", cmd_cd, 1}, {"pwd", cmd_pwd, 0},
    {"mem", cmd_mem, 0}, {"free", cmd_free, 0}, {"uptime", cmd_uptime, 0}, {"date", cmd_date, 0},
    {"id", cmd_id, 0}, {"whoami", cmd_whoami, 0}, {"hostname", cmd_hostname, 0}, {"uname", cmd_uname, 0},
    {"kill", cmd_kill, 0}, {"sleep", cmd_sleep, 0}, {"seq", cmd_seq, 0},
    {"true", cmd_true, 0}, {"false", cmd_false, 0}, {"sync", cmd_sync, 0}, {"halt", cmd_halt, 0},
    {"dmesg", cmd_dmesg, 0}, {"mount", cmd_mount, 0}, {"umount", cmd_umount, 0}, {"ps", cmd_ps, 0},
    {"env", cmd_env, 0}, {"export", cmd_export, 1}, {"unset", cmd_unset, 1},
    {NULL, NULL, 0}
};
