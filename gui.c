/*
 * Magnesium OS - the desktop: taskbar, clock, "element tile" start menu,
 * windows you can move and resize with the mouse, Settings, and the
 * Registry editor. It draws with text and colors, so it works on the
 * serial console, in QEMU -nographic, in Codespaces and on a VGA screen.
 * Original code by alxa, MIT licensed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/reboot.h>
#include "gui.h"
#include "registry.h"
#include "users.h"
#include "cmds.h"
#include "shell.h"
#include "version.h"

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MAXHOT 1500

/* ================= cells and drawing ================= */

typedef struct { uint32_t ch, fg, bg; uint8_t attr; } Cell;
enum { AT_BOLD = 1, AT_UL = 2 };

static Cell *cur, *prev;
static int W = 80, H = 24, detW = 80, detH = 24;
static int clx0, cly0, clx1, cly1;
static struct termios term_orig;
static int term_on;

static void set_clip(int x, int y, int w, int h) {
    clx0 = x < 0 ? 0 : x; cly0 = y < 0 ? 0 : y;
    clx1 = x + w > W ? W : x + w; cly1 = y + h > H ? H : y + h;
}
static void clip_all(void) { set_clip(0, 0, W, H); }

static void put(int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, int at) {
    if (x < clx0 || x >= clx1 || y < cly0 || y >= cly1) return;
    Cell *c = &cur[y * W + x];
    c->ch = ch; c->fg = fg; c->bg = bg; c->attr = (uint8_t)at;
}

static uint32_t u8next(const char **sp) {
    const unsigned char *s = (const unsigned char *)*sp;
    uint32_t c = *s;
    int n = 0;
    if (c < 0x80) { *sp += 1; return c; }
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *sp += 1; return '?'; }
    for (int i = 0; i < n; i++) {
        if ((s[1 + i] & 0xC0) != 0x80) { *sp += 1; return '?'; }
        c = (c << 6) | (s[1 + i] & 0x3F);
    }
    *sp += 1 + n;
    return c;
}

static int u8enc(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static int ulen(const char *s) { int n = 0; while (*s) { u8next(&s); n++; } return n; }

static int text(int x, int y, const char *s, uint32_t fg, uint32_t bg, int at, int maxw) {
    int n = 0;
    while (*s && (maxw < 0 || n < maxw)) { put(x + n, y, u8next(&s), fg, bg, at); n++; }
    return n;
}
static int textf(int x, int y, uint32_t fg, uint32_t bg, int at, int maxw, const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return text(x, y, b, fg, bg, at, maxw);
}
static void fill(int x, int y, int w, int h, uint32_t fg, uint32_t bg) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) put(x + i, y + j, ' ', fg, bg, 0);
}

/* ================= theme and settings snapshot ================= */

struct pal { uint32_t desk, bar, barfg, win, winfg, titleoff, titlefgoff, border, muted, sel, ok, bad; };
static const struct pal pals[3] = {
    /* Dark  */ {0x0B1220, 0x111A2B, 0xD6E2F5, 0x151F33, 0xE3ECFA, 0x1E2A44, 0xA9B8D3, 0x3A4A6B, 0x7F90B0, 0x24365A, 0x4CD08A, 0xFF6B6B},
    /* Light */ {0xDCE6F5, 0xF4F7FC, 0x1B2638, 0xFFFFFF, 0x1B2638, 0xE7EDF7, 0x4A5878, 0x9FB0CE, 0x6B7894, 0xDCE6F8, 0x1B9E5B, 0xD64545},
    /* Ember */ {0x140C08, 0x1E120C, 0xF3DCCB, 0x241610, 0xF6E6D8, 0x33201A, 0xC7A58E, 0x6B4636, 0x9C8070, 0x4A2E22, 0x7BD88F, 0xFF7A6B},
};
static const uint32_t accents[6] = {0x6EC1FF, 0xFF8A3D, 0x2CC9DC, 0x46C97A, 0xC77DFF, 0xF2B632};

struct cfg {
    struct pal p;
    uint32_t acc, accfg;
    int bgpat, ascii, top, clock, h12, secs, date, controls, labels, offset;
    int sizeW, sizeH;
    char pinned[256];
};
static struct cfg cf;

static int opt_index(const char *opts, const char *val) {
    int i = 0;
    const char *p = opts;
    while (*p) {
        const char *e = strchr(p, '|');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (strlen(val) == l && !strncasecmp(p, val, l)) return i;
        i++;
        if (!e) break;
        p = e + 1;
    }
    return -1;
}
static int opt_count(const char *opts) { int n = 1; for (const char *p = opts; *p; p++) if (*p == '|') n++; return n; }
static void opt_get(const char *opts, int idx, char *out, size_t n) {
    const char *p = opts;
    for (int i = 0; i < idx && p; i++) { p = strchr(p, '|'); if (p) p++; }
    if (!p) { out[0] = 0; return; }
    const char *e = strchr(p, '|');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l >= n) l = n - 1;
    memcpy(out, p, l);
    out[l] = 0;
}
static int lum(uint32_t c) { return (int)((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000); }

static void load_cfg(void) {
    reg_refresh();
    int t = opt_index("Dark|Light|Ember", reg_eff("Display/Theme", "Dark")); if (t < 0) t = 0;
    int a = opt_index("Flame|Orange|Cyan|Green|Magenta|Gold", reg_eff("Display/Accent", "Flame")); if (a < 0) a = 0;
    cf.p = pals[t];
    cf.acc = accents[a];
    cf.accfg = lum(cf.acc) > 140 ? 0x10151F : 0xFFFFFF;
    int b = opt_index("Solid|Dots|Grid", reg_eff("Desktop/Background", "Dots")); cf.bgpat = b < 0 ? 1 : b;
    cf.ascii = opt_index("Lines|ASCII", reg_eff("Display/Borders", "Lines")) == 1;
    cf.top = opt_index("Top|Bottom", reg_eff("Taskbar/Position", "Top")) != 1;
    cf.clock = reg_eff_int("Taskbar/Clock", 1);
    cf.h12 = opt_index("24-hour|12-hour", reg_eff("Taskbar/Format", "24-hour")) == 1;
    cf.secs = reg_eff_int("Taskbar/Seconds", 0);
    cf.date = reg_eff_int("Taskbar/Date", 1);
    cf.controls = reg_eff_int("Taskbar/Controls", 1);
    cf.labels = reg_eff_int("Taskbar/Labels", 1);
    cf.offset = reg_eff_int("Clock/Offset", 0);
    if (cf.offset < -12) cf.offset = -12;
    if (cf.offset > 14) cf.offset = 14;
    snprintf(cf.pinned, sizeof cf.pinned, "%s", reg_eff("Taskbar/Pinned", "settings,registry"));
    cf.sizeW = cf.sizeH = 0;
    const char *sz = reg_eff("Display/Size", "Auto");
    int sw, sh;
    if (sscanf(sz, "%dx%d", &sw, &sh) == 2 && sw >= 60 && sw <= 250 && sh >= 18 && sh <= 100) { cf.sizeW = sw; cf.sizeH = sh; }
}

/* ================= terminal ================= */

static void out_str(const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(1, s, n);
        if (w <= 0) { if (errno == EINTR) continue; return; }
        s += w; n -= (size_t)w;
    }
}

static int read_byte(int timeout_ms) {
    struct pollfd p = { .fd = 0, .events = POLLIN };
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r < 0) { if (errno == EINTR) continue; return -2; }
        if (r == 0) return -1;
        unsigned char c;
        ssize_t n = read(0, &c, 1);
        if (n == 1) return c;
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return -2;
    }
}

static void detect_size(void) {
    struct winsize ws;
    detW = 80; detH = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) { detW = ws.ws_col; detH = ws.ws_row; }
    else {
        out_str("\033[999C\033[999B\033[6n");
        char buf[32];
        int i = 0, c;
        while (i < (int)sizeof buf - 1 && (c = read_byte(300)) >= 0) { buf[i++] = (char)c; if (c == 'R') break; }
        buf[i] = 0;
        int r, cc;
        if (sscanf(buf, "\033[%d;%dR", &r, &cc) == 2 && r >= 10 && cc >= 30) { detH = r; detW = cc; }
    }
    if (detW > 250) detW = 250;
    if (detH > 100) detH = 100;
    if (detW < 60) detW = 60;
    if (detH < 18) detH = 18;
}

static void alloc_buffers(void) {
    free(cur); free(prev);
    cur = calloc((size_t)W * H, sizeof(Cell));
    prev = calloc((size_t)W * H, sizeof(Cell));
    for (int i = 0; i < W * H; i++) prev[i].ch = 0xFFFFFFFFu;     /* force a full redraw */
}

static void term_enter(void) {
    struct termios t;
    tcgetattr(0, &term_orig);
    t = term_orig;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(OPOST);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSADRAIN, &t);
    out_str("\033[?1049h\033[?25l\033[?1000h\033[?1002h\033[?1006h\033[2J");
    term_on = 1;
    for (int i = 0; i < W * H; i++) prev[i].ch = 0xFFFFFFFFu;
}

static void term_leave(void) {
    if (!term_on) return;
    out_str("\033[0m\033[?1006l\033[?1002l\033[?1000l\033[?25h\033[?1049l");
    tcsetattr(0, TCSADRAIN, &term_orig);
    term_on = 0;
}

static void flush_frame(void) {
    static char ob[400000];
    size_t o = 0;
    uint32_t cfg_ = 1, cbg = 1;
    int cat = -1;
    for (int y = 0; y < H; y++) {
        int first = -1, last = -1;
        for (int x = 0; x < W; x++)
            if (memcmp(&cur[y * W + x], &prev[y * W + x], sizeof(Cell))) { if (first < 0) first = x; last = x; }
        if (first < 0) continue;
        if (o + 64 > sizeof ob) break;
        o += (size_t)snprintf(ob + o, sizeof ob - o, "\033[%d;%dH", y + 1, first + 1);
        for (int x = first; x <= last; x++) {
            Cell *c = &cur[y * W + x];
            if (o + 80 > sizeof ob) break;
            if (c->fg != cfg_ || c->bg != cbg || c->attr != cat) {
                o += (size_t)snprintf(ob + o, sizeof ob - o, "\033[0%s%s;38;2;%u;%u;%u;48;2;%u;%u;%um",
                    (c->attr & AT_BOLD) ? ";1" : "", (c->attr & AT_UL) ? ";4" : "",
                    (c->fg >> 16) & 255, (c->fg >> 8) & 255, c->fg & 255,
                    (c->bg >> 16) & 255, (c->bg >> 8) & 255, c->bg & 255);
                cfg_ = c->fg; cbg = c->bg; cat = c->attr;
            }
            o += (size_t)u8enc(c->ch ? c->ch : ' ', ob + o);
        }
    }
    if (o) { ob[o] = 0; out_str(ob); }
    memcpy(prev, cur, (size_t)W * H * sizeof(Cell));
}

/* ================= input ================= */

enum { K_UP = 2000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_PGUP, K_PGDN, K_STAB,
       K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F10, K_MOUSE, K_TIMEOUT, K_EOF };
#define CK(c) ((c) & 0x1f)
static int mev_x, mev_y, mev_btn, mev_kind;      /* kind: 1 press, 0 release, 2 drag */

static int read_key(int timeout_ms) {
    int c = read_byte(timeout_ms);
    if (c == -1) return K_TIMEOUT;
    if (c < 0) return K_EOF;
    if (c >= 0xC0) {                               /* UTF-8 text */
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        char b[5] = { (char)c, 0, 0, 0, 0 };
        for (int i = 0; i < n; i++) { int d = read_byte(50); if (d < 0) break; b[1 + i] = (char)d; }
        const char *p = b;
        return (int)u8next(&p);
    }
    if (c != 27) return c;
    int a = read_byte(40);
    if (a < 0) return 27;
    int b = read_byte(40);
    if (b < 0) return 27;
    if (a == 'O') {
        switch (b) { case 'P': return K_F1; case 'Q': return K_F2; case 'R': return K_F3; case 'S': return K_F4;
                     case 'H': return K_HOME; case 'F': return K_END; }
        return 27;
    }
    if (a != '[') return 27;
    if (b == '<') {                                /* SGR mouse: ESC [ < b ; x ; y M/m */
        int v[3] = {0, 0, 0}, i = 0, d;
        while ((d = read_byte(40)) >= 0) {
            if (d >= '0' && d <= '9') v[i] = v[i] * 10 + (d - '0');
            else if (d == ';') { if (i < 2) i++; }
            else {
                mev_x = v[1] - 1; mev_y = v[2] - 1;
                mev_btn = (v[0] & 64) ? 64 + (v[0] & 1) : (v[0] & 3);
                mev_kind = d == 'm' ? 0 : ((v[0] & 32) ? 2 : 1);
                return K_MOUSE;
            }
        }
        return 27;
    }
    if (b == '[') { int d = read_byte(40); return d == 'A' ? K_F1 : d == 'B' ? K_F2 : d == 'C' ? K_F3 : d == 'D' ? K_F4 : 27; }
    if (b >= '0' && b <= '9') {
        int num = b - '0', d;
        while ((d = read_byte(40)) >= '0' && d <= '9') num = num * 10 + (d - '0');
        if (d == '~') {
            switch (num) {
            case 1: case 7: return K_HOME; case 4: case 8: return K_END; case 3: return K_DEL;
            case 5: return K_PGUP; case 6: return K_PGDN;
            case 11: return K_F1; case 12: return K_F2; case 13: return K_F3; case 14: return K_F4;
            case 15: return K_F5; case 17: return K_F6; case 18: return K_F7; case 21: return K_F10;
            }
        }
        return 27;
    }
    switch (b) {
    case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
    case 'H': return K_HOME; case 'F': return K_END; case 'Z': return K_STAB;
    }
    return 27;
}

/* ================= hotspots (mouse targets) ================= */

enum { A_NONE, A_START, A_PIN, A_TASKWIN, A_CLOSE, A_DESKTOP, A_HIDE, A_WINBODY, A_TITLE, A_EDGE,
       A_MENU, A_MENUITEM, A_MENUCLOSE, A_TILE, A_STARTCLOSE, A_SET_HOME, A_SET_CAT, A_SET_ROW,
       A_REG_NODE, A_REG_VAL, A_DLG_OK, A_DLG_CANCEL, A_SCROLLTILES };

struct hot { int x, y, w, h, act, a, b, wid; };
static struct hot hots[MAXHOT];
static int nhots, cur_wid;

static void hot_add(int x, int y, int w, int h, int act, int a, int b) {
    int x1 = x + w, y1 = y + h;
    if (x < clx0) x = clx0;
    if (y < cly0) y = cly0;
    if (x1 > clx1) x1 = clx1;
    if (y1 > cly1) y1 = cly1;
    if (x1 <= x || y1 <= y || nhots >= MAXHOT) return;
    hots[nhots++] = (struct hot){ x, y, x1 - x, y1 - y, act, a, b, cur_wid };
}
static const struct hot *hot_at(int x, int y) {
    for (int i = nhots - 1; i >= 0; i--)
        if (x >= hots[i].x && x < hots[i].x + hots[i].w && y >= hots[i].y && y < hots[i].y + hots[i].h) return &hots[i];
    return NULL;
}

/* ================= shared state ================= */

enum { WK_SETTINGS = 1, WK_REGISTRY = 2 };
struct win { int used, id, kind; char title[32]; int x, y, w, h, minimized; };
static struct win wins[4];
static int zord[4], nz, next_id = 1;
static int quit_code = -1;
static int start_open, menu_win, menu_idx;          /* menu_win: id of window whose menu is open */
static int desk_y0, desk_y1;                         /* usable rows [y0, y1) */
static int desk_hidden;                              /* "show desktop" state */
static char toast_msg[120];
static time_t toast_until;
static int mode_move, mode_resize;

struct drag { int on, mode, wid, ox, oy, edge, sx, sy, wx, wy, ww, wh; };   /* mode 1 move, 2 resize */
static struct drag drg;

static void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_msg, sizeof toast_msg, fmt, ap);
    va_end(ap);
    toast_until = time(NULL) + 4;
}

static struct win *win_by_id(int id) {
    for (int i = 0; i < N(wins); i++) if (wins[i].used && wins[i].id == id) return &wins[i];
    return NULL;
}
static struct win *win_by_kind(int k) {
    for (int i = 0; i < N(wins); i++) if (wins[i].used && wins[i].kind == k) return &wins[i];
    return NULL;
}
static struct win *active_win(void) {
    for (int i = nz - 1; i >= 0; i--) { struct win *w = win_by_id(zord[i]); if (w && !w->minimized) return w; }
    return NULL;
}
static void raise_win(struct win *w) {
    int i;
    for (i = 0; i < nz; i++) if (zord[i] == w->id) break;
    if (i == nz) return;
    for (; i < nz - 1; i++) zord[i] = zord[i + 1];
    zord[nz - 1] = w->id;
}

static void clamp_win(struct win *w) {
    int dh = desk_y1 - desk_y0;
    if (w->w > W) w->w = W;
    if (w->h > dh) w->h = dh;
    if (w->w < 34) w->w = 34 > W ? W : 34;
    if (w->h < 10) w->h = 10 > dh ? dh : 10;
    if (w->x < 0) w->x = 0;
    if (w->x + w->w > W) w->x = W - w->w;
    if (w->y < desk_y0) w->y = desk_y0;
    if (w->y + w->h > desk_y1) w->y = desk_y1 - w->h;
}

/* ================= registry-backed helpers ================= */

static int get_toggle(int hv, const char *key, const char *def) {
    const char *v = hv == HIVE_SYSTEM ? reg_get(HIVE_SYSTEM, key, def) : reg_eff(key, def);
    return v && (v[0] == '1' || !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcasecmp(v, "yes"));
}

static void save_error(const char *what) {
    if (errno == EACCES || errno == EPERM || errno == EROFS)
        toast("Cannot save %s: %s", what, getuid() == 0 ? strerror(errno) : "only root can change this");
    else toast("Cannot save %s: %s", what, strerror(errno));
}

static int is_pinned(const char *name) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cf.pinned);
    for (char *sp, *t = strtok_r(buf, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) {
        while (*t == ' ') t++;
        if (!strcmp(t, name)) return 1;
    }
    return 0;
}
static void set_pinned(const char *name, int on) {
    char buf[256], out[256] = "";
    snprintf(buf, sizeof buf, "%s", cf.pinned);
    for (char *sp, *t = strtok_r(buf, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) {
        while (*t == ' ') t++;
        if (!*t || !strcmp(t, name)) continue;
        if (strlen(out) + strlen(t) + 2 < sizeof out) { if (*out) strcat(out, ","); strcat(out, t); }
    }
    if (on && strlen(out) + strlen(name) + 2 < sizeof out) { if (*out) strcat(out, ","); strcat(out, name); }
    if (reg_set(HIVE_USER, "Taskbar/Pinned", out) < 0) save_error("pinned apps");
    load_cfg();
}

/* installed programs (executables in the command folders) */
static char apps[64][40];
static int napps;
static void scan_apps(void) {
    static const char *dirs[] = { "/bin", "/usr/bin", "/usr/local/bin", "/sbin", "/data/bin", NULL };
    napps = 0;
    for (int d = 0; dirs[d]; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) != NULL && napps < N(apps)) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "passwd") || strlen(e->d_name) >= sizeof apps[0]) continue;
            char full[300];
            struct stat st;
            snprintf(full, sizeof full, "%s/%s", dirs[d], e->d_name);
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || access(full, X_OK) != 0) continue;
            int dup = 0;
            for (int i = 0; i < napps; i++) if (!strcmp(apps[i], e->d_name)) dup = 1;
            if (!dup) snprintf(apps[napps++], sizeof apps[0], "%s", e->d_name);
        }
        closedir(dp);
    }
}

/* ================= dialogs ================= */

enum { D_NONE, D_INPUT, D_CONFIRM, D_INFO };
enum { P_SET_TEXT, P_SET_TIME, P_REG_EDIT, P_REG_NEWNAME, P_REG_NEWVAL, P_C_REBOOT, P_C_HALT, P_C_LOGOUT,
       P_C_RESET, P_C_DELKEY, P_C_DELVAL };
struct dlg { int type, purpose, len, cur; char title[48], msg[260], buf[256]; };
static struct dlg dlg;
static char pend_key[160];
static int pend_hive;

static void dlg_open(int type, int purpose, const char *title, const char *msg, const char *initial) {
    memset(&dlg, 0, sizeof dlg);
    dlg.type = type; dlg.purpose = purpose;
    snprintf(dlg.title, sizeof dlg.title, "%s", title);
    snprintf(dlg.msg, sizeof dlg.msg, "%s", msg);
    if (initial) snprintf(dlg.buf, sizeof dlg.buf, "%s", initial);
    dlg.len = dlg.cur = (int)strlen(dlg.buf);
}

static void draw_dialog(void) {
    if (!dlg.type) return;
    clip_all();
    hot_add(0, 0, W, H, A_NONE, 0, 0);                         /* blocks clicks behind */
    int w = W - 6 < 62 ? W - 6 : 62, h = dlg.type == D_INPUT ? 9 : 7;
    int x = (W - w) / 2, y = (H - h) / 2;
    const struct pal *p = &cf.p;
    cur_wid = 0;
    fill(x, y, w, h, p->winfg, p->win);
    for (int i = 0; i < w; i++) { put(x + i, y, cf.ascii ? '-' : 0x2500, cf.acc, p->win, 0); put(x + i, y + h - 1, cf.ascii ? '-' : 0x2500, cf.acc, p->win, 0); }
    for (int j = 0; j < h; j++) { put(x, y + j, cf.ascii ? '|' : 0x2502, cf.acc, p->win, 0); put(x + w - 1, y + j, cf.ascii ? '|' : 0x2502, cf.acc, p->win, 0); }
    text(x + 2, y, dlg.title, cf.accfg, cf.acc, AT_BOLD, w - 4);
    fill(x + 1, y, 1, 1, cf.accfg, cf.acc);
    text(x + 2, y + 2, dlg.msg, p->winfg, p->win, 0, w - 4);
    if (dlg.type == D_INPUT) {
        fill(x + 2, y + 4, w - 4, 1, p->winfg, p->sel);
        int start = dlg.cur > w - 7 ? dlg.cur - (w - 7) : 0;
        text(x + 3, y + 4, dlg.buf + start, p->winfg, p->sel, 0, w - 6);
        put(x + 3 + dlg.cur - start, y + 4, ' ', cf.accfg, cf.acc, AT_UL);
    }
    int by = y + h - 2;
    const char *okl = dlg.type == D_CONFIRM ? " Yes " : " OK ";
    int okw = ulen(okl);
    text(x + w - 4 - okw - 10, by, okl, cf.accfg, cf.acc, AT_BOLD, okw);
    hot_add(x + w - 4 - okw - 10, by, okw, 1, A_DLG_OK, 0, 0);
    if (dlg.type != D_INFO) {
        const char *cl = dlg.type == D_CONFIRM ? " No " : " Cancel ";
        text(x + w - 4 - ulen(cl), by, cl, p->winfg, p->titleoff, 0, 20);
        hot_add(x + w - 4 - ulen(cl), by, ulen(cl), 1, A_DLG_CANCEL, 0, 0);
    }
}

/* ================= launching programs outside the desktop ================= */

static void full_redraw(void) {
    for (int i = 0; i < W * H; i++) prev[i].ch = 0xFFFFFFFFu;
    out_str("\033[2J");
}

static void launch_external(const char *cmd) {
    term_leave();
    out_str("\r\n");
    shell_exec_line(cmd);
    fputs("\nPress Enter to return to the desktop...", stdout);
    fflush(stdout);
    char c;
    while (read(0, &c, 1) == 1 && c != '\n') ;
    term_enter();
    full_redraw();
}

/* ================= Settings ================= */

enum { R_HEAD, R_INFO, R_TOGGLE, R_CHOICE, R_TEXT, R_BUTTON, R_APP };
enum { I_OS, I_KERNEL, I_MEM, I_UPTIME, I_TIME, I_USER, I_HOME, I_ACCOUNTS, I_RAM, I_DATA, I_POLICY, I_REG };
enum { B_REBOOT, B_HALT, B_LOGOUT, B_SETTIME, B_PASSWD, B_REGEDIT, B_RESET };
struct row { int kind; const char *label, *sub, *key, *opts, *def; int sys, id; };

#define HEAD(l)            { R_HEAD, l, 0, 0, 0, 0, 0, 0 }
#define INFO(l, s, i)      { R_INFO, l, s, 0, 0, 0, 0, i }
#define TOG(l, s, k, d)    { R_TOGGLE, l, s, k, 0, d, 0, 0 }
#define CHO(l, s, k, o, d) { R_CHOICE, l, s, k, o, d, 0, 0 }
#define BTN(l, s, i)       { R_BUTTON, l, s, 0, 0, 0, 0, i }

static const struct row r_system[] = {
    HEAD("About"),
    INFO("System", "Name and version", I_OS),
    INFO("Kernel", "The Linux kernel underneath", I_KERNEL),
    INFO("Memory", "Total and free", I_MEM),
    INFO("Uptime", "Time since boot", I_UPTIME),
    HEAD("Startup"),
    TOG("Open the desktop at login", "Start here instead of the command line", "Desktop/AutoStart", "0"),
    HEAD("Power"),
    BTN("Restart", "Reboot the computer", B_REBOOT),
    BTN("Shut down", "Turn the computer off (needs power-off support)", B_HALT),
    BTN("Log out", "End this session", B_LOGOUT),
};
static const struct row r_display[] = {
    HEAD("Look"),
    CHO("Theme", "Colors of windows and taskbar", "Display/Theme", "Dark|Light|Ember", "Dark"),
    CHO("Accent color", "Highlights, buttons and the start tile", "Display/Accent", "Flame|Orange|Cyan|Green|Magenta|Gold", "Flame"),
    CHO("Desktop background", "Pattern behind the windows", "Desktop/Background", "Solid|Dots|Grid", "Dots"),
    CHO("Borders", "Lines draw boxes, ASCII works on any screen", "Display/Borders", "Lines|ASCII", "Lines"),
    HEAD("Screen"),
    CHO("Screen size", "Auto detects; pick a size if it looks wrong", "Display/Size", "Auto|80x24|100x30|120x35|132x43", "Auto"),
};
static const struct row r_taskbar[] = {
    HEAD("Layout"),
    CHO("Position", "Where the taskbar sits", "Taskbar/Position", "Top|Bottom", "Top"),
    TOG("Show app names", "Off shows two-letter symbols only", "Taskbar/Labels", "1"),
    TOG("Window buttons", "The X  O  \xe2\x80\x94 buttons (close, desktop, hide)", "Taskbar/Controls", "1"),
    { R_TEXT, "Pinned apps", "Comma-separated: settings, registry, or any command", "Taskbar/Pinned", 0, "settings,registry", 0, 0 },
    HEAD("Clock"),
    TOG("Show clock", "Time on the right of the taskbar", "Taskbar/Clock", "1"),
    CHO("Time format", "24-hour or 12-hour", "Taskbar/Format", "24-hour|12-hour", "24-hour"),
    TOG("Show seconds", "Update every second", "Taskbar/Seconds", "0"),
    TOG("Show date", "Weekday and date next to the time", "Taskbar/Date", "1"),
};
static const struct row r_time[] = {
    HEAD("Clock"),
    INFO("Now (UTC)", "From the computer's clock", I_TIME),
    CHO("Time zone offset", "Hours from UTC, used by the taskbar", "Clock/Offset",
        "-12|-11|-10|-9|-8|-7|-6|-5|-4|-3|-2|-1|0|1|2|3|4|5|6|7|8|9|10|11|12|13|14", "0"),
    HEAD("Set the clock"),
    BTN("Set date and time", "Needs the root account", B_SETTIME),
};
static const struct row r_accounts[] = {
    HEAD("You"),
    INFO("Signed in as", "Your account", I_USER),
    INFO("Home folder", "Where your files and settings live", I_HOME),
    INFO("Accounts", "Everyone who can log in", I_ACCOUNTS),
    HEAD("Actions"),
    BTN("Change password", "Opens the password tool", B_PASSWD),
    BTN("Log out", "End this session", B_LOGOUT),
};
static const struct row r_storage[] = {
    HEAD("Memory and disks"),
    INFO("System memory", "RAM used by the system and files", I_RAM),
    INFO("Data disk", "Mounted on /data when a disk is attached", I_DATA),
    INFO("Registry", "Stored settings and variables", I_REG),
};
static const struct row r_security[] = {
    HEAD("Passwords"),
    INFO("Rules in effect", "Applies to new passwords and failed logins", I_POLICY),
    { R_TEXT, "Minimum password length", "System setting, root only (4 to 64)", "Security/MinPasswordLength", 0, "8", 1, 0 },
    { R_TEXT, "Failed-login delay (seconds)", "System setting, root only (0 to 30)", "Security/LoginDelay", 0, "2", 1, 0 },
};
static const struct row r_terminal[] = {
    HEAD("Command line"),
    CHO("Prompt style", "Arrow shows  /folder ---->  Classic shows  /folder $", "Shell/Prompt", "Arrow|Classic", "Arrow"),
    TOG("Show user name", "Put your name in front of the folder", "Shell/ShowUser", "0"),
};
static const struct row r_advanced[] = {
    HEAD("Tools"),
    BTN("Open Registry Editor", "View and change every stored value", B_REGEDIT),
    HEAD("Reset"),
    BTN("Reset my settings", "Back to defaults; environment variables are kept", B_RESET),
};

struct cat { const char *name, *sym, *desc; const struct row *rows; int n; };
static const struct cat cats[] = {
    { "System",     "Sy", "About, startup, power",     r_system,   N(r_system) },
    { "Display",    "Dp", "Theme, colors, screen",     r_display,  N(r_display) },
    { "Taskbar",    "Tb", "Position, clock, pinned",   r_taskbar,  N(r_taskbar) },
    { "Date & time","Dt", "Clock and time zone",       r_time,     N(r_time) },
    { "Apps",       "Ap", "Installed programs, pins",  NULL,       -1 },
    { "Accounts",   "Ac", "Your account and users",    r_accounts, N(r_accounts) },
    { "Storage",    "St", "Memory and disk space",     r_storage,  N(r_storage) },
    { "Security",   "Sc", "Password rules",            r_security, N(r_security) },
    { "Terminal",   "Tm", "Prompt and shell",          r_terminal, N(r_terminal) },
    { "Advanced",   "Ad", "Registry and reset",        r_advanced, N(r_advanced) },
};
#define NCATS N(cats)

static struct { int page, sel, scroll, hsel, cols; } st = { -1, -1, 0, 0, 2 };

static int cat_nrows(int c) { return cats[c].n < 0 ? napps : cats[c].n; }
static void get_row(int c, int i, struct row *r, char *lab) {
    if (cats[c].n >= 0) { *r = cats[c].rows[i]; return; }
    memset(r, 0, sizeof *r);
    r->kind = R_APP;
    snprintf(lab, 40, "%s", apps[i]);
    r->label = lab;
    r->sub = "Show this program on the taskbar";
}
static int selectable(const struct row *r) { return r->kind >= R_TOGGLE; }

static void fmt_mb(char *b, size_t n, unsigned long long bytes) {
    if (bytes >= (1ULL << 30)) snprintf(b, n, "%.1f GB", (double)bytes / (1 << 30));
    else snprintf(b, n, "%.1f MB", (double)bytes / (1 << 20));
}

static void clock_str(char *b, size_t n, int full) {
    time_t t = time(NULL) + (time_t)cf.offset * 3600;
    struct tm tm;
    gmtime_r(&t, &tm);
    char d[40] = "", tt[40];
    if (full) { strftime(b, n, "%Y-%m-%d %H:%M:%S", &tm); return; }
    if (cf.date) strftime(d, sizeof d, "%a %d %b  ", &tm);
    strftime(tt, sizeof tt, cf.h12 ? (cf.secs ? "%I:%M:%S %p" : "%I:%M %p") : (cf.secs ? "%H:%M:%S" : "%H:%M"), &tm);
    snprintf(b, n, "%s%s", d, tt);
}

static void info_text(int id, char *b, size_t n) {
    struct sysinfo si;
    struct utsname u;
    struct statvfs sv;
    b[0] = 0;
    switch (id) {
    case I_OS: snprintf(b, n, "%s %s", OS_NAME, OS_VERSION_LONG); break;
    case I_KERNEL: if (uname(&u) == 0) snprintf(b, n, "%s %s %s", u.sysname, u.release, u.machine); break;
    case I_MEM: if (sysinfo(&si) == 0) {
        unsigned long long unit = si.mem_unit ? si.mem_unit : 1, tot = si.totalram * unit, fre = si.freeram * unit;
        char a[24], c[24]; fmt_mb(a, sizeof a, tot); fmt_mb(c, sizeof c, fre);
        snprintf(b, n, "%s total, %s free", a, c); } break;
    case I_UPTIME: if (sysinfo(&si) == 0) snprintf(b, n, "%ldd %02ld:%02ld:%02ld", si.uptime / 86400, (si.uptime / 3600) % 24, (si.uptime / 60) % 60, si.uptime % 60); break;
    case I_TIME: clock_str(b, n, 1); strncat(b, " UTC", n - strlen(b) - 1); break;
    case I_USER: { users_load(); const struct user *me = user_by_uid(getuid());
        snprintf(b, n, "%s (uid %u)", me ? me->name : "unknown", getuid()); } break;
    case I_HOME: snprintf(b, n, "%s", getenv("HOME") ? getenv("HOME") : "?"); break;
    case I_ACCOUNTS: { users_load(); int c = 0; b[0] = 0;
        for (uid_t uid = 0; uid < 70000 && c < 6; uid += (uid == 0 ? 1000 : 1)) {
            const struct user *u2 = user_by_uid(uid);
            if (!u2) { if (uid >= 1000 && uid > users_next_uid()) break; continue; }
            if (c++) strncat(b, ", ", n - strlen(b) - 1);
            strncat(b, u2->name, n - strlen(b) - 1);
        } if (!c) snprintf(b, n, "none"); } break;
    case I_RAM: if (statvfs("/", &sv) == 0 && sv.f_blocks) {
        char a[24], c[24];
        fmt_mb(a, sizeof a, (unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize);
        fmt_mb(c, sizeof c, (unsigned long long)sv.f_blocks * sv.f_frsize);
        snprintf(b, n, "%s used of %s", a, c); }
        else if (sysinfo(&si) == 0) { char a[24]; fmt_mb(a, sizeof a, (unsigned long long)si.totalram * (si.mem_unit ? si.mem_unit : 1)); snprintf(b, n, "RAM %s", a); }
        break;
    case I_DATA: if (access("/data/etc", F_OK) == 0 && statvfs("/data", &sv) == 0) {
        char a[24], c[24];
        fmt_mb(a, sizeof a, (unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize);
        fmt_mb(c, sizeof c, (unsigned long long)sv.f_blocks * sv.f_frsize);
        snprintf(b, n, "%s used of %s", a, c); } else snprintf(b, n, "No disk attached");
        break;
    case I_POLICY: snprintf(b, n, "%d+ characters, %ds delay", reg_get(HIVE_SYSTEM, "Security/MinPasswordLength", NULL) ? atoi(reg_get(HIVE_SYSTEM, "Security/MinPasswordLength", "8")) : 8,
                            reg_get(HIVE_SYSTEM, "Security/LoginDelay", NULL) ? atoi(reg_get(HIVE_SYSTEM, "Security/LoginDelay", "2")) : 2); break;
    case I_REG: snprintf(b, n, "System %d values, User %d values", reg_count(HIVE_SYSTEM), reg_count(HIVE_USER)); break;
    }
}

static void row_value(const struct row *r, char *b, size_t n) {
    int hv = r->sys ? HIVE_SYSTEM : HIVE_USER;
    switch (r->kind) {
    case R_TOGGLE: snprintf(b, n, "%s", get_toggle(hv, r->key, r->def) ? "ON" : "OFF"); break;
    case R_CHOICE: {
        const char *v = hv == HIVE_SYSTEM ? reg_get(hv, r->key, r->def) : reg_eff(r->key, r->def);
        int i = opt_index(r->opts, v);
        if (i < 0) i = opt_index(r->opts, r->def);
        opt_get(r->opts, i < 0 ? 0 : i, b, n);
        break; }
    case R_TEXT: snprintf(b, n, "%s", hv == HIVE_SYSTEM ? reg_get(hv, r->key, r->def) : reg_eff(r->key, r->def)); break;
    case R_INFO: info_text(r->id, b, n); break;
    default: b[0] = 0;
    }
}

static struct win *open_win(int kind);

static void row_activate(int ci, int ri, int dir) {
    struct row r;
    char lab[48], val[160];
    get_row(ci, ri, &r, lab);
    int hv = r.sys ? HIVE_SYSTEM : HIVE_USER;
    switch (r.kind) {
    case R_TOGGLE:
        if (reg_set(hv, r.key, get_toggle(hv, r.key, r.def) ? "0" : "1") < 0) save_error(r.label);
        break;
    case R_CHOICE: {
        row_value(&r, val, sizeof val);
        int idx = opt_index(r.opts, val), cnt = opt_count(r.opts);
        if (idx < 0) idx = 0;
        idx = (idx + dir + cnt) % cnt;
        opt_get(r.opts, idx, val, sizeof val);
        if (reg_set(hv, r.key, val) < 0) save_error(r.label);
        break; }
    case R_TEXT:
        row_value(&r, val, sizeof val);
        pend_hive = hv;
        snprintf(pend_key, sizeof pend_key, "%s", r.key);
        dlg_open(D_INPUT, P_SET_TEXT, r.label, r.sub ? r.sub : "", val);
        break;
    case R_APP: set_pinned(lab, !is_pinned(lab)); break;
    case R_BUTTON:
        switch (r.id) {
        case B_REBOOT: dlg_open(D_CONFIRM, P_C_REBOOT, "Restart", "Restart the computer now?", NULL); break;
        case B_HALT: dlg_open(D_CONFIRM, P_C_HALT, "Shut down", "Shut down? This build halts the system (close QEMU after).", NULL); break;
        case B_LOGOUT: dlg_open(D_CONFIRM, P_C_LOGOUT, "Log out", "End this session?", NULL); break;
        case B_RESET: dlg_open(D_CONFIRM, P_C_RESET, "Reset settings", "Reset all your settings to the defaults?", NULL); break;
        case B_SETTIME: { char t[40]; clock_str(t, sizeof t, 1); t[16] = 0;
            time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm); strftime(t, sizeof t, "%Y-%m-%d %H:%M", &tm);
            dlg_open(D_INPUT, P_SET_TIME, "Set date and time", "Enter UTC time as YYYY-MM-DD HH:MM", t); } break;
        case B_PASSWD: launch_external("passwd"); break;
        case B_REGEDIT: open_win(WK_REGISTRY); break;
        }
        break;
    }
    load_cfg();
}

static void open_cat(int c) {
    st.page = c; st.scroll = 0; st.sel = -1;
    if (cats[c].n < 0) scan_apps();
    for (int i = 0; i < cat_nrows(c); i++) {
        struct row r; char lab[48];
        get_row(c, i, &r, lab);
        if (selectable(&r)) { st.sel = i; break; }
    }
}

static int pill(int xr, int y, const char *s, uint32_t fg, uint32_t bg, int at) {
    int w = ulen(s) + 2;
    text(xr - w, y, " ", fg, bg, 0, 1);
    text(xr - w + 1, y, s, fg, bg, at, -1);
    text(xr - 1, y, " ", fg, bg, 0, 1);
    return w;
}

static void settings_draw(int x, int y, int cw, int ch) {
    const struct pal *p = &cf.p;
    set_clip(x, y, cw, ch);
    fill(x, y, cw, ch, p->winfg, p->win);
    if (st.page < 0) {                                       /* home: every category */
        text(x + 2, y, "Settings", cf.acc, p->win, AT_BOLD, -1);
        text(x + 2, y + 1, "Choose a category", p->muted, p->win, 0, -1);
        int cols = cw >= 66 ? 2 : 1;
        int tw = (cw - 2 - (cols - 1)) / cols;
        st.cols = cols;
        set_clip(x, y + 2, cw, ch - 2);
        for (int i = 0; i < NCATS; i++) {
            int tx = x + 1 + (i % cols) * (tw + 1), ty = y + 2 + (i / cols) * 3 - st.scroll;
            int hi = i == st.hsel;
            if (hi) fill(tx, ty, tw, 3, p->winfg, p->sel);
            uint32_t bg = hi ? p->sel : p->win, bc = hi ? cf.acc : p->border;
            put(tx + 1, ty, cf.ascii ? '+' : 0x250C, bc, bg, 0); put(tx + 2, ty, cf.ascii ? '-' : 0x2500, bc, bg, 0);
            put(tx + 3, ty, cf.ascii ? '-' : 0x2500, bc, bg, 0); put(tx + 4, ty, cf.ascii ? '+' : 0x2510, bc, bg, 0);
            put(tx + 1, ty + 1, cf.ascii ? '|' : 0x2502, bc, bg, 0); put(tx + 4, ty + 1, cf.ascii ? '|' : 0x2502, bc, bg, 0);
            text(tx + 2, ty + 1, cats[i].sym, cf.acc, bg, AT_BOLD, 2);
            put(tx + 1, ty + 2, cf.ascii ? '+' : 0x2514, bc, bg, 0); put(tx + 2, ty + 2, cf.ascii ? '-' : 0x2500, bc, bg, 0);
            put(tx + 3, ty + 2, cf.ascii ? '-' : 0x2500, bc, bg, 0); put(tx + 4, ty + 2, cf.ascii ? '+' : 0x2518, bc, bg, 0);
            text(tx + 6, ty, cats[i].name, p->winfg, bg, AT_BOLD, tw - 7);
            text(tx + 6, ty + 1, cats[i].desc, p->muted, bg, 0, tw - 7);
            hot_add(tx, ty, tw, 3, A_SET_CAT, i, 0);
        }
        return;
    }
    /* category view: nav on the left, settings on the right */
    text(x + 1, y, "Settings", cf.acc, p->win, AT_BOLD, 16);
    textf(x + 1, y + 1, p->muted, p->win, 0, 16, "%s Home", cf.ascii ? "<" : "\xe2\x80\xb9");
    hot_add(x, y + 1, 18, 1, A_SET_HOME, 0, 0);
    for (int i = 0; i < NCATS; i++) {
        int sel = i == st.page;
        fill(x, y + 2 + i, 18, 1, sel ? cf.accfg : p->winfg, sel ? cf.acc : p->win);
        textf(x + 1, y + 2 + i, sel ? cf.accfg : cf.acc, sel ? cf.acc : p->win, AT_BOLD, 2, "%s", cats[i].sym);
        text(x + 4, y + 2 + i, cats[i].name, sel ? cf.accfg : p->winfg, sel ? cf.acc : p->win, sel ? AT_BOLD : 0, 13);
        hot_add(x, y + 2 + i, 18, 1, A_SET_CAT, i, 0);
    }
    for (int j = 0; j < ch; j++) put(x + 18, y + j, cf.ascii ? '|' : 0x2502, p->border, p->win, 0);
    int x0 = x + 20, pw = cw - 21;
    text(x0, y, cats[st.page].name, cf.acc, p->win, AT_BOLD, pw);
    text(x0, y + 1, cats[st.page].desc, p->muted, p->win, 0, pw);
    set_clip(x0 - 1, y + 2, pw + 2, ch - 2);
    int nr = cat_nrows(st.page);
    if (nr == 0) text(x0, y + 3, "Nothing installed yet. Install a program with: install FILE", p->muted, p->win, 0, pw);
    for (int i = 0; i < nr; i++) {
        struct row r; char lab[48], val[160];
        get_row(st.page, i, &r, lab);
        int ty = y + 2 + 2 * i - st.scroll;
        if (ty + 2 <= y + 2 || ty >= y + ch) continue;
        if (r.kind == R_HEAD) { text(x0, ty + 1, r.label, cf.acc, p->win, AT_BOLD | AT_UL, pw); continue; }
        int sel = i == st.sel && selectable(&r);
        uint32_t bg = sel ? p->sel : p->win;
        if (sel) fill(x0 - 1, ty, pw + 2, 2, p->winfg, bg);
        text(x0, ty, r.label, p->winfg, bg, AT_BOLD, pw - 2);
        if (r.sub) text(x0, ty + 1, r.sub, p->muted, bg, 0, pw - 2);
        int xr = x0 + pw;
        if (r.kind == R_INFO) {
            info_text(r.id, val, sizeof val);
            int vl = ulen(val), room = pw - ulen(r.label) - 3;
            if (vl > room) vl = room > 0 ? room : 0;
            text(xr - vl, ty, val, cf.acc, bg, 0, vl);
        } else if (r.kind == R_TOGGLE || r.kind == R_APP) {
            int on = r.kind == R_APP ? is_pinned(lab) : get_toggle(r.sys ? HIVE_SYSTEM : HIVE_USER, r.key, r.def);
            pill(xr, ty, r.kind == R_APP ? (on ? "Pinned" : "Not pinned") : (on ? " ON " : " OFF "), on ? cf.accfg : p->winfg, on ? cf.acc : p->titleoff, AT_BOLD);
        } else if (r.kind == R_CHOICE) {
            row_value(&r, val, sizeof val);
            char s[100];
            snprintf(s, sizeof s, "%s %s %s", cf.ascii ? "<" : "\xe2\x80\xb9", val, cf.ascii ? ">" : "\xe2\x80\xba");
            pill(xr, ty, s, p->winfg, p->titleoff, 0);
        } else if (r.kind == R_TEXT) {
            row_value(&r, val, sizeof val);
            if (ulen(val) > 22) { val[21] = 0; strcat(val, "~"); }
            pill(xr, ty, val[0] ? val : "(empty)", p->winfg, p->titleoff, 0);
        } else if (r.kind == R_BUTTON) {
            int danger = r.id == B_HALT || r.id == B_REBOOT || r.id == B_RESET;
            pill(xr, ty, r.id == B_REGEDIT ? "Open" : r.id == B_PASSWD ? "Change" : r.id == B_LOGOUT ? "Log out" : r.id == B_SETTIME ? "Set" : "Run",
                 danger ? 0xFFFFFF : cf.accfg, danger ? p->bad : cf.acc, AT_BOLD);
        }
        if (selectable(&r)) hot_add(x0 - 1, ty, pw + 2, 2, A_SET_ROW, i, 0);
    }
}

static void settings_scroll_to_sel(int ch) {
    int view = ch - 2, top = 2 * st.sel;
    if (st.sel < 0) return;
    if (top < st.scroll) st.scroll = top;
    if (top + 2 > st.scroll + view) st.scroll = top + 2 - view;
    if (st.scroll < 0) st.scroll = 0;
}

static void settings_key(int k, int ch) {
    if (st.page < 0) {
        int cols = st.cols < 1 ? 1 : st.cols;
        if (k == K_LEFT && st.hsel > 0) st.hsel--;
        else if (k == K_RIGHT && st.hsel < NCATS - 1) st.hsel++;
        else if (k == K_UP && st.hsel - cols >= 0) st.hsel -= cols;
        else if (k == K_DOWN && st.hsel + cols < NCATS) st.hsel += cols;
        else if (k == 13 || k == ' ') open_cat(st.hsel);
        int top = (st.hsel / cols) * 3, view = ch - 2;
        if (top < st.scroll) st.scroll = top;
        if (top + 3 > st.scroll + view) st.scroll = top + 3 - view;
        return;
    }
    int nr = cat_nrows(st.page);
    if (k == K_UP || k == K_DOWN) {
        int d = k == K_UP ? -1 : 1;
        for (int i = st.sel + d; i >= 0 && i < nr; i += d) {
            struct row r; char lab[48];
            get_row(st.page, i, &r, lab);
            if (selectable(&r)) { st.sel = i; break; }
        }
        settings_scroll_to_sel(ch);
    } else if (k == K_LEFT || k == K_RIGHT) {
        if (st.sel >= 0) {
            struct row r; char lab[48];
            get_row(st.page, st.sel, &r, lab);
            if (r.kind == R_CHOICE) row_activate(st.page, st.sel, k == K_LEFT ? -1 : 1);
            else if (r.kind == R_TOGGLE || r.kind == R_APP) row_activate(st.page, st.sel, 1);
        }
    } else if (k == 13 || k == ' ') { if (st.sel >= 0) row_activate(st.page, st.sel, 1); }
    else if (k == 9) open_cat((st.page + 1) % NCATS);
    else if (k == K_STAB) open_cat((st.page + NCATS - 1) % NCATS);
    else if (k == 27 || k == 127 || k == 8) { st.page = -1; st.scroll = 0; }
    else if (k == K_PGDN) { st.scroll += 6; }
    else if (k == K_PGUP) { st.scroll -= 6; if (st.scroll < 0) st.scroll = 0; }
}

/* ================= Registry editor ================= */

#define MAXK 160
struct tnode { int hive; char path[MAXK]; int depth; char name[64]; int kids, open; };
static struct tnode tn[400];
static int ntn;
static char expd[80][MAXK + 4];
static int nexp;
static int vlist[400], nvl;
static struct { int focus, tsel, vsel, tscroll, vscroll; } rg;

static int exp_find(int hv, const char *path) {
    char k[MAXK + 4];
    snprintf(k, sizeof k, "%d:%s", hv, path);
    for (int i = 0; i < nexp; i++) if (!strcmp(expd[i], k)) return i;
    return -1;
}
static void exp_set(int hv, const char *path, int on) {
    int i = exp_find(hv, path);
    if (on && i < 0 && nexp < N(expd)) snprintf(expd[nexp++], sizeof expd[0], "%d:%s", hv, path);
    if (!on && i >= 0) { memmove(expd[i], expd[i + 1], (size_t)(nexp - i - 1) * sizeof expd[0]); nexp--; }
}

static char dirs_tmp[500][MAXK];
static int cmp_dir(const void *a, const void *b) {
    const char *x = a, *y = b;
    for (;; x++, y++) {
        unsigned char cx = *x == '/' ? 1 : (unsigned char)*x, cy = *y == '/' ? 1 : (unsigned char)*y;
        if (cx != cy) return cx < cy ? -1 : 1;
        if (!cx) return 0;
    }
}

static void build_tree(void) {
    ntn = 0;
    for (int hv = 0; hv < 2; hv++) {
        int nd = 0, n = reg_count(hv);
        for (int i = 0; i < n && nd < N(dirs_tmp) - 8; i++) {
            const char *key = reg_entry(hv, i, NULL);
            for (const char *s = strchr(key, '/'); s; s = strchr(s + 1, '/')) {
                size_t l = (size_t)(s - key);
                if (l >= MAXK) break;
                memcpy(dirs_tmp[nd], key, l);
                dirs_tmp[nd][l] = 0;
                nd++;
                if (nd >= N(dirs_tmp)) break;
            }
        }
        qsort(dirs_tmp, (size_t)nd, MAXK, cmp_dir);
        int u = 0;
        for (int i = 0; i < nd; i++) if (u == 0 || strcmp(dirs_tmp[u - 1], dirs_tmp[i])) { if (u != i) memcpy(dirs_tmp[u], dirs_tmp[i], MAXK); u++; }
        nd = u;
        if (ntn >= N(tn)) return;
        struct tnode *r = &tn[ntn++];
        memset(r, 0, sizeof *r);
        r->hive = hv; r->depth = 0; r->kids = nd > 0; r->open = exp_find(hv, "") >= 0;
        snprintf(r->name, sizeof r->name, "%s", reg_hive_name(hv));
        if (!r->open) continue;
        for (int i = 0; i < nd && ntn < N(tn); i++) {
            const char *path = dirs_tmp[i];
            int vis = 1, depth = 1;
            for (const char *s = strchr(path, '/'); s; s = strchr(s + 1, '/')) {
                char pre[MAXK];
                size_t l = (size_t)(s - path);
                memcpy(pre, path, l); pre[l] = 0;
                if (exp_find(hv, pre) < 0) vis = 0;
                depth++;
            }
            if (!vis) continue;
            struct tnode *t = &tn[ntn++];
            memset(t, 0, sizeof *t);
            t->hive = hv; t->depth = depth;
            snprintf(t->path, sizeof t->path, "%s", path);
            const char *last = strrchr(path, '/');
            snprintf(t->name, sizeof t->name, "%s", last ? last + 1 : path);
            size_t pl = strlen(path);
            t->kids = i + 1 < nd && !strncmp(dirs_tmp[i + 1], path, pl) && dirs_tmp[i + 1][pl] == '/';
            t->open = exp_find(hv, path) >= 0;
        }
    }
    if (rg.tsel >= ntn) rg.tsel = ntn - 1;
    if (rg.tsel < 0) rg.tsel = 0;
}

static void build_values(void) {
    nvl = 0;
    if (rg.tsel < 0 || rg.tsel >= ntn) return;
    const struct tnode *t = &tn[rg.tsel];
    size_t pl = strlen(t->path);
    for (int i = 0; i < reg_count(t->hive) && nvl < N(vlist); i++) {
        const char *key = reg_entry(t->hive, i, NULL);
        const char *rest;
        if (pl == 0) rest = key;
        else if (!strncmp(key, t->path, pl) && key[pl] == '/') rest = key + pl + 1;
        else continue;
        if (strchr(rest, '/')) continue;
        vlist[nvl++] = i;
    }
    if (rg.vsel >= nvl) rg.vsel = nvl - 1;
    if (rg.vsel < 0) rg.vsel = 0;
}

static void reg_full_key(const struct tnode *t, const char *name, char *out, size_t n) {
    if (t->path[0]) snprintf(out, n, "%s/%s", t->path, name); else snprintf(out, n, "%s", name);
}

static const char *reg_val_name(const struct tnode *t, int vi) {
    const char *key = reg_entry(t->hive, vlist[vi], NULL);
    return t->path[0] ? key + strlen(t->path) + 1 : key;
}

static void registry_draw(int x, int y, int cw, int ch) {
    const struct pal *p = &cf.p;
    build_tree();
    build_values();
    set_clip(x, y, cw, ch);
    fill(x, y, cw, ch, p->winfg, p->win);
    int lw = cw / 3 < 20 ? 20 : (cw / 3 > 30 ? 30 : cw / 3);
    int view = ch - 3;
    for (int j = 0; j < ch; j++) put(x + lw, y + j, cf.ascii ? '|' : 0x2502, p->border, p->win, 0);
    /* tree */
    text(x + 1, y, "Keys", cf.acc, p->win, AT_BOLD, lw - 2);
    set_clip(x, y + 1, lw, view);
    if (rg.tsel < rg.tscroll) rg.tscroll = rg.tsel;
    if (rg.tsel >= rg.tscroll + view) rg.tscroll = rg.tsel - view + 1;
    for (int i = 0; i < ntn; i++) {
        int ty = y + 1 + i - rg.tscroll;
        if (ty < y + 1 || ty >= y + 1 + view) continue;
        const struct tnode *t = &tn[i];
        int sel = i == rg.tsel;
        uint32_t bg = sel ? (rg.focus == 0 ? cf.acc : p->sel) : p->win, fg = sel ? (rg.focus == 0 ? cf.accfg : p->winfg) : p->winfg;
        fill(x, ty, lw, 1, fg, bg);
        int tx = x + 1 + t->depth * 2;
        const char *mk = t->kids ? (t->open ? (cf.ascii ? "-" : "\xe2\x96\xbe") : (cf.ascii ? "+" : "\xe2\x96\xb8")) : " ";
        text(tx, ty, mk, sel ? fg : cf.acc, bg, 0, 1);
        text(tx + 2, ty, t->name, fg, bg, t->depth == 0 ? AT_BOLD : 0, lw - (tx + 2 - x) - 1);
        hot_add(x, ty, lw, 1, A_REG_NODE, i, 0);
    }
    /* values */
    int rx = x + lw + 2, rw = cw - lw - 3;
    set_clip(x + lw + 1, y, cw - lw - 1, ch);
    const struct tnode *sel = rg.tsel < ntn ? &tn[rg.tsel] : NULL;
    int ro = sel && sel->hive == HIVE_SYSTEM && getuid() != 0;
    textf(rx, y, cf.acc, p->win, AT_BOLD, rw, "Values%s", ro ? "  (read-only: root can change the System hive)" : "");
    int nw = rw / 3 < 12 ? 12 : (rw / 3 > 24 ? 24 : rw / 3);
    text(rx, y + 1, "Name", p->muted, p->win, AT_UL, nw);
    text(rx + nw + 1, y + 1, "Value", p->muted, p->win, AT_UL, rw - nw - 1);
    int vview = ch - 4;
    set_clip(x + lw + 1, y + 2, cw - lw - 1, vview);
    if (rg.vsel < rg.vscroll) rg.vscroll = rg.vsel;
    if (rg.vsel >= rg.vscroll + vview) rg.vscroll = rg.vsel - vview + 1;
    if (rg.vscroll < 0) rg.vscroll = 0;
    for (int i = 0; sel && i < nvl; i++) {
        int ty = y + 2 + i - rg.vscroll;
        if (ty < y + 2 || ty >= y + 2 + vview) continue;
        int s = i == rg.vsel;
        uint32_t bg = s ? (rg.focus == 1 ? cf.acc : p->sel) : p->win, fg = s && rg.focus == 1 ? cf.accfg : p->winfg;
        fill(x + lw + 1, ty, cw - lw - 1, 1, fg, bg);
        const char *v;
        reg_entry(sel->hive, vlist[i], &v);
        char one[120];
        snprintf(one, sizeof one, "%s", v);
        for (char *c = one; *c; c++) if (*c == '\n') *c = ' ';
        text(rx, ty, reg_val_name(sel, i), fg, bg, AT_BOLD, nw);
        text(rx + nw + 1, ty, one, fg, bg, 0, rw - nw - 1);
        hot_add(x + lw + 1, ty, cw - lw - 1, 1, A_REG_VAL, i, 0);
    }
    if (sel && nvl == 0) text(rx, y + 3, "No values here. Press N to add one.", p->muted, p->win, 0, rw);
    set_clip(x, y, cw, ch);
    fill(x, y + ch - 1, cw, 1, p->muted, p->titleoff);
    char path[300];
    snprintf(path, sizeof path, "%s%s%s", sel ? reg_hive_name(sel->hive) : "", sel && sel->path[0] ? "\\" : "", sel ? sel->path : "");
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
    textf(x + 1, y + ch - 1, p->titlefgoff, p->titleoff, 0, cw - 2, "%s   N new  Enter edit  Del delete  Tab switch", path);
}

static void reg_new_value(void) {
    if (rg.tsel >= ntn) return;
    const struct tnode *t = &tn[rg.tsel];
    pend_hive = t->hive;
    snprintf(pend_key, sizeof pend_key, "%s", t->path);
    dlg_open(D_INPUT, P_REG_NEWNAME, "New value", "Name of the new value (letters, digits, / for sub-keys):", "");
}
static void reg_edit_value(void) {
    build_values();
    if (rg.tsel >= ntn || nvl == 0) return;
    const struct tnode *t = &tn[rg.tsel];
    const char *v;
    reg_entry(t->hive, vlist[rg.vsel], &v);
    pend_hive = t->hive;
    reg_full_key(t, reg_val_name(t, rg.vsel), pend_key, sizeof pend_key);
    char msg[260];
    snprintf(msg, sizeof msg, "New value for %s:", pend_key);
    dlg_open(D_INPUT, P_REG_EDIT, "Edit value", msg, v);
}
static void reg_delete_value(void) {
    build_values();
    if (rg.tsel >= ntn || nvl == 0) return;
    const struct tnode *t = &tn[rg.tsel];
    pend_hive = t->hive;
    reg_full_key(t, reg_val_name(t, rg.vsel), pend_key, sizeof pend_key);
    char msg[260];
    snprintf(msg, sizeof msg, "Delete the value %s?", pend_key);
    dlg_open(D_CONFIRM, P_C_DELVAL, "Delete value", msg, NULL);
}
static void reg_delete_key(void) {
    if (rg.tsel >= ntn || !tn[rg.tsel].path[0]) { toast("Select a key below System or User to delete it"); return; }
    pend_hive = tn[rg.tsel].hive;
    snprintf(pend_key, sizeof pend_key, "%s", tn[rg.tsel].path);
    char msg[260];
    snprintf(msg, sizeof msg, "Delete the key %s and everything in it?", pend_key);
    dlg_open(D_CONFIRM, P_C_DELKEY, "Delete key", msg, NULL);
}
static void reg_expand_all(int on) {
    nexp = 0;
    exp_set(HIVE_SYSTEM, "", 1); exp_set(HIVE_USER, "", 1);
    if (on)
        for (int hv = 0; hv < 2; hv++)
            for (int i = 0; i < reg_count(hv); i++) {
                const char *key = reg_entry(hv, i, NULL);
                for (const char *s = strchr(key, '/'); s; s = strchr(s + 1, '/')) {
                    char pre[MAXK];
                    size_t l = (size_t)(s - key);
                    if (l >= MAXK) break;
                    memcpy(pre, key, l); pre[l] = 0;
                    exp_set(hv, pre, 1);
                }
            }
    rg.tsel = 0; rg.vsel = 0;
}

static void registry_key(int k, int ch) {
    (void)ch;
    build_tree();
    build_values();
    if (k == 9) { rg.focus = !rg.focus; return; }
    if (k == 'n' || k == 'N') { reg_new_value(); return; }
    if (k == 'r' || k == 'R' || k == K_F5) { reg_refresh(); return; }
    if (rg.focus == 0) {
        const struct tnode *t = rg.tsel < ntn ? &tn[rg.tsel] : NULL;
        if (k == K_UP && rg.tsel > 0) { rg.tsel--; rg.vsel = 0; }
        else if (k == K_DOWN && rg.tsel < ntn - 1) { rg.tsel++; rg.vsel = 0; }
        else if (t && (k == K_RIGHT || k == 13 || k == ' ')) {
            if (t->kids && !t->open) exp_set(t->hive, t->path, 1);
            else if (k == 13 && t->kids) exp_set(t->hive, t->path, 0);
            else if (k == K_RIGHT) rg.focus = 1;
        } else if (t && k == K_LEFT) {
            if (t->open && t->kids) exp_set(t->hive, t->path, 0);
            else for (int i = rg.tsel - 1; i >= 0; i--) if (tn[i].hive == t->hive && tn[i].depth < t->depth) { rg.tsel = i; rg.vsel = 0; break; }
        } else if (k == K_DEL) reg_delete_key();
        else if (k == K_PGUP) { rg.tsel -= 8; if (rg.tsel < 0) rg.tsel = 0; rg.vsel = 0; }
        else if (k == K_PGDN) { rg.tsel += 8; if (rg.tsel > ntn - 1) rg.tsel = ntn - 1; rg.vsel = 0; }
    } else {
        if (k == K_UP && rg.vsel > 0) rg.vsel--;
        else if (k == K_DOWN && rg.vsel < nvl - 1) rg.vsel++;
        else if (k == 13 || k == 'e' || k == 'E') reg_edit_value();
        else if (k == K_DEL) reg_delete_value();
        else if (k == K_LEFT) rg.focus = 0;
        else if (k == K_PGUP) { rg.vsel -= 8; if (rg.vsel < 0) rg.vsel = 0; }
        else if (k == K_PGDN) { rg.vsel += 8; if (rg.vsel > nvl - 1) rg.vsel = nvl - 1; }
    }
}

/* ================= windows ================= */

static const char *const mn_settings[] = { "File", "View", NULL };
static const char *const mi_set0[] = { "Home page", "Close window", NULL };
static const char *const mi_set1[] = { "Previous category", "Next category", NULL };
static const char *const mn_registry[] = { "File", "Edit", "View", NULL };
static const char *const mi_reg0[] = { "Reload", "Close window", NULL };
static const char *const mi_reg1[] = { "New value...", "Edit value...", "Delete value", "Delete key", NULL };
static const char *const mi_reg2[] = { "Expand all", "Collapse all", NULL };
static const char *const *menu_names(int kind) { return kind == WK_SETTINGS ? mn_settings : mn_registry; }
static const char *const *menu_items(int kind, int m) {
    if (kind == WK_SETTINGS) return m == 0 ? mi_set0 : mi_set1;
    return m == 0 ? mi_reg0 : m == 1 ? mi_reg1 : mi_reg2;
}
static int menu_x[4], menu_sel;

static struct win *open_win(int kind) {
    struct win *w = win_by_kind(kind);
    if (w) { w->minimized = 0; raise_win(w); return w; }
    for (int i = 0; i < N(wins); i++) if (!wins[i].used) { w = &wins[i]; break; }
    if (!w) return NULL;
    memset(w, 0, sizeof *w);
    w->used = 1; w->id = next_id++; w->kind = kind;
    int dh = desk_y1 - desk_y0;
    if (kind == WK_SETTINGS) {
        snprintf(w->title, sizeof w->title, "Settings");
        w->w = W < 76 ? W : 72; w->h = dh < 24 ? dh : 22;
        st.page = -1; st.sel = -1; st.scroll = 0; st.hsel = 0;
        scan_apps();
    } else {
        snprintf(w->title, sizeof w->title, "Registry Editor");
        w->w = W < 74 ? W : 70; w->h = dh < 22 ? dh : 20;
        nexp = 0; exp_set(HIVE_SYSTEM, "", 1); exp_set(HIVE_USER, "", 1);
        memset(&rg, 0, sizeof rg);
    }
    w->x = (W - w->w) / 2 + nz * 2;
    w->y = desk_y0 + (dh - w->h) / 2 + nz;
    clamp_win(w);
    zord[nz++] = w->id;
    return w;
}
static void close_win(struct win *w) {
    int i;
    for (i = 0; i < nz; i++) if (zord[i] == w->id) break;
    for (; i < nz - 1; i++) zord[i] = zord[i + 1];
    if (nz > 0) nz--;
    w->used = 0;
    menu_win = 0;
}

static void draw_window(struct win *w, int active) {
    const struct pal *p = &cf.p;
    cur_wid = w->id;
    clip_all();
    hot_add(w->x, w->y, w->w, w->h, A_WINBODY, w->id, 0);
    uint32_t bc = active ? cf.acc : p->border;
    fill(w->x, w->y, w->w, w->h, p->winfg, p->win);
    uint32_t hh = cf.ascii ? '-' : (active ? 0x2550 : 0x2500), vv = cf.ascii ? '|' : (active ? 0x2551 : 0x2502);
    uint32_t tl = cf.ascii ? '+' : (active ? 0x2554 : 0x250C), tr = cf.ascii ? '+' : (active ? 0x2557 : 0x2510);
    uint32_t bl = cf.ascii ? '+' : (active ? 0x255A : 0x2514), br = cf.ascii ? '+' : (active ? 0x255D : 0x2518);
    for (int i = 1; i < w->w - 1; i++) { put(w->x + i, w->y, hh, bc, p->win, 0); put(w->x + i, w->y + w->h - 1, hh, bc, p->win, 0); }
    for (int j = 1; j < w->h - 1; j++) { put(w->x, w->y + j, vv, bc, p->win, 0); put(w->x + w->w - 1, w->y + j, vv, bc, p->win, 0); }
    put(w->x, w->y, tl, bc, p->win, 0); put(w->x + w->w - 1, w->y, tr, bc, p->win, 0);
    put(w->x, w->y + w->h - 1, bl, bc, p->win, 0);
    put(w->x + w->w - 1, w->y + w->h - 1, active ? (cf.ascii ? '#' : 0x25E2) : br, bc, p->win, 0);
    int ty = w->y + 1;
    uint32_t tbg = active ? cf.acc : p->titleoff, tfg = active ? cf.accfg : p->titlefgoff;
    fill(w->x + 1, ty, w->w - 2, 1, tfg, tbg);
    text(w->x + 2, ty, w->title, tfg, tbg, AT_BOLD, w->w - 4);
    hot_add(w->x + 1, w->y, w->w - 2, 2, A_TITLE, w->id, 0);
    hot_add(w->x, w->y + 1, 1, w->h - 2, A_EDGE, w->id, 1);
    hot_add(w->x + w->w - 1, w->y + 1, 1, w->h - 2, A_EDGE, w->id, 2);
    hot_add(w->x + 1, w->y + w->h - 1, w->w - 2, 1, A_EDGE, w->id, 4);
    hot_add(w->x, w->y + w->h - 1, 1, 1, A_EDGE, w->id, 5);
    hot_add(w->x + w->w - 1, w->y + w->h - 1, 1, 1, A_EDGE, w->id, 6);
    int mx = w->x + 2 + ulen(w->title) + 3;
    const char *const *names = menu_names(w->kind);
    for (int m = 0; names[m]; m++) {
        int len = ulen(names[m]), open = menu_win == w->id && menu_idx == m;
        uint32_t fg = open ? p->winfg : tfg, bg = open ? p->win : tbg;
        text(mx - 1, ty, " ", fg, bg, 0, 1);
        text(mx, ty, names[m], fg, bg, open ? AT_BOLD : 0, len);
        text(mx + len, ty, " ", fg, bg, 0, 1);
        hot_add(mx - 1, ty, len + 2, 1, A_MENU, w->id, m);
        if (active && m < 4) menu_x[m] = mx - 1;
        mx += len + 3;
    }
    put(w->x, w->y + 2, cf.ascii ? '+' : (active ? 0x255F : 0x251C), bc, p->win, 0);
    put(w->x + w->w - 1, w->y + 2, cf.ascii ? '+' : (active ? 0x2562 : 0x2524), bc, p->win, 0);
    for (int i = 1; i < w->w - 1; i++) put(w->x + i, w->y + 2, cf.ascii ? '-' : 0x2500, bc, p->win, 0);
    int cx = w->x + 1, cy = w->y + 3, cw = w->w - 2, ch = w->h - 4;
    set_clip(cx, cy, cw, ch);
    if (w->kind == WK_SETTINGS) settings_draw(cx, cy, cw, ch);
    else registry_draw(cx, cy, cw, ch);
    clip_all();
    cur_wid = 0;
}

static void draw_dropdown(void) {
    struct win *w = win_by_id(menu_win);
    if (!w || w->minimized) { menu_win = 0; return; }
    const struct pal *p = &cf.p;
    clip_all();
    hot_add(0, 0, W, H, A_MENUCLOSE, 0, 0);
    const char *const *items = menu_items(w->kind, menu_idx);
    int n = 0, mw = 0;
    for (; items[n]; n++) if (ulen(items[n]) > mw) mw = ulen(items[n]);
    mw += 4;
    int x = menu_x[menu_idx], y = w->y + 2;
    if (x + mw > W) x = W - mw;
    fill(x, y, mw, n + 2, p->winfg, p->win);
    for (int i = 1; i < mw - 1; i++) { put(x + i, y, cf.ascii ? '-' : 0x2500, cf.acc, p->win, 0); put(x + i, y + n + 1, cf.ascii ? '-' : 0x2500, cf.acc, p->win, 0); }
    for (int j = 1; j <= n; j++) { put(x, y + j, cf.ascii ? '|' : 0x2502, cf.acc, p->win, 0); put(x + mw - 1, y + j, cf.ascii ? '|' : 0x2502, cf.acc, p->win, 0); }
    put(x, y, cf.ascii ? '+' : 0x250C, cf.acc, p->win, 0); put(x + mw - 1, y, cf.ascii ? '+' : 0x2510, cf.acc, p->win, 0);
    put(x, y + n + 1, cf.ascii ? '+' : 0x2514, cf.acc, p->win, 0); put(x + mw - 1, y + n + 1, cf.ascii ? '+' : 0x2518, cf.acc, p->win, 0);
    for (int i = 0; i < n; i++) {
        int s = i == menu_sel;
        fill(x + 1, y + 1 + i, mw - 2, 1, s ? cf.accfg : p->winfg, s ? cf.acc : p->win);
        text(x + 2, y + 1 + i, items[i], s ? cf.accfg : p->winfg, s ? cf.acc : p->win, 0, mw - 4);
        hot_add(x + 1, y + 1 + i, mw - 2, 1, A_MENUITEM, w->id, menu_idx * 16 + i);
    }
}

static void menu_action(struct win *w, int m, int i) {
    if (w->kind == WK_SETTINGS) {
        if (m == 0) { if (i == 0) { st.page = -1; st.scroll = 0; } else close_win(w); }
        else { int d = i == 0 ? -1 : 1; if (st.page < 0) open_cat(d < 0 ? NCATS - 1 : 0); else open_cat((st.page + d + NCATS) % NCATS); }
    } else if (m == 0) { if (i == 0) reg_refresh(); else close_win(w); }
    else if (m == 1) { if (i == 0) reg_new_value(); else if (i == 1) reg_edit_value(); else if (i == 2) reg_delete_value(); else reg_delete_key(); }
    else reg_expand_all(i == 0);
}

/* ================= start menu (element tiles) ================= */

enum { TL_SETTINGS, TL_REGISTRY, TL_DESKTOP, TL_SHELL, TL_LOGOUT, TL_RESTART, TL_SHUTDOWN, TL_APP };
struct tile { char sym[3], name[32]; int kind; char cmd[48]; };
static struct tile tiles[110];
static int ntiles, vis[110], nvis, ssel, sscroll, scols = 4;
static char sq[32];

static void add_tile(const char *sym, const char *name, int kind, const char *cmd) {
    if (ntiles >= N(tiles)) return;
    struct tile *t = &tiles[ntiles++];
    snprintf(t->sym, sizeof t->sym, "%s", sym);
    snprintf(t->name, sizeof t->name, "%s", name);
    t->kind = kind;
    snprintf(t->cmd, sizeof t->cmd, "%s", cmd ? cmd : "");
}
static void filter_tiles(void) {
    nvis = 0;
    for (int i = 0; i < ntiles; i++) if (!sq[0] || strcasestr(tiles[i].name, sq)) vis[nvis++] = i;
    if (ssel >= nvis) ssel = nvis - 1;
    if (ssel < 0) ssel = 0;
}
static void build_tiles(void) {
    ntiles = 0;
    add_tile("St", "Settings", TL_SETTINGS, NULL);
    add_tile("Rg", "Registry", TL_REGISTRY, NULL);
    add_tile("Ds", "Desktop", TL_DESKTOP, NULL);
    add_tile("Sh", "Shell", TL_SHELL, NULL);
    add_tile("Lo", "Log out", TL_LOGOUT, NULL);
    add_tile("Rs", "Restart", TL_RESTART, NULL);
    add_tile("Sd", "Shut down", TL_SHUTDOWN, NULL);
    scan_apps();
    for (int i = 0; i < napps; i++) {
        char sym[3] = { (char)toupper((unsigned char)apps[i][0]), apps[i][1] ? (char)tolower((unsigned char)apps[i][1]) : ' ', 0 };
        add_tile(sym, apps[i], TL_APP, apps[i]);
    }
    sq[0] = 0; ssel = 0; sscroll = 0;
    filter_tiles();
}

static void show_desktop(void) {
    static int saved[4];
    if (!desk_hidden) {
        int any = 0;
        for (int i = 0; i < N(wins); i++) { saved[i] = wins[i].used && !wins[i].minimized; if (saved[i]) { wins[i].minimized = 1; any = 1; } }
        desk_hidden = any;
    } else {
        for (int i = 0; i < N(wins); i++) if (wins[i].used && saved[i]) wins[i].minimized = 0;
        desk_hidden = 0;
    }
}

static void launch_tile(int ti) {
    struct tile *t = &tiles[ti];
    start_open = 0;
    switch (t->kind) {
    case TL_SETTINGS: open_win(WK_SETTINGS); desk_hidden = 0; break;
    case TL_REGISTRY: open_win(WK_REGISTRY); desk_hidden = 0; break;
    case TL_DESKTOP: show_desktop(); break;
    case TL_SHELL: quit_code = GUI_TO_SHELL; break;
    case TL_LOGOUT: dlg_open(D_CONFIRM, P_C_LOGOUT, "Log out", "End this session?", NULL); break;
    case TL_RESTART: dlg_open(D_CONFIRM, P_C_REBOOT, "Restart", "Restart the computer now?", NULL); break;
    case TL_SHUTDOWN: dlg_open(D_CONFIRM, P_C_HALT, "Shut down", "Shut down? This build halts the system (close QEMU after).", NULL); break;
    case TL_APP: { char c[64]; snprintf(c, sizeof c, "%s", t->cmd); launch_external(c); } break;
    }
}

static void draw_start(void) {
    const struct pal *p = &cf.p;
    clip_all();
    hot_add(0, 0, W, H, A_STARTCLOSE, 0, 0);
    scols = (W - 3) / 13 < 4 ? (W - 3) / 13 : 4;
    if (scols < 1) scols = 1;
    int pw = scols * 13 + 3, dh = desk_y1 - desk_y0;
    int rows = (nvis + scols - 1) / scols, maxrows = (dh - 4) / 5;
    if (maxrows < 1) maxrows = 1;
    int vrows = rows < maxrows ? rows : maxrows;
    if (vrows < 1) vrows = 1;
    int ph = 4 + vrows * 5, px = 0, py = cf.top ? desk_y0 : desk_y1 - ph;
    int srow = ssel / scols;
    if (srow < sscroll) sscroll = srow;
    if (srow >= sscroll + vrows) sscroll = srow - vrows + 1;
    if (sscroll < 0) sscroll = 0;
    hot_add(px, py, pw, ph, A_NONE, 0, 0);
    fill(px, py, pw, ph, p->winfg, p->win);
    for (int i = 0; i < pw; i++) { put(px + i, py, cf.ascii ? '-' : 0x2500, cf.acc, p->win, 0); put(px + i, py + ph - 1, cf.ascii ? '-' : 0x2500, cf.acc, p->win, 0); }
    for (int j = 0; j < ph; j++) { put(px, py + j, cf.ascii ? '|' : 0x2502, cf.acc, p->win, 0); put(px + pw - 1, py + j, cf.ascii ? '|' : 0x2502, cf.acc, p->win, 0); }
    textf(px + 2, py, cf.accfg, cf.acc, AT_BOLD, pw - 4, " 12 Mg  %s %s ", OS_NAME, OS_VERSION);
    textf(px + 2, py + 1, p->muted, p->win, 0, 8, "Find:");
    text(px + 8, py + 1, sq, p->winfg, p->win, 0, pw - 12);
    put(px + 8 + ulen(sq), py + 1, ' ', cf.accfg, cf.acc, 0);
    for (int vi = sscroll * scols; vi < nvis; vi++) {
        int row = vi / scols - sscroll, col = vi % scols;
        if (row >= vrows) break;
        const struct tile *t = &tiles[vis[vi]];
        int tx = px + 1 + col * 13, ty = py + 2 + row * 5, s = vi == ssel;
        uint32_t bg = s ? p->sel : p->win, bc = s ? cf.acc : p->border;
        fill(tx, ty, 12, 5, p->winfg, bg);
        for (int i = 1; i < 11; i++) { put(tx + i, ty, cf.ascii ? '-' : 0x2500, bc, bg, 0); put(tx + i, ty + 4, cf.ascii ? '-' : 0x2500, bc, bg, 0); }
        for (int j = 1; j < 4; j++) { put(tx, ty + j, cf.ascii ? '|' : 0x2502, bc, bg, 0); put(tx + 11, ty + j, cf.ascii ? '|' : 0x2502, bc, bg, 0); }
        put(tx, ty, cf.ascii ? '+' : 0x250C, bc, bg, 0); put(tx + 11, ty, cf.ascii ? '+' : 0x2510, bc, bg, 0);
        put(tx, ty + 4, cf.ascii ? '+' : 0x2514, bc, bg, 0); put(tx + 11, ty + 4, cf.ascii ? '+' : 0x2518, bc, bg, 0);
        textf(tx + 2, ty + 1, p->muted, bg, 0, 4, "%d", vis[vi] + 1);
        text(tx + 5, ty + 2, t->sym, cf.acc, bg, AT_BOLD, 2);
        int nl = ulen(t->name) > 10 ? 10 : ulen(t->name);
        text(tx + 1 + (10 - nl) / 2, ty + 3, t->name, p->winfg, bg, 0, 10);
        hot_add(tx, ty, 12, 5, A_TILE, vi, 0);
    }
    if (nvis == 0) text(px + 3, py + 4, "No match", p->muted, p->win, 0, pw - 6);
    text(px + 2, py + ph - 1, " type to search - Enter open - Esc close ", p->muted, p->win, 0, pw - 4);
}

/* ================= taskbar ================= */

static const char *pin_label(const char *n, char *sym) {
    if (!strcmp(n, "settings")) { strcpy(sym, "St"); return "Settings"; }
    if (!strcmp(n, "registry")) { strcpy(sym, "Rg"); return "Registry"; }
    sym[0] = (char)toupper((unsigned char)n[0]); sym[1] = n[1] ? (char)tolower((unsigned char)n[1]) : ' '; sym[2] = 0;
    return n;
}

static void toggle_open(int kind) {
    struct win *w = win_by_kind(kind);
    desk_hidden = 0;
    if (!w) open_win(kind);
    else if (w->minimized) { w->minimized = 0; raise_win(w); }
    else if (w == active_win()) w->minimized = 1;
    else raise_win(w);
}

static char pins[16][40];
static int npins;

static void draw_taskbar(void) {
    const struct pal *p = &cf.p;
    int y = cf.top ? 0 : H - 1;
    clip_all();
    cur_wid = 0;
    fill(0, y, W, 1, p->barfg, p->bar);
    text(0, y, " 12 Mg ", cf.accfg, cf.acc, AT_BOLD, 7);
    hot_add(0, y, 7, 1, A_START, 0, 0);
    int x = 8, right = W;
    /* right side: window buttons and clock */
    char ck[80] = "";
    if (cf.clock) { clock_str(ck, sizeof ck, 0); right -= ulen(ck) + 2; text(right + 1, y, ck, p->barfg, p->bar, AT_BOLD, -1); }
    if (cf.controls) {
        right -= 12;
        text(right, y, " X ", 0xFFFFFF, p->bad, AT_BOLD, 3); hot_add(right, y, 3, 1, A_CLOSE, 0, 0);
        text(right + 4, y, " O ", p->winfg, p->titleoff, AT_BOLD, 3); hot_add(right + 4, y, 3, 1, A_DESKTOP, 0, 0);
        text(right + 8, y, " \xe2\x80\x94 ", p->winfg, p->titleoff, AT_BOLD, 3); hot_add(right + 8, y, 3, 1, A_HIDE, 0, 0);
        right -= 1;
    }
    /* pinned apps */
    npins = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cf.pinned);
    for (char *sp, *t = strtok_r(buf, ",", &sp); t && npins < N(pins); t = strtok_r(NULL, ",", &sp)) {
        while (*t == ' ') t++;
        if (*t && strlen(t) < sizeof pins[0]) snprintf(pins[npins++], sizeof pins[0], "%s", t);
    }
    struct win *act = active_win();
    for (int i = 0; i < npins; i++) {
        char sym[3];
        const char *lab = pin_label(pins[i], sym);
        int kind = !strcmp(pins[i], "settings") ? WK_SETTINGS : !strcmp(pins[i], "registry") ? WK_REGISTRY : 0;
        struct win *w = kind ? win_by_kind(kind) : NULL;
        int bw = (cf.labels ? ulen(lab) : 2) + 2;
        if (x + bw > right) break;
        uint32_t bg = w ? (w == act ? p->sel : p->titleoff) : p->bar, fg = w && w == act ? cf.acc : p->barfg;
        text(x, y, " ", fg, bg, 0, 1);
        text(x + 1, y, cf.labels ? lab : sym, fg, bg, w == act ? AT_BOLD : 0, bw - 2);
        text(x + bw - 1, y, w && w->minimized ? "." : " ", cf.acc, bg, AT_BOLD, 1);
        hot_add(x, y, bw, 1, A_PIN, i, 0);
        x += bw + 1;
    }
    /* open windows that are not pinned */
    for (int i = 0; i < N(wins); i++) {
        if (!wins[i].used || is_pinned(wins[i].kind == WK_SETTINGS ? "settings" : "registry")) continue;
        int bw = ulen(wins[i].title) + 2;
        if (x + bw > right) break;
        uint32_t bg = &wins[i] == act ? p->sel : p->titleoff;
        text(x, y, " ", p->barfg, bg, 0, 1);
        text(x + 1, y, wins[i].title, &wins[i] == act ? cf.acc : p->barfg, bg, 0, bw - 2);
        text(x + bw - 1, y, " ", p->barfg, bg, 0, 1);
        hot_add(x, y, bw, 1, A_TASKWIN, wins[i].id, 0);
        x += bw + 1;
    }
}

/* ================= rendering ================= */

static void render(void) {
    const struct pal *p = &cf.p;
    nhots = 0; cur_wid = 0;
    clip_all();
    fill(0, 0, W, H, p->winfg, p->desk);
    if (cf.bgpat)
        for (int y = desk_y0; y < desk_y1; y++)
            for (int x = 0; x < W; x++) {
                if (cf.bgpat == 1 && x % 4 == 0 && y % 2 == 0) put(x, y, cf.ascii ? '.' : 0xB7, p->border, p->desk, 0);
                if (cf.bgpat == 2 && ((x % 8 == 0 && y % 4 == 0))) put(x, y, '+', p->border, p->desk, 0);
            }
    int anyvis = 0;
    struct win *act = active_win();
    for (int i = 0; i < nz; i++) {
        struct win *w = win_by_id(zord[i]);
        if (!w || w->minimized) continue;
        anyvis = 1;
        draw_window(w, w == act);
    }
    if (!anyvis) {
        char h1[100], h2[100];
        snprintf(h1, sizeof h1, "%s %s", OS_NAME, OS_VERSION_LONG);
        snprintf(h2, sizeof h2, "Click Mg (top-left) or press Ctrl+G for the Start menu");
        int cy = (desk_y0 + desk_y1) / 2;
        text((W - ulen(h1)) / 2, cy - 1, h1, cf.acc, p->desk, AT_BOLD, -1);
        text((W - ulen(h2)) / 2, cy + 1, h2, p->muted, p->desk, 0, -1);
        text((W - 54) / 2 > 0 ? (W - 54) / 2 : 0, cy + 3, "Ctrl+N next window  Ctrl+E close  Ctrl+D desktop  Ctrl+Q exit", p->muted, p->desk, 0, -1);
    }
    draw_taskbar();
    if (start_open) draw_start();
    if (menu_win) draw_dropdown();
    draw_dialog();
    if (toast_msg[0] && time(NULL) < toast_until) {
        int tw = ulen(toast_msg) + 4;
        if (tw > W) tw = W;
        int ty = cf.top ? H - 1 : 0;
        clip_all();
        fill((W - tw) / 2, ty, tw, 1, 0xFFFFFF, 0x2D3A55);
        text((W - tw) / 2 + 2, ty, toast_msg, 0xFFFFFF, 0x2D3A55, AT_BOLD, tw - 4);
    }
}

/* ================= actions ================= */

static void dialog_ok(void) {
    struct dlg d = dlg;
    dlg.type = D_NONE;
    switch (d.purpose) {
    case P_SET_TEXT: {
        char val[256];
        snprintf(val, sizeof val, "%s", d.buf);
        if (!strcmp(pend_key, "Security/MinPasswordLength")) { int n = atoi(val); if (n < 4) n = 4; if (n > 64) n = 64; snprintf(val, sizeof val, "%d", n); }
        else if (!strcmp(pend_key, "Security/LoginDelay")) { int n = atoi(val); if (n < 0) n = 0; if (n > 30) n = 30; snprintf(val, sizeof val, "%d", n); }
        if (reg_set(pend_hive, pend_key, val) < 0) save_error("the setting");
        break; }
    case P_SET_TIME: {
        int Y, M, D, h, m;
        if (sscanf(d.buf, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) != 5 || Y < 2000 || Y > 2100 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59) {
            toast("Use the form YYYY-MM-DD HH:MM"); break;
        }
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m;
        struct timespec ts = { timegm(&tm), 0 };
        if (clock_settime(CLOCK_REALTIME, &ts) < 0) toast("Cannot set the time: %s", getuid() == 0 ? strerror(errno) : "only root can do this");
        else toast("Time set");
        break; }
    case P_REG_EDIT:
        if (reg_set(pend_hive, pend_key, d.buf) < 0) save_error(pend_key);
        break;
    case P_REG_NEWNAME: {
        if (!d.buf[0]) break;
        char full[160], msg[160];
        snprintf(full, sizeof full, "%s%s%s", pend_key, pend_key[0] ? "/" : "", d.buf);
        snprintf(pend_key, sizeof pend_key, "%s", full);
        snprintf(msg, sizeof msg, "Value for %s:", pend_key);
        dlg_open(D_INPUT, P_REG_NEWVAL, "New value", msg, "");
        break; }
    case P_REG_NEWVAL:
        if (reg_set(pend_hive, pend_key, d.buf) < 0) { if (errno == EINVAL) toast("Invalid name: %s", pend_key); else save_error(pend_key); }
        else {                                       /* open the tree down to the new value */
            exp_set(pend_hive, "", 1);
            for (const char *sl = strchr(pend_key, '/'); sl; sl = strchr(sl + 1, '/')) {
                char pre[MAXK];
                size_t l = (size_t)(sl - pend_key);
                if (l >= MAXK) break;
                memcpy(pre, pend_key, l); pre[l] = 0;
                exp_set(pend_hive, pre, 1);
            }
        }
        break;
    case P_C_REBOOT:
        sync();
        if (reboot(RB_AUTOBOOT) < 0) toast("Restart failed: %s", getuid() == 0 ? strerror(errno) : "only root can do this");
        break;
    case P_C_HALT:
        sync();
        if (reboot(RB_POWER_OFF) < 0) toast("Shut down failed: %s", getuid() == 0 ? strerror(errno) : "only root can do this");
        break;
    case P_C_LOGOUT: quit_code = GUI_LOGOUT; break;
    case P_C_RESET:
        for (int i = reg_count(HIVE_USER) - 1; i >= 0; i--) {
            const char *k = reg_entry(HIVE_USER, i, NULL);
            if (k && strncmp(k, "Environment/", 12)) { char key[160]; snprintf(key, sizeof key, "%s", k); reg_del(HIVE_USER, key); }
        }
        toast("Settings reset");
        break;
    case P_C_DELKEY: if (reg_del_prefix(pend_hive, pend_key) < 0) save_error(pend_key); else rg.vsel = 0; break;
    case P_C_DELVAL: if (reg_del(pend_hive, pend_key) < 0) save_error(pend_key); break;
    }
    load_cfg();
}

static void dialog_key(int k) {
    if (dlg.type == D_INPUT) {
        if (k == 13) { dialog_ok(); return; }
        if (k == 27) { dlg.type = D_NONE; return; }
        if ((k == 127 || k == 8) && dlg.cur > 0) {
            int s = dlg.cur - 1;
            while (s > 0 && ((unsigned char)dlg.buf[s] & 0xC0) == 0x80) s--;
            memmove(dlg.buf + s, dlg.buf + dlg.cur, (size_t)(dlg.len - dlg.cur + 1));
            dlg.len -= dlg.cur - s; dlg.cur = s;
        } else if (k == K_DEL && dlg.cur < dlg.len) {
            int e = dlg.cur + 1;
            while (e < dlg.len && ((unsigned char)dlg.buf[e] & 0xC0) == 0x80) e++;
            memmove(dlg.buf + dlg.cur, dlg.buf + e, (size_t)(dlg.len - e + 1));
            dlg.len -= e - dlg.cur;
        } else if (k == K_LEFT && dlg.cur > 0) { dlg.cur--; while (dlg.cur > 0 && ((unsigned char)dlg.buf[dlg.cur] & 0xC0) == 0x80) dlg.cur--; }
        else if (k == K_RIGHT && dlg.cur < dlg.len) { dlg.cur++; while (dlg.cur < dlg.len && ((unsigned char)dlg.buf[dlg.cur] & 0xC0) == 0x80) dlg.cur++; }
        else if (k == K_HOME) dlg.cur = 0;
        else if (k == K_END) dlg.cur = dlg.len;
        else if (k >= 32 && k != 127 && k < 0x110000 && !(k >= K_UP && k <= K_EOF)) {
            char enc[5];
            int n = u8enc((uint32_t)k, enc);
            if (dlg.len + n < (int)sizeof dlg.buf - 1) {
                memmove(dlg.buf + dlg.cur + n, dlg.buf + dlg.cur, (size_t)(dlg.len - dlg.cur + 1));
                memcpy(dlg.buf + dlg.cur, enc, (size_t)n);
                dlg.cur += n; dlg.len += n;
            }
        }
        return;
    }
    if (dlg.type == D_CONFIRM) {
        if (k == 13 || k == 'y' || k == 'Y') dialog_ok();
        else if (k == 27 || k == 'n' || k == 'N') dlg.type = D_NONE;
    } else if (k == 13 || k == 27) dlg.type = D_NONE;
}

static void do_drag(int x, int y) {
    struct win *w = win_by_id(drg.wid);
    if (!w) { drg.on = 0; return; }
    if (drg.mode == 1) { w->x = x - drg.ox; w->y = y - drg.oy; clamp_win(w); return; }
    int dx = x - drg.sx, dy = y - drg.sy;
    int nx = drg.wx, nw = drg.ww, nh = drg.wh;
    if (drg.edge & 1) { nx = drg.wx + dx; nw = drg.ww - dx; }
    if (drg.edge & 2) nw = drg.ww + dx;
    if (drg.edge & 4) nh = drg.wh + dy;
    if (nw < 34) { if (drg.edge & 1) nx -= 34 - nw; nw = 34; }
    if (nh < 10) nh = 10;
    if (nx < 0) { nw += nx; nx = 0; }
    if (nx + nw > W) nw = W - nx;
    if (w->y + nh > desk_y1) nh = desk_y1 - w->y;
    w->x = nx; w->w = nw; w->h = nh;
}

static void pin_click(int i) {
    if (i < 0 || i >= npins) return;
    char name[40];
    snprintf(name, sizeof name, "%s", pins[i]);
    if (!strcmp(name, "settings")) toggle_open(WK_SETTINGS);
    else if (!strcmp(name, "registry")) toggle_open(WK_REGISTRY);
    else launch_external(name);
}

static void on_mouse(void) {
    int x = mev_x, y = mev_y;
    if (mev_btn >= 64) {                                          /* wheel */
        int d = mev_btn == 64 ? -1 : 1;
        if (start_open) { ssel += d * scols; if (ssel < 0) ssel = 0; if (ssel >= nvis) ssel = nvis - 1; if (ssel < 0) ssel = 0; return; }
        for (int i = nz - 1; i >= 0; i--) {
            struct win *w = win_by_id(zord[i]);
            if (!w || w->minimized || x < w->x || x >= w->x + w->w || y < w->y || y >= w->y + w->h) continue;
            if (w->kind == WK_SETTINGS) { st.scroll += d * 3; if (st.scroll < 0) st.scroll = 0; }
            else if (x < w->x + 1 + (w->w - 2) / 3) { rg.tsel += d; rg.vsel = 0; if (rg.tsel < 0) rg.tsel = 0; }
            else { rg.vsel += d; if (rg.vsel < 0) rg.vsel = 0; }
            return;
        }
        return;
    }
    if (mev_btn != 0) return;
    if (mev_kind == 0) { drg.on = 0; return; }
    if (mev_kind == 2) { if (drg.on) do_drag(x, y); return; }
    const struct hot *h = hot_at(x, y);
    if (!h) return;
    struct hot hh = *h;
    struct win *w = hh.wid ? win_by_id(hh.wid) : NULL;
    if (w) raise_win(w);
    switch (hh.act) {
    case A_START: start_open = !start_open; if (start_open) { build_tiles(); menu_win = 0; } break;
    case A_STARTCLOSE: start_open = 0; break;
    case A_TILE: if (hh.a < nvis) launch_tile(vis[hh.a]); break;
    case A_PIN: pin_click(hh.a); break;
    case A_TASKWIN: { struct win *t = win_by_id(hh.a);
        if (t) { desk_hidden = 0; if (t->minimized) { t->minimized = 0; raise_win(t); } else if (t == active_win()) t->minimized = 1; else raise_win(t); } } break;
    case A_CLOSE: { struct win *a = active_win(); if (a) close_win(a); else toast("No window to close"); } break;
    case A_DESKTOP: show_desktop(); break;
    case A_HIDE: { struct win *a = active_win(); if (a) { a->minimized = 1; desk_hidden = 0; } } break;
    case A_TITLE: if (w) drg = (struct drag){ 1, 1, w->id, x - w->x, y - w->y, 0, x, y, w->x, w->y, w->w, w->h }; break;
    case A_EDGE: if (w) drg = (struct drag){ 1, 2, w->id, 0, 0, hh.b, x, y, w->x, w->y, w->w, w->h }; break;
    case A_MENU: if (menu_win == hh.a && menu_idx == hh.b) menu_win = 0; else { menu_win = hh.a; menu_idx = hh.b; menu_sel = 0; start_open = 0; } break;
    case A_MENUITEM: { struct win *t = win_by_id(hh.a); int m = hh.b / 16, i = hh.b % 16; menu_win = 0; if (t) menu_action(t, m, i); } break;
    case A_MENUCLOSE: menu_win = 0; break;
    case A_SET_HOME: st.page = -1; st.scroll = 0; break;
    case A_SET_CAT: st.hsel = hh.a; open_cat(hh.a); break;
    case A_SET_ROW: st.sel = hh.a; row_activate(st.page, hh.a, 1); break;
    case A_REG_NODE:
        rg.focus = 0;
        if (rg.tsel == hh.a && hh.a < ntn && tn[hh.a].kids) exp_set(tn[hh.a].hive, tn[hh.a].path, !tn[hh.a].open);
        else { rg.tsel = hh.a; rg.vsel = 0; }
        break;
    case A_REG_VAL: if (rg.vsel == hh.a && rg.focus == 1) reg_edit_value(); else { rg.vsel = hh.a; rg.focus = 1; } break;
    case A_DLG_OK: dialog_ok(); break;
    case A_DLG_CANCEL: dlg.type = D_NONE; break;
    }
}

static void next_window(void) {
    if (nz < 2) return;
    struct win *top = win_by_id(zord[nz - 1]);
    if (!top) return;
    int id = zord[nz - 1];
    for (int i = nz - 1; i > 0; i--) zord[i] = zord[i - 1];
    zord[0] = id;
    for (int i = 0; i < nz; i++) { struct win *w = win_by_id(zord[nz - 1]); if (w && w->minimized) { w->minimized = 0; break; } break; }
}

static void start_key(int k) {
    if (k == 27) { start_open = 0; return; }
    if (k == 13) { if (nvis > 0) launch_tile(vis[ssel]); return; }
    if (k == K_LEFT && ssel > 0) ssel--;
    else if (k == K_RIGHT && ssel < nvis - 1) ssel++;
    else if (k == K_UP && ssel - scols >= 0) ssel -= scols;
    else if (k == K_DOWN && ssel + scols < nvis) ssel += scols;
    else if (k == K_DOWN && ssel < nvis - 1) ssel = nvis - 1;
    else if ((k == 127 || k == 8) && sq[0]) { sq[strlen(sq) - 1] = 0; filter_tiles(); }
    else if (k >= 32 && k < 127 && strlen(sq) < sizeof sq - 1) { size_t l = strlen(sq); sq[l] = (char)k; sq[l + 1] = 0; filter_tiles(); ssel = 0; }
}

static void on_key(int k) {
    toast_msg[0] = 0;
    if (dlg.type) { dialog_key(k); return; }
    if (start_open) { start_key(k); return; }
    struct win *a = active_win();
    if (menu_win) {
        struct win *mw = win_by_id(menu_win);
        const char *const *items = mw ? menu_items(mw->kind, menu_idx) : NULL;
        int n = 0;
        while (items && items[n]) n++;
        if (k == 27) menu_win = 0;
        else if (k == K_UP && menu_sel > 0) menu_sel--;
        else if (k == K_DOWN && menu_sel < n - 1) menu_sel++;
        else if (k == K_LEFT || k == K_RIGHT) {
            int nm = 0;
            while (mw && menu_names(mw->kind)[nm]) nm++;
            menu_idx = (menu_idx + (k == K_LEFT ? nm - 1 : 1)) % (nm ? nm : 1); menu_sel = 0;
        } else if (k == 13 && mw) { int m = menu_idx, i = menu_sel; menu_win = 0; menu_action(mw, m, i); }
        return;
    }
    if (mode_move || mode_resize) {
        if (k == 13 || k == 27) { mode_move = mode_resize = 0; return; }
        if (a) {
            if (mode_move) { if (k == K_LEFT) a->x--; else if (k == K_RIGHT) a->x++; else if (k == K_UP) a->y--; else if (k == K_DOWN) a->y++; }
            else { if (k == K_LEFT) a->w--; else if (k == K_RIGHT) a->w++; else if (k == K_UP) a->h--; else if (k == K_DOWN) a->h++; }
            clamp_win(a);
        }
        return;
    }
    switch (k) {
    case CK('G'): case K_F1: start_open = 1; build_tiles(); return;
    case CK('N'): case K_F6: next_window(); return;
    case CK('E'): case K_F4: if (a) close_win(a); return;
    case CK('D'): case K_F7: show_desktop(); return;
    case CK('T'): if (a) { mode_move = 1; toast("Move mode: arrow keys move the window, Enter finishes"); } return;
    case CK('R'): if (a) { mode_resize = 1; toast("Resize mode: arrow keys resize the window, Enter finishes"); } return;
    case CK('Q'): quit_code = GUI_TO_SHELL; return;
    case K_F10: if (a) { menu_win = a->id; menu_idx = 0; menu_sel = 0; } return;
    }
    if (!a) return;
    if (a->kind == WK_SETTINGS) settings_key(k, a->h - 4);
    else registry_key(k, a->h - 4);
}

static void layout(void) {
    desk_y0 = cf.top ? 1 : 0;
    desk_y1 = cf.top ? H : H - 1;
    for (int i = 0; i < N(wins); i++) if (wins[i].used) clamp_win(&wins[i]);
}

static void apply_size(void) {
    int nw = cf.sizeW ? cf.sizeW : detW, nh = cf.sizeH ? cf.sizeH : detH;
    if (nw == W && nh == H && cur) return;
    W = nw; H = nh;
    alloc_buffers();
    if (term_on) out_str("\033[2J");
}

int gui_run(void) {
    if (!isatty(0) || !isatty(1)) { fputs("desktop: needs a terminal\n", stderr); return GUI_TO_SHELL; }
    memset(wins, 0, sizeof wins);
    nz = 0; next_id = 1; start_open = 0; menu_win = 0; dlg.type = D_NONE; desk_hidden = 0;
    toast_msg[0] = 0; mode_move = mode_resize = 0; quit_code = -1; drg.on = 0;
    W = 80; H = 24;
    load_cfg();
    alloc_buffers();
    term_enter();
    detect_size();
        W = 0; H = 0;                                   /* force apply_size to rebuild buffers */
    apply_size();
    layout();
    toast("Welcome. Ctrl+G opens the Start menu, Ctrl+Q returns to the command line.");
    while (quit_code < 0) {
        load_cfg();
        apply_size();
        layout();
        render();
        flush_frame();
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        int wait = 1000 - (int)(ts.tv_nsec / 1000000) + 5;
        int k = read_key(wait);
        if (k == K_TIMEOUT) continue;
        if (k == K_EOF) { quit_code = GUI_TO_SHELL; break; }
        if (k == K_MOUSE) on_mouse(); else on_key(k);
    }
    term_leave();
    free(cur); free(prev);
    cur = prev = NULL;
    out_str("\033[0m\033[2J\033[H");
    return quit_code;
}
