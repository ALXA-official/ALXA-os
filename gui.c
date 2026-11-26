/*
 * Magnesium OS - VaporGlass pixel desktop, smooth anti-aliased UI, themes,
 * manual customization, network & hardware driver panels, Start menu,
 * Registry Editor, windowed PTY Terminal, built-in /programs apps, and login.
 *
 * Works identically on native DRM/KMS GPU drivers (hardware modesetting,
 * double-buffered VSync page-flipping, hardware cursor) and on firmware
 * Basic Display framebuffers (UEFI GOP / BIOS VESA simpledrm & fbdev).
 *
 * Original code by alxa, MIT licensed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include "gui.h"
#include "gui_apps.h"
#include "imgcodec.h"
#include "gfx.h"
#include "net.h"
#include "registry.h"
#include "users.h"
#include "cmds.h"
#include "shell.h"
#include "version.h"

#define N(a)   ((int)(sizeof(a) / sizeof((a)[0])))
#define CK(c)  ((c) & 0x1F)
#define MAXHOT 2048
#define BAR_H   40
#define TITLE_H 34
#define MENU_H  26
#define MAXWINS 10

/* ================= Themes, VaporGlass & Manual Overrides ================= */

struct pal {
    uint32_t desk_top, desk_bot, bar, barfg, win, win_alt, winfg;
    uint32_t titleoff, titlefgoff, border, muted, sel, input_bg, ok, bad;
    uint32_t def_acc;
};

#define THEME_LIST  "Magnesium|Frost|Aurora|Obsidian|Ember|Cobalt|Sakura|Classic"
#define ACCENT_LIST "Sky|Orange|Cyan|Emerald|Violet|Gold|Crimson|Mint"
#define BG_LIST     "Aurora|Gradient|Nebula|Prism|Grid|Solid|Picture"
#define GLASS_LIST  "VaporGlass|Frosted|Clear|Solid"
#define BLUR_LIST   "Off|Low|Medium|High"
#define FONT_LIST   "Smooth|Subpixel|Crisp"

static const struct pal pals[8] = {
    /* 0: Magnesium (Signature deep slate-cobalt mineral glass) */
    { 0x071124, 0x132B4C, 0x0C1629, 0xE2ECF9, 0x111D33, 0x172642, 0xEAF2FC,
      0x1B2B48, 0x9FB3D1, 0x334B73, 0x8499BA, 0x22385E, 0x0B1322, 0x34D399, 0xF06565, 0x38BDF8 },
    /* 1: Frost (Arctic bright silver-ice crystal glass) */
    { 0xCFE2F7, 0x9EC0E6, 0xEEF4FC, 0x142033, 0xF7FAFF, 0xEBF1FA, 0x162235,
      0xDDE7F5, 0x4B5C78, 0xA8BBD8, 0x5E708E, 0xD2E3FA, 0xE6EEF9, 0x159957, 0xDC4C4C, 0x0284C7 },
    /* 2: Aurora (Northern-lights emerald & deep midnight teal) */
    { 0x051923, 0x0B3842, 0x081E28, 0xDCF7F2, 0x0C2530, 0x123240, 0xE6FAF6,
      0x163B4A, 0x92BDB8, 0x285E6E, 0x7BA8A3, 0x1B4A5B, 0x071820, 0x34D399, 0xF87171, 0x2DD4BF },
    /* 3: Obsidian (Ultra-dark carbon studio + luminous rim) */
    { 0x05070B, 0x101521, 0x090C12, 0xE5E9F2, 0x0E121B, 0x141A26, 0xECEFF6,
      0x1A2130, 0x8E9BB2, 0x2B364D, 0x77839B, 0x1F293D, 0x080B10, 0x4ADE80, 0xF87171, 0x60A5FA },
    /* 4: Ember (Volcanic warm copper & obsidian bronze) */
    { 0x1A0D08, 0x361A10, 0x1C0F0A, 0xF7E4D4, 0x24140E, 0x2E1B13, 0xF9ECE1,
      0x382118, 0xCCA892, 0x664232, 0xA48573, 0x4B2D20, 0x150B08, 0x6EE7B7, 0xFB7185, 0xFB923C },
    /* 5: Cobalt (Royal sapphire studio) */
    { 0x081438, 0x162F6E, 0x0C1A42, 0xE4ECFF, 0x102252, 0x162C66, 0xEDF2FF,
      0x1E377A, 0xA3B8E8, 0x3A58A6, 0x89A0D6, 0x254494, 0x0A1638, 0x34D399, 0xF87171, 0x60A5FA },
    /* 6: Sakura (Twilight plum & rose-quartz glass) */
    { 0x1C0E24, 0x381B3F, 0x1F1028, 0xF7E6F5, 0x281533, 0x331C40, 0xFAF0F9,
      0x3E234D, 0xCFA8CA, 0x6B3E7A, 0xAD86AA, 0x512D63, 0x180C20, 0x4ADE80, 0xFB7185, 0xF472B6 },
    /* 7: Classic (Crisp opaque flat workstation) */
    { 0x1B2838, 0x243447, 0x131B26, 0xE2E8F0, 0x1E293B, 0x263449, 0xF1F5F9,
      0x334155, 0x94A3B8, 0x475569, 0x94A3B8, 0x334155, 0x0F172A, 0x22C55E, 0xEF4444, 0x38BDF8 },
};

static const uint32_t accents[8] = {
    0x38BDF8, 0xFB923C, 0x22D3EE, 0x34D399, 0xC084FC, 0xFBBF24, 0xF43F5E, 0x2DD4BF
};

struct cfg {
    struct pal p;
    uint32_t acc, accfg;
    int theme_idx;
    int glass_mode;          /* 0=VaporGlass, 1=Frosted, 2=Clear, 3=Solid */
    int glass_alpha;         /* 0..255 */
    int blur_r;              /* 0..12 */
    int sheen;               /* 0..40 */
    int radius;              /* 0..16 */
    int font_mode;           /* GFX_FONT_SMOOTH / SUBPIXEL / CRISP */
    int bgpat;               /* 0=Aurora, 1=Gradient, 2=Nebula, 3=Prism, 4=Grid, 5=Solid, 6=Picture */
    int refresh;             /* target refresh rate in Hz (Display/Refresh) */
    char pic_path[256];      /* Desktop/Picture: wallpaper image file */
    char cursor_path[256];   /* Display/CursorFile: cursor image file */
    int desk_icons;          /* 1 = show desktop shortcut tiles */
    int floating_bar;        /* 1 = floating VaporGlass dock */
    int top, clock, h12, secs, date, controls, labels, offset;
    int reqW, reqH;
    char pinned[256];
};
static struct cfg cf;

/* Cached static wallpaper buffer so complex Aurora/Nebula/Prism art draws in <0.4ms */
static uint32_t *wp_cache;
static int wp_w, wp_h, wp_pat;
static uint32_t wp_top, wp_bot, wp_acc;

static int parse_hex_rgb(const char *s, uint32_t *out) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    if (strlen(s) < 6) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? (c - '0') :
                (c >= 'a' && c <= 'f') ? (c - 'a' + 10) :
                (c >= 'A' && c <= 'F') ? (c - 'A' + 10) : -1;
        if (d < 0) return 0;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return 1;
}

static int opt_index(const char *opts, const char *val) {
    if (!opts || !val) return -1;
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
static int lum(uint32_t c) {
    return (int)((((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000);
}

static char res_opts_buf[320] = "Auto|1920x1080|1600x900|1280x800|1280x720|1024x768|800x600";

static void rebuild_res_opts(void) {
    const struct gfx_info *gi = gfx_get_info();
    if (!gi || !gi->modeset || gi->nmodes <= 0) {
        if (gi && gi->width > 0)
            snprintf(res_opts_buf, sizeof res_opts_buf, "%dx%d (Firmware)", gi->width, gi->height);
        else
            snprintf(res_opts_buf, sizeof res_opts_buf, "Auto");
        return;
    }
    snprintf(res_opts_buf, sizeof res_opts_buf, "Auto");
    for (int i = 0; i < gi->nmodes; i++) {
        char item[32];
        snprintf(item, sizeof item, "|%dx%d", gi->modes[i].w, gi->modes[i].h);
        if (strlen(res_opts_buf) + strlen(item) + 1 < sizeof res_opts_buf)
            strcat(res_opts_buf, item);
    }
}

/* Cursor image: loaded once per path. Failing files fall back to the built-in arrow. */
static char cur_applied_path[256];
static void sync_cursor(void) {
    if (!strcmp(cur_applied_path, cf.cursor_path)) return;
    snprintf(cur_applied_path, sizeof cur_applied_path, "%s", cf.cursor_path);
    mg_image ci = { 0, 0, NULL };
    char err[160];
    if (cf.cursor_path[0] && img_load_file(cf.cursor_path, &ci, err, sizeof err) == 0 &&
        gfx_cursor_set(ci.px, ci.w, ci.h, 0, 0) == 0) {
        img_free(&ci);
        return;
    }
    img_free(&ci);
    gfx_cursor_set(NULL, 0, 0, 0, 0);
}

static void load_cfg(void) {
    reg_refresh();
    const char *tname = reg_eff("Display/Theme", "Magnesium");
    int t = opt_index(THEME_LIST, tname);
    if (t < 0) {
        if (!strcasecmp(tname, "Dark")) t = 0;
        else if (!strcasecmp(tname, "Light")) t = 1;
        else t = 0;
    }
    cf.theme_idx = t;
    cf.p = pals[t];

    const char *aname = reg_eff("Display/Accent", "Sky");
    int a = opt_index(ACCENT_LIST, aname);
    if (a < 0 && !strcasecmp(aname, "Flame")) a = 0;
    cf.acc = (a >= 0) ? accents[a] : cf.p.def_acc;

    /* Manual #RRGGBB hex overrides */
    uint32_t hex_c;
    if (parse_hex_rgb(reg_eff("Manual/AccentHex", ""), &hex_c)) cf.acc = hex_c;
    if (parse_hex_rgb(reg_eff("Manual/DeskTopHex", ""), &hex_c)) cf.p.desk_top = hex_c;
    if (parse_hex_rgb(reg_eff("Manual/DeskBotHex", ""), &hex_c)) cf.p.desk_bot = hex_c;
    if (parse_hex_rgb(reg_eff("Manual/WindowHex", ""), &hex_c)) {
        cf.p.win = hex_c;
        cf.p.bar = hex_c;
    }
    cf.accfg = lum(cf.acc) > 145 ? 0x0B1220 : 0xFFFFFF;

    int gm = opt_index(GLASS_LIST, reg_eff("Display/Glass", t == 7 ? "Solid" : "VaporGlass"));
    cf.glass_mode = gm < 0 ? 0 : gm;

    int op_pct = reg_eff_int("Display/GlassOpacity", cf.glass_mode == 2 ? 62 : 78);
    if (op_pct < 38) op_pct = 38;
    if (op_pct > 100) op_pct = 100;
    cf.glass_alpha = cf.glass_mode == 3 ? 255 : (op_pct * 255) / 100;

    int bl = opt_index(BLUR_LIST, reg_eff("Display/GlassBlur", "Medium"));
    if (bl < 0) bl = 2;
    static const int blur_px[4] = { 0, 3, 6, 10 };
    cf.blur_r = (cf.glass_mode == 3) ? 0 : (cf.glass_mode == 2 ? blur_px[bl] / 2 : blur_px[bl]);
    cf.sheen  = (cf.glass_mode == 0) ? 24 : (cf.glass_mode == 2 ? 14 : 0);

    cf.radius = reg_eff_int("Display/CornerRadius", t == 7 ? 4 : 10);
    if (cf.radius < 0) cf.radius = 0;
    if (cf.radius > 16) cf.radius = 16;

    int fm_idx = opt_index(FONT_LIST, reg_eff("Display/FontSmoothing", "Smooth"));
    cf.font_mode = fm_idx < 0 ? GFX_FONT_SMOOTH : fm_idx;
    gfx_set_font_mode(cf.font_mode);

    int b = opt_index(BG_LIST, reg_eff("Desktop/Background", "Aurora"));
    if (b < 0 && !strcasecmp(reg_eff("Desktop/Background", ""), "Dots")) b = 4;
    cf.bgpat = b < 0 ? 0 : b;
    snprintf(cf.pic_path, sizeof cf.pic_path, "%s", reg_eff("Desktop/Picture", ""));
    snprintf(cf.cursor_path, sizeof cf.cursor_path, "%s", reg_eff("Display/CursorFile", ""));
    cf.refresh = reg_eff_int("Display/Refresh", 60);
    if (cf.refresh != gfx_get_refresh()) gfx_set_refresh(cf.refresh);
    sync_cursor();
    cf.desk_icons = reg_eff_int("Desktop/Icons", 1);
    cf.floating_bar = opt_index("Floating|Docked", reg_eff("Taskbar/Style", t == 7 ? "Docked" : "Floating")) == 0;
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
    snprintf(cf.pinned, sizeof cf.pinned, "%s",
             reg_eff("Taskbar/Pinned", "files,terminal,notes,netcenter,settings"));

    cf.reqW = cf.reqH = 0;
    const char *rz = reg_eff("Display/Resolution", "Auto");
    int rw, rh;
    if (sscanf(rz, "%dx%d", &rw, &rh) == 2 && rw >= 640 && rh >= 400) {
        cf.reqW = rw; cf.reqH = rh;
    }
}

/* ================= Hotspots & Clipping ================= */

enum {
    A_NONE = 0, A_START, A_PIN, A_TASKWIN, A_CLOSE, A_DESKTOP, A_HIDE,
    A_WINBODY, A_TITLE, A_WIN_CLOSE, A_WIN_MIN, A_WIN_MAX, A_EDGE,
    A_MENU, A_MENUITEM, A_MENUCLOSE, A_TILE, A_STARTCLOSE,
    A_SET_HOME, A_SET_CAT, A_SET_ROW,
    A_REG_NODE, A_REG_VAL,
    A_DLG_OK, A_DLG_CANCEL,
    A_DESK_ICON, A_CTX_ITEM, A_CTX_CLOSE
};

struct hot { int x, y, w, h, act, a, b, wid; };
static struct hot hots[MAXHOT];
static int nhots, cur_wid;
static int clip_hx0, clip_hy0, clip_hx1, clip_hy1;
static int hover_x = -1, hover_y = -1;

static void set_ui_clip(int x, int y, int w, int h) {
    int W = gfx_width(), H = gfx_height();
    clip_hx0 = x < 0 ? 0 : x;
    clip_hy0 = y < 0 ? 0 : y;
    clip_hx1 = x + w > W ? W : x + w;
    clip_hy1 = y + h > H ? H : y + h;
    gfx_clip(x, y, w, h);
}
static void reset_ui_clip(void) {
    clip_hx0 = 0; clip_hy0 = 0;
    clip_hx1 = gfx_width(); clip_hy1 = gfx_height();
    gfx_clip_reset();
}

static void hot_add(int x, int y, int w, int h, int act, int a, int b) {
    int x1 = x + w, y1 = y + h;
    if (x < clip_hx0) x = clip_hx0;
    if (y < clip_hy0) y = clip_hy0;
    if (x1 > clip_hx1) x1 = clip_hx1;
    if (y1 > clip_hy1) y1 = clip_hy1;
    if (x1 <= x || y1 <= y || nhots >= MAXHOT) return;
    hots[nhots++] = (struct hot){ x, y, x1 - x, y1 - y, act, a, b, cur_wid };
}
static const struct hot *hot_at(int x, int y) {
    for (int i = nhots - 1; i >= 0; i--)
        if (x >= hots[i].x && x < hots[i].x + hots[i].w &&
            y >= hots[i].y && y < hots[i].y + hots[i].h) return &hots[i];
    return NULL;
}
static int point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* ================= Windows & Desktop State ================= */

struct win {
    int used, id, kind;
    char title[40], sym[4];
    int x, y, w, h, minimized, maximized;
    int rx, ry, rw, rh; /* saved restore bounds when maximized */
};

static struct win wins[MAXWINS];
static int zord[MAXWINS], nz, next_id = 1;
static int quit_code = -1;
static int start_open, menu_win, menu_idx, menu_sel, menu_x[4];
static int ctx_open, ctx_x, ctx_y;
static int desk_y0, desk_y1, desk_hidden;
static char toast_msg[160];
static time_t toast_until;
static int mode_move, mode_resize;

struct drag { int on, mode, wid, ox, oy, edge, sx, sy, wx, wy, ww, wh; };
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
    for (int i = nz - 1; i >= 0; i--) {
        struct win *w = win_by_id(zord[i]);
        if (w && !w->minimized) return w;
    }
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
    int W = gfx_width(), dh = desk_y1 - desk_y0;
    if (w->maximized) {
        w->x = 4; w->y = desk_y0 + 4;
        w->w = W - 8; w->h = dh - 8;
        return;
    }
    int minw = w->kind == WK_CALC ? 280 : 340;
    int minh = 220;
    if (w->w > W - 12) w->w = W - 12;
    if (w->h > dh - 8) w->h = dh - 8;
    if (w->w < minw) w->w = W - 12 < minw ? W - 12 : minw;
    if (w->h < minh) w->h = dh - 8 < minh ? dh - 8 : minh;
    if (w->x < 2) w->x = 2;
    if (w->y < desk_y0 + 2) w->y = desk_y0 + 2;
    if (w->x + w->w > W - 2) w->x = W - 2 - w->w;
    if (w->y + w->h > desk_y1 - 2) w->y = desk_y1 - 2 - w->h;
}

static void toggle_maximize(struct win *w) {
    if (!w) return;
    if (!w->maximized) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->maximized = 1;
    } else {
        w->maximized = 0;
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
    }
    clamp_win(w);
}

/* ================= Windowed PTY Terminal Engine ================= */

#define TERM_MAXR 80
#define TERM_MAXC 200

struct tcell { char ch; uint8_t fg, bg, bold; };

static const uint32_t ansi_pal[16] = {
    0x0B101C, 0xE05A5A, 0x45C97D, 0xE6B450, 0x5EB8FF, 0xC77DFF, 0x3AD0E6, 0xDCE7F7,
    0x5A6A85, 0xFF7A7A, 0x6BE59C, 0xFFD06B, 0x82C8FF, 0xDDA2FF, 0x6CE4F5, 0xFFFFFF
};

static struct {
    int pty_fd;
    pid_t pid;
    int rows, cols, cx, cy;
    uint8_t fg, bg, bold;
    int esc_state, csi_params[8], csi_n;
    struct tcell grid[TERM_MAXR][TERM_MAXC];
} term = { -1, -1, 22, 80, 0, 0, 7, 16, 0, 0, {0}, 0, {{{0}}} };

static void term_clear_row(int r) {
    for (int c = 0; c < TERM_MAXC; c++)
        term.grid[r][c] = (struct tcell){ ' ', 7, 16, 0 };
}
static void term_reset_grid(void) {
    for (int r = 0; r < TERM_MAXR; r++) term_clear_row(r);
    term.cx = term.cy = 0;
    term.fg = 7; term.bg = 16; term.bold = 0; term.esc_state = 0;
}
static void term_scroll_up(void) {
    for (int r = 0; r < term.rows - 1; r++)
        memcpy(term.grid[r], term.grid[r + 1], sizeof term.grid[0]);
    term_clear_row(term.rows - 1);
}
static void term_handle_sgr(void) {
    if (term.csi_n == 0) { term.csi_params[0] = 0; term.csi_n = 1; }
    for (int i = 0; i < term.csi_n; i++) {
        int p = term.csi_params[i];
        if (p == 0) { term.fg = 7; term.bg = 16; term.bold = 0; }
        else if (p == 1) term.bold = 1;
        else if (p == 22) term.bold = 0;
        else if (p >= 30 && p <= 37) term.fg = (uint8_t)(p - 30);
        else if (p == 39) term.fg = 7;
        else if (p >= 40 && p <= 47) term.bg = (uint8_t)(p - 40);
        else if (p == 49) term.bg = 16;
        else if (p >= 90 && p <= 97) term.fg = (uint8_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) term.bg = (uint8_t)(p - 100 + 8);
    }
}
static void term_putc(unsigned char ch) {
    if (term.esc_state == 1) {
        if (ch == '[') { term.esc_state = 2; term.csi_n = 0; memset(term.csi_params, 0, sizeof term.csi_params); }
        else if (ch == 'c') { term_reset_grid(); term.esc_state = 0; }
        else term.esc_state = 0;
        return;
    }
    if (term.esc_state == 2) {
        if (ch == '?') return;
        if (ch >= '0' && ch <= '9') {
            if (term.csi_n == 0) term.csi_n = 1;
            term.csi_params[term.csi_n - 1] = term.csi_params[term.csi_n - 1] * 10 + (ch - '0');
            return;
        }
        if (ch == ';') { if (term.csi_n < 8) term.csi_n++; return; }
        int p1 = term.csi_n >= 1 && term.csi_params[0] > 0 ? term.csi_params[0] : 1;
        int p2 = term.csi_n >= 2 && term.csi_params[1] > 0 ? term.csi_params[1] : 1;
        switch (ch) {
        case 'A': term.cy -= p1; if (term.cy < 0) term.cy = 0; break;
        case 'B': term.cy += p1; if (term.cy >= term.rows) term.cy = term.rows - 1; break;
        case 'C': term.cx += p1; if (term.cx >= term.cols) term.cx = term.cols - 1; break;
        case 'D': term.cx -= p1; if (term.cx < 0) term.cx = 0; break;
        case 'G': term.cx = p1 - 1; if (term.cx >= term.cols) term.cx = term.cols - 1; break;
        case 'H': case 'f':
            term.cy = p1 - 1; term.cx = p2 - 1;
            if (term.cy < 0) term.cy = 0; else if (term.cy >= term.rows) term.cy = term.rows - 1;
            if (term.cx < 0) term.cx = 0; else if (term.cx >= term.cols) term.cx = term.cols - 1;
            break;
        case 'J':
            if (term.csi_params[0] == 2) { for (int r = 0; r < term.rows; r++) term_clear_row(r); term.cx = term.cy = 0; }
            else {
                for (int c = term.cx; c < term.cols; c++) term.grid[term.cy][c] = (struct tcell){ ' ', 7, 16, 0 };
                for (int r = term.cy + 1; r < term.rows; r++) term_clear_row(r);
            }
            break;
        case 'K':
            for (int c = term.cx; c < term.cols; c++) term.grid[term.cy][c] = (struct tcell){ ' ', 7, 16, 0 };
            break;
        case 'm': term_handle_sgr(); break;
        }
        term.esc_state = 0;
        return;
    }

    if (ch == 27) { term.esc_state = 1; return; }
    if (ch == '\r') { term.cx = 0; return; }
    if (ch == '\n') { term.cy++; if (term.cy >= term.rows) { term_scroll_up(); term.cy = term.rows - 1; } return; }
    if (ch == '\b' || ch == 127) { if (term.cx > 0) term.cx--; return; }
    if (ch == '\t') { term.cx = (term.cx + 8) & ~7; if (term.cx >= term.cols) term.cx = term.cols - 1; return; }
    if (ch < 32) return;

    if (term.cx >= term.cols) {
        term.cx = 0; term.cy++;
        if (term.cy >= term.rows) { term_scroll_up(); term.cy = term.rows - 1; }
    }
    term.grid[term.cy][term.cx++] = (struct tcell){ (char)(ch < 127 ? ch : '?'), term.fg, term.bg, term.bold };
}

static void term_resize_pty(int cols, int rows) {
    if (cols < 20) cols = 20; else if (cols > TERM_MAXC) cols = TERM_MAXC;
    if (rows < 6)  rows = 6;  else if (rows > TERM_MAXR) rows = TERM_MAXR;
    if (cols == term.cols && rows == term.rows) return;
    term.cols = cols; term.rows = rows;
    if (term.cx >= cols) term.cx = cols - 1;
    if (term.cy >= rows) term.cy = rows - 1;
    if (term.pty_fd >= 0) {
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(term.pty_fd, TIOCSWINSZ, &ws);
        if (term.pid > 0) kill(-term.pid, SIGWINCH);
    }
}

static void term_stop(void) {
    if (term.pty_fd >= 0) { close(term.pty_fd); term.pty_fd = -1; }
    if (term.pid > 0) {
        kill(-term.pid, SIGHUP);
        waitpid(term.pid, NULL, WNOHANG);
        term.pid = -1;
    }
}

static int term_start(int cols, int rows, const char *init_cmd) {
    if (term.pty_fd >= 0) return 0;
    int mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (mfd < 0) return -1;
    int unlock = 0;
    unsigned int pnum = 0;
    if (ioctl(mfd, TIOCSPTLCK, &unlock) < 0 || ioctl(mfd, TIOCGPTN, &pnum) < 0) {
        close(mfd);
        return -1;
    }
    char sname[48];
    snprintf(sname, sizeof sname, "/dev/pts/%u", pnum);

    term.cols = cols > 0 ? cols : 80;
    term.rows = rows > 0 ? rows : 22;
    term_reset_grid();

    pid_t pid = fork();
    if (pid < 0) { close(mfd); return -1; }
    if (pid == 0) {
        close(mfd);
        setsid();
        int sfd = open(sname, O_RDWR);
        if (sfd < 0) _exit(1);
        ioctl(sfd, TIOCSCTTY, 0);
        struct winsize ws = { (unsigned short)term.rows, (unsigned short)term.cols, 0, 0 };
        ioctl(sfd, TIOCSWINSZ, &ws);
        dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
        if (sfd > 2) close(sfd);
        setenv("TERM", "xterm-256color", 1);
        setenv("MG_IN_DESKTOP", "1", 1);
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        if (init_cmd && init_cmd[0]) shell_exec_line(init_cmd);
        shell_session();
        _exit(0);
    }
    int flags = fcntl(mfd, F_GETFL, 0);
    if (flags >= 0) fcntl(mfd, F_SETFL, flags | O_NONBLOCK);
    term.pty_fd = mfd;
    term.pid = pid;
    return 0;
}

static int term_pump(void) {
    if (term.pty_fd < 0) return 0;
    unsigned char buf[1024];
    int got = 0;
    for (;;) {
        ssize_t n = read(term.pty_fd, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) term_putc(buf[i]);
            got = 1;
            continue;
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            term_stop();
            return -1;
        }
        break;
    }
    if (term.pid > 0) {
        int st_w;
        if (waitpid(term.pid, &st_w, WNOHANG) == term.pid) {
            term.pid = -1;
            term_stop();
            return -1;
        }
    }
    return got;
}

static void term_send_key(int k) {
    if (term.pty_fd < 0) return;
    const char *seq = NULL;
    switch (k) {
    case GK_UP:    seq = "\033[A"; break;
    case GK_DOWN:  seq = "\033[B"; break;
    case GK_RIGHT: seq = "\033[C"; break;
    case GK_LEFT:  seq = "\033[D"; break;
    case GK_HOME:  seq = "\033[H"; break;
    case GK_END:   seq = "\033[F"; break;
    case GK_DEL:   seq = "\033[3~"; break;
    case GK_PGUP:  seq = "\033[5~"; break;
    case GK_PGDN:  seq = "\033[6~"; break;
    }
    if (seq) {
        ssize_t r = write(term.pty_fd, seq, strlen(seq));
        (void)r;
        return;
    }
    if (k > 0 && k < 128) {
        unsigned char c = (unsigned char)(k == 13 ? '\r' : k);
        ssize_t r = write(term.pty_fd, &c, 1);
        (void)r;
    }
}

static void terminal_draw(int x, int y, int cw, int ch, int active) {
    set_ui_clip(x, y, cw, ch);
    gfx_rect_alpha(x, y, cw, ch, 0x070B14, cf.glass_mode == 3 ? 255 : 220);

    int cols = (cw - 16) / 8;
    int rows = (ch - 12) / 16;
    term_resize_pty(cols, rows);
    if (term.pty_fd < 0) {
        gfx_text(x + 12, y + 12, "Terminal session ended. Press Enter to start a new shell.", cf.p.muted, 0, 1, cw - 24);
        return;
    }

    int ox = x + 8, oy = y + 6;
    char s[2] = { 0, 0 };
    for (int r = 0; r < term.rows; r++) {
        int py = oy + r * 16;
        for (int c = 0; c < term.cols; c++) {
            struct tcell tc = term.grid[r][c];
            int px = ox + c * 8;
            if (tc.bg < 16) gfx_rect(px, py, 8, 16, ansi_pal[tc.bg]);
            if (tc.ch > ' ') {
                uint8_t fgidx = tc.fg < 16 ? tc.fg : 7;
                if (tc.bold && fgidx < 8) fgidx = (uint8_t)(fgidx + 8);
                s[0] = tc.ch;
                gfx_text(px, py, s, ansi_pal[fgidx], tc.bold, 1, 8);
            }
        }
    }
    int curx = ox + term.cx * 8, cury = oy + term.cy * 16;
    if (active) {
        gfx_rect(curx, cury, 8, 16, cf.acc);
        struct tcell tc = term.grid[term.cy][term.cx];
        if (tc.ch > ' ') { s[0] = tc.ch; gfx_text(curx, cury, s, cf.accfg, 1, 1, 8); }
    } else {
        gfx_box(curx, cury, 8, 16, cf.p.muted, 1);
    }
}

/* ================= Callbacks exported to gui_apps.c ================= */

static struct win *open_win_arg(int kind, const char *arg);
static struct win *open_win(int kind) { return open_win_arg(kind, NULL); }

void ui_get_theme(struct ui_theme *out) {
    out->win      = cf.p.win;
    out->win_alt  = cf.p.win_alt;
    out->winfg    = cf.p.winfg;
    out->titleoff = cf.p.titleoff;
    out->border   = cf.p.border;
    out->muted    = cf.p.muted;
    out->sel      = cf.p.sel;
    out->input_bg = cf.p.input_bg;
    out->ok       = cf.p.ok;
    out->bad      = cf.p.bad;
    out->acc      = cf.acc;
    out->accfg    = cf.accfg;
    out->radius   = cf.radius;
}
void ui_set_clip(int x, int y, int w, int h) { set_ui_clip(x, y, w, h); }
void ui_hot_add(int x, int y, int w, int h, int act, int a, int b) { hot_add(x, y, w, h, act, a, b); }
int  ui_hover_in(int x, int y, int w, int h) { return point_in(hover_x, hover_y, x, y, w, h); }
void ui_toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast_msg, sizeof toast_msg, fmt, ap);
    va_end(ap);
    toast_until = time(NULL) + 4;
}
void ui_open_app(int kind, const char *arg) { open_win_arg(kind, arg); desk_hidden = 0; }
void ui_reload_manual_cfg(void) { mg_conf_load_all(); load_cfg(); }

/* ================= Registry-backed helpers & App Scanner ================= */

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
        if (!strcasecmp(t, name)) return 1;
    }
    return 0;
}
static void set_pinned(const char *name, int on) {
    char buf[256], out[256] = "";
    snprintf(buf, sizeof buf, "%s", cf.pinned);
    for (char *sp, *t = strtok_r(buf, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) {
        while (*t == ' ') t++;
        if (!*t || !strcasecmp(t, name)) continue;
        if (strlen(out) + strlen(t) + 2 < sizeof out) { if (*out) strcat(out, ","); strcat(out, t); }
    }
    if (on && strlen(out) + strlen(name) + 2 < sizeof out) { if (*out) strcat(out, ","); strcat(out, name); }
    if (reg_set(HIVE_USER, "Taskbar/Pinned", out) < 0) save_error("pinned apps");
    load_cfg();
}

static char apps[80][40];
static int napps;
static void scan_apps(void) {
    static const char *dirs[] = { "/programs", "/data/programs", "/bin", "/usr/bin", "/usr/local/bin", "/sbin", "/data/bin", NULL };
    napps = 0;
    for (int d = 0; dirs[d]; d++) {
        DIR *dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) != NULL && napps < N(apps)) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "passwd") || strlen(e->d_name) >= sizeof apps[0]) continue;
            char full[300];
            struct stat st_f;
            snprintf(full, sizeof full, "%s/%s", dirs[d], e->d_name);
            if (stat(full, &st_f) != 0 || !S_ISREG(st_f.st_mode) || access(full, X_OK) != 0) continue;
            int dup = 0;
            for (int i = 0; i < napps; i++) if (!strcmp(apps[i], e->d_name)) dup = 1;
            if (!dup) snprintf(apps[napps++], sizeof apps[0], "%s", e->d_name);
        }
        closedir(dp);
    }
}

void ui_run_terminal_cmd(const char *cmd) {
    int wk = apps_kind_from_name(cmd, NULL, NULL);
    if (wk > 0) { open_win(wk); return; }
    struct win *tw = open_win(WK_TERMINAL);
    if (tw && term.pty_fd >= 0) {
        ssize_t w1 = write(term.pty_fd, cmd, strlen(cmd));
        ssize_t w2 = write(term.pty_fd, "\r", 1);
        (void)w1; (void)w2;
        return;
    }
    gfx_suspend();
    printf("\r\n");
    shell_exec_line(cmd);
    fputs("\nPress Enter to return to the desktop...", stdout);
    fflush(stdout);
    char c;
    while (read(0, &c, 1) == 1 && c != '\n') ;
    gfx_resume();
}

/* ================= Dialogs ================= */

enum { D_NONE, D_INPUT, D_CONFIRM, D_INFO };
enum {
    P_SET_TEXT, P_SET_TIME, P_REG_EDIT, P_REG_NEWNAME, P_REG_NEWVAL,
    P_C_REBOOT, P_C_HALT, P_C_LOGOUT, P_C_RESET, P_C_DELKEY, P_C_DELVAL
};
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

static void draw_button(int x, int y, int w, int h, const char *label, uint32_t fg, uint32_t bg, int act, int a, int b) {
    int hov = point_in(hover_x, hover_y, x, y, w, h);
    uint32_t fillc = bg;
    if (hov) {
        int r = ((bg >> 16) & 255) + 20; if (r > 255) r = 255;
        int g = ((bg >> 8) & 255) + 20;  if (g > 255) g = 255;
        int bl = (bg & 255) + 20;        if (bl > 255) bl = 255;
        fillc = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
    }
    int rad = cf.radius > 7 ? 7 : cf.radius;
    gfx_rrect(x, y, w, h, rad, fillc);
    int tw = gfx_text_width(label, 1);
    gfx_text(x + (w - tw) / 2, y + (h - 16) / 2, label, fg, 1, 1, w - 8);
    if (act) hot_add(x, y, w, h, act, a, b);
}

static void draw_dialog(void) {
    if (!dlg.type) return;
    int W = gfx_width(), H = gfx_height();
    const struct pal *p = &cf.p;
    reset_ui_clip();
    gfx_rect_alpha(0, 0, W, H, 0x000000, 115);
    hot_add(0, 0, W, H, A_NONE, 0, 0);

    int w = W - 40 < 480 ? W - 40 : 480;
    int h = dlg.type == D_INPUT ? 188 : 148;
    int x = (W - w) / 2, y = (H - h) / 2;
    cur_wid = 0;

    gfx_shadow(x, y, w, h, cf.radius);
    gfx_glass_panel(x, y, w, h, cf.radius, p->win, cf.glass_alpha, cf.blur_r, cf.acc, cf.sheen);
    gfx_rrect_alpha(x + 1, y + 1, w - 2, 34, cf.radius, cf.acc, 215);
    gfx_text(x + 16, y + 9, dlg.title, cf.accfg, 1, 1, w - 32);
    gfx_text(x + 16, y + 50, dlg.msg, p->winfg, 0, 1, w - 32);

    if (dlg.type == D_INPUT) {
        int ix = x + 16, iy = y + 80, iw = w - 32, ih = 32;
        gfx_rrect(ix, iy, iw, ih, 6, p->input_bg);
        gfx_rrect_outline(ix, iy, iw, ih, 6, cf.acc);
        int maxchars = (iw - 20) / 8;
        int start = dlg.cur > maxchars ? dlg.cur - maxchars : 0;
        gfx_text(ix + 10, iy + 8, dlg.buf + start, p->winfg, 0, 1, iw - 20);
        int cx = ix + 10 + (dlg.cur - start) * 8;
        gfx_rect(cx, iy + 7, 2, 18, cf.acc);
    }

    int by = y + h - 42;
    const char *okl = dlg.type == D_CONFIRM ? "Yes" : "OK";
    if (dlg.type != D_INFO) {
        const char *cl = dlg.type == D_CONFIRM ? "No" : "Cancel";
        draw_button(x + w - 102, by, 86, 28, cl, p->winfg, p->titleoff, A_DLG_CANCEL, 0, 0);
        draw_button(x + w - 198, by, 86, 28, okl, cf.accfg, cf.acc, A_DLG_OK, 0, 0);
    } else {
        draw_button(x + w - 102, by, 86, 28, okl, cf.accfg, cf.acc, A_DLG_OK, 0, 0);
    }
}

/* ================= Settings (Themes, VaporGlass, Manual, Network, Drivers) ================= */

enum { R_HEAD, R_INFO, R_TOGGLE, R_CHOICE, R_TEXT, R_BUTTON, R_APP, R_DRV };
enum {
    I_OS, I_KERNEL, I_MEM, I_UPTIME, I_TIME, I_USER, I_HOME, I_ACCOUNTS,
    I_RAM, I_DATA, I_POLICY, I_REG, I_GPU, I_GPUMODE, I_NET_ADAPTER, I_NET_ADDR
};
enum {
    B_REBOOT, B_HALT, B_LOGOUT, B_SETTIME, B_PASSWD, B_REGEDIT, B_RESET,
    B_EDIT_USERCONF, B_EDIT_SYSCONF, B_RELOAD_CONF, B_CLEAR_MANUAL,
    B_NET_DHCP, B_NET_APPLY, B_NET_CENTER, B_OPEN_SYSMON
};

struct row { int kind; const char *label, *sub, *key, *opts, *def; int sys, id; };

#define HEAD(l)            { R_HEAD, l, 0, 0, 0, 0, 0, 0 }
#define INFO(l, s, i)      { R_INFO, l, s, 0, 0, 0, 0, i }
#define TOG(l, s, k, d)    { R_TOGGLE, l, s, k, 0, d, 0, 0 }
#define CHO(l, s, k, o, d) { R_CHOICE, l, s, k, o, d, 0, 0 }
#define BTN(l, s, i)       { R_BUTTON, l, s, 0, 0, 0, 0, i }

static const struct row r_system[] = {
    HEAD("About"),
    INFO("System", "Name and version", I_OS),
    INFO("Graphics", "Active display driver", I_GPU),
    INFO("Network", "Primary network interface", I_NET_ADAPTER),
    INFO("Kernel", "The Linux kernel underneath", I_KERNEL),
    INFO("Memory", "Total and free", I_MEM),
    INFO("Uptime", "Time since boot", I_UPTIME),
    HEAD("Startup"),
    TOG("Open the desktop at login", "Start here instead of the command line", "Desktop/AutoStart", "1"),
    HEAD("Power"),
    BTN("Restart", "Reboot the computer", B_REBOOT),
    BTN("Shut down", "Power the computer off", B_HALT),
    BTN("Log out", "End this session", B_LOGOUT),
};

static const struct row r_display[] = {
    HEAD("Graphics Adapter"),
    INFO("Adapter", "Active GPU driver or Basic Display", I_GPU),
    INFO("Pipeline", "Scanout, cursor, and modesetting", I_GPUMODE),
    CHO("Resolution", "Live GPU modeset, or fixed firmware mode", "Display/Resolution", res_opts_buf, "Auto"),
    HEAD("Themes & VaporGlass"),
    CHO("Theme", "8 built-in Magnesium color & glass palettes", "Display/Theme", THEME_LIST, "Magnesium"),
    CHO("Accent color", "Highlights, active headers, and 12 Mg badge", "Display/Accent", ACCENT_LIST, "Sky"),
    CHO("Glass effect", "Translucent frosted crystal optics (not Windows Aero)", "Display/Glass", GLASS_LIST, "VaporGlass"),
    CHO("Font smoothing", "Anti-aliased smooth, LCD subpixel, or crisp bitmap", "Display/FontSmoothing", FONT_LIST, "Smooth"),
    CHO("Wallpaper", "Aurora, gradient, nebula, prism, grid, solid, or picture", "Desktop/Background", BG_LIST, "Aurora"),
    { R_TEXT, "Wallpaper picture file", "Used by Picture wallpaper: BMP, PNG, JPG, GIF, TGA and more", "Desktop/Picture", 0, "", 0, 0 },
    CHO("Refresh rate", "Target frame rate; the nearest display mode is chosen", "Display/Refresh", "24|30|60|75|90|120|144|165|240", "60"),
    { R_TEXT, "Cursor image file", "PNG or other picture up to 64x64 px; empty = built-in arrow", "Display/CursorFile", 0, "", 0, 0 },
};

static const struct row r_manual[] = {
    HEAD("Manual Glass & Geometry Tuning"),
    CHO("Glass opacity (%)", "Panel translucency from 45% to 100%", "Display/GlassOpacity", "45|58|68|78|86|94|100", "78"),
    CHO("Backdrop blur", "2-pass frosted blur strength behind windows", "Display/GlassBlur", BLUR_LIST, "Medium"),
    CHO("Corner radius", "Window & control rounding in pixels", "Display/CornerRadius", "0|4|8|10|14", "10"),
    TOG("Desktop shortcut icons", "Show Programs, Notes, Network & Terminal on desktop", "Desktop/Icons", "1"),
    HEAD("Manual Hex Color Overrides (#RRGGBB)"),
    { R_TEXT, "Custom accent (#RRGGBB)", "Overrides theme accent (e.g. #00E5FF, empty = default)", "Manual/AccentHex", 0, "", 0, 0 },
    { R_TEXT, "Custom wallpaper top (#RRGGBB)", "Manual top gradient color", "Manual/DeskTopHex", 0, "", 0, 0 },
    { R_TEXT, "Custom wallpaper bottom (#RRGGBB)", "Manual bottom gradient color", "Manual/DeskBotHex", 0, "", 0, 0 },
    { R_TEXT, "Custom window glass (#RRGGBB)", "Manual window & bar tint color", "Manual/WindowHex", 0, "", 0, 0 },
    BTN("Clear manual color overrides", "Return colors to the selected theme preset", B_CLEAR_MANUAL),
    HEAD("Config Files (mg.conf)"),
    BTN("Edit ~/.mg.conf in Notes", "Open your personal manual config file", B_EDIT_USERCONF),
    BTN("Edit /etc/mg.conf in Notes", "Open system-wide manual config file", B_EDIT_SYSCONF),
    BTN("Reload mg.conf now", "Apply manual file edits immediately", B_RELOAD_CONF),
};

static const struct row r_network[] = {
    HEAD("Status"),
    INFO("Adapter & driver", "Detected Ethernet/VirtIO/USB NIC", I_NET_ADAPTER),
    INFO("IPv4 & routing", "Current IP, gateway, and DNS server", I_NET_ADDR),
    BTN("Open Network Center & Web Reader", "Ping tools & live HTTP page viewer", B_NET_CENTER),
    HEAD("Configuration (DHCP or Manual)"),
    { R_CHOICE, "Address mode", "Automatic DHCP lease or manual static IPv4", "Network/Mode", "DHCP|Manual", "DHCP", 1, 0 },
    BTN("Acquire DHCP lease now", "Send DHCPDISCOVER on primary adapter (root)", B_NET_DHCP),
    { R_TEXT, "Manual IPv4 address", "Used when mode is Manual (e.g. 10.0.2.15)", "Network/IP", 0, "10.0.2.15", 1, 0 },
    { R_TEXT, "Manual subnet mask", "Subnet mask (e.g. 255.255.255.0)", "Network/Netmask", 0, "255.255.255.0", 1, 0 },
    { R_TEXT, "Manual default gateway", "Router IP (e.g. 10.0.2.2)", "Network/Gateway", 0, "10.0.2.2", 1, 0 },
    { R_TEXT, "DNS nameserver", "UDP DNS server (e.g. 10.0.2.3 or 1.1.1.1)", "Network/DNS", 0, "10.0.2.3", 1, 0 },
    BTN("Apply network settings", "Configure adapter with selected mode (root)", B_NET_APPLY),
};

static const struct row r_taskbar[] = {
    HEAD("Layout"),
    CHO("Position", "Where the taskbar sits", "Taskbar/Position", "Top|Bottom", "Top"),
    CHO("Taskbar style", "Floating VaporGlass dock or edge-to-edge bar", "Taskbar/Style", "Floating|Docked", "Floating"),
    TOG("Show app names", "Off shows two-letter element symbols only", "Taskbar/Labels", "1"),
    TOG("Window buttons", "The X  O  \xe2\x80\x94 buttons on the taskbar", "Taskbar/Controls", "1"),
    { R_TEXT, "Pinned apps", "Comma-separated: files, terminal, notes, calc, netcenter, settings", "Taskbar/Pinned", 0, "files,terminal,notes,netcenter,settings", 0, 0 },
    HEAD("Clock"),
    TOG("Show clock", "Time on the right of the taskbar", "Taskbar/Clock", "1"),
    CHO("Time format", "24-hour or 12-hour", "Taskbar/Format", "24-hour|12-hour", "24-hour"),
    TOG("Show seconds", "Update every second", "Taskbar/Seconds", 0 ? "1" : "0"),
    TOG("Show date", "Weekday and date next to the time", "Taskbar/Date", "1"),
};

static const struct row r_time[] = {
    HEAD("Clock"),
    INFO("Now (UTC)", "From the computer's hardware RTC clock", I_TIME),
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
    BTN("Open System & Driver Monitor", "Live CPU, memory, tasks & hardware drivers", B_OPEN_SYSMON),
    HEAD("Reset"),
    BTN("Reset my settings", "Back to defaults; environment variables are kept", B_RESET),
};

/* n == -1: Apps list; n == -2: Hardware Drivers list */
struct cat { const char *name, *sym, *desc; const struct row *rows; int n; };
static const struct cat cats[] = {
    { "System",     "Sy", "About, GPU, startup, power",   r_system,   N(r_system) },
    { "Display",    "Dp", "Themes, VaporGlass, smooth font", r_display, N(r_display) },
    { "Manual",     "Mn", "Hex colors, blur, mg.conf",    r_manual,   N(r_manual) },
    { "Network",    "Nw", "DHCP, manual IP, DNS, HTTP",   r_network,  N(r_network) },
    { "Drivers",    "Dv", "GPU, NIC, disk, USB & input",  NULL,       -2 },
    { "Apps",       "Ap", "Built-in /programs & pins",    NULL,       -1 },
    { "Taskbar",    "Tb", "Floating dock, clock, pins",   r_taskbar,  N(r_taskbar) },
    { "Date & time","Dt", "RTC clock and time zone",      r_time,     N(r_time) },
    { "Accounts",   "Ac", "Your account and users",       r_accounts, N(r_accounts) },
    { "Storage",    "St", "Memory and disk space",        r_storage,  N(r_storage) },
    { "Security",   "Sc", "Password rules",               r_security, N(r_security) },
    { "Terminal",   "Tm", "Prompt and shell",             r_terminal, N(r_terminal) },
    { "Advanced",   "Ad", "Registry, monitor, reset",     r_advanced, N(r_advanced) },
};
#define NCATS N(cats)

static struct hw_driver_item set_drvs[64];
static int n_set_drvs;
static struct { int page, sel, scroll, hsel, cols; } st = { -1, -1, 0, 0, 2 };

static int cat_nrows(int c) {
    if (cats[c].n == -1) return napps;
    if (cats[c].n == -2) return n_set_drvs + 1;
    return cats[c].n;
}
static void get_row(int c, int i, struct row *r, char *lab, char *sub_buf) {
    if (cats[c].n >= 0) {
        *r = cats[c].rows[i];
        if (r->key && !strcmp(r->key, "Display/Resolution")) {
            rebuild_res_opts();
            r->opts = res_opts_buf;
        }
        return;
    }
    memset(r, 0, sizeof *r);
    if (cats[c].n == -2) {
        if (i == 0) {
            r->kind = R_BUTTON; r->id = B_OPEN_SYSMON;
            r->label = "Open System & Driver Monitor";
            r->sub = "Live graphs, process list, and full hardware table";
            return;
        }
        int di = i - 1;
        r->kind = R_DRV;
        snprintf(lab, 64, "[%s] %s", set_drvs[di].category, set_drvs[di].driver);
        snprintf(sub_buf, 128, "%s  (%s)", set_drvs[di].desc, set_drvs[di].slot);
        r->label = lab;
        r->sub = sub_buf;
        r->id = set_drvs[di].active;
        return;
    }
    r->kind = R_APP;
    snprintf(lab, 64, "%s", apps[i]);
    r->label = lab;
    r->sub = "Click to toggle taskbar pin (or launch from 12 Mg / /programs)";
}
static int selectable(const struct row *r) { return r->kind >= R_TOGGLE && r->kind != R_DRV; }

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
    const struct gfx_info *gi = gfx_get_info();
    b[0] = 0;
    switch (id) {
    case I_OS: snprintf(b, n, "%s %s", OS_NAME, OS_VERSION_LONG); break;
    case I_GPU:
        if (gi->backend == GFX_GPU_DRIVER)
            snprintf(b, n, "GPU: %s (%dx%d @ %dHz)", gi->driver, gi->width, gi->height, gi->hz);
        else
            snprintf(b, n, "Basic Display: %s (%dx%d)", gi->driver, gi->width, gi->height);
        break;
    case I_GPUMODE:
        if (gi->backend == GFX_GPU_DRIVER)
            snprintf(b, n, "%s \xc2\xb7 %s \xc2\xb7 KMS",
                     gi->page_flip ? "VSync Flip" : "Direct Scanout",
                     gi->hw_cursor ? "HW Cursor" : "SW Cursor");
        else
            snprintf(b, n, "Firmware FB \xc2\xb7 Shadow Blit \xc2\xb7 SW Cursor");
        break;
    case I_NET_ADAPTER: {
        struct net_iface ifs[8];
        int cnt = net_list_ifaces(ifs, 8), pick = -1;
        for (int i = 0; i < cnt; i++) if (!ifs[i].loopback) { pick = i; break; }
        if (pick >= 0)
            snprintf(b, n, "%s (%s, %s)", ifs[pick].name, ifs[pick].driver, ifs[pick].up ? "UP" : "DOWN");
        else
            snprintf(b, n, "Loopback only");
        break;
    }
    case I_NET_ADDR: {
        struct net_iface ifs[8];
        int cnt = net_list_ifaces(ifs, 8), pick = -1;
        for (int i = 0; i < cnt; i++) if (!ifs[i].loopback) { pick = i; break; }
        if (pick >= 0)
            snprintf(b, n, "%s  gw %s  dns %s", ifs[pick].ip,
                     ifs[pick].gateway[0] ? ifs[pick].gateway : "-", ifs[pick].dns);
        else
            snprintf(b, n, "127.0.0.1");
        break;
    }
    case I_KERNEL: if (uname(&u) == 0) snprintf(b, n, "%.32s %.64s %.32s", u.sysname, u.release, u.machine); break;
    case I_MEM:
        if (sysinfo(&si) == 0) {
            unsigned long long unit = si.mem_unit ? si.mem_unit : 1;
            char a[24], c[24];
            fmt_mb(a, sizeof a, si.totalram * unit);
            fmt_mb(c, sizeof c, si.freeram * unit);
            snprintf(b, n, "%s total, %s free", a, c);
        }
        break;
    case I_UPTIME:
        if (sysinfo(&si) == 0)
            snprintf(b, n, "%ldd %02ld:%02ld:%02ld", si.uptime / 86400, (si.uptime / 3600) % 24, (si.uptime / 60) % 60, si.uptime % 60);
        break;
    case I_TIME: clock_str(b, n, 1); strncat(b, " UTC", n - strlen(b) - 1); break;
    case I_USER: {
        users_load();
        const struct user *me = user_by_uid(getuid());
        snprintf(b, n, "%s (uid %u)", me ? me->name : "unknown", getuid());
        break;
    }
    case I_HOME: snprintf(b, n, "%s", getenv("HOME") ? getenv("HOME") : "?"); break;
    case I_ACCOUNTS: {
        users_load();
        int c = 0; b[0] = 0;
        for (uid_t uid = 0; uid < 70000 && c < 6; uid += (uid == 0 ? 1000 : 1)) {
            const struct user *u2 = user_by_uid(uid);
            if (!u2) { if (uid >= 1000 && uid > users_next_uid()) break; continue; }
            if (c++) strncat(b, ", ", n - strlen(b) - 1);
            strncat(b, u2->name, n - strlen(b) - 1);
        }
        if (!c) snprintf(b, n, "none");
        break;
    }
    case I_RAM:
        if (statvfs("/", &sv) == 0 && sv.f_blocks) {
            char a[24], c[24];
            fmt_mb(a, sizeof a, (unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize);
            fmt_mb(c, sizeof c, (unsigned long long)sv.f_blocks * sv.f_frsize);
            snprintf(b, n, "%s used of %s", a, c);
        } else if (sysinfo(&si) == 0) {
            char a[24];
            fmt_mb(a, sizeof a, (unsigned long long)si.totalram * (si.mem_unit ? si.mem_unit : 1));
            snprintf(b, n, "RAM %s", a);
        }
        break;
    case I_DATA:
        if (access("/data/etc", F_OK) == 0 && statvfs("/data", &sv) == 0) {
            char a[24], c[24];
            fmt_mb(a, sizeof a, (unsigned long long)(sv.f_blocks - sv.f_bfree) * sv.f_frsize);
            fmt_mb(c, sizeof c, (unsigned long long)sv.f_blocks * sv.f_frsize);
            snprintf(b, n, "%s used of %s", a, c);
        } else snprintf(b, n, "No disk attached");
        break;
    case I_POLICY:
        snprintf(b, n, "%d+ chars, %ds delay",
                 atoi(reg_get(HIVE_SYSTEM, "Security/MinPasswordLength", "8")),
                 atoi(reg_get(HIVE_SYSTEM, "Security/LoginDelay", "2")));
        break;
    case I_REG:
        snprintf(b, n, "System %d values, User %d values", reg_count(HIVE_SYSTEM), reg_count(HIVE_USER));
        break;
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
        break;
    }
    case R_TEXT: snprintf(b, n, "%s", hv == HIVE_SYSTEM ? reg_get(hv, r->key, r->def) : reg_eff(r->key, r->def)); break;
    case R_INFO: info_text(r->id, b, n); break;
    default: b[0] = 0;
    }
}

static void layout_desktop(void);

static void apply_display_resolution_from_cfg(void) {
    const struct gfx_info *gi = gfx_get_info();
    if (!gi || !gi->modeset) return;
    if (gfx_set_resolution(cf.reqW, cf.reqH) == 0) {
        layout_desktop();
        gfx_mark_all_dirty();
    }
}

static void row_activate(int ci, int ri, int dir) {
    struct row r;
    char lab[64], sub_b[128], val[160];
    get_row(ci, ri, &r, lab, sub_b);
    int hv = r.sys ? HIVE_SYSTEM : HIVE_USER;
    switch (r.kind) {
    case R_TOGGLE:
        if (reg_set(hv, r.key, get_toggle(hv, r.key, r.def) ? "0" : "1") < 0) save_error(r.label);
        load_cfg();
        layout_desktop();
        return;
    case R_CHOICE: {
        if (r.key && !strcmp(r.key, "Display/Resolution") && !gfx_get_info()->modeset) {
            toast("Basic Display mode uses fixed boot firmware resolution (load a GPU driver for KMS modeset)");
            return;
        }
        row_value(&r, val, sizeof val);
        int idx = opt_index(r.opts, val), cnt = opt_count(r.opts);
        if (idx < 0) idx = 0;
        idx = (idx + dir + cnt) % cnt;
        opt_get(r.opts, idx, val, sizeof val);
        if (reg_set(hv, r.key, val) < 0) save_error(r.label);
        load_cfg();
        layout_desktop();
        if (r.key && !strcmp(r.key, "Display/Resolution")) apply_display_resolution_from_cfg();
        return;
    }
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
        case B_HALT: dlg_open(D_CONFIRM, P_C_HALT, "Shut down", "Power the computer off now?", NULL); break;
        case B_LOGOUT: dlg_open(D_CONFIRM, P_C_LOGOUT, "Log out", "End this session?", NULL); break;
        case B_RESET: dlg_open(D_CONFIRM, P_C_RESET, "Reset settings", "Reset all your settings to the defaults?", NULL); break;
        case B_SETTIME: {
            char t[40];
            time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm);
            strftime(t, sizeof t, "%Y-%m-%d %H:%M", &tm);
            dlg_open(D_INPUT, P_SET_TIME, "Set date and time", "Enter UTC time as YYYY-MM-DD HH:MM", t);
            break;
        }
        case B_PASSWD: ui_run_terminal_cmd("passwd"); break;
        case B_REGEDIT: open_win(WK_REGISTRY); break;
        case B_OPEN_SYSMON: open_win(WK_SYSMON); break;
        case B_EDIT_USERCONF: {
            char p[200];
            open_win_arg(WK_NOTES, mg_conf_user_path(p, sizeof p));
            break;
        }
        case B_EDIT_SYSCONF: {
            char p[200];
            mg_conf_ensure_template();
            open_win_arg(WK_NOTES, mg_conf_sys_path(p, sizeof p));
            break;
        }
        case B_RELOAD_CONF: {
            int n = mg_conf_load_all();
            load_cfg();
            layout_desktop();
            toast("Reloaded mg.conf (%d setting%s applied)", n, n == 1 ? "" : "s");
            break;
        }
        case B_CLEAR_MANUAL:
            reg_del(HIVE_USER, "Manual/AccentHex");
            reg_del(HIVE_USER, "Manual/DeskTopHex");
            reg_del(HIVE_USER, "Manual/DeskBotHex");
            reg_del(HIVE_USER, "Manual/WindowHex");
            load_cfg();
            toast("Cleared manual hex color overrides");
            break;
        case B_NET_DHCP: {
            char msg[160];
            if (net_dhcp(NULL, msg, sizeof msg) == 0) toast("DHCP: %s", msg);
            else toast("DHCP failed: %s", msg);
            break;
        }
        case B_NET_APPLY: {
            char msg[160];
            if (net_apply_registry(msg, sizeof msg) == 0) toast("Network: %s", msg);
            else toast("%s", msg);
            break;
        }
        case B_NET_CENTER: open_win(WK_NETCENTER); break;
        }
        break;
    }
    load_cfg();
}

static void open_cat(int c) {
    st.page = c; st.scroll = 0; st.sel = -1;
    if (cats[c].n == -1) scan_apps();
    if (cats[c].n == -2) n_set_drvs = drv_scan(set_drvs, N(set_drvs));
    for (int i = 0; i < cat_nrows(c); i++) {
        struct row r; char lab[64], sub_b[128];
        get_row(c, i, &r, lab, sub_b);
        if (selectable(&r)) { st.sel = i; break; }
    }
}

static void draw_pill_right(int xr, int cy, const char *s, uint32_t fg, uint32_t bg, int bold) {
    int pw = gfx_text_width(s, 1) + 20;
    int x = xr - pw, y = cy - 12;
    gfx_rrect(x, y, pw, 24, 12, bg);
    gfx_text(x + 10, y + 4, s, fg, bold, 1, pw - 16);
}

static void settings_draw(int x, int y, int cw, int ch) {
    const struct pal *p = &cf.p;
    set_ui_clip(x, y, cw, ch);
    if (cf.glass_mode == 3) gfx_rect(x, y, cw, ch, p->win);

    if (st.page < 0) {
        gfx_text(x + 20, y + 14, "Settings", cf.acc, 1, 1, cw - 40);
        gfx_text(x + 20, y + 34, "Themes, VaporGlass, smooth fonts, manual overrides, network & drivers", p->muted, 0, 1, cw - 40);
        int cols = cw >= 520 ? 2 : 1;
        int gap = 10;
        int tw = (cw - 40 - (cols - 1) * gap) / cols;
        int th = 56;
        st.cols = cols;
        set_ui_clip(x, y + 58, cw, ch - 62);
        for (int i = 0; i < NCATS; i++) {
            int tx = x + 20 + (i % cols) * (tw + gap);
            int ty = y + 60 + (i / cols) * (th + 8) - st.scroll * 24;
            if (ty + th < y + 58 || ty >= y + ch) continue;
            int hi = i == st.hsel || point_in(hover_x, hover_y, tx, ty, tw, th);
            uint32_t bg = hi ? p->sel : p->win_alt;
            gfx_rrect(tx, ty, tw, th, 8, bg);
            gfx_rrect_outline(tx, ty, tw, th, 8, hi ? cf.acc : p->border);
            gfx_rrect(tx + 10, ty + 8, 40, 40, 6, p->win);
            gfx_rrect_outline(tx + 10, ty + 8, 40, 40, 6, cf.acc);
            int sw = gfx_text_width(cats[i].sym, 1);
            gfx_text(tx + 10 + (40 - sw) / 2, ty + 20, cats[i].sym, cf.acc, 1, 1, 36);
            gfx_text(tx + 60, ty + 10, cats[i].name, p->winfg, 1, 1, tw - 70);
            gfx_text(tx + 60, ty + 30, cats[i].desc, p->muted, 0, 1, tw - 70);
            hot_add(tx, ty, tw, th, A_SET_CAT, i, 0);
        }
        return;
    }

    int nav_w = 168;
    gfx_rect_alpha(x, y, nav_w, ch, p->win_alt, cf.glass_mode == 3 ? 255 : 155);
    gfx_vline(x + nav_w, y, ch, p->border);
    gfx_rrect(x + 8, y + 8, nav_w - 16, 26, 6, p->titleoff);
    gfx_text(x + 16, y + 13, "\xe2\x80\xb9 All settings", p->winfg, 0, 1, nav_w - 24);
    hot_add(x + 8, y + 8, nav_w - 16, 26, A_SET_HOME, 0, 0);

    for (int i = 0; i < NCATS; i++) {
        int ry = y + 40 + i * 28;
        if (ry + 26 > y + ch) break;
        int sel = i == st.page;
        int hov = point_in(hover_x, hover_y, x + 6, ry, nav_w - 12, 26);
        if (sel || hov) gfx_rrect(x + 6, ry, nav_w - 12, 26, 6, sel ? cf.acc : p->sel);
        gfx_text(x + 12, ry + 5, cats[i].sym, sel ? cf.accfg : cf.acc, 1, 1, 24);
        gfx_text(x + 38, ry + 5, cats[i].name, sel ? cf.accfg : p->winfg, sel, 1, nav_w - 46);
        hot_add(x + 6, ry, nav_w - 12, 26, A_SET_CAT, i, 0);
    }

    int x0 = x + nav_w + 18, pw = cw - nav_w - 36;
    gfx_text(x0, y + 12, cats[st.page].name, cf.acc, 1, 1, pw);
    gfx_text(x0, y + 32, cats[st.page].desc, p->muted, 0, 1, pw);
    gfx_hline(x0, y + 54, pw, p->border);

    set_ui_clip(x + nav_w + 1, y + 56, cw - nav_w - 1, ch - 58);
    int nr = cat_nrows(st.page);
    if (nr == 0) {
        gfx_text(x0, y + 72, "No items found.", p->muted, 0, 1, pw);
        return;
    }

    int cur_y = y + 64 - st.scroll * 26;
    for (int i = 0; i < nr; i++) {
        struct row r; char lab[64], sub_b[128], val[160];
        get_row(st.page, i, &r, lab, sub_b);
        int rh = r.kind == R_HEAD ? 30 : 48;
        if (cur_y + rh > y + 56 && cur_y < y + ch) {
            if (r.kind == R_HEAD) {
                gfx_text(x0, cur_y + 10, r.label, cf.acc, 1, 1, pw);
            } else {
                int sel = (i == st.sel && selectable(&r)) || (selectable(&r) && point_in(hover_x, hover_y, x0 - 6, cur_y, pw + 12, rh - 4));
                gfx_rrect(x0 - 6, cur_y, pw + 12, rh - 4, 6, sel ? p->sel : p->win_alt);
                gfx_text(x0 + 4, cur_y + 6, r.label, p->winfg, 1, 1, pw - 140);
                if (r.sub) gfx_text(x0 + 4, cur_y + 24, r.sub, p->muted, 0, 1, pw - 160);
                int xr = x0 + pw - 2, midy = cur_y + (rh - 4) / 2;
                if (r.kind == R_INFO) {
                    info_text(r.id, val, sizeof val);
                    int max_px = pw - gfx_text_width(r.label, 1) - 24;
                    if (max_px > 64) {
                        int vw = gfx_text_width(val, 1);
                        if (vw > max_px) vw = (max_px / 8) * 8;
                        gfx_text(xr - vw, cur_y + 6, val, cf.acc, 0, 1, max_px);
                    }
                } else if (r.kind == R_DRV) {
                    draw_pill_right(xr, midy, r.id ? "Active" : "Unbound",
                                    r.id ? 0x0B1220 : 0xFFFFFF, r.id ? p->ok : p->bad, 1);
                } else if (r.kind == R_TOGGLE || r.kind == R_APP) {
                    int on = r.kind == R_APP ? is_pinned(lab) : get_toggle(r.sys ? HIVE_SYSTEM : HIVE_USER, r.key, r.def);
                    draw_pill_right(xr, midy, r.kind == R_APP ? (on ? "Pinned" : "Pin") : (on ? "ON" : "OFF"),
                                    on ? cf.accfg : p->winfg, on ? cf.acc : p->titleoff, 1);
                } else if (r.kind == R_CHOICE) {
                    row_value(&r, val, sizeof val);
                    char s[120];
                    snprintf(s, sizeof s, "\xe2\x80\xb9 %s \xe2\x80\xba", val);
                    draw_pill_right(xr, midy, s, p->winfg, p->titleoff, 0);
                } else if (r.kind == R_TEXT) {
                    row_value(&r, val, sizeof val);
                    if (strlen(val) > 18) { val[17] = '~'; val[18] = 0; }
                    draw_pill_right(xr, midy, val[0] ? val : "(default)", p->winfg, p->titleoff, 0);
                } else if (r.kind == R_BUTTON) {
                    int danger = r.id == B_HALT || r.id == B_REBOOT || r.id == B_RESET;
                    const char *blab = (r.id == B_REGEDIT || r.id == B_NET_CENTER || r.id == B_OPEN_SYSMON ||
                                        r.id == B_EDIT_USERCONF || r.id == B_EDIT_SYSCONF) ? "Open" :
                                       r.id == B_PASSWD ? "Change" : r.id == B_LOGOUT ? "Log out" :
                                       r.id == B_SETTIME ? "Set" : "Apply";
                    draw_pill_right(xr, midy, blab, danger ? 0xFFFFFF : cf.accfg, danger ? p->bad : cf.acc, 1);
                }
                if (selectable(&r)) hot_add(x0 - 6, cur_y, pw + 12, rh - 4, A_SET_ROW, i, 0);
            }
        }
        cur_y += rh;
    }
}

static void settings_key(int k, int ch) {
    if (st.page < 0) {
        int cols = st.cols < 1 ? 1 : st.cols;
        if (k == GK_LEFT && st.hsel > 0) st.hsel--;
        else if (k == GK_RIGHT && st.hsel < NCATS - 1) st.hsel++;
        else if (k == GK_UP && st.hsel - cols >= 0) st.hsel -= cols;
        else if (k == GK_DOWN && st.hsel + cols < NCATS) st.hsel += cols;
        else if (k == 13 || k == ' ') open_cat(st.hsel);
        return;
    }
    int nr = cat_nrows(st.page);
    if (k == GK_UP || k == GK_DOWN) {
        int d = k == GK_UP ? -1 : 1;
        for (int i = st.sel + d; i >= 0 && i < nr; i += d) {
            struct row r; char lab[64], sub_b[128];
            get_row(st.page, i, &r, lab, sub_b);
            if (selectable(&r)) { st.sel = i; break; }
        }
        int vis_rows = (ch - 64) / 48;
        if (vis_rows < 2) vis_rows = 2;
        if (st.sel * 2 < st.scroll) st.scroll = st.sel * 2;
        if (st.sel * 2 > st.scroll + vis_rows * 2 - 2) st.scroll = st.sel * 2 - vis_rows * 2 + 2;
        if (st.scroll < 0) st.scroll = 0;
    } else if (k == GK_LEFT || k == GK_RIGHT) {
        if (st.sel >= 0) {
            struct row r; char lab[64], sub_b[128];
            get_row(st.page, st.sel, &r, lab, sub_b);
            if (r.kind == R_CHOICE) row_activate(st.page, st.sel, k == GK_LEFT ? -1 : 1);
            else if (r.kind == R_TOGGLE || r.kind == R_APP) row_activate(st.page, st.sel, 1);
        }
    } else if (k == 13 || k == ' ') {
        if (st.sel >= 0) row_activate(st.page, st.sel, 1);
    } else if (k == 9) open_cat((st.page + 1) % NCATS);
    else if (k == GK_STAB) open_cat((st.page + NCATS - 1) % NCATS);
    else if (k == 27 || k == 127 || k == 8) { st.page = -1; st.scroll = 0; }
    else if (k == GK_PGDN) st.scroll += 5;
    else if (k == GK_PGUP) { st.scroll -= 5; if (st.scroll < 0) st.scroll = 0; }
}

/* ================= Registry Editor ================= */

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
        for (int i = 0; i < nd; i++)
            if (u == 0 || strcmp(dirs_tmp[u - 1], dirs_tmp[i])) { if (u != i) memcpy(dirs_tmp[u], dirs_tmp[i], MAXK); u++; }
        nd = u;
        if (ntn >= N(tn)) return;
        struct tnode *r = &tn[ntn++];
        memset(r, 0, sizeof *r);
        r->hive = hv; r->depth = 0; r->kids = nd > 0; r->open = exp_find(hv, "") >= 0;
        snprintf(r->name, sizeof r->name, "%s", reg_hive_name(hv));
        if (!r->open) continue;
        for (int i = 0; i < nd && ntn < N(tn); i++) {
            const char *path = dirs_tmp[i];
            int vis_ = 1, depth = 1;
            for (const char *s = strchr(path, '/'); s; s = strchr(s + 1, '/')) {
                char pre[MAXK];
                size_t l = (size_t)(s - path);
                memcpy(pre, path, l); pre[l] = 0;
                if (exp_find(hv, pre) < 0) vis_ = 0;
                depth++;
            }
            if (!vis_) continue;
            struct tnode *t = &tn[ntn++];
            memset(t, 0, sizeof *t);
            t->hive = hv; t->depth = depth;
            snprintf(t->path, sizeof t->path, "%.159s", path);
            const char *last = strrchr(path, '/');
            snprintf(t->name, sizeof t->name, "%.63s", last ? last + 1 : path);
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
    if (t->path[0]) snprintf(out, n, "%.90s/%.64s", t->path, name);
    else snprintf(out, n, "%.155s", name);
}
static const char *reg_val_name(const struct tnode *t, int vi) {
    const char *key = reg_entry(t->hive, vlist[vi], NULL);
    return t->path[0] ? key + strlen(t->path) + 1 : key;
}

static void registry_draw(int x, int y, int cw, int ch) {
    const struct pal *p = &cf.p;
    build_tree();
    build_values();
    set_ui_clip(x, y, cw, ch);
    if (cf.glass_mode == 3) gfx_rect(x, y, cw, ch, p->win);

    int lw = cw / 3 < 180 ? 180 : (cw / 3 > 250 ? 250 : cw / 3);
    int foot_h = 26, body_h = ch - foot_h;
    gfx_rect_alpha(x, y, lw, body_h, p->win_alt, cf.glass_mode == 3 ? 255 : 155);
    gfx_vline(x + lw, y, body_h, p->border);

    gfx_text(x + 12, y + 8, "Keys", cf.acc, 1, 1, lw - 20);
    gfx_hline(x, y + 30, lw, p->border);

    int row_h = 24;
    int tview = (body_h - 34) / row_h;
    if (tview < 1) tview = 1;
    if (rg.tsel < rg.tscroll) rg.tscroll = rg.tsel;
    if (rg.tsel >= rg.tscroll + tview) rg.tscroll = rg.tsel - tview + 1;

    set_ui_clip(x, y + 32, lw, body_h - 32);
    for (int i = 0; i < ntn; i++) {
        int ry = y + 34 + (i - rg.tscroll) * row_h;
        if (ry + row_h <= y + 32 || ry >= y + body_h) continue;
        const struct tnode *t = &tn[i];
        int sel = i == rg.tsel;
        uint32_t bg = sel ? (rg.focus == 0 ? cf.acc : p->sel) : p->win_alt;
        uint32_t fg = sel && rg.focus == 0 ? cf.accfg : p->winfg;
        if (sel) gfx_rrect(x + 4, ry, lw - 8, row_h - 2, 4, bg);
        int tx = x + 10 + t->depth * 14;
        const char *mk = t->kids ? (t->open ? "\xe2\x96\xbe" : "\xe2\x96\xb8") : " ";
        gfx_text(tx, ry + 3, mk, sel && rg.focus == 0 ? fg : cf.acc, 0, 1, 12);
        gfx_text(tx + 14, ry + 3, t->name, fg, t->depth == 0, 1, lw - (tx + 14 - x) - 6);
        hot_add(x + 4, ry, lw - 8, row_h - 2, A_REG_NODE, i, 0);
    }

    int rx = x + lw + 12, rw = cw - lw - 24;
    set_ui_clip(x + lw + 1, y, cw - lw - 1, body_h);
    const struct tnode *sel = rg.tsel < ntn ? &tn[rg.tsel] : NULL;
    int ro = sel && sel->hive == HIVE_SYSTEM && getuid() != 0;
    gfx_textf(rx, y + 8, cf.acc, 1, 1, rw, "Values%s", ro ? "  (read-only: root can edit System)" : "");
    gfx_hline(x + lw + 1, y + 30, cw - lw - 1, p->border);

    int nw = rw / 3 < 110 ? 110 : (rw / 3 > 200 ? 200 : rw / 3);
    gfx_text(rx, y + 36, "Name", p->muted, 1, 1, nw);
    gfx_text(rx + nw + 12, y + 36, "Value", p->muted, 1, 1, rw - nw - 12);
    gfx_hline(x + lw + 1, y + 56, cw - lw - 1, p->border);

    int vview = (body_h - 60) / row_h;
    if (vview < 1) vview = 1;
    if (rg.vsel < rg.vscroll) rg.vscroll = rg.vsel;
    if (rg.vsel >= rg.vscroll + vview) rg.vscroll = rg.vsel - vview + 1;
    if (rg.vscroll < 0) rg.vscroll = 0;

    set_ui_clip(x + lw + 1, y + 58, cw - lw - 1, body_h - 58);
    for (int i = 0; sel && i < nvl; i++) {
        int ry = y + 60 + (i - rg.vscroll) * row_h;
        if (ry + row_h <= y + 58 || ry >= y + body_h) continue;
        int s = i == rg.vsel;
        uint32_t bg = s ? (rg.focus == 1 ? cf.acc : p->sel) : p->win;
        uint32_t fg = s && rg.focus == 1 ? cf.accfg : p->winfg;
        if (s) gfx_rrect(x + lw + 6, ry, cw - lw - 12, row_h - 2, 4, bg);
        const char *v;
        reg_entry(sel->hive, vlist[i], &v);
        char one[120];
        snprintf(one, sizeof one, "%s", v);
        for (char *c = one; *c; c++) if (*c == '\n') *c = ' ';
        gfx_text(rx, ry + 3, reg_val_name(sel, i), fg, 1, 1, nw);
        gfx_text(rx + nw + 12, ry + 3, one, fg, 0, 1, rw - nw - 12);
        hot_add(x + lw + 6, ry, cw - lw - 12, row_h - 2, A_REG_VAL, i, 0);
    }
    if (sel && nvl == 0)
        gfx_text(rx, y + 72, "No values here. Press N to add one.", p->muted, 0, 1, rw);

    set_ui_clip(x, y, cw, ch);
    gfx_rect(x, y + ch - foot_h, cw, foot_h, p->titleoff);
    char path[300];
    snprintf(path, sizeof path, "%s%s%s", sel ? reg_hive_name(sel->hive) : "",
             sel && sel->path[0] ? "\\" : "", sel ? sel->path : "");
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
    gfx_textf(x + 10, y + ch - foot_h + 5, p->titlefgoff, 0, 1, cw - 20,
              "%s   \xc2\xb7   N new   Enter edit   Del delete   Tab switch", path);
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

static void registry_key(int k) {
    build_tree();
    build_values();
    if (k == 9) { rg.focus = !rg.focus; return; }
    if (k == 'n' || k == 'N') { reg_new_value(); return; }
    if (k == 'r' || k == 'R' || k == GK_F5) { reg_refresh(); return; }
    if (rg.focus == 0) {
        const struct tnode *t = rg.tsel < ntn ? &tn[rg.tsel] : NULL;
        if (k == GK_UP && rg.tsel > 0) { rg.tsel--; rg.vsel = 0; }
        else if (k == GK_DOWN && rg.tsel < ntn - 1) { rg.tsel++; rg.vsel = 0; }
        else if (t && (k == GK_RIGHT || k == 13 || k == ' ')) {
            if (t->kids && !t->open) exp_set(t->hive, t->path, 1);
            else if (k == 13 && t->kids) exp_set(t->hive, t->path, 0);
            else if (k == GK_RIGHT) rg.focus = 1;
        } else if (t && k == GK_LEFT) {
            if (t->open && t->kids) exp_set(t->hive, t->path, 0);
            else for (int i = rg.tsel - 1; i >= 0; i--)
                if (tn[i].hive == t->hive && tn[i].depth < t->depth) { rg.tsel = i; rg.vsel = 0; break; }
        } else if (k == GK_DEL) reg_delete_key();
    } else {
        if (k == GK_UP && rg.vsel > 0) rg.vsel--;
        else if (k == GK_DOWN && rg.vsel < nvl - 1) rg.vsel++;
        else if (k == 13 || k == 'e' || k == 'E') reg_edit_value();
        else if (k == GK_DEL) reg_delete_value();
        else if (k == GK_LEFT) rg.focus = 0;
    }
}

/* ================= VaporGlass Window Chrome & Menus ================= */

static const char *const mn_settings[] = { "File", "View", NULL };
static const char *const mi_set0[] = { "Home page", "Close window", NULL };
static const char *const mi_set1[] = { "Previous category", "Next category", NULL };

static const char *const mn_registry[] = { "File", "Edit", "View", NULL };
static const char *const mi_reg0[] = { "Reload", "Close window", NULL };
static const char *const mi_reg1[] = { "New value...", "Edit value...", "Delete value", "Delete key", NULL };
static const char *const mi_reg2[] = { "Expand all", "Collapse all", NULL };

static const char *const mn_terminal[] = { "Shell", NULL };
static const char *const mi_term0[] = { "Clear terminal", "Restart shell", "Close window", NULL };

static const char *const mn_app_def[] = { "Window", NULL };
static const char *const mi_app0[] = { "Maximize / Restore", "Minimize", "Close window", NULL };

static const char *const *menu_names(int kind) {
    if (kind == WK_SETTINGS) return mn_settings;
    if (kind == WK_REGISTRY) return mn_registry;
    if (kind == WK_TERMINAL) return mn_terminal;
    return mn_app_def;
}
static const char *const *menu_items(int kind, int m) {
    if (kind == WK_SETTINGS) return m == 0 ? mi_set0 : mi_set1;
    if (kind == WK_REGISTRY) return m == 0 ? mi_reg0 : m == 1 ? mi_reg1 : mi_reg2;
    if (kind == WK_TERMINAL) return mi_term0;
    return mi_app0;
}

static void close_win(struct win *w) {
    if (w->kind == WK_TERMINAL) term_stop();
    media_release(w->kind);
    int i;
    for (i = 0; i < nz; i++) if (zord[i] == w->id) break;
    for (; i < nz - 1; i++) zord[i] = zord[i + 1];
    if (nz > 0) nz--;
    w->used = 0;
    menu_win = 0;
}

static const char *kind_sym(int kind) {
    switch (kind) {
    case WK_SETTINGS:  return "St";
    case WK_REGISTRY:  return "Rg";
    case WK_TERMINAL:  return "Tm";
    case WK_FILES:     return "Fl";
    case WK_NOTES:     return "Nt";
    case WK_CALC:      return "Ca";
    case WK_CANVAS:    return "Cv";
    case WK_SYSMON:    return "Sm";
    case WK_NETCENTER: return "Nc";
    case WK_MINES:     return "Gm";
    case WK_IMGVIEW:   return "Pv";
    case WK_VIDEO:     return "Vp";
    case WK_BROWSER:   return "Wb";
    default:           return "Mg";
    }
}

static struct win *open_win_arg(int kind, const char *arg) {
    struct win *w = win_by_kind(kind);
    if (w) {
        w->minimized = 0;
        raise_win(w);
        if (kind == WK_TERMINAL && term.pty_fd < 0)
            term_start((w->w - 20) / 8, (w->h - TITLE_H - MENU_H - 16) / 16, NULL);
        else if (kind >= WK_FILES && arg && arg[0]) {
            int dw = w->w, dh = w->h;
            apps_init_window(kind, arg, w->title, &dw, &dh);
        }
        return w;
    }
    for (int i = 0; i < N(wins); i++) if (!wins[i].used) { w = &wins[i]; break; }
    if (!w) { toast("Too many open windows (close one first)"); return NULL; }
    memset(w, 0, sizeof *w);
    w->used = 1; w->id = next_id++; w->kind = kind;
    snprintf(w->sym, sizeof w->sym, "%s", kind_sym(kind));

    int W = gfx_width(), dh = desk_y1 - desk_y0;
    if (kind == WK_SETTINGS) {
        snprintf(w->title, sizeof w->title, "Settings");
        w->w = W - 80 < 780 ? W - 40 : 780;
        w->h = dh - 50 < 510 ? dh - 24 : 510;
        st.page = -1; st.sel = -1; st.scroll = 0; st.hsel = 0;
        scan_apps();
    } else if (kind == WK_REGISTRY) {
        snprintf(w->title, sizeof w->title, "Registry Editor");
        w->w = W - 80 < 740 ? W - 40 : 740;
        w->h = dh - 50 < 470 ? dh - 24 : 470;
        nexp = 0; exp_set(HIVE_SYSTEM, "", 1); exp_set(HIVE_USER, "", 1);
        memset(&rg, 0, sizeof rg);
    } else if (kind == WK_TERMINAL) {
        snprintf(w->title, sizeof w->title, "Terminal");
        w->w = W - 80 < 680 ? W - 40 : 680;
        w->h = dh - 50 < 430 ? dh - 24 : 430;
        if (term_start((w->w - 20) / 8, (w->h - TITLE_H - MENU_H - 16) / 16, NULL) < 0)
            toast("Could not open /dev/ptmx; check devpts mount");
    } else {
        int defw = 640, defh = 420;
        apps_init_window(kind, arg, w->title, &defw, &defh);
        w->w = defw; w->h = defh;
    }
    int left_pad = (cf.desk_icons && W - w->w > 130) ? 52 : 0;
    w->x = left_pad + (W - left_pad - w->w) / 2 + (nz % 5) * 38 - 54;
    w->y = desk_y0 + (dh - w->h) / 2 + (nz % 5) * 30 - 36;
    clamp_win(w);
    zord[nz++] = w->id;
    return w;
}

static void draw_window(struct win *w, int active) {
    const struct pal *p = &cf.p;
    cur_wid = w->id;
    reset_ui_clip();
    hot_add(w->x, w->y, w->w, w->h, A_WINBODY, w->id, 0);

    int rad = w->maximized ? 4 : cf.radius;
    uint32_t rim = active ? cf.acc : p->border;

    /* Magnesium VaporGlass outer shell (real frosted blur + mineral sheen + luminous rim) */
    gfx_shadow(w->x, w->y, w->w, w->h, rad);
    gfx_glass_panel(w->x, w->y, w->w, w->h, rad, p->win, cf.glass_alpha, cf.blur_r, rim, cf.sheen);

    /* Translucent ionized titlebar (uniquely Magnesium: periodic element badge + pill controls) */
    uint32_t tbg = active ? cf.acc : p->titleoff;
    int talpha = cf.glass_mode == 3 ? 255 : (active ? 190 : 140);
    gfx_rrect_alpha(w->x + 1, w->y + 1, w->w - 2, TITLE_H, rad, tbg, talpha);
    uint32_t tfg = active ? cf.accfg : p->titlefgoff;

    /* Periodic symbol pill on left of header */
    gfx_rrect_alpha(w->x + 9, w->y + 6, 28, 22, 5, active ? 0x09111F : p->win, active ? 185 : 180);
    gfx_text(w->x + 15, w->y + 9, w->sym, active ? cf.acc : p->muted, 1, 1, 22);
    gfx_text(w->x + 44, w->y + 9, w->title, tfg, 1, 1, w->w - 150);

    hot_add(w->x + 6, w->y, w->w - 108, TITLE_H, A_TITLE, w->id, 0);

    /* Minimize / Maximize / Close mineral pills */
    int bx_close = w->x + w->w - 33;
    int bx_max   = w->x + w->w - 63;
    int bx_min   = w->x + w->w - 93;
    int by = w->y + 6;

    int hmin = point_in(hover_x, hover_y, bx_min, by, 25, 22);
    gfx_rrect_alpha(bx_min, by, 25, 22, 6, hmin ? p->sel : 0x0F172A, active ? 175 : 140);
    gfx_text(bx_min + 8, by + 3, "\xe2\x80\x94", active ? 0xFFFFFF : p->winfg, 1, 1, 16);
    hot_add(bx_min, by, 25, 22, A_WIN_MIN, w->id, 0);

    int hmax = point_in(hover_x, hover_y, bx_max, by, 25, 22);
    gfx_rrect_alpha(bx_max, by, 25, 22, 6, hmax ? p->sel : 0x0F172A, active ? 175 : 140);
    gfx_box(bx_max + 8, by + 6, 9, 9, active ? 0xFFFFFF : p->winfg, 1);
    hot_add(bx_max, by, 25, 22, A_WIN_MAX, w->id, 0);

    int hcls = point_in(hover_x, hover_y, bx_close, by, 25, 22);
    gfx_rrect(bx_close, by, 25, 22, 6, hcls ? 0xFF5555 : p->bad);
    gfx_text(bx_close + 9, by + 3, "\xc3\x97", 0xFFFFFF, 1, 1, 16);
    hot_add(bx_close, by, 25, 22, A_WIN_CLOSE, w->id, 0);

    /* Menu bar */
    int my = w->y + TITLE_H;
    gfx_rect_alpha(w->x + 1, my, w->w - 2, MENU_H, p->win_alt, cf.glass_mode == 3 ? 255 : 210);
    gfx_hline(w->x + 1, my + MENU_H - 1, w->w - 2, p->border);
    int mx = w->x + 10;
    const char *const *names = menu_names(w->kind);
    for (int m = 0; names[m]; m++) {
        int mw = gfx_text_width(names[m], 1) + 16;
        int open = menu_win == w->id && menu_idx == m;
        if (open || point_in(hover_x, hover_y, mx, my + 2, mw, MENU_H - 4))
            gfx_rrect(mx, my + 2, mw, MENU_H - 4, 4, open ? cf.acc : p->sel);
        gfx_text(mx + 8, my + 5, names[m], open ? cf.accfg : p->winfg, open, 1, mw);
        hot_add(mx, my, mw, MENU_H, A_MENU, w->id, m);
        if (active && m < 4) menu_x[m] = mx;
        mx += mw + 4;
    }

    /* Client content */
    int cx = w->x + 2, cy = w->y + TITLE_H + MENU_H, cw = w->w - 4, ch = w->h - TITLE_H - MENU_H - 3;
    if (w->kind == WK_SETTINGS) settings_draw(cx, cy, cw, ch);
    else if (w->kind == WK_REGISTRY) registry_draw(cx, cy, cw, ch);
    else if (w->kind == WK_TERMINAL) terminal_draw(cx, cy, cw, ch, active);
    else apps_draw_window(w->kind, cx, cy, cw, ch, active);

    reset_ui_clip();
    gfx_rrect_outline(w->x, w->y, w->w, w->h, rad, rim);

    if (!w->maximized) {
        for (int g = 0; g < 3; g++) {
            int off = 4 + g * 4;
            gfx_hline(w->x + w->w - off - 2, w->y + w->h - 3, 2, rim);
            gfx_vline(w->x + w->w - 3, w->y + w->h - off - 2, 2, rim);
        }
        hot_add(w->x, w->y + TITLE_H, 6, w->h - TITLE_H - 8, A_EDGE, w->id, 1);
        hot_add(w->x + w->w - 6, w->y + TITLE_H, 6, w->h - TITLE_H - 8, A_EDGE, w->id, 2);
        hot_add(w->x + 8, w->y + w->h - 6, w->w - 16, 6, A_EDGE, w->id, 4);
        hot_add(w->x, w->y + w->h - 10, 10, 10, A_EDGE, w->id, 5);
        hot_add(w->x + w->w - 12, w->y + w->h - 12, 12, 12, A_EDGE, w->id, 6);
    }
    cur_wid = 0;
}

static void draw_dropdown(void) {
    struct win *w = win_by_id(menu_win);
    if (!w || w->minimized) { menu_win = 0; return; }
    const struct pal *p = &cf.p;
    int W = gfx_width(), H = gfx_height();
    reset_ui_clip();
    hot_add(0, 0, W, H, A_MENUCLOSE, 0, 0);

    const char *const *items = menu_items(w->kind, menu_idx);
    int n = 0, mw = 152;
    for (; items[n]; n++) {
        int iw = gfx_text_width(items[n], 1) + 28;
        if (iw > mw) mw = iw;
    }
    int mh = n * 26 + 8;
    int x = menu_x[menu_idx], y = w->y + TITLE_H + MENU_H;
    if (x + mw > W - 4) x = W - mw - 4;
    gfx_shadow(x, y, mw, mh, 7);
    gfx_glass_panel(x, y, mw, mh, 7, p->win, cf.glass_alpha, cf.blur_r, cf.acc, cf.sheen);
    for (int i = 0; i < n; i++) {
        int iy = y + 4 + i * 26;
        int s = i == menu_sel || point_in(hover_x, hover_y, x + 4, iy, mw - 8, 24);
        if (s) gfx_rrect(x + 4, iy, mw - 8, 24, 4, cf.acc);
        gfx_text(x + 12, iy + 4, items[i], s ? cf.accfg : p->winfg, 0, 1, mw - 24);
        hot_add(x + 4, iy, mw - 8, 24, A_MENUITEM, w->id, menu_idx * 16 + i);
    }
}

static void menu_action(struct win *w, int m, int i) {
    if (w->kind == WK_SETTINGS) {
        if (m == 0) { if (i == 0) { st.page = -1; st.scroll = 0; } else close_win(w); }
        else { int d = i == 0 ? -1 : 1; if (st.page < 0) open_cat(d < 0 ? NCATS - 1 : 0); else open_cat((st.page + d + NCATS) % NCATS); }
    } else if (w->kind == WK_REGISTRY) {
        if (m == 0) { if (i == 0) reg_refresh(); else close_win(w); }
        else if (m == 1) { if (i == 0) reg_new_value(); else if (i == 1) reg_edit_value(); else if (i == 2) reg_delete_value(); else reg_delete_key(); }
        else reg_expand_all(i == 0);
    } else if (w->kind == WK_TERMINAL) {
        if (i == 0) term_reset_grid();
        else if (i == 1) { term_stop(); term_start(term.cols, term.rows, NULL); }
        else close_win(w);
    } else {
        if (i == 0) toggle_maximize(w);
        else if (i == 1) w->minimized = 1;
        else close_win(w);
    }
}

/* ================= Start Menu (Periodic-Table VaporGlass Launcher) ================= */

enum {
    TL_PROGRAMS, TL_FILES, TL_NOTES, TL_CALC, TL_CANVAS, TL_SYSMON, TL_NETCENTER, TL_MINES,
    TL_PICTURE, TL_VIDEO, TL_BROWSER,
    TL_TERMINAL, TL_SETTINGS, TL_REGISTRY, TL_DESKTOP, TL_CONSOLE, TL_LOGOUT, TL_RESTART, TL_SHUTDOWN, TL_APP
};
struct tile { char sym[3], name[32]; int kind; char cmd[48]; };
static struct tile tiles[128];
static int ntiles, vis[128], nvis, ssel, sscroll, scols = 4;
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
    for (int i = 0; i < ntiles; i++)
        if (!sq[0] || strcasestr(tiles[i].name, sq)) vis[nvis++] = i;
    if (ssel >= nvis) ssel = nvis - 1;
    if (ssel < 0) ssel = 0;
}
static void build_tiles(void) {
    ntiles = 0;
    add_tile("Pr", "Programs",       TL_PROGRAMS,  NULL);
    add_tile("Fl", "Files",          TL_FILES,     NULL);
    add_tile("Nt", "Notes",          TL_NOTES,     NULL);
    add_tile("Ca", "Calculator",     TL_CALC,      NULL);
    add_tile("Cv", "Canvas",         TL_CANVAS,    NULL);
    add_tile("Nc", "Network Center", TL_NETCENTER, NULL);
    add_tile("Sm", "System Monitor", TL_SYSMON,    NULL);
    add_tile("Gm", "Mg Mines",       TL_MINES,     NULL);
    add_tile("Pv", "Picture Viewer", TL_PICTURE,   NULL);
    add_tile("Vp", "Video Player",   TL_VIDEO,     NULL);
    add_tile("Wb", "Web Browser",    TL_BROWSER,   NULL);
    add_tile("Tm", "Terminal",       TL_TERMINAL,  NULL);
    add_tile("St", "Settings",       TL_SETTINGS,  NULL);
    add_tile("Rg", "Registry",       TL_REGISTRY,  NULL);
    add_tile("Ds", "Show Desktop",   TL_DESKTOP,   NULL);
    add_tile("Cn", "Console",        TL_CONSOLE,   NULL);
    add_tile("Lo", "Log out",        TL_LOGOUT,    NULL);
    add_tile("Rs", "Restart",        TL_RESTART,   NULL);
    add_tile("Sd", "Shut down",      TL_SHUTDOWN,  NULL);

    scan_apps();
    for (int i = 0; i < napps; i++) {
        if (apps_kind_from_name(apps[i], NULL, NULL) > 0) continue; /* already featured above */
        char sym[3] = {
            (char)toupper((unsigned char)apps[i][0]),
            apps[i][1] ? (char)tolower((unsigned char)apps[i][1]) : ' ', 0
        };
        add_tile(sym, apps[i], TL_APP, apps[i]);
    }
    sq[0] = 0; ssel = 0; sscroll = 0;
    filter_tiles();
}

static void show_desktop(void) {
    static int saved[MAXWINS];
    if (!desk_hidden) {
        int any = 0;
        for (int i = 0; i < N(wins); i++) {
            saved[i] = wins[i].used && !wins[i].minimized;
            if (saved[i]) { wins[i].minimized = 1; any = 1; }
        }
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
    case TL_PROGRAMS:  open_win_arg(WK_FILES, "/programs"); desk_hidden = 0; break;
    case TL_FILES:     open_win(WK_FILES); desk_hidden = 0; break;
    case TL_NOTES:     open_win(WK_NOTES); desk_hidden = 0; break;
    case TL_CALC:      open_win(WK_CALC); desk_hidden = 0; break;
    case TL_CANVAS:    open_win(WK_CANVAS); desk_hidden = 0; break;
    case TL_SYSMON:    open_win(WK_SYSMON); desk_hidden = 0; break;
    case TL_NETCENTER: open_win(WK_NETCENTER); desk_hidden = 0; break;
    case TL_MINES:     open_win(WK_MINES); desk_hidden = 0; break;
    case TL_PICTURE:   open_win(WK_IMGVIEW); desk_hidden = 0; break;
    case TL_VIDEO:     open_win(WK_VIDEO); desk_hidden = 0; break;
    case TL_BROWSER:   open_win(WK_BROWSER); desk_hidden = 0; break;
    case TL_TERMINAL:  open_win(WK_TERMINAL); desk_hidden = 0; break;
    case TL_SETTINGS:  open_win(WK_SETTINGS); desk_hidden = 0; break;
    case TL_REGISTRY:  open_win(WK_REGISTRY); desk_hidden = 0; break;
    case TL_DESKTOP:   show_desktop(); break;
    case TL_CONSOLE:   quit_code = GUI_TO_SHELL; break;
    case TL_LOGOUT:    dlg_open(D_CONFIRM, P_C_LOGOUT, "Log out", "End this session?", NULL); break;
    case TL_RESTART:   dlg_open(D_CONFIRM, P_C_REBOOT, "Restart", "Restart the computer now?", NULL); break;
    case TL_SHUTDOWN:  dlg_open(D_CONFIRM, P_C_HALT, "Shut down", "Power the computer off now?", NULL); break;
    case TL_APP:       { char c[64]; snprintf(c, sizeof c, "%s", t->cmd); ui_run_terminal_cmd(c); } break;
    }
}

static void draw_start(void) {
    const struct pal *p = &cf.p;
    int W = gfx_width(), H = gfx_height();
    reset_ui_clip();
    hot_add(0, 0, W, H, A_STARTCLOSE, 0, 0);

    int tile_w = 126, tile_h = 84, gap = 10;
    scols = (W - 40) / (tile_w + gap);
    if (scols > 4) scols = 4;
    if (scols < 2) scols = 2;

    int pw = 24 + scols * tile_w + (scols - 1) * gap;
    int dh = desk_y1 - desk_y0;
    int rows = (nvis + scols - 1) / scols;
    int maxrows = (dh - 116) / (tile_h + gap);
    if (maxrows > 4) maxrows = 4;
    if (maxrows < 1) maxrows = 1;
    int vrows = rows < maxrows ? rows : maxrows;
    if (vrows < 1) vrows = 1;
    int ph = 108 + vrows * tile_h + (vrows - 1) * gap;
    int px = cf.floating_bar ? 10 : 8;
    int py = cf.top ? desk_y0 + 6 : desk_y1 - ph - 6;

    int srow = ssel / scols;
    if (srow < sscroll) sscroll = srow;
    if (srow >= sscroll + vrows) sscroll = srow - vrows + 1;
    if (sscroll < 0) sscroll = 0;

    hot_add(px, py, pw, ph, A_NONE, 0, 0);
    gfx_shadow(px, py, pw, ph, cf.radius + 2);
    gfx_glass_panel(px, py, pw, ph, cf.radius + 2, p->win, cf.glass_alpha, cf.blur_r + 2, cf.acc, cf.sheen);

    gfx_rrect(px + 12, py + 10, 56, 26, 6, cf.acc);
    gfx_text(px + 18, py + 15, "12 Mg", cf.accfg, 1, 1, 48);
    gfx_textf(px + 78, py + 15, p->winfg, 1, 1, pw - 92, "%s %s", OS_NAME, OS_VERSION);

    gfx_rrect_alpha(px + 12, py + 44, pw - 24, 28, 6, p->input_bg, 220);
    gfx_rrect_outline(px + 12, py + 44, pw - 24, 28, 6, p->border);
    if (sq[0]) {
        gfx_text(px + 22, py + 50, sq, p->winfg, 0, 1, pw - 44);
        gfx_rect(px + 22 + gfx_text_width(sq, 1), py + 49, 2, 18, cf.acc);
    } else {
        gfx_text(px + 22, py + 50, "Search /programs, tools & settings...", p->muted, 0, 1, pw - 44);
    }

    for (int vi = sscroll * scols; vi < nvis; vi++) {
        int row = vi / scols - sscroll, col = vi % scols;
        if (row >= vrows) break;
        const struct tile *t = &tiles[vis[vi]];
        int tx = px + 12 + col * (tile_w + gap);
        int ty = py + 82 + row * (tile_h + gap);
        int s = vi == ssel || point_in(hover_x, hover_y, tx, ty, tile_w, tile_h);
        gfx_rrect_alpha(tx, ty, tile_w, tile_h, 8, s ? p->sel : p->win_alt, s ? 235 : 185);
        gfx_rrect_outline(tx, ty, tile_w, tile_h, 8, s ? cf.acc : p->border);
        gfx_textf(tx + 8, ty + 5, p->muted, 0, 1, 32, "%d", vis[vi] + 1);
        int symw = gfx_text_width(t->sym, 2);
        gfx_text(tx + (tile_w - symw) / 2, ty + 19, t->sym, cf.acc, 1, 2, tile_w - 8);
        int nw = gfx_text_width(t->name, 1);
        int maxnw = tile_w - 12;
        int nx = nw < maxnw ? tx + (tile_w - nw) / 2 : tx + 6;
        gfx_text(nx, ty + tile_h - 21, t->name, p->winfg, 0, 1, maxnw);
        hot_add(tx, ty, tile_w, tile_h, A_TILE, vi, 0);
    }
    if (nvis == 0) gfx_text(px + 20, py + 96, "No matching apps", p->muted, 0, 1, pw - 40);
    gfx_text(px + 14, py + ph - 20, "Enter open  \xc2\xb7  Wheel/Arrows browse  \xc2\xb7  Esc close", p->muted, 0, 1, pw - 28);
}

/* ================= Taskbar (Floating VaporGlass Dock or Edge Bar) ================= */

static const char *pin_label(const char *n, char *sym, int *kind_out) {
    if (!strcasecmp(n, "terminal")) { strcpy(sym, "Tm"); *kind_out = WK_TERMINAL; return "Terminal"; }
    if (!strcasecmp(n, "settings")) { strcpy(sym, "St"); *kind_out = WK_SETTINGS; return "Settings"; }
    if (!strcasecmp(n, "registry")) { strcpy(sym, "Rg"); *kind_out = WK_REGISTRY; return "Registry"; }
    const char *asym = NULL, *alab = NULL;
    int ak = apps_kind_from_name(n, &asym, &alab);
    if (ak > 0) {
        snprintf(sym, 3, "%s", asym);
        *kind_out = ak;
        return alab;
    }
    sym[0] = (char)toupper((unsigned char)n[0]);
    sym[1] = n[1] ? (char)tolower((unsigned char)n[1]) : ' ';
    sym[2] = 0;
    *kind_out = 0;
    return n;
}

static const char *win_pin_key(int kind) {
    switch (kind) {
    case WK_TERMINAL:  return "terminal";
    case WK_SETTINGS:  return "settings";
    case WK_REGISTRY:  return "registry";
    case WK_FILES:     return "files";
    case WK_NOTES:     return "notes";
    case WK_CALC:      return "calc";
    case WK_CANVAS:    return "canvas";
    case WK_SYSMON:    return "sysmon";
    case WK_NETCENTER: return "netcenter";
    case WK_MINES:     return "mines";
    case WK_IMGVIEW:   return "imgview";
    case WK_VIDEO:     return "videoview";
    case WK_BROWSER:   return "browser";
    default:           return "";
    }
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
    int W = gfx_width(), H = gfx_height();
    int margin_x = cf.floating_bar ? 10 : 0;
    int margin_y = cf.floating_bar ? 5 : 0;
    int bx = margin_x, bw_bar = W - 2 * margin_x;
    int y = cf.top ? margin_y : H - BAR_H - margin_y;
    int rad = cf.floating_bar ? (cf.radius > 12 ? 12 : cf.radius) : 0;
    reset_ui_clip();
    cur_wid = 0;

    if (cf.floating_bar) {
        gfx_shadow(bx, y, bw_bar, BAR_H, rad);
        gfx_glass_panel(bx, y, bw_bar, BAR_H, rad, p->bar, cf.glass_alpha, cf.blur_r, p->border, cf.sheen);
    } else {
        gfx_glass_panel(0, y, W, BAR_H, 0, p->bar, cf.glass_alpha, cf.blur_r, p->border, cf.sheen / 2);
    }

    /* 12 Mg Start button */
    gfx_rrect(bx + 6, y + 6, 68, BAR_H - 12, 7, cf.acc);
    gfx_text(bx + 16, y + 12, "12 Mg", cf.accfg, 1, 1, 56);
    hot_add(bx + 6, y + 4, 68, BAR_H - 8, A_START, 0, 0);

    int x = bx + 82, right = bx + bw_bar - 10;

    if (cf.clock) {
        char ck[80] = "";
        clock_str(ck, sizeof ck, 0);
        int cw = gfx_text_width(ck, 1);
        right -= cw + 14;
        gfx_text(right + 6, y + 12, ck, p->barfg, 1, 1, cw + 8);
    }

    if (cf.controls) {
        right -= 96;
        draw_button(right,      y + 7, 28, BAR_H - 14, "\xc3\x97", 0xFFFFFF, p->bad, A_CLOSE, 0, 0);
        draw_button(right + 32, y + 7, 28, BAR_H - 14, "O", p->winfg, p->titleoff, A_DESKTOP, 0, 0);
        draw_button(right + 64, y + 7, 28, BAR_H - 14, "\xe2\x80\x94", p->winfg, p->titleoff, A_HIDE, 0, 0);
        right -= 8;
    }

    npins = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", cf.pinned);
    for (char *sp, *t = strtok_r(buf, ",", &sp); t && npins < N(pins); t = strtok_r(NULL, ",", &sp)) {
        while (*t == ' ') t++;
        if (*t && strlen(t) < sizeof pins[0]) snprintf(pins[npins++], sizeof pins[0], "%s", t);
    }

    struct win *act = active_win();
    for (int i = 0; i < npins; i++) {
        char sym[4];
        int kind = 0;
        const char *lab = pin_label(pins[i], sym, &kind);
        struct win *w = kind ? win_by_kind(kind) : NULL;
        const char *disp = cf.labels ? lab : sym;
        int bw = gfx_text_width(disp, 1) + 22;
        if (x + bw > right) break;
        int hov = point_in(hover_x, hover_y, x, y + 6, bw, BAR_H - 12);
        if (w || hov) {
            uint32_t bg = w ? (w == act ? p->sel : p->titleoff) : p->win_alt;
            gfx_rrect_alpha(x, y + 6, bw, BAR_H - 12, 6, bg, 210);
        }
        if (w) gfx_rect(x + 8, y + BAR_H - 6, bw - 16, 2, w == act ? cf.acc : p->muted);
        gfx_text(x + 11, y + 12, disp, w && w == act ? cf.acc : p->barfg, w == act, 1, bw - 14);
        hot_add(x, y + 4, bw, BAR_H - 8, A_PIN, i, 0);
        x += bw + 5;
    }

    for (int i = 0; i < N(wins); i++) {
        if (!wins[i].used) continue;
        const char *pk = win_pin_key(wins[i].kind);
        if (pk[0] && is_pinned(pk)) continue;
        if (wins[i].kind == WK_FILES && is_pinned("programs")) continue;
        int bw = gfx_text_width(wins[i].title, 1) + 22;
        if (x + bw > right) break;
        int is_a = &wins[i] == act;
        gfx_rrect_alpha(x, y + 6, bw, BAR_H - 12, 6, is_a ? p->sel : p->titleoff, 215);
        gfx_rect(x + 8, y + BAR_H - 6, bw - 16, 2, is_a ? cf.acc : p->muted);
        gfx_text(x + 11, y + 12, wins[i].title, is_a ? cf.acc : p->barfg, is_a, 1, bw - 14);
        hot_add(x, y + 4, bw, BAR_H - 8, A_TASKWIN, wins[i].id, 0);
        x += bw + 5;
    }
}

/* ================= Wallpaper Generator (Aurora, Nebula, Prism, Gradient) ================= */

static inline uint32_t add_rgb_clamped(uint32_t base, uint32_t glow, int weight) {
    if (weight <= 0) return base;
    int r = (int)((base >> 16) & 255) + (((int)((glow >> 16) & 255) * weight) >> 8);
    int g = (int)((base >> 8) & 255)  + (((int)((glow >> 8) & 255) * weight) >> 8);
    int b = (int)(base & 255)         + (((int)(glow & 255) * weight) >> 8);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Picture wallpaper: the file is decoded when its path changes, then cover-fitted
 * (scaled to fill, centre-cropped) into wp_cache. Transparent pixels show the gradient. */
static mg_image wp_pic;
static char wp_pic_path[256];
static void paint_picture(int W, int H) {
    if (!wp_pic.px || !wp_cache) return;
    double sx = (double)W / wp_pic.w, sy = (double)H / wp_pic.h;
    double s = sx > sy ? sx : sy;
    int tw = (int)(wp_pic.w * s + 0.5), th = (int)(wp_pic.h * s + 0.5);
    if (tw < W) tw = W;
    if (th < H) th = H;
    mg_image sc = { 0, 0, NULL };
    if (img_resample(&wp_pic, tw, th, &sc) != 0 || !sc.px) return;
    int ox = (tw - W) / 2, oy = (th - H) / 2;
    for (int y = 0; y < H; y++) {
        const uint32_t *src = &sc.px[(size_t)(y + oy) * tw + ox];
        uint32_t *dst = &wp_cache[y * W];
        for (int x = 0; x < W; x++) {
            uint32_t c = src[x];
            unsigned a = c >> 24;
            if (a == 255) { dst[x] = c & 0xFFFFFFu; continue; }
            if (a == 0) continue;
            uint32_t d = dst[x];
            unsigned ia = 255 - a;
            unsigned r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * ia) / 255;
            unsigned g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * ia) / 255;
            unsigned b = ((c & 255) * a + (d & 255) * ia) / 255;
            dst[x] = (r << 16) | (g << 8) | b;
        }
    }
    img_free(&sc);
}

static void rebuild_wallpaper_cache(int W, int H) {
    if (W <= 0 || H <= 0) return;
    if (strcmp(wp_pic_path, cf.pic_path) != 0) {
        snprintf(wp_pic_path, sizeof wp_pic_path, "%s", cf.pic_path);
        img_free(&wp_pic);
        char err[160];
        if (cf.pic_path[0]) img_load_file(cf.pic_path, &wp_pic, err, sizeof err);
        wp_w = 0;   /* force a rebuild below */
    }
    if (wp_cache && wp_w == W && wp_h == H && wp_pat == cf.bgpat &&
        wp_top == cf.p.desk_top && wp_bot == cf.p.desk_bot && wp_acc == cf.acc)
        return;

    free(wp_cache);
    wp_cache = malloc((size_t)(W * H) * sizeof(uint32_t));
    if (!wp_cache) return;
    wp_w = W; wp_h = H; wp_pat = cf.bgpat;
    wp_top = cf.p.desk_top; wp_bot = cf.p.desk_bot; wp_acc = cf.acc;

    int r0 = (wp_top >> 16) & 255, g0 = (wp_top >> 8) & 255, b0 = wp_top & 255;
    int r1 = (wp_bot >> 16) & 255, g1 = (wp_bot >> 8) & 255, b1 = wp_bot & 255;
    int denom = H > 1 ? H - 1 : 1;

    for (int y = 0; y < H; y++) {
        uint32_t base = cf.bgpat == 5 ? wp_top :
                        (((uint32_t)(r0 + (r1 - r0) * y / denom) << 16) |
                         ((uint32_t)(g0 + (g1 - g0) * y / denom) << 8) |
                         (uint32_t)(b0 + (b1 - b0) * y / denom));
        uint32_t *row = &wp_cache[y * W];
        for (int x = 0; x < W; x++) row[x] = base;
    }

    if (cf.bgpat == 0) {
        /* Aurora: organic smooth S-curve luminous mineral ribbons */
        uint32_t c2 = 0x2DD4BFu;
        for (int x = 0; x < W; x++) {
            /* Normalized u in [-256, 256]; smooth cubic wave u*(3*256^2 - u^2) */
            long long u = (long long)x * 512 / (W > 1 ? W - 1 : 1) - 256;
            long long s1 = (u * (3LL * 65536 - u * u)) / (2LL * 65536); /* [-256..256] */
            int cy1 = (H * 42) / 100 + (int)((s1 * H) / 1500);
            long long u2 = ((long long)(W - 1 - x) * 512) / (W > 1 ? W - 1 : 1) - 256;
            long long s2 = (u2 * (3LL * 65536 - u2 * u2)) / (2LL * 65536);
            int cy2 = (H * 58) / 100 - (int)((s2 * H) / 1700);
            for (int y = 0; y < H; y++) {
                int d1 = abs(y - cy1);
                if (d1 < 150) {
                    int w = ((150 - d1) * (150 - d1) * 82) / (150 * 150);
                    wp_cache[y * W + x] = add_rgb_clamped(wp_cache[y * W + x], wp_acc, w);
                }
                int d2 = abs(y - cy2);
                if (d2 < 125) {
                    int w = ((125 - d2) * (125 - d2) * 62) / (125 * 125);
                    wp_cache[y * W + x] = add_rgb_clamped(wp_cache[y * W + x], c2, w);
                }
            }
        }
    } else if (cf.bgpat == 2) {
        /* Nebula: soft glowing radial orbs */
        int cx1 = (W * 28) / 100, cy1 = (H * 36) / 100, rad1 = (H * 46) / 100;
        int cx2 = (W * 74) / 100, cy2 = (H * 64) / 100, rad2 = (H * 42) / 100;
        int r1sq = rad1 * rad1 + 1, r2sq = rad2 * rad2 + 1;
        for (int y = 0; y < H; y++) {
            uint32_t *row = &wp_cache[y * W];
            for (int x = 0; x < W; x++) {
                int d1 = (x - cx1) * (x - cx1) + (y - cy1) * (y - cy1);
                if (d1 < r1sq) {
                    int w = (int)(((long long)(r1sq - d1) * 95LL) / r1sq);
                    row[x] = add_rgb_clamped(row[x], wp_acc, w);
                }
                int d2 = (x - cx2) * (x - cx2) + (y - cy2) * (y - cy2);
                if (d2 < r2sq) {
                    int w = (int)(((long long)(r2sq - d2) * 75LL) / r2sq);
                    row[x] = add_rgb_clamped(row[x], 0xC084FCu, w);
                }
            }
        }
    } else if (cf.bgpat == 3) {
        /* Prism: diagonal crystalline light facets */
        for (int y = 0; y < H; y++) {
            uint32_t *row = &wp_cache[y * W];
            for (int x = 0; x < W; x++) {
                int u = (x + y * 2) % 260;
                int v = (x * 2 - y + 2000) % 310;
                if (u < 90) row[x] = add_rgb_clamped(row[x], wp_acc, ((90 - u) * 38) / 90);
                if (v < 70) row[x] = add_rgb_clamped(row[x], 0xFFFFFFu, ((70 - v) * 20) / 70);
            }
        }
    } else if (cf.bgpat == 4) {
        /* Grid */
        for (int y = 0; y < H; y++) {
            uint32_t *row = &wp_cache[y * W];
            for (int x = 0; x < W; x++) {
                if (x % 32 == 0 || y % 32 == 0)
                    row[x] = add_rgb_clamped(row[x], cf.p.border, 70);
            }
        }
    }
    if (cf.bgpat == 6) paint_picture(W, H);
}

static void draw_wallpaper(void) {
    int W = gfx_width(), H = gfx_height();
    reset_ui_clip();
    rebuild_wallpaper_cache(W, H);
    if (!wp_cache) {
        gfx_gradient_v(0, 0, W, H, cf.p.desk_top, cf.p.desk_bot);
        return;
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) gfx_put(x, y, wp_cache[y * W + x]);
    }
}

/* ================= Desktop Shortcut Icons & Right-Click Context Menu ================= */

static const struct { const char *sym, *label; int kind; const char *arg; } desk_shortcuts[] = {
    { "Pr", "Programs", WK_FILES,     "/programs" },
    { "Nt", "Notes",    WK_NOTES,     NULL },
    { "Ca", "Calc",     WK_CALC,      NULL },
    { "Cv", "Canvas",   WK_CANVAS,    NULL },
    { "Nc", "Network",  WK_NETCENTER, NULL },
    { "Sm", "Monitor",  WK_SYSMON,    NULL },
    { "Tm", "Terminal", WK_TERMINAL,  NULL },
};

static void draw_desktop_icons(void) {
    if (!cf.desk_icons) return;
    const struct pal *p = &cf.p;
    reset_ui_clip();
    cur_wid = 0;
    int ix = 16, iy = desk_y0 + 14;
    int iw = 76, ih = 66, gap = 10;

    for (int i = 0; i < N(desk_shortcuts); i++) {
        if (iy + ih > desk_y1 - 8) {
            iy = desk_y0 + 14;
            ix += iw + 12;
        }
        int hov = point_in(hover_x, hover_y, ix, iy, iw, ih);
        gfx_rrect_alpha(ix, iy, iw, ih, 8, hov ? p->sel : p->win, hov ? 205 : 135);
        gfx_rrect_outline(ix, iy, iw, ih, 8, hov ? cf.acc : p->border);
        int sw = gfx_text_width(desk_shortcuts[i].sym, 2);
        gfx_text(ix + (iw - sw) / 2, iy + 8, desk_shortcuts[i].sym, cf.acc, 1, 2, iw - 8);
        int lw = gfx_text_width(desk_shortcuts[i].label, 1);
        gfx_text(ix + (iw - lw) / 2, iy + 44, desk_shortcuts[i].label, p->winfg, 0, 1, iw - 6);
        hot_add(ix, iy, iw, ih, A_DESK_ICON, i, 0);
        iy += ih + gap;
    }
}

static const char *const ctx_labels[] = {
    "Open /programs folder",
    "Open Terminal",
    "Next Theme Preset",
    "Toggle VaporGlass",
    "Edit ~/.mg.conf (Manual)",
    "Display & Theme Settings",
    NULL
};

static void draw_context_menu(void) {
    if (!ctx_open) return;
    int W = gfx_width(), H = gfx_height();
    reset_ui_clip();
    cur_wid = 0;
    hot_add(0, 0, W, H, A_CTX_CLOSE, 0, 0);

    int mw = 224, mh = 6 * 28 + 10;
    int x = ctx_x, y = ctx_y;
    if (x + mw > W - 8) x = W - mw - 8;
    if (y + mh > H - 8) y = H - mh - 8;

    gfx_shadow(x, y, mw, mh, 8);
    gfx_glass_panel(x, y, mw, mh, 8, cf.p.win, cf.glass_alpha, cf.blur_r + 2, cf.acc, cf.sheen);
    for (int i = 0; ctx_labels[i]; i++) {
        int iy = y + 5 + i * 28;
        int hov = point_in(hover_x, hover_y, x + 5, iy, mw - 10, 25);
        if (hov) gfx_rrect(x + 5, iy, mw - 10, 25, 5, cf.acc);
        gfx_text(x + 14, iy + 4, ctx_labels[i], hov ? cf.accfg : cf.p.winfg, hov, 1, mw - 24);
        hot_add(x + 5, iy, mw - 10, 25, A_CTX_ITEM, i, 0);
    }
}

static void render_desktop(void) {
    const struct pal *p = &cf.p;
    int W = gfx_width(), H = gfx_height();
    nhots = 0; cur_wid = 0;

    draw_wallpaper();
    draw_desktop_icons();

    int anyvis = 0;
    struct win *act = active_win();
    for (int i = 0; i < nz; i++) {
        struct win *w = win_by_id(zord[i]);
        if (!w || w->minimized) continue;
        anyvis = 1;
        draw_window(w, w == act);
    }

    if (!anyvis && !start_open) {
        const struct gfx_info *gi = gfx_get_info();
        char h1[120], h2[140], h3[160];
        snprintf(h1, sizeof h1, "%s %s", OS_NAME, OS_VERSION_LONG);
        snprintf(h2, sizeof h2, "Click 12 Mg, open Programs (/programs), or right-click desktop");
        snprintf(h3, sizeof h3, "%s  \xc2\xb7  %dx%d  \xc2\xb7  VaporGlass", gi->desc, gi->width, gi->height);
        int cy = (desk_y0 + desk_y1) / 2;
        gfx_text((W - gfx_text_width(h1, 2)) / 2, cy - 42, h1, cf.acc, 1, 2, W - 32);
        gfx_text((W - gfx_text_width(h2, 1)) / 2, cy + 4, h2, p->winfg, 0, 1, W - 32);
        gfx_text((W - gfx_text_width(h3, 1)) / 2, cy + 28, h3, p->muted, 0, 1, W - 32);
    }

    draw_taskbar();
    if (start_open) draw_start();
    if (menu_win) draw_dropdown();
    if (ctx_open) draw_context_menu();
    draw_dialog();

    if (toast_msg[0] && time(NULL) < toast_until) {
        int tw = gfx_text_width(toast_msg, 1) + 32;
        if (tw > W - 24) tw = W - 24;
        int tx = (W - tw) / 2;
        int ty = cf.top ? H - 46 : BAR_H + 14;
        reset_ui_clip();
        gfx_shadow(tx, ty, tw, 32, 8);
        gfx_glass_panel(tx, ty, tw, 32, 8, p->win, 225, cf.blur_r, cf.acc, cf.sheen);
        gfx_text(tx + 16, ty + 8, toast_msg, p->winfg, 1, 1, tw - 32);
    }
}

/* ================= Dialog & Input Actions ================= */

static void dialog_ok(void) {
    struct dlg d = dlg;
    dlg.type = D_NONE;
    switch (d.purpose) {
    case P_SET_TEXT: {
        char val[256];
        snprintf(val, sizeof val, "%s", d.buf);
        if (!strcmp(pend_key, "Security/MinPasswordLength")) {
            int n = atoi(val); if (n < 4) n = 4; if (n > 64) n = 64; snprintf(val, sizeof val, "%d", n);
        } else if (!strcmp(pend_key, "Security/LoginDelay")) {
            int n = atoi(val); if (n < 0) n = 0; if (n > 30) n = 30; snprintf(val, sizeof val, "%d", n);
        }
        if (reg_set(pend_hive, pend_key, val) < 0) save_error("the setting");
        load_cfg();
        layout_desktop();
        if (!strcmp(pend_key, "Display/CursorFile") && val[0] && !gfx_cursor_is_custom())
            toast("Could not load that cursor image. Use PNG or BMP up to 64x64 px.");
        break;
    }
    case P_SET_TIME: {
        int Y, M, D, h, m;
        if (sscanf(d.buf, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) != 5 ||
            Y < 2000 || Y > 2100 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59) {
            toast("Use the form YYYY-MM-DD HH:MM");
            break;
        }
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D; tm.tm_hour = h; tm.tm_min = m;
        struct timespec ts = { timegm(&tm), 0 };
        if (clock_settime(CLOCK_REALTIME, &ts) < 0)
            toast("Cannot set the time: %s", getuid() == 0 ? strerror(errno) : "only root can do this");
        else toast("Time set");
        break;
    }
    case P_REG_EDIT:
        if (reg_set(pend_hive, pend_key, d.buf) < 0) save_error(pend_key);
        break;
    case P_REG_NEWNAME: {
        if (!d.buf[0]) break;
        char full[160], msg[200];
        snprintf(full, sizeof full, "%.90s%s%.64s", pend_key, pend_key[0] ? "/" : "", d.buf);
        snprintf(pend_key, sizeof pend_key, "%s", full);
        snprintf(msg, sizeof msg, "Value for %s:", pend_key);
        dlg_open(D_INPUT, P_REG_NEWVAL, "New value", msg, "");
        break;
    }
    case P_REG_NEWVAL:
        if (reg_set(pend_hive, pend_key, d.buf) < 0) {
            if (errno == EINVAL) toast("Invalid name: %s", pend_key); else save_error(pend_key);
        } else {
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
        if (reboot(RB_AUTOBOOT) < 0)
            toast("Restart failed: %s", getuid() == 0 ? strerror(errno) : "only root can do this");
        break;
    case P_C_HALT:
        sync();
        if (reboot(RB_POWER_OFF) < 0)
            toast("Shut down failed: %s", getuid() == 0 ? strerror(errno) : "only root can do this");
        break;
    case P_C_LOGOUT: quit_code = GUI_LOGOUT; break;
    case P_C_RESET:
        for (int i = reg_count(HIVE_USER) - 1; i >= 0; i--) {
            const char *k = reg_entry(HIVE_USER, i, NULL);
            if (k && strncmp(k, "Environment/", 12)) {
                char key[160]; snprintf(key, sizeof key, "%s", k); reg_del(HIVE_USER, key);
            }
        }
        toast("Settings reset");
        break;
    case P_C_DELKEY:
        if (reg_del_prefix(pend_hive, pend_key) < 0) save_error(pend_key); else rg.vsel = 0;
        break;
    case P_C_DELVAL:
        if (reg_del(pend_hive, pend_key) < 0) save_error(pend_key);
        break;
    }
    load_cfg();
    layout_desktop();
}

static void edit_buf_key(char *buf, size_t cap, int *len, int *cur, int k) {
    if ((k == 127 || k == 8) && *cur > 0) {
        memmove(buf + *cur - 1, buf + *cur, (size_t)(*len - *cur + 1));
        (*cur)--; (*len)--;
    } else if (k == GK_DEL && *cur < *len) {
        memmove(buf + *cur, buf + *cur + 1, (size_t)(*len - *cur));
        (*len)--;
    } else if (k == GK_LEFT && *cur > 0) (*cur)--;
    else if (k == GK_RIGHT && *cur < *len) (*cur)++;
    else if (k == GK_HOME) *cur = 0;
    else if (k == GK_END) *cur = *len;
    else if (k >= 32 && k <= 126 && (size_t)(*len + 1) < cap) {
        memmove(buf + *cur + 1, buf + *cur, (size_t)(*len - *cur + 1));
        buf[(*cur)++] = (char)k;
        (*len)++;
    }
}

static void dialog_key(int k) {
    if (dlg.type == D_INPUT) {
        if (k == 13) { dialog_ok(); return; }
        if (k == 27) { dlg.type = D_NONE; return; }
        edit_buf_key(dlg.buf, sizeof dlg.buf, &dlg.len, &dlg.cur, k);
        return;
    }
    if (dlg.type == D_CONFIRM) {
        if (k == 13 || k == 'y' || k == 'Y') dialog_ok();
        else if (k == 27 || k == 'n' || k == 'N') dlg.type = D_NONE;
    } else if (k == 13 || k == 27) dlg.type = D_NONE;
}

static void do_drag(int x, int y) {
    struct win *w = win_by_id(drg.wid);
    if (!w || w->maximized) { drg.on = 0; return; }
    if (drg.mode == 1) { w->x = x - drg.ox; w->y = y - drg.oy; clamp_win(w); return; }
    int W = gfx_width();
    int minw = w->kind == WK_CALC ? 280 : 340;
    int dx = x - drg.sx, dy = y - drg.sy;
    int nx = drg.wx, nw = drg.ww, nh = drg.wh;
    if (drg.edge & 1) { nx = drg.wx + dx; nw = drg.ww - dx; }
    if (drg.edge & 2) nw = drg.ww + dx;
    if (drg.edge & 4) nh = drg.wh + dy;
    if (nw < minw) { if (drg.edge & 1) nx -= minw - nw; nw = minw; }
    if (nh < 220) nh = 220;
    if (nx < 0) { nw += nx; nx = 0; }
    if (nx + nw > W) nw = W - nx;
    if (w->y + nh > desk_y1) nh = desk_y1 - w->y;
    w->x = nx; w->w = nw; w->h = nh;
}

static void pin_click(int i) {
    if (i < 0 || i >= npins) return;
    char name[40];
    snprintf(name, sizeof name, "%s", pins[i]);
    if (!strcasecmp(name, "terminal")) toggle_open(WK_TERMINAL);
    else if (!strcasecmp(name, "settings")) toggle_open(WK_SETTINGS);
    else if (!strcasecmp(name, "registry")) toggle_open(WK_REGISTRY);
    else {
        int wk = apps_kind_from_name(name, NULL, NULL);
        if (wk > 0) toggle_open(wk);
        else ui_run_terminal_cmd(name);
    }
}

static void handle_ctx_action(int idx) {
    ctx_open = 0;
    switch (idx) {
    case 0: open_win_arg(WK_FILES, "/programs"); break;
    case 1: open_win(WK_TERMINAL); break;
    case 2: {
        int next = (cf.theme_idx + 1) % 8;
        char tname[32];
        opt_get(THEME_LIST, next, tname, sizeof tname);
        reg_set(HIVE_USER, "Display/Theme", tname);
        load_cfg();
        toast("Theme: %s", tname);
        break;
    }
    case 3: {
        const char *next = cf.glass_mode == 0 ? "Solid" : "VaporGlass";
        reg_set(HIVE_USER, "Display/Glass", next);
        load_cfg();
        toast("Glass mode: %s", next);
        break;
    }
    case 4: {
        char p[200];
        open_win_arg(WK_NOTES, mg_conf_user_path(p, sizeof p));
        break;
    }
    case 5:
        open_win(WK_SETTINGS);
        open_cat(1);
        break;
    }
}

static int on_mouse(const struct gfx_mouse *mev) {
    int x = mev->x, y = mev->y;
    int prev_hx = hover_x, prev_hy = hover_y;
    hover_x = x; hover_y = y;

    if (mev->kind == GM_WHEEL) {
        int d = mev->wheel;
        if (start_open) {
            ssel += d * scols;
            if (ssel < 0) ssel = 0;
            if (ssel >= nvis) ssel = nvis - 1;
            if (ssel < 0) ssel = 0;
            return 1;
        }
        for (int i = nz - 1; i >= 0; i--) {
            struct win *w = win_by_id(zord[i]);
            if (!w || w->minimized || !point_in(x, y, w->x, w->y, w->w, w->h)) continue;
            if (w->kind == WK_SETTINGS) { st.scroll += d * 2; if (st.scroll < 0) st.scroll = 0; }
            else if (w->kind == WK_REGISTRY) {
                if (x < w->x + w->w / 3) { rg.tsel += d; rg.vsel = 0; if (rg.tsel < 0) rg.tsel = 0; }
                else { rg.vsel += d; if (rg.vsel < 0) rg.vsel = 0; }
            } else if (w->kind >= WK_FILES) {
                apps_on_wheel(w->kind, d);
            }
            return 1;
        }
        return 0;
    }

    struct win *act = active_win();
    if (act && act->kind >= WK_FILES &&
        apps_on_mouse_drag(act->kind, act->x + 2, act->y + TITLE_H + MENU_H,
                           act->w - 4, act->h - TITLE_H - MENU_H - 3, mev)) {
        return 1;
    }

    if (mev->kind == GM_MOVE) {
        if (drg.on && (mev->buttons_down & 1)) { do_drag(x, y); return 1; }
        const struct hot *h0 = hot_at(prev_hx, prev_hy);
        const struct hot *h1 = hot_at(x, y);
        return h0 != h1;
    }

    if (mev->kind == GM_RELEASE) { drg.on = 0; return 0; }

    /* Right-click support: cell flagging in Mg Mines, or Desktop context menu */
    if (mev->btn == 2 && mev->kind == GM_PRESS) {
        const struct hot *rh = hot_at(x, y);
        if (rh && rh->act == ACT_MINE_CELL) {
            apps_on_hotspot(rh->act, rh->a, rh->b, x, y, 2);
            return 1;
        }
        if (!rh && y >= desk_y0 && y < desk_y1) {
            ctx_open = 1; ctx_x = x; ctx_y = y; start_open = 0; menu_win = 0;
            return 1;
        }
        return 0;
    }

    if (mev->btn != 0) return 0;

    const struct hot *h = hot_at(x, y);
    if (!h) {
        if (ctx_open) { ctx_open = 0; return 1; }
        return 0;
    }
    struct hot hh = *h;
    struct win *w = hh.wid ? win_by_id(hh.wid) : NULL;
    if (w) raise_win(w);

    if (hh.act >= ACT_APP_BASE) {
        apps_on_hotspot(hh.act, hh.a, hh.b, x, y, mev->btn);
        return 1;
    }

    switch (hh.act) {
    case A_START: start_open = !start_open; ctx_open = 0; if (start_open) { build_tiles(); menu_win = 0; } break;
    case A_STARTCLOSE: start_open = 0; break;
    case A_TILE: if (hh.a < nvis) launch_tile(vis[hh.a]); break;
    case A_PIN: pin_click(hh.a); break;
    case A_DESK_ICON:
        if (hh.a >= 0 && hh.a < N(desk_shortcuts)) {
            open_win_arg(desk_shortcuts[hh.a].kind, desk_shortcuts[hh.a].arg);
            desk_hidden = 0;
        }
        break;
    case A_CTX_ITEM: handle_ctx_action(hh.a); break;
    case A_CTX_CLOSE: ctx_open = 0; break;
    case A_TASKWIN: {
        struct win *t = win_by_id(hh.a);
        if (t) {
            desk_hidden = 0;
            if (t->minimized) { t->minimized = 0; raise_win(t); }
            else if (t == active_win()) t->minimized = 1;
            else raise_win(t);
        }
        break;
    }
    case A_CLOSE: { struct win *a = active_win(); if (a) close_win(a); else toast("No window to close"); break; }
    case A_DESKTOP: show_desktop(); break;
    case A_HIDE: { struct win *a = active_win(); if (a) { a->minimized = 1; desk_hidden = 0; } break; }
    case A_WIN_CLOSE: if (w) close_win(w); break;
    case A_WIN_MIN: if (w) { w->minimized = 1; desk_hidden = 0; } break;
    case A_WIN_MAX: if (w) toggle_maximize(w); break;
    case A_TITLE:
        if (w && !w->maximized)
            drg = (struct drag){ 1, 1, w->id, x - w->x, y - w->y, 0, x, y, w->x, w->y, w->w, w->h };
        break;
    case A_EDGE:
        if (w && !w->maximized)
            drg = (struct drag){ 1, 2, w->id, 0, 0, hh.b, x, y, w->x, w->y, w->w, w->h };
        break;
    case A_MENU:
        if (menu_win == hh.a && menu_idx == hh.b) menu_win = 0;
        else { menu_win = hh.a; menu_idx = hh.b; menu_sel = 0; start_open = 0; }
        break;
    case A_MENUITEM: {
        struct win *t = win_by_id(hh.a);
        int m = hh.b / 16, i = hh.b % 16;
        menu_win = 0;
        if (t) menu_action(t, m, i);
        break;
    }
    case A_MENUCLOSE: menu_win = 0; break;
    case A_SET_HOME: st.page = -1; st.scroll = 0; break;
    case A_SET_CAT: st.hsel = hh.a; open_cat(hh.a); break;
    case A_SET_ROW: st.sel = hh.a; row_activate(st.page, hh.a, 1); break;
    case A_REG_NODE:
        rg.focus = 0;
        if (rg.tsel == hh.a && hh.a < ntn && tn[hh.a].kids) exp_set(tn[hh.a].hive, tn[hh.a].path, !tn[hh.a].open);
        else { rg.tsel = hh.a; rg.vsel = 0; }
        break;
    case A_REG_VAL:
        if (rg.vsel == hh.a && rg.focus == 1) reg_edit_value();
        else { rg.vsel = hh.a; rg.focus = 1; }
        break;
    case A_DLG_OK: dialog_ok(); break;
    case A_DLG_CANCEL: dlg.type = D_NONE; break;
    }
    return 1;
}

static void next_window(void) {
    if (nz < 2) return;
    int id = zord[nz - 1];
    for (int i = nz - 1; i > 0; i--) zord[i] = zord[i - 1];
    zord[0] = id;
    struct win *w = win_by_id(zord[nz - 1]);
    if (w && w->minimized) w->minimized = 0;
}

static void start_key(int k) {
    if (k == 27) { start_open = 0; return; }
    if (k == 13) { if (nvis > 0) launch_tile(vis[ssel]); return; }
    if (k == GK_LEFT && ssel > 0) ssel--;
    else if (k == GK_RIGHT && ssel < nvis - 1) ssel++;
    else if (k == GK_UP && ssel - scols >= 0) ssel -= scols;
    else if (k == GK_DOWN && ssel + scols < nvis) ssel += scols;
    else if (k == GK_DOWN && ssel < nvis - 1) ssel = nvis - 1;
    else if ((k == 127 || k == 8) && sq[0]) { sq[strlen(sq) - 1] = 0; filter_tiles(); }
    else if (k >= 32 && k < 127 && strlen(sq) < sizeof sq - 1) {
        size_t l = strlen(sq); sq[l] = (char)k; sq[l + 1] = 0; filter_tiles(); ssel = 0;
    }
}

static void on_key(int k) {
    toast_msg[0] = 0;
    if (ctx_open && k == 27) { ctx_open = 0; return; }
    if (dlg.type) { dialog_key(k); return; }
    if (start_open) { start_key(k); return; }
    struct win *a = active_win();

    if (menu_win) {
        struct win *mw = win_by_id(menu_win);
        const char *const *items = mw ? menu_items(mw->kind, menu_idx) : NULL;
        int n = 0;
        while (items && items[n]) n++;
        if (k == 27) menu_win = 0;
        else if (k == GK_UP && menu_sel > 0) menu_sel--;
        else if (k == GK_DOWN && menu_sel < n - 1) menu_sel++;
        else if (k == GK_LEFT || k == GK_RIGHT) {
            int nm = 0;
            while (mw && menu_names(mw->kind)[nm]) nm++;
            menu_idx = (menu_idx + (k == GK_LEFT ? nm - 1 : 1)) % (nm ? nm : 1);
            menu_sel = 0;
        } else if (k == 13 && mw) {
            int m = menu_idx, i = menu_sel;
            menu_win = 0;
            menu_action(mw, m, i);
        }
        return;
    }

    if (mode_move || mode_resize) {
        if (k == 13 || k == 27) { mode_move = mode_resize = 0; return; }
        if (a && !a->maximized) {
            int step = 16;
            if (mode_move) {
                if (k == GK_LEFT) a->x -= step; else if (k == GK_RIGHT) a->x += step;
                else if (k == GK_UP) a->y -= step; else if (k == GK_DOWN) a->y += step;
            } else {
                if (k == GK_LEFT) a->w -= step; else if (k == GK_RIGHT) a->w += step;
                else if (k == GK_UP) a->h -= step; else if (k == GK_DOWN) a->h += step;
            }
            clamp_win(a);
        }
        return;
    }

    switch (k) {
    case CK('G'): case GK_F1: start_open = 1; build_tiles(); return;
    case CK('N'): case GK_F6: next_window(); return;
    case GK_F4:   if (a) close_win(a); return;
    case GK_F7:   show_desktop(); return;
    case CK('Q'): quit_code = GUI_TO_SHELL; return;
    case GK_F10:  if (a) { menu_win = a->id; menu_idx = 0; menu_sel = 0; } return;
    }

    if (!a || (a->kind != WK_TERMINAL && a->kind != WK_NOTES)) {
        switch (k) {
        case CK('E'): if (a) close_win(a); return;
        case CK('D'): show_desktop(); return;
        case CK('T'): if (a) { mode_move = 1; toast("Move mode: arrow keys move the window, Enter finishes"); } return;
        case CK('R'): if (a) { mode_resize = 1; toast("Resize mode: arrow keys resize the window, Enter finishes"); } return;
        }
    }

    if (!a) return;
    if (a->kind == WK_SETTINGS) settings_key(k, a->h - TITLE_H - MENU_H);
    else if (a->kind == WK_REGISTRY) registry_key(k);
    else if (a->kind == WK_TERMINAL) {
        if (term.pty_fd < 0 && k == 13)
            term_start((a->w - 20) / 8, (a->h - TITLE_H - MENU_H - 16) / 16, NULL);
        else
            term_send_key(k);
    } else if (a->kind >= WK_FILES) {
        apps_on_key(a->kind, k);
    }
}

static int poll_desktop_ipc(void) {
    int fd = open("/tmp/.mg-desktop.ipc", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    unlink("/tmp/.mg-desktop.ipc");
    if (n <= 0) return 0;
    buf[n] = 0;

    int changed = 0;
    char *sp = NULL;
    for (char *ln = strtok_r(buf, "\r\n", &sp); ln; ln = strtok_r(NULL, "\r\n", &sp)) {
        if (strncmp(ln, "OPEN ", 5) != 0) continue;
        char *app = ln + 5;
        while (*app == ' ') app++;
        char *arg = strchr(app, ' ');
        if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }
        int wk = apps_kind_from_name(app, NULL, NULL);
        if (wk > 0) {
            open_win_arg(wk, arg && arg[0] ? arg : NULL);
            desk_hidden = 0;
            changed = 1;
        }
    }
    return changed;
}

static void layout_desktop(void) {
    int H = gfx_height();
    int pad = cf.floating_bar ? 8 : 0;
    desk_y0 = cf.top ? (BAR_H + pad) : 0;
    desk_y1 = cf.top ? H : (H - BAR_H - pad);
    for (int i = 0; i < N(wins); i++) if (wins[i].used) clamp_win(&wins[i]);
}

int gui_run(void) {
    mg_conf_ensure_template();
    mg_conf_load_all();
    load_cfg();
    if (gfx_open(cf.reqW, cf.reqH) < 0) {
        fputs("desktop: no graphics display found (/dev/dri or /dev/fb0); boot without -nographic\n", stderr);
        return GUI_TO_SHELL;
    }

    unlink("/tmp/.mg-desktop.ipc");
    memset(wins, 0, sizeof wins);
    nz = 0; next_id = 1; start_open = 0; menu_win = 0; ctx_open = 0;
    dlg.type = D_NONE; desk_hidden = 0;
    toast_msg[0] = 0; mode_move = mode_resize = 0; quit_code = -1; drg.on = 0;
    gfx_get_cursor(&hover_x, &hover_y);

    rebuild_res_opts();
    layout_desktop();
    const struct gfx_info *gi = gfx_get_info();
    toast("Welcome. %s (%dx%d). Click 12 Mg or Programs.",
          gi->backend == GFX_GPU_DRIVER ? gi->driver : "Basic Display", gi->width, gi->height);

    /* Optional test hook to pre-open showcase windows when rendering screenshots */
    const char *test_open = getenv("MG_TEST_OPEN");
    if (test_open && test_open[0]) {
        char tb[160];
        snprintf(tb, sizeof tb, "%s", test_open);
        char *sp = NULL;
        for (char *t = strtok_r(tb, ",", &sp); t; t = strtok_r(NULL, ",", &sp)) {
            if (!strcmp(t, "start")) { start_open = 1; build_tiles(); }
            else if (!strcmp(t, "settings_dp")) { open_win(WK_SETTINGS); open_cat(1); }
            else if (!strcmp(t, "settings_mn")) { open_win(WK_SETTINGS); open_cat(2); }
            else if (!strcmp(t, "settings_nw")) { open_win(WK_SETTINGS); open_cat(3); }
            else if (!strcmp(t, "settings_dv")) { open_win(WK_SETTINGS); open_cat(4); }
            else if (!strcmp(t, "terminal")) open_win(WK_TERMINAL);
            else if (!strcmp(t, "registry")) open_win(WK_REGISTRY);
            else {
                int wk = apps_kind_from_name(t, NULL, NULL);
                if (wk > 0) open_win(wk);
            }
        }
    }

    int need_draw = 1, video_playing = 0;
    time_t last_sec = 0, last_hotplug = time(NULL);

    while (quit_code < 0) {
        time_t now = time(NULL);
        if (now != last_sec) {
            last_sec = now;
            need_draw = 1;
        }
        if (poll_desktop_ipc()) need_draw = 1;

        if (now - last_hotplug >= 2) {
            last_hotplug = now;
            if (gfx_check_hotplug(cf.reqW == 0)) {
                rebuild_res_opts();
                layout_desktop();
                need_draw = 1;
            }
        }
        if (term.pty_fd >= 0) {
            int r = term_pump();
            if (r < 0) {
                struct win *tw = win_by_kind(WK_TERMINAL);
                if (tw) close_win(tw);
                need_draw = 1;
            } else if (r > 0) need_draw = 1;
        }

        /* A playing video needs a redraw every frame (paced by gfx_present). */
        video_playing = 0;
        for (int i = 0; i < N(wins); i++)
            if (wins[i].used && !wins[i].minimized && media_needs_tick(wins[i].kind)) video_playing = 1;
        if (video_playing) need_draw = 1;

        if (need_draw) {
            render_desktop();
            gfx_present();
            need_draw = 0;
        } else {
            gfx_present();
        }

        if (getenv("MG_TEST_ONCE")) {
            quit_code = GUI_TO_SHELL;
            break;
        }

        int pty_ready = 0;
        struct gfx_mouse mev;
        int k = gfx_poll_event(video_playing ? 16 : 250, term.pty_fd, &pty_ready, &mev);
        if (pty_ready) {
            int r = term_pump();
            if (r < 0) {
                struct win *tw = win_by_kind(WK_TERMINAL);
                if (tw) close_win(tw);
            }
            need_draw = 1;
        }
        if (k == GK_TIMEOUT) continue;
        if (k == GK_EOF) { quit_code = GUI_TO_SHELL; break; }
        if (k == GK_MOUSE) {
            if (on_mouse(&mev)) need_draw = 1;
        } else {
            on_key(k);
            need_draw = 1;
        }
    }

    term_stop();
    gfx_close();
    return quit_code;
}

/* ================= VaporGlass First-Start & Login Screens ================= */

static void draw_auth_backdrop(const char *subtitle) {
    load_cfg();
    const struct pal *p = &cf.p;
    int W = gfx_width(), H = gfx_height();
    reset_ui_clip();
    draw_wallpaper();

    gfx_rrect(24, 22, 64, 28, 7, cf.acc);
    gfx_text(32, 28, "12 Mg", cf.accfg, 1, 1, 56);
    gfx_textf(98, 28, p->winfg, 1, 1, W - 260, "%s %s", OS_NAME, OS_VERSION_LONG);
    if (subtitle) gfx_text(98, 48, subtitle, p->muted, 0, 1, W - 260);

    char ck[64];
    time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm);
    strftime(ck, sizeof ck, "%a %d %b  %H:%M UTC", &tm);
    int cw = gfx_text_width(ck, 1);
    gfx_text(W - cw - 24, 28, ck, p->winfg, 1, 1, cw + 8);

    const struct gfx_info *gi = gfx_get_info();
    char gpubuf[160];
    snprintf(gpubuf, sizeof gpubuf, "%s  \xc2\xb7  %dx%d", gi->desc, gi->width, gi->height);
    int gw = gfx_text_width(gpubuf, 1) + 24;
    gfx_glass_panel(20, H - 44, gw, 26, 7, p->bar, 195, 4, p->border, 16);
    gfx_text(32, H - 39, gpubuf, gi->backend == GFX_GPU_DRIVER ? cf.acc : p->muted, 0, 1, gw - 16);
}

static void draw_text_field(int x, int y, int w, const char *label, const char *val, int cur, int masked, int focused) {
    const struct pal *p = &cf.p;
    gfx_text(x, y, label, p->muted, 1, 1, w);
    int fy = y + 20;
    gfx_rrect_alpha(x, fy, w, 34, 6, p->input_bg, 225);
    gfx_rrect_outline(x, fy, w, 34, 6, focused ? cf.acc : p->border);
    char disp[140];
    size_t l = strlen(val);
    if (l >= sizeof disp) l = sizeof disp - 1;
    if (masked) { memset(disp, '*', l); disp[l] = 0; }
    else { memcpy(disp, val, l); disp[l] = 0; }
    int maxc = (w - 20) / 8;
    int start = cur > maxc ? cur - maxc : 0;
    if ((int)strlen(disp) > start)
        gfx_text(x + 10, fy + 9, disp + start, p->winfg, 0, 1, w - 20);
    if (focused) {
        int cx = x + 10 + (cur - start) * 8;
        gfx_rect(cx, fy + 8, 2, 18, cf.acc);
    }
}

int gui_first_start(void) {
    if (gfx_open(0, 0) < 0) return -1;
    gfx_get_cursor(&hover_x, &hover_y);

    int step = 0;
    char ubuf[64] = "", pw1[128] = "", pw2[128] = "", err[140] = "";
    int ulen = 0, ucur = 0, l1 = 0, c1 = 0, l2 = 0, c2 = 0, focus = 0;
    int minlen = atoi(reg_get(HIVE_SYSTEM, "Security/MinPasswordLength", "8"));
    if (minlen < 4) minlen = 4;

    for (;;) {
        int W = gfx_width(), H = gfx_height();
        nhots = 0; cur_wid = 0;
        draw_auth_backdrop("First start setup");
        const struct pal *p = &cf.p;

        int cw = 440, ch = step == 0 ? 290 : 350;
        int cx = (W - cw) / 2, cy = (H - ch) / 2;
        gfx_shadow(cx, cy, cw, ch, 12);
        gfx_glass_panel(cx, cy, cw, ch, 12, p->win, 200, 6, cf.acc, 24);

        if (step == 0) {
            gfx_text(cx + 24, cy + 20, "Create Administrator Account", p->winfg, 1, 1, cw - 48);
            gfx_textf(cx + 24, cy + 42, p->muted, 0, 1, cw - 48,
                      "Set the 'root' password (%d+ characters).", minlen);
            draw_text_field(cx + 24, cy + 76, cw - 48, "New root password", pw1, c1, 1, focus == 0);
            hot_add(cx + 24, cy + 96, cw - 48, 34, 100, 0, 0);
            draw_text_field(cx + 24, cy + 144, cw - 48, "Confirm root password", pw2, c2, 1, focus == 1);
            hot_add(cx + 24, cy + 164, cw - 48, 34, 100, 1, 0);
            if (err[0]) gfx_text(cx + 24, cy + 212, err, p->bad, 0, 1, cw - 48);
            draw_button(cx + cw - 134, cy + ch - 46, 110, 30, "Continue", cf.accfg, cf.acc, 101, 0, 0);
        } else {
            gfx_text(cx + 24, cy + 20, "Create Regular User (Optional)", p->winfg, 1, 1, cw - 48);
            gfx_text(cx + 24, cy + 42, "Recommended for daily work, or click Skip.", p->muted, 0, 1, cw - 48);
            draw_text_field(cx + 24, cy + 72, cw - 48, "Username", ubuf, ucur, 0, focus == 0);
            hot_add(cx + 24, cy + 92, cw - 48, 34, 100, 0, 0);
            draw_text_field(cx + 24, cy + 138, cw - 48, "Password", pw1, c1, 1, focus == 1);
            hot_add(cx + 24, cy + 158, cw - 48, 34, 100, 1, 0);
            draw_text_field(cx + 24, cy + 204, cw - 48, "Confirm password", pw2, c2, 1, focus == 2);
            hot_add(cx + 24, cy + 224, cw - 48, 34, 100, 2, 0);
            if (err[0]) gfx_text(cx + 24, cy + 270, err, p->bad, 0, 1, cw - 48);
            draw_button(cx + cw - 232, cy + ch - 46, 90, 30, "Skip", p->winfg, p->titleoff, 102, 0, 0);
            draw_button(cx + cw - 134, cy + ch - 46, 110, 30, "Finish", cf.accfg, cf.acc, 101, 0, 0);
        }

        gfx_present();
        if (getenv("MG_TEST_ONCE")) break;

        struct gfx_mouse mev;
        int k = gfx_poll_event(500, -1, NULL, &mev);
        if (k == GK_TIMEOUT) continue;

        int submit = 0, skip = 0;
        if (k == GK_MOUSE) {
            hover_x = mev.x; hover_y = mev.y;
            if (mev.kind == GM_PRESS && mev.btn == 0) {
                const struct hot *h = hot_at(mev.x, mev.y);
                if (h) {
                    if (h->act == 100) focus = h->a;
                    else if (h->act == 101) submit = 1;
                    else if (h->act == 102) skip = 1;
                }
            }
            continue;
        }
        if (k == 9 || k == GK_DOWN) { focus = (focus + 1) % (step == 0 ? 2 : 3); continue; }
        if (k == GK_STAB || k == GK_UP) { focus = (focus + (step == 0 ? 1 : 2)) % (step == 0 ? 2 : 3); continue; }
        if (k == 13) {
            if (step == 0 && focus == 0) { focus = 1; continue; }
            if (step == 1 && focus == 0 && !ubuf[0]) skip = 1;
            else if (step == 1 && focus < 2) { focus++; continue; }
            else submit = 1;
        } else {
            err[0] = 0;
            if (step == 0) {
                if (focus == 0) edit_buf_key(pw1, sizeof pw1, &l1, &c1, k);
                else edit_buf_key(pw2, sizeof pw2, &l2, &c2, k);
            } else {
                if (focus == 0) edit_buf_key(ubuf, sizeof ubuf, &ulen, &ucur, k);
                else if (focus == 1) edit_buf_key(pw1, sizeof pw1, &l1, &c1, k);
                else edit_buf_key(pw2, sizeof pw2, &l2, &c2, k);
            }
        }

        if (skip && step == 1) { users_save(); break; }
        if (submit) {
            if (step == 0) {
                if (l1 < minlen) { snprintf(err, sizeof err, "Password must be at least %d characters.", minlen); continue; }
                if (strcmp(pw1, pw2) != 0) { snprintf(err, sizeof err, "Passwords do not match."); continue; }
                char home[96];
                snprintf(home, sizeof home, "%s/root", users_homebase());
                if (user_add("root", pw1, 0, 0, home) < 0) { snprintf(err, sizeof err, "Could not create root."); continue; }
                user_make_home(home, 0, 0);
                memset(pw1, 0, sizeof pw1); memset(pw2, 0, sizeof pw2);
                l1 = c1 = l2 = c2 = 0; err[0] = 0; step = 1; focus = 0;
            } else {
                if (!ubuf[0]) { users_save(); break; }
                if (!user_valid_name(ubuf) || !strcmp(ubuf, "root")) {
                    snprintf(err, sizeof err, "Use lowercase letters, digits, - or _.");
                    continue;
                }
                if (l1 < minlen) { snprintf(err, sizeof err, "Password must be at least %d characters.", minlen); continue; }
                if (strcmp(pw1, pw2) != 0) { snprintf(err, sizeof err, "Passwords do not match."); continue; }
                uid_t uid = users_next_uid();
                char home[96];
                snprintf(home, sizeof home, "%s/%s", users_homebase(), ubuf);
                if (user_add(ubuf, pw1, uid, uid, home) == 0) user_make_home(home, uid, uid);
                users_save();
                break;
            }
        }
    }

    memset(pw1, 0, sizeof pw1);
    memset(pw2, 0, sizeof pw2);
    gfx_close();
    return 0;
}

int gui_login(char *out_user, size_t n) {
    if (gfx_open(0, 0) < 0) return -1;
    gfx_get_cursor(&hover_x, &hover_y);

    users_load();
    char accts[8][40];
    int naccts = 0;
    for (uid_t uid = 1000; uid < 65000 && naccts < 7; uid++) {
        const struct user *u = user_by_uid(uid);
        if (!u) { if (uid > users_next_uid()) break; continue; }
        snprintf(accts[naccts++], sizeof accts[0], "%s", u->name);
    }
    if (user_by_uid(0) && naccts < 8) snprintf(accts[naccts++], sizeof accts[0], "root");

    char ubuf[64] = "", pw[128] = "", err[120] = "";
    if (naccts > 0) snprintf(ubuf, sizeof ubuf, "%s", accts[0]);
    int ulen = (int)strlen(ubuf), ucur = ulen, plen = 0, pcur = 0;
    int focus = ulen > 0 ? 1 : 0;
    static int failures;

    for (;;) {
        int W = gfx_width(), H = gfx_height();
        nhots = 0; cur_wid = 0;
        draw_auth_backdrop("Sign in to start your session");
        const struct pal *p = &cf.p;

        int cw = 420, ch = naccts > 0 ? 310 : 270;
        int cx = (W - cw) / 2, cy = (H - ch) / 2;
        gfx_shadow(cx, cy, cw, ch, 12);
        gfx_glass_panel(cx, cy, cw, ch, 12, p->win, 200, 6, cf.acc, 24);

        gfx_text(cx + 24, cy + 20, "Sign in", p->winfg, 1, 1, cw - 48);

        int fy = cy + 50;
        if (naccts > 0) {
            int ux = cx + 24;
            for (int i = 0; i < naccts; i++) {
                int bw = gfx_text_width(accts[i], 1) + 20;
                if (ux + bw > cx + cw - 24) break;
                int sel = !strcmp(ubuf, accts[i]);
                gfx_rrect(ux, fy, bw, 26, 13, sel ? cf.acc : p->titleoff);
                gfx_text(ux + 10, fy + 5, accts[i], sel ? cf.accfg : p->winfg, sel, 1, bw - 12);
                hot_add(ux, fy, bw, 26, 150, i, 0);
                ux += bw + 8;
            }
            fy += 40;
        }

        draw_text_field(cx + 24, fy, cw - 48, "Username", ubuf, ucur, 0, focus == 0);
        hot_add(cx + 24, fy + 20, cw - 48, 34, 151, 0, 0);

        draw_text_field(cx + 24, fy + 66, cw - 48, "Password", pw, pcur, 1, focus == 1);
        hot_add(cx + 24, fy + 86, cw - 48, 34, 151, 1, 0);

        if (err[0]) gfx_text(cx + 24, fy + 132, err, p->bad, 0, 1, cw - 48);
        draw_button(cx + cw - 134, cy + ch - 46, 110, 30, "Sign in", cf.accfg, cf.acc, 152, 0, 0);

        draw_button(W - 224, H - 46, 94, 28, "Restart", p->winfg, p->titleoff, 153, 0, 0);
        draw_button(W - 120, H - 46, 100, 28, "Shut down", 0xFFFFFF, p->bad, 154, 0, 0);

        gfx_present();
        if (getenv("MG_TEST_ONCE")) { memset(pw, 0, sizeof pw); gfx_close(); return -1; }

        struct gfx_mouse mev;
        int k = gfx_poll_event(500, -1, NULL, &mev);
        if (k == GK_TIMEOUT) continue;

        int try_login = 0;
        if (k == GK_MOUSE) {
            hover_x = mev.x; hover_y = mev.y;
            if (mev.kind == GM_PRESS && mev.btn == 0) {
                const struct hot *h = hot_at(mev.x, mev.y);
                if (h) {
                    if (h->act == 150) {
                        snprintf(ubuf, sizeof ubuf, "%s", accts[h->a]);
                        ulen = ucur = (int)strlen(ubuf);
                        focus = 1; err[0] = 0;
                    } else if (h->act == 151) focus = h->a;
                    else if (h->act == 152) try_login = 1;
                    else if (h->act == 153) { sync(); reboot(RB_AUTOBOOT); }
                    else if (h->act == 154) { sync(); reboot(RB_POWER_OFF); }
                }
            }
            continue;
        }

        if (k == 9 || k == GK_DOWN || k == GK_UP || k == GK_STAB) { focus = !focus; continue; }
        if (k == 13) {
            if (focus == 0 && ubuf[0]) { focus = 1; continue; }
            try_login = 1;
        } else {
            err[0] = 0;
            if (focus == 0) edit_buf_key(ubuf, sizeof ubuf, &ulen, &ucur, k);
            else edit_buf_key(pw, sizeof pw, &plen, &pcur, k);
        }

        if (try_login && ubuf[0]) {
            users_load();
            const struct user *u = user_find(ubuf);
            int ok = user_verify(u, pw);
            memset(pw, 0, sizeof pw);
            plen = pcur = 0;
            if (ok) {
                failures = 0;
                snprintf(out_user, n, "%s", u->name);
                gfx_close();
                return 0;
            }
            failures++;
            int base = atoi(reg_get(HIVE_SYSTEM, "Security/LoginDelay", "2"));
            if (base < 0) base = 0;
            if (base > 30) base = 30;
            snprintf(err, sizeof err, "Checking...");
            gfx_present();
            sleep((unsigned)(failures < 3 ? base : base * 2 + 1));
            snprintf(err, sizeof err, "Incorrect username or password.");
        }
    }
}
