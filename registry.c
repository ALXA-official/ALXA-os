/*
 * Magnesium OS - the registry: named values that hold real settings and
 * system variables. Original code by alxa, MIT licensed.
 * File format: one "Key/Path=value" per line ('\n' and '\\' are escaped).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include "registry.h"

#define MAXKEY 120
#define MAXVAL 400

struct entry { char key[MAXKEY]; char val[MAXVAL]; };
struct hive {
    char path[256];
    struct entry *e;
    int n, cap;
    time_t mtime;
    long mnsec;
    int loaded;
};

static struct hive hives[2];

const char *reg_hive_name(int h) { return h == HIVE_SYSTEM ? "System" : "User"; }

void reg_set_paths(const char *sys, const char *usr) {
    if (sys) { snprintf(hives[HIVE_SYSTEM].path, sizeof hives[0].path, "%s", sys); hives[0].loaded = 0; }
    if (usr) { snprintf(hives[HIVE_USER].path, sizeof hives[1].path, "%s", usr); hives[1].loaded = 0; }
}

static int valid_key(const char *k) {
    size_t l = strlen(k);
    if (l == 0 || l >= MAXKEY || k[0] == '/' || k[l - 1] == '/') return 0;
    for (size_t i = 0; i < l; i++)
        if (k[i] == '=' || k[i] == '\n' || (unsigned char)k[i] < 32 || (k[i] == '/' && k[i + 1] == '/')) return 0;
    return 1;
}

static int cmp_key(const void *a, const void *b) {
    const char *x = ((const struct entry *)a)->key, *y = ((const struct entry *)b)->key;
    for (;; x++, y++) {
        unsigned char cx = *x == '/' ? 1 : (unsigned char)*x, cy = *y == '/' ? 1 : (unsigned char)*y;
        if (cx != cy) return cx < cy ? -1 : 1;
        if (!cx) return 0;
    }
}

static void unescape(char *dst, size_t n, const char *s) {
    size_t o = 0;
    for (; *s && o + 1 < n; s++) {
        if (*s == '\\' && s[1] == 'n') { dst[o++] = '\n'; s++; }
        else if (*s == '\\' && s[1] == '\\') { dst[o++] = '\\'; s++; }
        else dst[o++] = *s;
    }
    dst[o] = 0;
}

static void load(struct hive *h) {
    h->n = 0;
    h->loaded = 1;
    h->mtime = 0; h->mnsec = 0;
    if (!h->path[0]) return;
    struct stat st;
    if (stat(h->path, &st) == 0) { h->mtime = st.st_mtime; h->mnsec = st.st_mtim.tv_nsec; }
    FILE *f = fopen(h->path, "r");
    if (!f) return;
    char line[MAXKEY + MAXVAL + 8];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        if (!valid_key(line)) continue;
        if (h->n == h->cap) {
            h->cap = h->cap ? h->cap * 2 : 64;
            h->e = realloc(h->e, (size_t)h->cap * sizeof *h->e);
            if (!h->e) { h->n = 0; h->cap = 0; fclose(f); return; }
        }
        struct entry *e = &h->e[h->n++];
        snprintf(e->key, sizeof e->key, "%s", line);
        unescape(e->val, sizeof e->val, eq + 1);
    }
    fclose(f);
    qsort(h->e, (size_t)h->n, sizeof *h->e, cmp_key);
}

static struct hive *get_hive(int hv) {
    struct hive *h = &hives[hv == HIVE_SYSTEM ? 0 : 1];
    if (!h->loaded) load(h);
    return h;
}

void reg_refresh(void) {
    for (int i = 0; i < 2; i++) {
        struct hive *h = &hives[i];
        if (!h->loaded || !h->path[0]) continue;
        struct stat st;
        time_t mt = 0; long ns = 0;
        if (stat(h->path, &st) == 0) { mt = st.st_mtime; ns = st.st_mtim.tv_nsec; }
        if (mt != h->mtime || ns != h->mnsec) load(h);
    }
}

static struct entry *find(struct hive *h, const char *key) {
    for (int i = 0; i < h->n; i++) if (!strcmp(h->e[i].key, key)) return &h->e[i];
    return NULL;
}

const char *reg_get(int hv, const char *key, const char *def) {
    struct entry *e = find(get_hive(hv), key);
    return e ? e->val : def;
}

const char *reg_eff(const char *key, const char *def) {
    struct entry *e = find(get_hive(HIVE_USER), key);
    if (!e) e = find(get_hive(HIVE_SYSTEM), key);
    return e ? e->val : def;
}

int reg_eff_int(const char *key, int def) {
    const char *v = reg_eff(key, NULL);
    return v && *v ? atoi(v) : def;
}

static int save(struct hive *h) {
    if (!h->path[0]) { errno = ENOENT; return -1; }
    char tmp[272];
    snprintf(tmp, sizeof tmp, "%s.tmp", h->path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, h == &hives[0] ? 0644 : 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp); return -1; }
    fputs("# Magnesium OS registry\n", f);
    for (int i = 0; i < h->n; i++) {
        fprintf(f, "%s=", h->e[i].key);
        for (const char *p = h->e[i].val; *p; p++) {
            if (*p == '\n') fputs("\\n", f);
            else if (*p == '\\') fputs("\\\\", f);
            else fputc(*p, f);
        }
        fputc('\n', f);
    }
    fflush(f);
    fsync(fd);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, h->path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    struct stat st;
    if (stat(h->path, &st) == 0) { h->mtime = st.st_mtime; h->mnsec = st.st_mtim.tv_nsec; }
    return 0;
}

int reg_set(int hv, const char *key, const char *val) {
    if (!valid_key(key)) { errno = EINVAL; return -1; }
    struct hive *h = get_hive(hv);
    struct entry *e = find(h, key);
    if (e) snprintf(e->val, sizeof e->val, "%s", val);
    else {
        if (h->n == h->cap) {
            h->cap = h->cap ? h->cap * 2 : 64;
            h->e = realloc(h->e, (size_t)h->cap * sizeof *h->e);
            if (!h->e) { h->n = 0; h->cap = 0; errno = ENOMEM; return -1; }
        }
        e = &h->e[h->n++];
        snprintf(e->key, sizeof e->key, "%s", key);
        snprintf(e->val, sizeof e->val, "%s", val);
        qsort(h->e, (size_t)h->n, sizeof *h->e, cmp_key);
    }
    if (save(h) < 0) {                       /* undo: reload what is really on disk */
        int err = errno;
        load(h);
        errno = err;
        return -1;
    }
    return 0;
}

int reg_del(int hv, const char *key) {
    struct hive *h = get_hive(hv);
    for (int i = 0; i < h->n; i++)
        if (!strcmp(h->e[i].key, key)) {
            memmove(&h->e[i], &h->e[i + 1], (size_t)(h->n - i - 1) * sizeof *h->e);
            h->n--;
            if (save(h) < 0) { int err = errno; load(h); errno = err; return -1; }
            return 0;
        }
    errno = ENOENT;
    return -1;
}

int reg_del_prefix(int hv, const char *prefix) {
    struct hive *h = get_hive(hv);
    size_t pl = strlen(prefix);
    int removed = 0;
    for (int i = 0; i < h->n;) {
        if (!strncmp(h->e[i].key, prefix, pl) && (h->e[i].key[pl] == '/' || h->e[i].key[pl] == 0)) {
            memmove(&h->e[i], &h->e[i + 1], (size_t)(h->n - i - 1) * sizeof *h->e);
            h->n--; removed++;
        } else i++;
    }
    if (removed && save(h) < 0) { int err = errno; load(h); errno = err; return -1; }
    return removed;
}

int reg_count(int hv) { return get_hive(hv)->n; }

const char *reg_entry(int hv, int i, const char **val) {
    struct hive *h = get_hive(hv);
    if (i < 0 || i >= h->n) return NULL;
    if (val) *val = h->e[i].val;
    return h->e[i].key;
}

int reg_apply_env(void) {
    int n = 0;
    reg_refresh();                              /* never apply a stale copy */
    for (int hv = 0; hv < 2; hv++) {           /* system first, then the user's own */
        struct hive *h = get_hive(hv);
        for (int i = 0; i < h->n; i++) {
            const char *k = h->e[i].key;
            if (strncmp(k, "Environment/", 12)) continue;
            const char *name = k + 12;
            if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) continue;
            int ok = 1;
            for (const char *p = name; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_')) ok = 0;
            if (ok && setenv(name, h->e[i].val, 1) == 0) n++;
        }
    }
    return n;
}

/* ---- the 'reg' command: reg list [KEY] | get KEY | set KEY VALUE | del KEY ---- */

static int split_key(const char *arg, int *hv, const char **key) {
    if (!strncmp(arg, "System/", 7)) { *hv = HIVE_SYSTEM; *key = arg + 7; }
    else if (!strncmp(arg, "User/", 5)) { *hv = HIVE_USER; *key = arg + 5; }
    else { *hv = HIVE_USER; *key = arg; }
    return 1;
}

int cmd_reg(int argc, char **argv) {
    reg_refresh();
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        const char *pre = argc > 2 ? argv[2] : "";
        int hv_only = -1;
        if (!strncmp(pre, "System", 6) && (!pre[6] || pre[6] == '/')) { hv_only = HIVE_SYSTEM; pre += pre[6] ? 7 : 6; }
        else if (!strncmp(pre, "User", 4) && (!pre[4] || pre[4] == '/')) { hv_only = HIVE_USER; pre += pre[4] ? 5 : 4; }
        for (int hv = 0; hv < 2; hv++) {
            if (hv_only >= 0 && hv != hv_only) continue;
            for (int i = 0; i < reg_count(hv); i++) {
                const char *v = "", *k = reg_entry(hv, i, &v);
                if (strncmp(k, pre, strlen(pre))) continue;
                printf("%s/%s = %s\n", reg_hive_name(hv), k, v);
            }
        }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "get")) {
        int hv; const char *k;
        split_key(argv[2], &hv, &k);
        const char *v = reg_get(hv, k, NULL);
        if (!v) { fprintf(stderr, "reg: %s not found\n", argv[2]); return 1; }
        puts(v);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "set")) {
        int hv; const char *k;
        split_key(argv[2], &hv, &k);
        if (reg_set(hv, k, argv[3]) < 0) { fprintf(stderr, "reg: cannot set %s: %s\n", argv[2], strerror(errno)); return 1; }
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "del")) {
        int hv; const char *k;
        split_key(argv[2], &hv, &k);
        if (reg_del(hv, k) < 0) { fprintf(stderr, "reg: cannot delete %s: %s\n", argv[2], strerror(errno)); return 1; }
        return 0;
    }
    fputs("usage: reg list [KEY] | reg get KEY | reg set KEY VALUE | reg del KEY\n"
          "       KEY starts with System/ or User/ (default User/)\n", stderr);
    return 1;
}
