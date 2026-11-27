/*
 * Magnesium OS - built-in VaporGlass desktop applications (backed by /programs):
 *   Files, Notes, Calculator, Canvas, System & Driver Monitor,
 *   Network Center & Web Reader, and Mg Mines.
 * Original code by alxa, MIT licensed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include "gui_apps.h"
#include "imgcodec.h"
#include "extapp.h"
#include "net.h"
#include "cmds.h"

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void app_btn(const struct ui_theme *t, int x, int y, int w, int h,
                    const char *label, int active_style, int act, int a, int b) {
    int hov = ui_hover_in(x, y, w, h);
    uint32_t bg = active_style ? t->acc : (hov ? t->sel : t->titleoff);
    uint32_t fg = active_style ? t->accfg : t->winfg;
    int r = t->radius > 6 ? 6 : t->radius;
    gfx_rrect(x, y, w, h, r, bg);
    gfx_rrect_outline(x, y, w, h, r, (hov || active_style) ? t->acc : t->border);
    int tw = gfx_text_width(label, 1);
    int tx = x + (w - tw) / 2;
    if (tx < x + 4) tx = x + 4;
    gfx_text(tx, y + (h - 16) / 2, label, fg, active_style, 1, w - 8);
    if (act) ui_hot_add(x, y, w, h, act, a, b);
}

int apps_kind_from_name(const char *name, const char **sym_out, const char **label_out) {
    if (!name || !name[0]) return 0;
    const char *b = strrchr(name, '/');
    if (b) name = b + 1;
    static const struct { const char *cmd; int kind; const char *sym, *label; } map[] = {
        { "files",     WK_FILES,     "Fl", "Files" },
        { "programs",  WK_FILES,     "Pr", "Programs" },
        { "notes",     WK_NOTES,     "Nt", "Notes" },
        { "calc",      WK_CALC,      "Ca", "Calculator" },
        { "canvas",    WK_CANVAS,    "Cv", "Canvas" },
        { "paint",     WK_CANVAS,    "Cv", "Canvas" },
        { "sysmon",    WK_SYSMON,    "Sm", "System Monitor" },
        { "netcenter", WK_NETCENTER, "Nc", "Network Center" },
        { "mines",     WK_MINES,     "Gm", "Mg Mines" },
        { "imgview",   WK_IMGVIEW,   "Pv", "Picture Viewer" },
        { "videoview", WK_VIDEO,     "Vp", "Video Player" },
        { "browser",   WK_BROWSER,   "Wb", "Web Browser" },
    };
    for (int i = 0; i < N(map); i++) {
        if (!strcasecmp(name, map[i].cmd)) {
            if (sym_out) *sym_out = map[i].sym;
            if (label_out) *label_out = map[i].label;
            return map[i].kind;
        }
    }
    return 0;
}

/* ============================================================================
 * 1. FILES (/programs/files) - Graphical File & /programs Manager
 * ============================================================================ */

struct fentry {
    char name[96];
    int is_dir;
    int is_exec;
    long long size;
};

static struct {
    char cwd[256];
    struct fentry items[192];
    int nitems, sel, scroll;
    int prompt_mode; /* 0=none, 1=new file, 2=new folder */
    char input[64];
    int ilen;
} fm;

static int cmp_fentry(const void *a, const void *b) {
    const struct fentry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void files_scan(void) {
    if (!fm.cwd[0]) snprintf(fm.cwd, sizeof fm.cwd, "/programs");
    DIR *dp = opendir(fm.cwd);
    if (!dp) {
        snprintf(fm.cwd, sizeof fm.cwd, "/");
        dp = opendir(fm.cwd);
        if (!dp) return;
    }
    fm.nitems = 0;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL && fm.nitems < N(fm.items)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[512];
        snprintf(full, sizeof full, "%s/%s", !strcmp(fm.cwd, "/") ? "" : fm.cwd, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        struct fentry *it = &fm.items[fm.nitems++];
        snprintf(it->name, sizeof it->name, "%.95s", e->d_name);
        it->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        it->is_exec = (!it->is_dir && access(full, X_OK) == 0) ? 1 : 0;
        it->size = (long long)st.st_size;
    }
    closedir(dp);
    qsort(fm.items, (size_t)fm.nitems, sizeof fm.items[0], cmp_fentry);
    if (fm.sel >= fm.nitems) fm.sel = fm.nitems - 1;
    if (fm.sel < 0) fm.sel = 0;
}

static void files_go(const char *path) {
    if (!path || !path[0]) return;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(fm.cwd, sizeof fm.cwd, "%.255s", path);
        fm.sel = 0; fm.scroll = 0;
        files_scan();
    } else {
        ui_toast("Folder not available: %s", path);
    }
}

static void files_up(void) {
    if (!strcmp(fm.cwd, "/")) return;
    char *s = strrchr(fm.cwd, '/');
    if (!s || s == fm.cwd) snprintf(fm.cwd, sizeof fm.cwd, "/");
    else *s = 0;
    fm.sel = 0; fm.scroll = 0;
    files_scan();
}

static void files_open_selected(void) {
    if (fm.sel < 0 || fm.sel >= fm.nitems) return;
    struct fentry *it = &fm.items[fm.sel];
    char full[512];
    snprintf(full, sizeof full, "%s/%s", !strcmp(fm.cwd, "/") ? "" : fm.cwd, it->name);
    if (it->is_dir) {
        files_go(full);
        return;
    }
    int wk = apps_kind_from_name(it->name, NULL, NULL);
    if (wk > 0) {
        ui_open_app(wk, NULL);
        return;
    }
    /* Pictures and MPEG videos open in their viewers */
    if (img_is_supported(it->name)) { ui_open_app(WK_IMGVIEW, full); return; }
    {
        const char *dot = strrchr(it->name, '.');
        if (dot && (!strcasecmp(dot, ".mpg") || !strcasecmp(dot, ".mpeg"))) { ui_open_app(WK_VIDEO, full); return; }
    }
    if (it->is_exec) {
        ui_run_terminal_cmd(full);
        return;
    }
    /* Open regular files in Notes */
    ui_open_app(WK_NOTES, full);
}

static void files_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    /* Toolbar */
    gfx_rect(x, y, cw, 36, t->win_alt);
    gfx_hline(x, y + 35, cw, t->border);
    app_btn(t, x + 8,   y + 5, 52, 26, "\xe2\x80\xb9 Up", 0, ACT_FILES_TOOL, 1, 0);
    app_btn(t, x + 66,  y + 5, 76, 26, "+ File", fm.prompt_mode == 1, ACT_FILES_TOOL, 2, 0);
    app_btn(t, x + 148, y + 5, 88, 26, "+ Folder", fm.prompt_mode == 2, ACT_FILES_TOOL, 3, 0);
    app_btn(t, x + 242, y + 5, 68, 26, "Delete", 0, ACT_FILES_TOOL, 4, 0);
    app_btn(t, x + 316, y + 5, 72, 26, "Reload", 0, ACT_FILES_TOOL, 5, 0);

    int px = x + 398;
    if (cw - 406 > 80) {
        gfx_rrect(px, y + 5, cw - 406, 26, 5, t->input_bg);
        gfx_rrect_outline(px, y + 5, cw - 406, 26, 5, t->border);
        gfx_text(px + 8, y + 10, fm.cwd, t->acc, 1, 1, cw - 422);
    }

    int body_y = y + 36, body_h = ch - 36;
    if (fm.prompt_mode) {
        gfx_rect(x, body_y, cw, 34, t->sel);
        gfx_hline(x, body_y + 33, cw, t->acc);
        const char *pr = fm.prompt_mode == 1 ? "New file name:" : "New folder name:";
        gfx_text(x + 12, body_y + 9, pr, t->winfg, 1, 1, 150);
        int ix = x + 148, iw = cw - 240;
        gfx_rrect(ix, body_y + 4, iw, 26, 4, t->input_bg);
        gfx_rrect_outline(ix, body_y + 4, iw, 26, 4, t->acc);
        gfx_text(ix + 8, body_y + 9, fm.input, t->winfg, 0, 1, iw - 16);
        gfx_rect(ix + 8 + fm.ilen * 8, body_y + 8, 2, 18, t->acc);
        app_btn(t, ix + iw + 8, body_y + 4, 72, 26, "Create", 1, ACT_FILES_TOOL, 6, 0);
        body_y += 34; body_h -= 34;
    }

    /* Left places sidebar */
    int sw = 152;
    gfx_rect(x, body_y, sw, body_h, t->win_alt);
    gfx_vline(x + sw, body_y, body_h, t->border);
    static const struct { const char *sym, *label, *path; } places[] = {
        { "Pr", "Programs",   "/programs" },
        { "Hm", "Home",       "~" },
        { "Dt", "Data Disk",  "/data" },
        { "Et", "Config/etc", "/etc" },
        { "Bn", "System Bin", "/bin" },
        { "Rt", "Root (/)",   "/" },
    };
    gfx_text(x + 12, body_y + 8, "Places", t->muted, 1, 1, sw - 20);
    for (int i = 0; i < N(places); i++) {
        int ry = body_y + 30 + i * 32;
        if (ry + 28 > body_y + body_h) break;
        const char *pdir = !strcmp(places[i].path, "~") ? (getenv("HOME") ? getenv("HOME") : "/root") : places[i].path;
        int cur = !strcmp(fm.cwd, pdir);
        int hov = ui_hover_in(x + 6, ry, sw - 12, 28);
        if (cur || hov) gfx_rrect(x + 6, ry, sw - 12, 28, 6, cur ? t->acc : t->sel);
        gfx_text(x + 12, ry + 6, places[i].sym, cur ? t->accfg : t->acc, 1, 1, 24);
        gfx_text(x + 38, ry + 6, places[i].label, cur ? t->accfg : t->winfg, cur, 1, sw - 48);
        ui_hot_add(x + 6, ry, sw - 12, 28, ACT_FILES_NAV, i, 0);
    }

    /* Main file list */
    int lx = x + sw + 1, lw = cw - sw - 1;
    ui_set_clip(lx, body_y, lw, body_h - 24);
    int row_h = 28;
    int vis = (body_h - 32) / row_h;
    if (vis < 1) vis = 1;
    if (fm.sel < fm.scroll) fm.scroll = fm.sel;
    if (fm.sel >= fm.scroll + vis) fm.scroll = fm.sel - vis + 1;
    if (fm.scroll < 0) fm.scroll = 0;

    if (fm.nitems == 0) {
        gfx_text(lx + 18, body_y + 18, "This folder is empty.", t->muted, 0, 1, lw - 36);
    }
    for (int i = 0; i < fm.nitems; i++) {
        int ry = body_y + 6 + (i - fm.scroll) * row_h;
        if (ry + row_h <= body_y || ry >= body_y + body_h - 24) continue;
        const struct fentry *it = &fm.items[i];
        int s = (i == fm.sel);
        int hov = ui_hover_in(lx + 6, ry, lw - 12, row_h - 2);
        if (s || hov) gfx_rrect(lx + 6, ry, lw - 12, row_h - 2, 5, s ? t->acc : t->sel);

        const char *tag = it->is_dir ? "DIR" : (it->is_exec ? "APP" : "TXT");
        uint32_t tag_bg = it->is_dir ? t->titleoff : (it->is_exec ? (s ? t->win : t->acc) : t->win_alt);
        uint32_t tag_fg = it->is_dir ? t->acc : (it->is_exec ? (s ? t->acc : t->accfg) : t->muted);
        gfx_rrect(lx + 12, ry + 3, 36, 20, 4, tag_bg);
        gfx_text(lx + 18, ry + 5, tag, tag_fg, 1, 1, 30);

        uint32_t nfg = s ? t->accfg : t->winfg;
        gfx_text(lx + 56, ry + 5, it->name, nfg, it->is_dir || it->is_exec, 1, lw - 150);

        char sz[32];
        if (it->is_dir) snprintf(sz, sizeof sz, "Folder");
        else if (it->size >= 1024 * 1024) snprintf(sz, sizeof sz, "%.1f MB", (double)it->size / (1024.0 * 1024.0));
        else if (it->size >= 1024) snprintf(sz, sizeof sz, "%.1f KB", (double)it->size / 1024.0);
        else snprintf(sz, sizeof sz, "%lld B", it->size);
        int sw_px = gfx_text_width(sz, 1);
        gfx_text(lx + lw - sw_px - 16, ry + 5, sz, s ? t->accfg : t->muted, 0, 1, 96);

        ui_hot_add(lx + 6, ry, lw - 12, row_h - 2, ACT_FILES_ITEM, i, 0);
    }

    ui_set_clip(x, y, cw, ch);
    gfx_rect(lx, y + ch - 24, lw, 24, t->titleoff);
    gfx_textf(lx + 10, y + ch - 20, t->muted, 0, 1, lw - 20,
              "%d item%s  \xc2\xb7  Click once to select, click again (or Enter) to open/run",
              fm.nitems, fm.nitems == 1 ? "" : "s");
}

/* ============================================================================
 * 2. NOTES (/programs/notes) - Smooth Text & Manual Config Editor
 * ============================================================================ */

#define NT_MAXL 256
#define NT_MAXC 160

static struct {
    char path[256];
    char lines[NT_MAXL][NT_MAXC];
    int nlines, row, col, scroll, dirty;
} nt;

static void notes_clear(void) {
    memset(&nt, 0, sizeof nt);
    nt.nlines = 1;
}

static void notes_load(const char *path) {
    notes_clear();
    if (!path || !path[0]) {
        char up[200];
        snprintf(nt.path, sizeof nt.path, "%s", mg_conf_user_path(up, sizeof up));
        snprintf(nt.lines[0], NT_MAXC, "# Welcome to Magnesium Notes!");
        snprintf(nt.lines[1], NT_MAXC, "# Edit files or manual OS settings (~/.mg.conf) and press Ctrl+S / Save.");
        nt.lines[2][0] = 0;
        snprintf(nt.lines[3], NT_MAXC, "# Example manual overrides (uncomment to apply):");
        snprintf(nt.lines[4], NT_MAXC, "# Display/Theme = Aurora");
        snprintf(nt.lines[5], NT_MAXC, "# Display/Glass = VaporGlass");
        snprintf(nt.lines[6], NT_MAXC, "# Manual/AccentHex = #38BDF8");
        nt.nlines = 7;
        if (access(nt.path, F_OK) != 0) return;
        path = nt.path;
    }
    snprintf(nt.path, sizeof nt.path, "%.255s", path);
    FILE *f = fopen(path, "r");
    if (!f) {
        nt.nlines = 1;
        nt.lines[0][0] = 0;
        return;
    }
    nt.nlines = 0;
    char buf[256];
    while (nt.nlines < NT_MAXL && fgets(buf, sizeof buf, f)) {
        char *nl = strpbrk(buf, "\r\n");
        if (nl) *nl = 0;
        snprintf(nt.lines[nt.nlines++], NT_MAXC, "%.*s", NT_MAXC - 1, buf);
    }
    fclose(f);
    if (nt.nlines == 0) nt.nlines = 1;
    nt.dirty = 0;
}

static void notes_save(void) {
    if (!nt.path[0]) {
        const char *h = getenv("HOME");
        snprintf(nt.path, sizeof nt.path, "%s/note.txt", h && h[0] ? h : "/tmp");
    }
    FILE *f = fopen(nt.path, "w");
    if (!f) {
        ui_toast("Cannot save %s: %s", nt.path, strerror(errno));
        return;
    }
    for (int i = 0; i < nt.nlines; i++) fprintf(f, "%s\n", nt.lines[i]);
    fclose(f);
    nt.dirty = 0;
    if (strstr(nt.path, "mg.conf")) {
        ui_reload_manual_cfg();
        ui_toast("Saved %s & applied manual changes!", nt.path);
    } else {
        ui_toast("Saved %s (%d lines)", nt.path, nt.nlines);
    }
}

static void notes_draw(const struct ui_theme *t, int x, int y, int cw, int ch, int active) {
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->input_bg);

    /* Toolbar */
    gfx_rect(x, y, cw, 36, t->win_alt);
    gfx_hline(x, y + 35, cw, t->border);
    app_btn(t, x + 8,   y + 5, 54, 26, "New", 0, ACT_NOTES_TOOL, 1, 0);
    app_btn(t, x + 68,  y + 5, 114, 26, "~/.mg.conf", 0, ACT_NOTES_TOOL, 2, 0);
    app_btn(t, x + 188, y + 5, 116, 26, "/etc/mg.conf", 0, ACT_NOTES_TOOL, 3, 0);
    app_btn(t, x + 310, y + 5, 92, 26, nt.dirty ? "Save *" : "Save", nt.dirty, ACT_NOTES_TOOL, 4, 0);
    gfx_textf(x + 412, y + 10, t->muted, 0, 1, cw - 420, "%s%s",
              nt.path[0] ? nt.path : "(untitled)", nt.dirty ? " [modified]" : "");

    int ty = y + 36, th = ch - 60;
    int vis = (th - 8) / 18;
    if (vis < 1) vis = 1;
    if (nt.row < nt.scroll) nt.scroll = nt.row;
    if (nt.row >= nt.scroll + vis) nt.scroll = nt.row - vis + 1;
    if (nt.scroll < 0) nt.scroll = 0;

    int gut_w = 44;
    gfx_rect(x, ty, gut_w, th, t->win);
    gfx_vline(x + gut_w, ty, th, t->border);

    ui_set_clip(x, ty, cw, th);
    ui_hot_add(x + gut_w + 1, ty, cw - gut_w - 1, th, ACT_NOTES_AREA, x + gut_w + 8, ty + 4);

    for (int i = 0; i < vis && nt.scroll + i < nt.nlines; i++) {
        int ln = nt.scroll + i;
        int ry = ty + 4 + i * 18;
        if (ln == nt.row) gfx_rect(x + gut_w + 1, ry - 1, cw - gut_w - 2, 18, t->sel);
        gfx_textf(x + 6, ry, ln == nt.row ? t->acc : t->muted, 0, 1, gut_w - 10, "%3d", ln + 1);
        gfx_text(x + gut_w + 8, ry, nt.lines[ln], t->winfg, 0, 1, cw - gut_w - 14);
        if (active && ln == nt.row) {
            int cx = x + gut_w + 8 + nt.col * 8;
            if (cx < x + cw - 4) gfx_rect(cx, ry, 2, 16, t->acc);
        }
    }

    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y + ch - 24, cw, 24, t->titleoff);
    gfx_textf(x + 10, y + ch - 20, t->muted, 0, 1, cw - 20,
              "Ln %d, Col %d  \xc2\xb7  %d lines  \xc2\xb7  Ctrl+S Save", nt.row + 1, nt.col + 1, nt.nlines);
}

static void notes_key(int k) {
    if (k == 19 /* Ctrl+S */) { notes_save(); return; }
    char *ln = nt.lines[nt.row];
    int len = (int)strlen(ln);
    if (nt.col > len) nt.col = len;

    if (k == GK_UP && nt.row > 0) { nt.row--; int nl = (int)strlen(nt.lines[nt.row]); if (nt.col > nl) nt.col = nl; }
    else if (k == GK_DOWN && nt.row < nt.nlines - 1) { nt.row++; int nl = (int)strlen(nt.lines[nt.row]); if (nt.col > nl) nt.col = nl; }
    else if (k == GK_LEFT) { if (nt.col > 0) nt.col--; else if (nt.row > 0) { nt.row--; nt.col = (int)strlen(nt.lines[nt.row]); } }
    else if (k == GK_RIGHT) { if (nt.col < len) nt.col++; else if (nt.row < nt.nlines - 1) { nt.row++; nt.col = 0; } }
    else if (k == GK_HOME) nt.col = 0;
    else if (k == GK_END) nt.col = len;
    else if (k == GK_PGUP) { nt.row -= 12; if (nt.row < 0) nt.row = 0; }
    else if (k == GK_PGDN) { nt.row += 12; if (nt.row >= nt.nlines) nt.row = nt.nlines - 1; }
    else if (k == 13 && nt.nlines < NT_MAXL) {
        char tail[NT_MAXC];
        snprintf(tail, sizeof tail, "%s", ln + nt.col);
        for (int i = nt.nlines; i > nt.row + 1; i--) memcpy(nt.lines[i], nt.lines[i - 1], NT_MAXC);
        memcpy(nt.lines[nt.row + 1], tail, NT_MAXC);
        ln[nt.col] = 0;
        nt.nlines++; nt.row++; nt.col = 0; nt.dirty = 1;
    } else if (k == 127 || k == 8) {
        if (nt.col > 0) {
            memmove(ln + nt.col - 1, ln + nt.col, (size_t)(len - nt.col + 1));
            nt.col--; nt.dirty = 1;
        } else if (nt.row > 0) {
            char cur_copy[NT_MAXC];
            memcpy(cur_copy, ln, NT_MAXC);
            int plen = (int)strlen(nt.lines[nt.row - 1]);
            int avail = NT_MAXC - 1 - plen;
            if (avail > 0) snprintf(nt.lines[nt.row - 1] + plen, (size_t)(avail + 1), "%.*s", avail, cur_copy);
            for (int i = nt.row; i < nt.nlines - 1; i++) memcpy(nt.lines[i], nt.lines[i + 1], NT_MAXC);
            nt.nlines--; nt.row--; nt.col = plen; nt.dirty = 1;
        }
    } else if (k == GK_DEL) {
        if (nt.col < len) { memmove(ln + nt.col, ln + nt.col + 1, (size_t)(len - nt.col)); nt.dirty = 1; }
    } else if (k == 9) {
        for (int s = 0; s < 4 && (int)strlen(ln) + 1 < NT_MAXC; s++) {
            int l = (int)strlen(ln);
            memmove(ln + nt.col + 1, ln + nt.col, (size_t)(l - nt.col + 1));
            ln[nt.col++] = ' ';
        }
        nt.dirty = 1;
    } else if (k >= 32 && k <= 126 && len + 1 < NT_MAXC) {
        memmove(ln + nt.col + 1, ln + nt.col, (size_t)(len - nt.col + 1));
        ln[nt.col++] = (char)k;
        nt.dirty = 1;
    }
}

/* ============================================================================
 * 3. CALCULATOR (/programs/calc) - VaporGlass Scientific/Desk Calculator
 * ============================================================================ */

static struct {
    char disp[40];
    char expr[64];
    double acc;
    char op;
    int fresh;
} cc;

static double approx_sqrt(double v) {
    if (v <= 0.0) return 0.0;
    double g = v > 1.0 ? v * 0.5 : 1.0;
    for (int i = 0; i < 24; i++) g = 0.5 * (g + v / g);
    return g;
}

static void calc_reset(void) {
    snprintf(cc.disp, sizeof cc.disp, "0");
    cc.expr[0] = 0;
    cc.acc = 0.0; cc.op = 0; cc.fresh = 1;
}

static void calc_apply_pending(double cur) {
    if (!cc.op) cc.acc = cur;
    else if (cc.op == '+') cc.acc += cur;
    else if (cc.op == '-') cc.acc -= cur;
    else if (cc.op == '*') cc.acc *= cur;
    else if (cc.op == '/') cc.acc = cur != 0.0 ? cc.acc / cur : 0.0;
}

static void calc_press(const char *b) {
    if (!strcmp(b, "C")) { calc_reset(); return; }
    double cur = atof(cc.disp);
    if (!strcmp(b, "\xc2\xb1")) {
        snprintf(cc.disp, sizeof cc.disp, "%.10g", -cur);
        return;
    }
    if (!strcmp(b, "%")) {
        snprintf(cc.disp, sizeof cc.disp, "%.10g", cur * 0.01);
        cc.fresh = 1;
        return;
    }
    if (!strcmp(b, "sqrt")) {
        snprintf(cc.expr, sizeof cc.expr, "sqrt(%.8g)", cur);
        snprintf(cc.disp, sizeof cc.disp, "%.10g", approx_sqrt(cur));
        cc.fresh = 1;
        return;
    }
    if ((b[0] >= '0' && b[0] <= '9') || b[0] == '.') {
        if (cc.fresh) {
            snprintf(cc.disp, sizeof cc.disp, "%s", b[0] == '.' ? "0." : b);
            cc.fresh = 0;
        } else if (strlen(cc.disp) < 18) {
            if (b[0] == '.' && strchr(cc.disp, '.')) return;
            strcat(cc.disp, b);
        }
        return;
    }
    if (!strcmp(b, "=")) {
        if (cc.op) {
            snprintf(cc.expr, sizeof cc.expr, "%.8g %c %.8g =", cc.acc, cc.op, cur);
            calc_apply_pending(cur);
            snprintf(cc.disp, sizeof cc.disp, "%.10g", cc.acc);
            cc.op = 0;
            cc.fresh = 1;
        }
        return;
    }
    char nop = b[0];
    if (!strcmp(b, "\xc3\xb7")) nop = '/';
    else if (!strcmp(b, "\xc3\x97")) nop = '*';
    else if (!strcmp(b, "\xe2\x88\x92") || !strcmp(b, "-")) nop = '-';
    calc_apply_pending(cur);
    cc.op = nop;
    snprintf(cc.expr, sizeof cc.expr, "%.10g %c", cc.acc, cc.op);
    snprintf(cc.disp, sizeof cc.disp, "%.10g", cc.acc);
    cc.fresh = 1;
}

static const char *const calc_btns[20] = {
    "C",  "\xc2\xb1", "%",    "\xc3\xb7",
    "7",  "8",        "9",    "\xc3\x97",
    "4",  "5",        "6",    "-",
    "1",  "2",        "3",    "+",
    "0",  ".",        "sqrt", "="
};

static void calc_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    int pad = 12;
    int dh = 76;
    gfx_rrect(x + pad, y + pad, cw - 2 * pad, dh, 8, t->input_bg);
    gfx_rrect_outline(x + pad, y + pad, cw - 2 * pad, dh, 8, t->border);
    if (cc.expr[0]) {
        int ew = gfx_text_width(cc.expr, 1);
        gfx_text(x + cw - pad - ew - 12, y + pad + 8, cc.expr, t->muted, 0, 1, cw - 40);
    }
    int dw = gfx_text_width(cc.disp, 2);
    int dx = x + cw - pad - dw - 12;
    if (dx < x + pad + 8) dx = x + pad + 8;
    gfx_text(dx, y + pad + 32, cc.disp, t->winfg, 1, 2, cw - 2 * pad - 16);

    int gy = y + pad + dh + 10;
    int gh = ch - (dh + 2 * pad + 10);
    int gap = 8;
    int bw = (cw - 2 * pad - 3 * gap) / 4;
    int bh = (gh - 4 * gap) / 5;
    if (bh < 28) bh = 28;

    for (int i = 0; i < 20; i++) {
        int r = i / 4, c = i % 4;
        int bx = x + pad + c * (bw + gap);
        int by = gy + r * (bh + gap);
        int is_op = (c == 3 || i == 19);
        app_btn(t, bx, by, bw, bh, calc_btns[i], is_op && i == 19, ACT_CALC_BTN, i, 0);
    }
}

/* ============================================================================
 * 4. CANVAS (/programs/canvas) - Pixel Paint & Drawing Studio
 * ============================================================================ */

#define CV_W 560
#define CV_H 340

static const uint32_t cv_pal[12] = {
    0xFFFFFF, 0x38BDF8, 0x22D3EE, 0x34D399, 0xFBBF24, 0xFB923C,
    0xF87171, 0xC084FC, 0xF472B6, 0x94A3B8, 0x1E293B, 0x090D16
};

static struct {
    uint32_t *pix;
    int tool; /* 0=Pen, 1=Line, 2=Rect, 3=Circle, 4=Fill, 5=Erase */
    int col_idx, bsize;
    int drawing, sx, sy, lx, ly;
    int ox, oy;
} cv;

static void cv_ensure(void) {
    if (!cv.pix) {
        cv.pix = malloc((size_t)(CV_W * CV_H) * sizeof(uint32_t));
        if (cv.pix) {
            for (int i = 0; i < CV_W * CV_H; i++) cv.pix[i] = 0x0F172Au;
        }
        cv.col_idx = 1;
        cv.bsize = 3;
    }
}

static void cv_put_dot(int x, int y, int r, uint32_t c) {
    if (!cv.pix) return;
    int hr = r / 2;
    for (int dy = -hr; dy <= hr; dy++) {
        int py = y + dy;
        if (py < 0 || py >= CV_H) continue;
        for (int dx = -hr; dx <= hr; dx++) {
            int px = x + dx;
            if (px >= 0 && px < CV_W && dx * dx + dy * dy <= hr * hr + 1)
                cv.pix[py * CV_W + px] = c;
        }
    }
}

static void cv_line(int x0, int y0, int x1, int y1, int r, uint32_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        cv_put_dot(x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void cv_flood(int sx, int sy, uint32_t repl) {
    if (!cv.pix || sx < 0 || sx >= CV_W || sy < 0 || sy >= CV_H) return;
    uint32_t target = cv.pix[sy * CV_W + sx];
    if (target == repl) return;
    int *q = malloc((size_t)(CV_W * CV_H) * sizeof(int));
    if (!q) return;
    int head = 0, tail = 0;
    q[tail++] = sy * CV_W + sx;
    cv.pix[sy * CV_W + sx] = repl;
    while (head < tail) {
        int p = q[head++];
        int x = p % CV_W, y = p / CV_W;
        const int dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; d++) {
            int nx = x + dx[d], ny = y + dy[d];
            if (nx >= 0 && nx < CV_W && ny >= 0 && ny < CV_H && cv.pix[ny * CV_W + nx] == target) {
                cv.pix[ny * CV_W + nx] = repl;
                q[tail++] = ny * CV_W + nx;
            }
        }
    }
    free(q);
}

static void cv_save_ppm(void) {
    cv_ensure();
    if (!cv.pix) return;
    const char *h = getenv("HOME");
    char path[256];
    snprintf(path, sizeof path, "%s/canvas.ppm", h && h[0] ? h : "/tmp");
    FILE *f = fopen(path, "wb");
    if (!f) { ui_toast("Save failed: %s", strerror(errno)); return; }
    fprintf(f, "P6\n%d %d\n255\n", CV_W, CV_H);
    for (int i = 0; i < CV_W * CV_H; i++) {
        uint32_t c = cv.pix[i];
        uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    ui_toast("Artwork saved to %s", path);
}

static void canvas_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    cv_ensure();
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    gfx_rect(x, y, cw, 36, t->win_alt);
    gfx_hline(x, y + 35, cw, t->border);
    static const char *const tnames[6] = { "Pen", "Line", "Rect", "Circle", "Fill", "Erase" };
    int bx = x + 8;
    for (int i = 0; i < 6; i++) {
        int w = gfx_text_width(tnames[i], 1) + 16;
        app_btn(t, bx, y + 5, w, 26, tnames[i], cv.tool == i, ACT_CANV_TOOL, i, 0);
        bx += w + 4;
    }
    static const int bsizes[3] = { 2, 5, 10 };
    static const char *const blab[3] = { "S", "M", "L" };
    bx += 6;
    for (int i = 0; i < 3; i++) {
        app_btn(t, bx, y + 5, 28, 26, blab[i], cv.bsize == bsizes[i], ACT_CANV_SIZE, bsizes[i], 0);
        bx += 32;
    }
    app_btn(t, bx + 4,  y + 5, 58, 26, "Clear", 0, ACT_CANV_TOOL, 10, 0);
    app_btn(t, bx + 66, y + 5, 76, 26, "Save PPM", 0, ACT_CANV_TOOL, 11, 0);

    /* Left color palette */
    int py0 = y + 36, ph = ch - 36;
    gfx_rect(x, py0, 44, ph, t->win_alt);
    gfx_vline(x + 44, py0, ph, t->border);
    for (int i = 0; i < 12; i++) {
        int sy = py0 + 8 + i * 26;
        if (sy + 22 > py0 + ph) break;
        gfx_rrect(x + 8, sy, 28, 22, 4, cv_pal[i]);
        gfx_rrect_outline(x + 8, sy, 28, 22, 4, cv.col_idx == i ? t->acc : t->border);
        if (cv.col_idx == i) gfx_box(x + 6, sy - 2, 32, 26, t->acc, 2);
        ui_hot_add(x + 6, sy, 32, 22, ACT_CANV_COL, i, 0);
    }

    int pad_x = x + 54, pad_y = py0 + 8;
    int dw = cw - 64 < CV_W ? cw - 64 : CV_W;
    int dh = ph - 16 < CV_H ? ph - 16 : CV_H;
    if (dw < 40 || dh < 40 || !cv.pix) return;
    cv.ox = pad_x; cv.oy = pad_y;

    ui_set_clip(pad_x, pad_y, dw, dh);
    for (int row = 0; row < dh; row++) {
        for (int col = 0; col < dw; col++) {
            gfx_put(pad_x + col, pad_y + row, cv.pix[row * CV_W + col]);
        }
    }
    if (cv.drawing && (cv.tool >= 1 && cv.tool <= 3)) {
        uint32_t c = cv_pal[cv.col_idx];
        if (cv.tool == 1) gfx_line(pad_x + cv.sx, pad_y + cv.sy, pad_x + cv.lx, pad_y + cv.ly, c, cv.bsize);
        else if (cv.tool == 2) {
            int rx = cv.sx < cv.lx ? cv.sx : cv.lx, ry = cv.sy < cv.ly ? cv.sy : cv.ly;
            int rw = abs(cv.lx - cv.sx) + 1, rh = abs(cv.ly - cv.sy) + 1;
            gfx_box(pad_x + rx, pad_y + ry, rw, rh, c, cv.bsize > 2 ? 2 : 1);
        } else if (cv.tool == 3) {
            int dx = cv.lx - cv.sx, dy = cv.ly - cv.sy;
            int rad = (int)approx_sqrt((double)(dx * dx + dy * dy));
            gfx_circle(pad_x + cv.sx, pad_y + cv.sy, rad, c, 0);
        }
    }
    ui_set_clip(x, y, cw, ch);
    gfx_box(pad_x - 1, pad_y - 1, dw + 2, dh + 2, t->border, 1);
    ui_hot_add(pad_x, pad_y, dw, dh, ACT_CANV_PAD, 0, 0);
}

/* ============================================================================
 * 5. SYSTEM & DRIVER MONITOR (/programs/sysmon)
 * ============================================================================ */

struct proc_row { int pid; char state; long rss_kb; char name[48]; };

static struct {
    int tab; /* 0 = Performance & Processes, 1 = Hardware Drivers */
    int mem_hist[60];
    int cpu_hist[60];
    unsigned long long prev_total, prev_idle;
    time_t last_sample;
    struct proc_row procs[96];
    int nprocs, psel, pscroll;
    struct hw_driver_item drvs[64];
    int ndrvs, dscroll;
} sm;

static void sysmon_sample(int force) {
    time_t now = time(NULL);
    if (!force && now == sm.last_sample && sm.nprocs > 0) return;
    sm.last_sample = now;

    /* CPU delta from /proc/stat */
    int cpu_pct = 4;
    FILE *sf = fopen("/proc/stat", "r");
    if (sf) {
        unsigned long long u = 0, n = 0, s = 0, id = 0, io = 0, ir = 0, si = 0;
        if (fscanf(sf, "cpu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &id, &io, &ir, &si) >= 4) {
            unsigned long long tot = u + n + s + id + io + ir + si;
            unsigned long long idle = id + io;
            if (sm.prev_total > 0 && tot > sm.prev_total) {
                unsigned long long dt = tot - sm.prev_total;
                unsigned long long di = idle - sm.prev_idle;
                cpu_pct = (int)(((dt - di) * 100ULL) / dt);
            }
            sm.prev_total = tot; sm.prev_idle = idle;
        }
        fclose(sf);
    }

    /* Memory % */
    int mem_pct = 15;
    struct sysinfo si;
    if (sysinfo(&si) == 0 && si.totalram > 0)
        mem_pct = (int)(((si.totalram - si.freeram) * 100ULL) / si.totalram);

    memmove(&sm.cpu_hist[0], &sm.cpu_hist[1], 59 * sizeof(int));
    memmove(&sm.mem_hist[0], &sm.mem_hist[1], 59 * sizeof(int));
    sm.cpu_hist[59] = cpu_pct < 1 ? 1 : (cpu_pct > 100 ? 100 : cpu_pct);
    sm.mem_hist[59] = mem_pct < 1 ? 1 : (mem_pct > 100 ? 100 : mem_pct);

    /* Processes */
    sm.nprocs = 0;
    DIR *dp = opendir("/proc");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) != NULL && sm.nprocs < N(sm.procs)) {
            if (!isdigit((unsigned char)e->d_name[0])) continue;
            char p[300], line[256];
            snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            if (fgets(line, sizeof line, f)) {
                char *lp = strchr(line, '('), *rp = strrchr(line, ')');
                if (lp && rp && rp > lp) {
                    struct proc_row *pr = &sm.procs[sm.nprocs++];
                    pr->pid = atoi(e->d_name);
                    size_t l = (size_t)(rp - lp - 1);
                    if (l >= sizeof pr->name) l = sizeof pr->name - 1;
                    memcpy(pr->name, lp + 1, l); pr->name[l] = 0;
                    pr->state = (rp[1] == ' ' && rp[2]) ? rp[2] : 'S';
                    long rss_pages = 0;
                    /* field 24 after pid+comm is rss (22nd token after ') ') */
                    char *tok = rp + 2, *sp = NULL;
                    int idx = 0;
                    for (char *t = strtok_r(tok, " ", &sp); t; t = strtok_r(NULL, " ", &sp)) {
                        if (++idx == 22) { rss_pages = atol(t); break; }
                    }
                    pr->rss_kb = rss_pages * 4;
                }
            }
            fclose(f);
        }
        closedir(dp);
    }
    if (sm.psel >= sm.nprocs) sm.psel = sm.nprocs - 1;
    if (sm.psel < 0) sm.psel = 0;

    if (force || sm.ndrvs == 0) sm.ndrvs = drv_scan(sm.drvs, N(sm.drvs));
}

static void sysmon_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    sysmon_sample(0);
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    gfx_rect(x, y, cw, 36, t->win_alt);
    gfx_hline(x, y + 35, cw, t->border);
    app_btn(t, x + 8,   y + 5, 168, 26, "Performance & Tasks", sm.tab == 0, ACT_SMON_TAB, 0, 0);
    app_btn(t, x + 182, y + 5, 148, 26, "Hardware Drivers",    sm.tab == 1, ACT_SMON_TAB, 1, 0);
    app_btn(t, x + cw - 86, y + 5, 78, 26, "Refresh", 0, ACT_SMON_BTN, 1, 0);
    if (sm.tab == 0)
        app_btn(t, x + cw - 180, y + 5, 88, 26, "End Task", 0, ACT_SMON_BTN, 2, 0);

    int by = y + 44;
    if (sm.tab == 0) {
        /* Sparkline charts for CPU & Memory */
        int gw = (cw - 36) / 2, gh = 86;
        for (int c = 0; c < 2; c++) {
            int gx = x + 12 + c * (gw + 12);
            gfx_rrect(gx, by, gw, gh, 6, t->input_bg);
            gfx_rrect_outline(gx, by, gw, gh, 6, t->border);
            const int *arr = c == 0 ? sm.cpu_hist : sm.mem_hist;
            uint32_t col = c == 0 ? t->acc : t->ok;
            gfx_textf(gx + 10, by + 6, col, 1, 1, gw - 20, "%s: %d%%",
                      c == 0 ? "CPU Load" : "RAM Usage", arr[59]);
            int px0 = gx + 8, pw = gw - 16, py_base = by + gh - 8, ph_max = gh - 34;
            for (int i = 1; i < 60; i++) {
                int x0 = px0 + ((i - 1) * pw) / 59;
                int x1 = px0 + (i * pw) / 59;
                int y0 = py_base - (arr[i - 1] * ph_max) / 100;
                int y1 = py_base - (arr[i] * ph_max) / 100;
                gfx_line(x0, y0, x1, y1, col, 2);
            }
        }

        int ty = by + gh + 10, th = ch - (gh + 58);
        gfx_text(x + 16, ty, "PID    STATE   MEM        PROCESS", t->muted, 1, 1, cw - 32);
        gfx_hline(x + 12, ty + 20, cw - 24, t->border);
        ui_set_clip(x + 12, ty + 22, cw - 24, th - 24);
        int row_h = 24, vis = (th - 26) / row_h;
        if (vis < 1) vis = 1;
        if (sm.psel < sm.pscroll) sm.pscroll = sm.psel;
        if (sm.psel >= sm.pscroll + vis) sm.pscroll = sm.psel - vis + 1;
        for (int i = 0; i < sm.nprocs; i++) {
            int ry = ty + 24 + (i - sm.pscroll) * row_h;
            if (ry + row_h <= ty + 22 || ry >= ty + th) continue;
            int s = i == sm.psel;
            if (s) gfx_rrect(x + 12, ry, cw - 24, row_h - 2, 4, t->acc);
            uint32_t fg = s ? t->accfg : t->winfg;
            gfx_textf(x + 16, ry + 3, fg, s, 1, cw - 32, "%-6d   %c     %6ld KB   %s",
                      sm.procs[i].pid, sm.procs[i].state, sm.procs[i].rss_kb, sm.procs[i].name);
            ui_hot_add(x + 12, ry, cw - 24, row_h - 2, ACT_SMON_ROW, i, 0);
        }
        return;
    }

    /* Hardware Drivers tab */
    gfx_text(x + 16, by, "CLASS     DRIVER              DEVICE / HARDWARE", t->muted, 1, 1, cw - 32);
    gfx_hline(x + 12, by + 20, cw - 24, t->border);
    ui_set_clip(x + 12, by + 22, cw - 24, ch - 72);
    int row_h = 28;
    for (int i = 0; i < sm.ndrvs; i++) {
        int ry = by + 26 + (i - sm.dscroll) * row_h;
        if (ry + row_h <= by + 22 || ry >= y + ch - 4) continue;
        gfx_rrect(x + 12, ry, cw - 24, row_h - 3, 5, t->win_alt);
        gfx_text(x + 18, ry + 5, sm.drvs[i].category, t->acc, 1, 1, 72);
        gfx_text(x + 98, ry + 5, sm.drvs[i].driver, sm.drvs[i].active ? t->ok : t->bad, 1, 1, 148);
        gfx_text(x + 258, ry + 5, sm.drvs[i].desc, t->winfg, 0, 1, cw - 276);
    }
}

/* ============================================================================
 * 6. NETWORK CENTER & WEB READER (/programs/netcenter)
 * ============================================================================ */

static struct {
    char url[192];
    int ulen, focus_url;
    char status[160];
    char page_lines[240][112];
    int npage, scroll;
    int inited;
} nc;

static void nc_set_text(const char *txt, int wrap_cols) {
    nc.npage = 0;
    nc.scroll = 0;
    if (wrap_cols < 32) wrap_cols = 72;
    if (wrap_cols > 108) wrap_cols = 108;
    const char *p = txt;
    while (*p && nc.npage < N(nc.page_lines)) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        if (seg == 0) {
            nc.page_lines[nc.npage++][0] = 0;
            if (!nl) break;
            p = nl + 1;
            continue;
        }
        while (seg > 0 && nc.npage < N(nc.page_lines)) {
            size_t take = seg <= (size_t)wrap_cols ? seg : (size_t)wrap_cols;
            if (take < seg) {
                size_t sp = take;
                while (sp > 20 && p[sp] != ' ') sp--;
                if (sp > 20) take = sp;
            }
            memcpy(nc.page_lines[nc.npage], p, take);
            nc.page_lines[nc.npage][take] = 0;
            nc.npage++;
            p += take;
            seg -= take;
            while (seg > 0 && *p == ' ') { p++; seg--; }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void nc_fetch_url(void) {
    if (!nc.url[0]) return;
    size_t cap = 65536;
    char *raw = malloc(cap), *clean = malloc(cap);
    if (!raw || !clean) { free(raw); free(clean); return; }
    int code = 0;
    char err[128] = "";
    int n = net_http_get(nc.url, raw, cap, &code, err, sizeof err);
    if (n < 0) {
        snprintf(nc.status, sizeof nc.status, "Error: %s", err);
        nc_set_text(nc.status, 80);
    } else {
        snprintf(nc.status, sizeof nc.status, "HTTP %d OK - %d bytes received from %.96s", code, n, nc.url);
        if (strcasestr(raw, "<html") || strcasestr(raw, "<body") || strcasestr(raw, "<p"))
            net_html_to_text(raw, clean, cap);
        else
            snprintf(clean, cap, "%s", raw);
        nc_set_text(clean, 82);
    }
    free(raw);
    free(clean);
}

static void netcenter_draw(const struct ui_theme *t, int x, int y, int cw, int ch, int active) {
    if (!nc.inited) {
        nc.inited = 1;
        snprintf(nc.url, sizeof nc.url, "http://example.com");
        nc.ulen = (int)strlen(nc.url);
        snprintf(nc.status, sizeof nc.status, "Ready - click DHCP Auto to acquire IP, or enter an http:// URL and click Fetch.");
        nc_set_text("Magnesium Network Center & HTTP Reader\n\n"
                    "- Click [DHCP Auto] to request an IPv4 lease on your network driver (virtio_net, e1000, r8169...).\n"
                    "- Click [Ping Gateway] or [Ping 1.1.1.1] to test connectivity and DNS.\n"
                    "- Type any http:// URL above and press Enter or [Fetch] to read web pages right here.", 80);
    }

    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    /* NIC & driver summary strip */
    struct net_iface ifs[8];
    int nif = net_list_ifaces(ifs, 8), pick = -1;
    for (int i = 0; i < nif; i++) if (!ifs[i].loopback) { pick = i; break; }

    gfx_rect(x, y, cw, 68, t->win_alt);
    gfx_hline(x, y + 67, cw, t->border);
    if (pick >= 0) {
        gfx_textf(x + 12, y + 8, t->winfg, 1, 1, cw - 300,
                  "Adapter: %s (%s)   IPv4: %s   GW: %s   DNS: %s",
                  ifs[pick].name, ifs[pick].driver, ifs[pick].ip,
                  ifs[pick].gateway[0] ? ifs[pick].gateway : "none", ifs[pick].dns);
    } else {
        gfx_text(x + 12, y + 8, "Adapter: loopback only (attach -nic user,model=virtio-net-pci for internet)",
                 t->muted, 0, 1, cw - 24);
    }
    app_btn(t, x + cw - 274, y + 4, 88, 24, "DHCP Auto", 0, ACT_NET_BTN, 1, 0);
    app_btn(t, x + cw - 180, y + 4, 84, 24, "Ping GW",   0, ACT_NET_BTN, 2, 0);
    app_btn(t, x + cw - 90,  y + 4, 82, 24, "Ping DNS",  0, ACT_NET_BTN, 3, 0);

    /* URL Bar */
    int ux = x + 12, uy = y + 34, uw = cw - 108;
    gfx_rrect(ux, uy, uw, 28, 5, t->input_bg);
    gfx_rrect_outline(ux, uy, uw, 28, 5, nc.focus_url ? t->acc : t->border);
    gfx_text(ux + 10, uy + 6, nc.url, t->winfg, 0, 1, uw - 20);
    if (active && nc.focus_url)
        gfx_rect(ux + 10 + nc.ulen * 8, uy + 5, 2, 18, t->acc);
    ui_hot_add(ux, uy, uw, 28, ACT_NET_URL, 0, 0);
    app_btn(t, ux + uw + 8, uy, 76, 28, "Fetch", 1, ACT_NET_BTN, 4, 0);

    /* Reader area */
    int ry0 = y + 72, rh = ch - 96;
    gfx_rect(x + 8, ry0, cw - 16, rh, t->input_bg);
    gfx_box(x + 8, ry0, cw - 16, rh, t->border, 1);
    ui_set_clip(x + 12, ry0 + 4, cw - 24, rh - 8);
    int vis = (rh - 8) / 18;
    if (nc.scroll > nc.npage - vis) nc.scroll = nc.npage - vis;
    if (nc.scroll < 0) nc.scroll = 0;
    for (int i = 0; i < vis && nc.scroll + i < nc.npage; i++) {
        gfx_text(x + 16, ry0 + 6 + i * 18, nc.page_lines[nc.scroll + i], t->winfg, 0, 1, cw - 32);
    }

    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y + ch - 24, cw, 24, t->titleoff);
    gfx_text(x + 10, y + ch - 20, nc.status, t->acc, 0, 1, cw - 20);
}

/* ============================================================================
 * 7. MG MINES (/programs/mines) - Magnesium VaporGlass Minesweeper
 * ============================================================================ */

#define MN_W 12
#define MN_H 9
#define MN_MINES 15

static struct {
    uint8_t mine[MN_H][MN_W];
    uint8_t state[MN_H][MN_W]; /* 0=hidden, 1=open, 2=flag */
    int over, won, flag_mode, flags_used, first_click;
} mn;

static void mines_new(void) {
    memset(&mn, 0, sizeof mn);
    mn.first_click = 1;
}

static int mines_adj(int r, int c) {
    int n = 0;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++) {
            int nr = r + dr, nc2 = c + dc;
            if (nr >= 0 && nr < MN_H && nc2 >= 0 && nc2 < MN_W && mn.mine[nr][nc2]) n++;
        }
    return n;
}

static void mines_place(int safe_r, int safe_c) {
    srand((unsigned)(time(NULL) ^ (getpid() << 8)));
    int placed = 0;
    while (placed < MN_MINES) {
        int r = rand() % MN_H, c = rand() % MN_W;
        if (abs(r - safe_r) <= 1 && abs(c - safe_c) <= 1) continue;
        if (!mn.mine[r][c]) { mn.mine[r][c] = 1; placed++; }
    }
}

static void mines_reveal(int r, int c) {
    if (r < 0 || r >= MN_H || c < 0 || c >= MN_W || mn.state[r][c] != 0) return;
    if (mn.first_click) { mines_place(r, c); mn.first_click = 0; }
    mn.state[r][c] = 1;
    if (mn.mine[r][c]) {
        mn.over = 1;
        for (int rr = 0; rr < MN_H; rr++)
            for (int cc2 = 0; cc2 < MN_W; cc2++)
                if (mn.mine[rr][cc2]) mn.state[rr][cc2] = 1;
        return;
    }
    if (mines_adj(r, c) == 0) {
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                if (dr || dc) mines_reveal(r + dr, c + dc);
    }
    int hidden_safe = 0;
    for (int rr = 0; rr < MN_H; rr++)
        for (int cc2 = 0; cc2 < MN_W; cc2++)
            if (!mn.mine[rr][cc2] && mn.state[rr][cc2] != 1) hidden_safe++;
    if (hidden_safe == 0) { mn.over = 1; mn.won = 1; }
}

static void mines_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    gfx_rect(x, y, cw, 38, t->win_alt);
    gfx_hline(x, y + 37, cw, t->border);
    app_btn(t, x + 10, y + 6, 92, 26, "New Game", 0, ACT_MINE_BTN, 1, 0);
    app_btn(t, x + 110, y + 6, 128, 26, mn.flag_mode ? "Flag Mode: ON" : "Flag Mode: OFF",
            mn.flag_mode, ACT_MINE_BTN, 2, 0);
    gfx_textf(x + 250, y + 11, mn.won ? t->ok : (mn.over ? t->bad : t->acc), 1, 1, cw - 260,
              "%s", mn.won ? "Cleared! 12 Mg Victory!" :
                    (mn.over ? "Boom! Hit New Game" : "Mines: 15  (Right-click or Flag Mode)"));

    int avail_w = cw - 24, avail_h = ch - 54;
    int cs = avail_w / MN_W < avail_h / MN_H ? avail_w / MN_W : avail_h / MN_H;
    if (cs < 20) cs = 20;
    int bx0 = x + (cw - cs * MN_W) / 2;
    int by0 = y + 44 + (avail_h - cs * MN_H) / 2;

    static const uint32_t ncols[9] = {
        0, 0x38BDF8, 0x34D399, 0xF87171, 0xC084FC, 0xFB923C, 0x22D3EE, 0xFBBF24, 0xE2E8F0
    };

    for (int r = 0; r < MN_H; r++) {
        for (int c = 0; c < MN_W; c++) {
            int cx = bx0 + c * cs, cy = by0 + r * cs;
            uint8_t st = mn.state[r][c];
            uint32_t bg = st == 1 ? (mn.mine[r][c] ? t->bad : t->input_bg) :
                          (ui_hover_in(cx, cy, cs - 2, cs - 2) ? t->sel : t->titleoff);
            gfx_rrect(cx, cy, cs - 3, cs - 3, 5, bg);
            gfx_rrect_outline(cx, cy, cs - 3, cs - 3, 5, st == 2 ? t->acc : t->border);
            if (st == 2) {
                gfx_text(cx + (cs - 11) / 2, cy + (cs - 18) / 2, "F", t->acc, 1, 1, cs);
            } else if (st == 1 && mn.mine[r][c]) {
                gfx_text(cx + (cs - 11) / 2, cy + (cs - 18) / 2, "*", 0xFFFFFF, 1, 1, cs);
            } else if (st == 1) {
                int adj = mines_adj(r, c);
                if (adj > 0) {
                    char s[2] = { (char)('0' + adj), 0 };
                    gfx_text(cx + (cs - 11) / 2, cy + (cs - 18) / 2, s, ncols[adj], 1, 1, cs);
                }
            }
            ui_hot_add(cx, cy, cs - 3, cs - 3, ACT_MINE_CELL, r, c);
        }
    }
}

/* ============================================================================
 * Dispatcher entry points called by gui.c
 * ============================================================================ */

void apps_init_window(int kind, const char *arg, char *title_out, int *def_w, int *def_h) {
    switch (kind) {
    case WK_FILES:
        snprintf(title_out, 40, "Files");
        *def_w = 680; *def_h = 440;
        snprintf(fm.cwd, sizeof fm.cwd, "%s", (arg && arg[0]) ? arg : "/programs");
        fm.prompt_mode = 0; fm.sel = 0; fm.scroll = 0;
        files_scan();
        break;
    case WK_NOTES:
        snprintf(title_out, 40, "Notes");
        *def_w = 660; *def_h = 440;
        if (arg && arg[0]) notes_load(arg);
        else if (nt.nlines == 0) notes_load(NULL);
        break;
    case WK_CALC:
        snprintf(title_out, 40, "Calculator");
        *def_w = 320; *def_h = 400;
        if (!cc.disp[0]) calc_reset();
        break;
    case WK_CANVAS:
        snprintf(title_out, 40, "Canvas");
        *def_w = 650; *def_h = 420;
        cv_ensure();
        break;
    case WK_SYSMON:
        snprintf(title_out, 40, "System Monitor");
        *def_w = 660; *def_h = 450;
        sysmon_sample(1);
        break;
    case WK_NETCENTER:
        snprintf(title_out, 40, "Network Center");
        *def_w = 680; *def_h = 450;
        if (arg && arg[0]) {
            nc.inited = 1;
            snprintf(nc.url, sizeof nc.url, "%.191s", arg);
            nc.ulen = (int)strlen(nc.url);
            nc_fetch_url();
        }
        break;
    case WK_IMGVIEW:
    case WK_VIDEO:
    case WK_BROWSER:
        media_init_window(kind, arg, title_out, def_w, def_h);
        break;
    case WK_MINES:
        snprintf(title_out, 40, "Mg Mines");
        *def_w = 460; *def_h = 390;
        if (!mn.first_click && !mn.over) { /* keep running game */ }
        else mines_new();
        break;
    }
}

void apps_draw_window(int kind, int x, int y, int cw, int ch, int active) {
    struct ui_theme t;
    ui_get_theme(&t);
    if (kind >= WK_EXT0 && kind < WK_EXT0 + EXT_MAX) {
        ui_set_clip(x, y, cw, ch);
        gfx_rect(x, y, cw, ch, t.win);
        ext_draw(kind - WK_EXT0, x, y, cw, ch);
        return;
    }
    switch (kind) {
    case WK_IMGVIEW:
    case WK_VIDEO:
    case WK_BROWSER:
        media_draw(kind, &t, x, y, cw, ch); break;
    case WK_FILES:     files_draw(&t, x, y, cw, ch); break;
    case WK_NOTES:     notes_draw(&t, x, y, cw, ch, active); break;
    case WK_CALC:      calc_draw(&t, x, y, cw, ch); break;
    case WK_CANVAS:    canvas_draw(&t, x, y, cw, ch); break;
    case WK_SYSMON:    sysmon_draw(&t, x, y, cw, ch); break;
    case WK_NETCENTER: netcenter_draw(&t, x, y, cw, ch, active); break;
    case WK_MINES:     mines_draw(&t, x, y, cw, ch); break;
    }
}

void apps_on_hotspot(int act, int a, int b, int mx, int my, int mouse_btn) {
    if (act >= ACT_IMG_BTN && act <= ACT_WEB_ADDR) { media_hotspot(act, a, b); return; }
    switch (act) {
    case ACT_FILES_NAV: {
        static const char *const paths[6] = { "/programs", "~", "/data", "/etc", "/bin", "/" };
        if (a >= 0 && a < 6) {
            const char *p = !strcmp(paths[a], "~") ? (getenv("HOME") ? getenv("HOME") : "/root") : paths[a];
            files_go(p);
        }
        break;
    }
    case ACT_FILES_TOOL:
        if (a == 1) files_up();
        else if (a == 2) { fm.prompt_mode = fm.prompt_mode == 1 ? 0 : 1; fm.input[0] = 0; fm.ilen = 0; }
        else if (a == 3) { fm.prompt_mode = fm.prompt_mode == 2 ? 0 : 2; fm.input[0] = 0; fm.ilen = 0; }
        else if (a == 4 && fm.sel >= 0 && fm.sel < fm.nitems) {
            char full[512];
            snprintf(full, sizeof full, "%s/%s", !strcmp(fm.cwd, "/") ? "" : fm.cwd, fm.items[fm.sel].name);
            if (remove_tree(full) < 0) ui_toast("Delete failed: %s", strerror(errno));
            else files_scan();
        } else if (a == 5) files_scan();
        else if (a == 6 && fm.input[0]) {
            char full[512];
            snprintf(full, sizeof full, "%s/%s", !strcmp(fm.cwd, "/") ? "" : fm.cwd, fm.input);
            if (fm.prompt_mode == 1) {
                int fd = open(full, O_CREAT | O_WRONLY, 0644);
                if (fd >= 0) close(fd); else ui_toast("Create failed: %s", strerror(errno));
            } else {
                if (mkdir(full, 0755) < 0) ui_toast("mkdir failed: %s", strerror(errno));
            }
            fm.prompt_mode = 0;
            files_scan();
        }
        break;
    case ACT_FILES_ITEM:
        if (fm.sel == a) files_open_selected();
        else fm.sel = a;
        break;
    case ACT_NOTES_TOOL:
        if (a == 1) notes_clear();
        else if (a == 2) { char p[200]; notes_load(mg_conf_user_path(p, sizeof p)); }
        else if (a == 3) { char p[200]; mg_conf_ensure_template(); notes_load(mg_conf_sys_path(p, sizeof p)); }
        else if (a == 4) notes_save();
        break;
    case ACT_NOTES_AREA: {
        int ln = nt.scroll + (my - b) / 18;
        if (ln < 0) ln = 0;
        if (ln >= nt.nlines) ln = nt.nlines - 1;
        nt.row = ln;
        int col = (mx - a + 4) / 8;
        int l = (int)strlen(nt.lines[ln]);
        nt.col = col < 0 ? 0 : (col > l ? l : col);
        break;
    }
    case ACT_CALC_BTN:
        if (a >= 0 && a < 20) calc_press(calc_btns[a]);
        break;
    case ACT_CANV_TOOL:
        if (a < 6) cv.tool = a;
        else if (a == 10 && cv.pix) { for (int i = 0; i < CV_W * CV_H; i++) cv.pix[i] = 0x0F172Au; }
        else if (a == 11) cv_save_ppm();
        break;
    case ACT_CANV_COL:
        if (a >= 0 && a < 12) cv.col_idx = a;
        break;
    case ACT_CANV_SIZE:
        cv.bsize = a;
        break;
    case ACT_CANV_PAD: {
        int lx = mx - cv.ox, ly = my - cv.oy;
        uint32_t col = cv.tool == 5 ? 0x0F172Au : cv_pal[cv.col_idx];
        if (cv.tool == 4) { cv_flood(lx, ly, col); break; }
        cv.drawing = 1; cv.sx = cv.lx = lx; cv.sy = cv.ly = ly;
        if (cv.tool == 0 || cv.tool == 5) cv_put_dot(lx, ly, cv.tool == 5 ? cv.bsize * 2 : cv.bsize, col);
        break;
    }
    case ACT_SMON_TAB: sm.tab = a; sysmon_sample(1); break;
    case ACT_SMON_ROW: sm.psel = a; break;
    case ACT_SMON_BTN:
        if (a == 1) sysmon_sample(1);
        else if (a == 2 && sm.psel >= 0 && sm.psel < sm.nprocs) {
            int pid = sm.procs[sm.psel].pid;
            if (pid <= 1) ui_toast("Cannot terminate PID 1 (init)");
            else if (kill(pid, SIGTERM) < 0) ui_toast("kill %d: %s", pid, strerror(errno));
            else { ui_toast("Sent SIGTERM to PID %d", pid); sysmon_sample(1); }
        }
        break;
    case ACT_NET_URL: nc.focus_url = 1; break;
    case ACT_NET_BTN:
        nc.focus_url = 0;
        if (a == 1) {
            if (net_dhcp(NULL, nc.status, sizeof nc.status) == 0) ui_toast("DHCP lease acquired!");
            else ui_toast("%s", nc.status);
        } else if (a == 2 || a == 3) {
            const char *target = a == 2 ? "10.0.2.2" : "1.1.1.1";
            int ms = 0, tcp_fb = 0; char ip[24] = "", err[96] = "";
            if (net_ping_once(target, 1, ip, sizeof ip, &ms, &tcp_fb, err, sizeof err) == 0)
                snprintf(nc.status, sizeof nc.status, "Reply from %s: %d ms%s", ip, ms, tcp_fb ? " (TCP-80)" : " (ICMP)");
            else
                snprintf(nc.status, sizeof nc.status, "Ping %s: %s", target, err);
        } else if (a == 4) {
            nc_fetch_url();
        }
        break;
    case ACT_MINE_BTN:
        if (a == 1) mines_new();
        else if (a == 2) mn.flag_mode = !mn.flag_mode;
        break;
    case ACT_MINE_CELL:
        if (mn.over) break;
        if (mouse_btn == 2 || mn.flag_mode) {
            if (mn.state[a][b] == 0) mn.state[a][b] = 2;
            else if (mn.state[a][b] == 2) mn.state[a][b] = 0;
        } else {
            mines_reveal(a, b);
        }
        break;
    }
}

int apps_on_mouse_drag(int kind, int x, int y, int cw, int ch, const struct gfx_mouse *mev) {
    (void)x; (void)y; (void)cw; (void)ch;
    if (kind != WK_CANVAS || !cv.drawing) return 0;
    int lx = mev->x - cv.ox, ly = mev->y - cv.oy;
    if (lx < 0) lx = 0; else if (lx >= CV_W) lx = CV_W - 1;
    if (ly < 0) ly = 0; else if (ly >= CV_H) ly = CV_H - 1;
    uint32_t col = cv.tool == 5 ? 0x0F172Au : cv_pal[cv.col_idx];

    if (mev->kind == GM_MOVE && (mev->buttons_down & 1)) {
        if (cv.tool == 0 || cv.tool == 5)
            cv_line(cv.lx, cv.ly, lx, ly, cv.tool == 5 ? cv.bsize * 2 : cv.bsize, col);
        cv.lx = lx; cv.ly = ly;
        return 1;
    }
    if (mev->kind == GM_RELEASE) {
        if (cv.tool == 1) cv_line(cv.sx, cv.sy, lx, ly, cv.bsize, col);
        else if (cv.tool == 2) {
            int x0 = cv.sx < lx ? cv.sx : lx, x1 = cv.sx > lx ? cv.sx : lx;
            int y0 = cv.sy < ly ? cv.sy : ly, y1 = cv.sy > ly ? cv.sy : ly;
            cv_line(x0, y0, x1, y0, cv.bsize, col); cv_line(x0, y1, x1, y1, cv.bsize, col);
            cv_line(x0, y0, x0, y1, cv.bsize, col); cv_line(x1, y0, x1, y1, cv.bsize, col);
        } else if (cv.tool == 3) {
            int dx = lx - cv.sx, dy = ly - cv.sy;
            int rad = (int)approx_sqrt((double)(dx * dx + dy * dy));
            for (int deg = 0; deg < 360; deg++) {
                /* Midpoint circle into cv.pix */
                (void)deg;
            }
            int cx = cv.sx, cy = cv.sy, px = rad, py = 0, err = 1 - rad;
            while (px >= py) {
                cv_put_dot(cx + px, cy + py, cv.bsize, col); cv_put_dot(cx + py, cy + px, cv.bsize, col);
                cv_put_dot(cx - py, cy + px, cv.bsize, col); cv_put_dot(cx - px, cy + py, cv.bsize, col);
                cv_put_dot(cx - px, cy - py, cv.bsize, col); cv_put_dot(cx - py, cy - px, cv.bsize, col);
                cv_put_dot(cx + py, cy - px, cv.bsize, col); cv_put_dot(cx + px, cy - py, cv.bsize, col);
                py++;
                if (err < 0) err += 2 * py + 1;
                else { px--; err += 2 * (py - px) + 1; }
            }
        }
        cv.drawing = 0;
        return 1;
    }
    return 0;
}

void apps_on_wheel(int kind, int delta) {
    if (kind == WK_IMGVIEW || kind == WK_BROWSER) { media_wheel(kind, delta); return; }
    if (kind == WK_FILES) { fm.scroll += delta * 2; if (fm.scroll < 0) fm.scroll = 0; }
    else if (kind == WK_NOTES) { nt.scroll += delta * 3; if (nt.scroll < 0) nt.scroll = 0; }
    else if (kind == WK_SYSMON) {
        if (sm.tab == 0) { sm.pscroll += delta * 2; if (sm.pscroll < 0) sm.pscroll = 0; }
        else { sm.dscroll += delta * 2; if (sm.dscroll < 0) sm.dscroll = 0; }
    } else if (kind == WK_NETCENTER) { nc.scroll += delta * 3; if (nc.scroll < 0) nc.scroll = 0; }
}

void apps_on_key(int kind, int k) {
    if (kind >= WK_EXT0 && kind < WK_EXT0 + EXT_MAX) { ext_key(kind - WK_EXT0, k); return; }
    if (kind == WK_IMGVIEW || kind == WK_VIDEO || kind == WK_BROWSER) { media_key(kind, k); return; }
    if (kind == WK_FILES) {
        if (fm.prompt_mode) {
            if (k == 27) { fm.prompt_mode = 0; return; }
            if (k == 13) { apps_on_hotspot(ACT_FILES_TOOL, 6, 0, 0, 0, 0); return; }
            if ((k == 127 || k == 8) && fm.ilen > 0) fm.input[--fm.ilen] = 0;
            else if (k >= 32 && k < 127 && fm.ilen + 1 < (int)sizeof fm.input && k != '/') {
                fm.input[fm.ilen++] = (char)k; fm.input[fm.ilen] = 0;
            }
            return;
        }
        if (k == GK_UP && fm.sel > 0) fm.sel--;
        else if (k == GK_DOWN && fm.sel < fm.nitems - 1) fm.sel++;
        else if (k == 13) files_open_selected();
        else if (k == 127 || k == 8) files_up();
    } else if (kind == WK_NOTES) {
        notes_key(k);
    } else if (kind == WK_CALC) {
        char s[2] = { (char)k, 0 };
        if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/' || k == '%')
            calc_press(s);
        else if (k == 13 || k == '=') calc_press("=");
        else if (k == 27 || k == 'c' || k == 'C') calc_reset();
        else if ((k == 127 || k == 8) && !cc.fresh) {
            size_t l = strlen(cc.disp);
            if (l > 1) cc.disp[l - 1] = 0; else snprintf(cc.disp, sizeof cc.disp, "0");
        }
    } else if (kind == WK_SYSMON) {
        if (k == 9) sm.tab = !sm.tab;
        else if (k == GK_UP && sm.psel > 0) sm.psel--;
        else if (k == GK_DOWN && sm.psel < sm.nprocs - 1) sm.psel++;
    } else if (kind == WK_NETCENTER) {
        if (k == 13) { nc_fetch_url(); return; }
        if ((k == 127 || k == 8) && nc.ulen > 0) nc.url[--nc.ulen] = 0;
        else if (k >= 32 && k < 127 && nc.ulen + 1 < (int)sizeof nc.url) {
            nc.url[nc.ulen++] = (char)k; nc.url[nc.ulen] = 0;
            nc.focus_url = 1;
        }
    }
}

int apps_needs_tick(int kind) {
    return kind == WK_SYSMON || media_needs_tick(kind);
}
