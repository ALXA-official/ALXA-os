/*
 * Magnesium OS - media and web windows:
 *   Picture Viewer  - any PNG/JPEG/BMP/GIF/TGA/PSD/PIC/PNM image (imgcodec.c)
 *   Video Player    - MPEG-1 video (.mpg/.mpeg) via pl_mpeg; audio not played yet
 *   Web Browser     - simple HTTP page reader with an address bar (no HTTPS yet)
 * Uses pl_mpeg (MIT, third_party/) for MPEG-1 decoding.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include "gui_apps.h"
#include "imgcodec.h"
#include "net.h"

#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg.h"

#define MEDIA_BAR_H   30     /* toolbar height for the picture viewer */
#define VIDEO_BAR_H   40     /* control bar height for the video player */
#define WEB_BAR_H     62     /* address bar + status line for the browser */
#define WEB_COLS      104    /* wrap width of browser text, in characters */
#define WEB_MAXL      2400   /* browser lines kept in memory */
#define WEB_HIST      12

static long long mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void base_name(const char *path, char *out, size_t n) {
    const char *b = strrchr(path, '/');
    snprintf(out, n, "%s", b ? b + 1 : path);
}

/* ============================ Picture Viewer ============================ */

static const int zoom_steps[] = { 10, 25, 50, 75, 100, 150, 200, 300, 400, 800 };
#define ZOOM_N ((int)(sizeof zoom_steps / sizeof zoom_steps[0]))

static struct {
    char path[384];
    char name[128];
    char fmt[16];
    char err[200];
    mg_image src;          /* decoded picture */
    mg_image view;         /* resampled copy for the current size/zoom */
    int zoom_idx;          /* -1 = fit to window, else index into zoom_steps */
    int key_w, key_h;      /* window size the view was built for */
    int key_zoom;
} iv;

static void iv_reset_view(void) {
    img_free(&iv.view);
    iv.key_w = iv.key_h = -1;
    iv.key_zoom = -2;
}

static void iv_open(const char *path) {
    img_free(&iv.src);
    iv_reset_view();
    iv.err[0] = 0;
    iv.zoom_idx = -1;   /* start in fit-to-window mode */
    snprintf(iv.path, sizeof iv.path, "%s", path ? path : "");
    base_name(iv.path, iv.name, sizeof iv.name);
    snprintf(iv.fmt, sizeof iv.fmt, "%s", img_format_name(iv.path));
    if (!path || !path[0]) {
        snprintf(iv.err, sizeof iv.err, "Open a picture from Files, or run: imgview <file>");
        return;
    }
    char err[160] = "";
    if (img_load_file(iv.path, &iv.src, err, sizeof err) != 0)
        snprintf(iv.err, sizeof iv.err, "%s", err);
}

/* Move to the previous/next supported picture in the same folder. */
static int cmp_names(const void *a, const void *b) {
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}
static void iv_step(int dir) {
    if (!iv.path[0]) return;
    char dirp[384];
    snprintf(dirp, sizeof dirp, "%s", iv.path);
    char *slash = strrchr(dirp, '/');
    if (!slash) return;
    if (slash == dirp) slash[1] = 0; else *slash = 0;

    DIR *d = opendir(dirp);
    if (!d) return;
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || !img_is_supported(de->d_name)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            char **nn = realloc(names, (size_t)cap * sizeof *names);
            if (!nn) break;
            names = nn;
        }
        names[n] = strdup(de->d_name);
        if (names[n]) n++;
    }
    closedir(d);
    if (n > 1) {
        qsort(names, (size_t)n, sizeof *names, cmp_names);
        int cur = -1;
        for (int i = 0; i < n; i++) if (!strcmp(names[i], iv.name)) { cur = i; break; }
        if (cur >= 0) {
            int nxt = (cur + dir + n) % n;
            char full[384];
            snprintf(full, sizeof full, "%s%s%s", dirp, (dirp[strlen(dirp) - 1] == '/') ? "" : "/", names[nxt]);
            iv_open(full);
        }
    }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

static void iv_zoom(int delta) {
    if (iv.zoom_idx < 0) {
        /* Leaving fit mode: start at the 100% step. */
        iv.zoom_idx = 4;
    } else {
        iv.zoom_idx += delta;
    }
    if (iv.zoom_idx < 0) iv.zoom_idx = -1;      /* back to fit */
    if (iv.zoom_idx >= ZOOM_N) iv.zoom_idx = ZOOM_N - 1;
}

/* Build iv.view for an area of aw x ah (cached until size or zoom changes). */
static void iv_ensure_view(int aw, int ah) {
    if (!iv.src.px) return;
    if (iv.view.px && iv.key_w == aw && iv.key_h == ah && iv.key_zoom == iv.zoom_idx) return;
    int tw, th;
    if (iv.zoom_idx < 0) {
        /* Fit: never enlarge past 100%, keep the aspect ratio. */
        double sx = (double)aw / iv.src.w, sy = (double)ah / iv.src.h;
        double s = sx < sy ? sx : sy;
        if (s > 1.0) s = 1.0;
        tw = (int)(iv.src.w * s);
        th = (int)(iv.src.h * s);
    } else {
        tw = iv.src.w * zoom_steps[iv.zoom_idx] / 100;
        th = iv.src.h * zoom_steps[iv.zoom_idx] / 100;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    /* Zoom is limited so the resampled copy stays within the memory budget. */
    while ((unsigned long long)tw * (unsigned long long)th > IMG_MAX_PIXELS && iv.zoom_idx > 0) {
        iv.zoom_idx--;
        tw = iv.src.w * zoom_steps[iv.zoom_idx] / 100;
        th = iv.src.h * zoom_steps[iv.zoom_idx] / 100;
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    img_free(&iv.view);
    img_resample(&iv.src, tw, th, &iv.view);
    iv.key_w = aw;
    iv.key_h = ah;
    iv.key_zoom = iv.zoom_idx;
}

static void iv_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    /* Toolbar */
    gfx_rect(x, y, cw, MEDIA_BAR_H, t->win_alt);
    gfx_hline(x, y + MEDIA_BAR_H - 1, cw, t->border);
    int bx = x + 8, by = y + 3, bh = MEDIA_BAR_H - 6;
    struct { const char *label; int id; int w; } btns[] = {
        { "Fit", 1, 40 }, { "100%", 2, 52 }, { "-", 3, 26 }, { "+", 4, 26 },
        { "<", 5, 26 }, { ">", 6, 26 }
    };
    for (int i = 0; i < (int)(sizeof btns / sizeof btns[0]); i++) {
        int hov = ui_hover_in(bx, by, btns[i].w, bh);
        int on = (btns[i].id == 1 && iv.zoom_idx < 0) || (btns[i].id == 2 && iv.zoom_idx == 4);
        uint32_t bg = on ? t->acc : (hov ? t->sel : t->titleoff);
        uint32_t fg = on ? t->accfg : t->winfg;
        gfx_rrect(bx, by, btns[i].w, bh, 5, bg);
        gfx_rrect_outline(bx, by, btns[i].w, bh, 5, t->border);
        int tw = gfx_text_width(btns[i].label, 1);
        gfx_text(bx + (btns[i].w - tw) / 2, by + (bh - 16) / 2, btns[i].label, fg, on, 1, btns[i].w - 4);
        ui_hot_add(bx, by, btns[i].w, bh, ACT_IMG_BTN, btns[i].id, 0);
        bx += btns[i].w + 4;
    }
    char info[220];
    if (iv.src.px) {
        if (iv.zoom_idx < 0)
            snprintf(info, sizeof info, "%s  -  %s  -  %dx%d  -  fit", iv.name, iv.fmt, iv.src.w, iv.src.h);
        else
            snprintf(info, sizeof info, "%s  -  %s  -  %dx%d  -  %d%%", iv.name, iv.fmt, iv.src.w, iv.src.h,
                     zoom_steps[iv.zoom_idx]);
    } else {
        snprintf(info, sizeof info, "%s", iv.name[0] ? iv.name : "Picture Viewer");
    }
    gfx_text(bx + 10, y + 8, info, t->muted, 0, 1, x + cw - bx - 18);

    /* Picture area */
    int ax = x, ay = y + MEDIA_BAR_H, aw = cw, ah = ch - MEDIA_BAR_H;
    if (ah <= 0) return;
    gfx_rect(ax, ay, aw, ah, 0x0B0F19);
    if (!iv.src.px) {
        int tw = gfx_text_width(iv.err, 1);
        gfx_text(ax + (aw - tw) / 2, ay + ah / 2 - 8, iv.err, t->muted, 0, 1, aw - 20);
        return;
    }
    iv_ensure_view(aw, ah);
    if (!iv.view.px) return;
    int dw = iv.view.w < aw ? iv.view.w : aw;
    int dh = iv.view.h < ah ? iv.view.h : ah;
    int ox = iv.view.w > aw ? (iv.view.w - aw) / 2 : 0;
    int oy = iv.view.h > ah ? (iv.view.h - ah) / 2 : 0;
    int dx = iv.view.w < aw ? (aw - iv.view.w) / 2 : 0;
    int dy = iv.view.h < ah ? (ah - iv.view.h) / 2 : 0;
    const uint32_t *src = iv.view.px + (size_t)oy * iv.view.w + ox;
    gfx_blit_alpha(ax + dx, ay + dy, dw, dh, src, iv.view.w);
}

/* ============================ Video Player ============================ */

static struct {
    plm_t *plm;
    char path[384];
    char name[128];
    char err[200];
    uint32_t *frame;       /* latest decoded frame, 0x00RRGGBB */
    int w, h;
    double dur;
    int playing;
    int ended;
    long long last_us;
} vid;

static void vid_on_frame(plm_t *p, plm_frame_t *f, void *user) {
    (void)p;
    (void)user;
    if (!vid.frame) return;
    /* pl_mpeg writes B,G,R,A bytes, i.e. 0xAARRGGBB as a little-endian word. */
    plm_frame_to_bgra(f, (uint8_t *)vid.frame, vid.w * 4);
}

static void vid_close(void) {
    if (vid.plm) plm_destroy(vid.plm);
    vid.plm = NULL;
    free(vid.frame);
    vid.frame = NULL;
    vid.playing = 0;
    vid.ended = 0;
}

static void vid_open(const char *path) {
    vid_close();
    vid.err[0] = 0;
    snprintf(vid.path, sizeof vid.path, "%s", path ? path : "");
    base_name(vid.path, vid.name, sizeof vid.name);
    if (!path || !path[0]) {
        snprintf(vid.err, sizeof vid.err, "Open a video from Files, or run: videoview <file>");
        return;
    }
    struct stat st;
    if (stat(path, &st) != 0) { snprintf(vid.err, sizeof vid.err, "File not found."); return; }

    vid.plm = plm_create_with_filename(path);
    if (!vid.plm || !plm_has_headers(vid.plm)) {
        if (vid.plm) plm_destroy(vid.plm);
        vid.plm = NULL;
        snprintf(vid.err, sizeof vid.err, "Not an MPEG-1 video (.mpg/.mpeg). Other formats are not supported yet.");
        return;
    }
    plm_set_audio_enabled(vid.plm, 0);   /* no audio output driver yet */
    plm_set_loop(vid.plm, 0);
    vid.w = plm_get_width(vid.plm);
    vid.h = plm_get_height(vid.plm);
    if (vid.w <= 0 || vid.h <= 0 || (unsigned long long)vid.w * (unsigned long long)vid.h > IMG_MAX_PIXELS) {
        vid_close();
        snprintf(vid.err, sizeof vid.err, "Video frame size is not supported.");
        return;
    }
    vid.frame = calloc((size_t)vid.w * (size_t)vid.h, sizeof(uint32_t));
    if (!vid.frame) { vid_close(); snprintf(vid.err, sizeof vid.err, "Out of memory."); return; }
    vid.dur = plm_get_duration(vid.plm);
    plm_set_video_decode_callback(vid.plm, vid_on_frame, NULL);
    vid.playing = 1;
    vid.last_us = mono_us();
    /* Decode the first frame straight away so the window is never blank. */
    plm_decode(vid.plm, 1.0 / 30.0);
}

static void vid_seek(double t) {
    if (!vid.plm) return;
    if (t < 0) t = 0;
    if (vid.dur > 0 && t > vid.dur) t = vid.dur;
    plm_seek(vid.plm, t, 0);
    vid.ended = 0;
    vid.last_us = mono_us();
}

static void vid_tick(void) {
    if (!vid.plm || !vid.playing) return;
    long long now = mono_us();
    double dt = (double)(now - vid.last_us) / 1e6;
    vid.last_us = now;
    if (dt > 0.25) dt = 0.25;       /* a stalled window must not jump far ahead */
    if (dt <= 0) return;
    plm_decode(vid.plm, dt);
    if (plm_has_ended(vid.plm)) {
        vid.playing = 0;
        vid.ended = 1;
    }
}

static void vid_toggle(void) {
    if (!vid.plm) return;
    if (vid.ended) { vid_seek(0); vid.ended = 0; }
    vid.playing = !vid.playing;
    vid.last_us = mono_us();
}

static void vid_fmt_time(double s, char *out, size_t n) {
    if (s < 0) s = 0;
    int t = (int)(s + 0.5);
    snprintf(out, n, "%02d:%02d", t / 60, t % 60);
}

static void vid_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, 0x000000);
    int ay = y, ah = ch - VIDEO_BAR_H;
    if (!vid.plm || !vid.frame) {
        int tw = gfx_text_width(vid.err[0] ? vid.err : "No video open", 1);
        gfx_text(x + (cw - tw) / 2, y + ah / 2 - 8, vid.err[0] ? vid.err : "No video open",
                 t->muted, 0, 1, cw - 20);
    } else {
        /* Fit the frame into the picture area, keeping its aspect ratio. */
        int dw = cw, dh = ah;
        if ((long long)vid.w * ah > (long long)vid.h * cw) dh = (int)((long long)vid.h * cw / vid.w);
        else dw = (int)((long long)vid.w * ah / vid.h);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        gfx_blit_scaled(x + (cw - dw) / 2, ay + (ah - dh) / 2, dw, dh, vid.frame, vid.w, vid.h);
    }

    /* Control bar */
    int by = y + ah;
    gfx_rect(x, by, cw, VIDEO_BAR_H, t->win_alt);
    gfx_hline(x, by, cw, t->border);
    int bx = x + 8, bty = by + 6, bh = VIDEO_BAR_H - 12;
    struct { const char *label; int id; int w; } btns[] = {
        { vid.playing ? "Pause" : "Play", 1, 60 }, { "Restart", 2, 66 },
        { "-10s", 3, 50 }, { "+10s", 4, 50 }
    };
    for (int i = 0; i < 4; i++) {
        int hov = ui_hover_in(bx, bty, btns[i].w, bh);
        uint32_t bg = (i == 0) ? t->acc : (hov ? t->sel : t->titleoff);
        uint32_t fg = (i == 0) ? t->accfg : t->winfg;
        gfx_rrect(bx, bty, btns[i].w, bh, 5, bg);
        gfx_rrect_outline(bx, bty, btns[i].w, bh, 5, t->border);
        int tw = gfx_text_width(btns[i].label, 1);
        gfx_text(bx + (btns[i].w - tw) / 2, bty + (bh - 16) / 2, btns[i].label, fg, 0, 1, btns[i].w - 4);
        ui_hot_add(bx, bty, btns[i].w, bh, ACT_VID_BTN, btns[i].id, 0);
        bx += btns[i].w + 6;
    }

    char tpos[32] = "";
    if (vid.plm) {
        char a[16], b[16];
        vid_fmt_time(plm_get_time(vid.plm), a, sizeof a);
        vid_fmt_time(vid.dur, b, sizeof b);
        snprintf(tpos, sizeof tpos, "%s / %s", a, b);
    }
    int tw = gfx_text_width(tpos, 1);
    int sx = bx + 10, sw = x + cw - sx - tw - 24;
    if (sw < 40) sw = 40;
    double frac = (vid.plm && vid.dur > 0) ? plm_get_time(vid.plm) / vid.dur : 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int sbar_y = by + VIDEO_BAR_H / 2 - 3;
    gfx_rrect(sx, sbar_y, sw, 6, 3, t->titleoff);
    int fill = (int)(sw * frac);
    if (fill > 0) gfx_rrect(sx, sbar_y, fill < 6 ? 6 : fill, 6, 3, t->acc);
    gfx_text(x + cw - tw - 12, by + (VIDEO_BAR_H - 16) / 2, tpos, t->muted, 0, 1, tw + 4);
    if (vid.ended) {
        int ew = gfx_text_width("Finished - press Restart", 1);
        gfx_text(x + (cw - ew) / 2, y + ah / 2 - 8, "Finished - press Restart", t->winfg, 0, 1, cw - 20);
    }
}

/* ============================ Web Browser ============================ */

static struct {
    char addr[512];
    int alen;
    int focus_addr;
    char status[220];
    char lines[WEB_MAXL][WEB_COLS + 1];
    int nlines, scroll;
    char hist[WEB_HIST][512];
    int nhist, hcur;
    int inited;
} web;

static void web_set_lines(const char *txt) {
    web.nlines = 0;
    web.scroll = 0;
    const char *p = txt;
    while (*p && web.nlines < WEB_MAXL) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        if (seg == 0) {
            web.lines[web.nlines++][0] = 0;
        }
        while (seg > 0 && web.nlines < WEB_MAXL) {
            size_t take = seg <= WEB_COLS ? seg : WEB_COLS;
            if (take < seg) {
                size_t sp = take;
                while (sp > 20 && p[sp] != ' ') sp--;
                if (sp > 20) take = sp;
            }
            memcpy(web.lines[web.nlines], p, take);
            web.lines[web.nlines][take] = 0;
            web.nlines++;
            p += take;
            seg -= take;
            while (seg > 0 && *p == ' ') { p++; seg--; }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

static void web_help(void) {
    web_set_lines(
        "Magnesium Web Browser\n\n"
        "Type an address above and press Enter (or click Go). Addresses without a\n"
        "scheme use http://. Pages are shown as plain text.\n\n"
        "Limits of this version:\n"
        "  - HTTP only. https:// sites need TLS, which is not built in yet.\n"
        "  - Images, scripts and styles are not loaded; links are shown as text.\n\n"
        "Needs a network connection: attach one with the DHCP button in Network Center.");
    snprintf(web.status, sizeof web.status, "Ready. Enter an http:// address.");
}

static void web_remember(const char *url) {
    if (web.nhist > 0 && !strcmp(web.hist[web.hcur], url)) return;
    if (web.hcur + 1 < web.nhist) web.nhist = web.hcur + 1;
    if (web.nhist == WEB_HIST) {
        memmove(web.hist[0], web.hist[1], (size_t)(WEB_HIST - 1) * sizeof web.hist[0]);
        web.nhist--;
    }
    snprintf(web.hist[web.nhist], sizeof web.hist[0], "%s", url);
    web.hcur = web.nhist;
    web.nhist++;
}

static void web_load(const char *url, int remember) {
    char u[512];
    snprintf(u, sizeof u, "%s", url);
    if (!strstr(u, "://")) {
        char t[512];
        snprintf(t, sizeof t, "http://%s", u);
        snprintf(u, sizeof u, "%s", t);
    }
    snprintf(web.addr, sizeof web.addr, "%s", u);
    web.alen = (int)strlen(web.addr);

    if (!strncasecmp(u, "https://", 8)) {
        web_set_lines(
            "This page needs HTTPS.\n\n"
            "Magnesium OS does not include TLS (encrypted connections) yet, so secure\n"
            "sites cannot be loaded. Try the same address with http:// if the site offers it.");
        snprintf(web.status, sizeof web.status, "HTTPS is not supported yet.");
        return;
    }
    if (strncasecmp(u, "http://", 7) != 0) {
        snprintf(web.status, sizeof web.status, "Unsupported address.");
        return;
    }

    size_t cap = 256 * 1024;
    char *raw = malloc(cap), *clean = malloc(cap);
    if (!raw || !clean) {
        free(raw);
        free(clean);
        snprintf(web.status, sizeof web.status, "Out of memory.");
        return;
    }
    int code = 0;
    char err[128] = "";
    int n = net_http_get(u, raw, cap, &code, err, sizeof err);
    if (n < 0) {
        snprintf(web.status, sizeof web.status, "Error: %s", err[0] ? err : "request failed");
        char msg[300];
        snprintf(msg, sizeof msg, "Could not load %s\n\n%s\n\nCheck the address and your network connection.",
                 u, err[0] ? err : "request failed");
        web_set_lines(msg);
    } else {
        snprintf(web.status, sizeof web.status, "HTTP %d - %d bytes from %.120s", code, n, u);
        if (strcasestr(raw, "<html") || strcasestr(raw, "<body") || strcasestr(raw, "<p"))
            net_html_to_text(raw, clean, cap);
        else
            snprintf(clean, cap, "%s", raw);
        web_set_lines(clean);
    }
    free(raw);
    free(clean);
    if (remember) web_remember(u);
}

static void web_back(void) {
    if (web.hcur <= 0) return;
    web.hcur--;
    web_load(web.hist[web.hcur], 0);
}

static void web_init(void) {
    if (web.inited) return;
    web.inited = 1;
    web_help();
    snprintf(web.addr, sizeof web.addr, "http://example.com");
    web.alen = (int)strlen(web.addr);
}

static void web_draw(const struct ui_theme *t, int x, int y, int cw, int ch) {
    web_init();
    ui_set_clip(x, y, cw, ch);
    gfx_rect(x, y, cw, ch, t->win);

    /* Address bar */
    gfx_rect(x, y, cw, WEB_BAR_H, t->win_alt);
    gfx_hline(x, y + WEB_BAR_H - 1, cw, t->border);
    int by = y + 6, bh = 26;
    int hov_b = ui_hover_in(x + 8, by, 30, bh);
    gfx_rrect(x + 8, by, 30, bh, 5, hov_b ? t->sel : t->titleoff);
    gfx_text(x + 17, by + 5, "<", t->winfg, 1, 1, 14);
    ui_hot_add(x + 8, by, 30, bh, ACT_WEB_BTN, 1, 0);
    int hov_r = ui_hover_in(x + 42, by, 64, bh);
    gfx_rrect(x + 42, by, 64, bh, 5, hov_r ? t->sel : t->titleoff);
    gfx_text(x + 52, by + 5, "Reload", t->winfg, 0, 1, 56);
    ui_hot_add(x + 42, by, 64, bh, ACT_WEB_BTN, 2, 0);

    int gw = 56;
    int fw = cw - (42 + 64 + 8) - gw - 16;
    if (fw < 80) fw = 80;
    int fx = x + 114;
    gfx_rrect(fx, by, fw, bh, 5, t->input_bg);
    gfx_rrect_outline(fx, by, fw, bh, 5, web.focus_addr ? t->acc : t->border);
    int tw = gfx_text_width(web.addr, 1);
    int maxw = fw - 16;
    int off = tw > maxw ? tw - maxw : 0;   /* keep the end of long addresses visible */
    gfx_text(fx + 8 - off, by + 5, web.addr, t->winfg, 0, 1, fw - 12 + off);
    if (web.focus_addr) {
        int cx = fx + 8 + tw - off;
        if (cx > fx + fw - 6) cx = fx + fw - 6;
        gfx_rect(cx, by + 4, 2, bh - 8, t->acc);
    }
    ui_hot_add(fx, by, fw, bh, ACT_WEB_ADDR, 0, 0);
    int gx = fx + fw + 8;
    int hov_g = ui_hover_in(gx, by, gw, bh);
    gfx_rrect(gx, by, gw, bh, 5, hov_g ? t->acc : t->titleoff);
    gfx_text(gx + 14, by + 5, "Go", hov_g ? t->accfg : t->winfg, 1, 1, gw - 4);
    ui_hot_add(gx, by, gw, bh, ACT_WEB_BTN, 3, 0);
    gfx_text(x + 10, y + 38, web.status, t->muted, 0, 1, cw - 20);

    /* Page */
    int ty = y + WEB_BAR_H + 4;
    int visible = (y + ch - ty) / 18;
    for (int i = 0; i < visible; i++) {
        int li = web.scroll + i;
        if (li >= web.nlines) break;
        gfx_text(x + 12, ty + i * 18, web.lines[li], t->winfg, 0, 1, cw - 24);
    }
    if (web.nlines > visible && visible > 0) {
        int track_h = ch - WEB_BAR_H - 8;
        int thumb_h = track_h * visible / web.nlines;
        if (thumb_h < 16) thumb_h = 16;
        int max_scroll = web.nlines - visible;
        int thumb_y = (max_scroll > 0) ? (track_h - thumb_h) * web.scroll / max_scroll : 0;
        gfx_rrect(x + cw - 8, ty + thumb_y, 5, thumb_h, 2, t->border);
    }
}

/* ============================ Shared dispatch ============================ */

void media_init_window(int kind, const char *arg, char *title_out, int *def_w, int *def_h) {
    switch (kind) {
    case WK_IMGVIEW:
        iv_open(arg);
        snprintf(title_out, 40, "Picture Viewer%s%.24s", iv.name[0] ? " - " : "", iv.name);
        *def_w = 760; *def_h = 540;
        break;
    case WK_VIDEO:
        vid_open(arg);
        snprintf(title_out, 40, "Video Player%s%.24s", vid.name[0] ? " - " : "", vid.name);
        *def_w = 680; *def_h = 520;
        break;
    case WK_BROWSER:
        web_init();
        if (arg && arg[0]) web_load(arg, 1);
        snprintf(title_out, 40, "Web Browser");
        *def_w = 800; *def_h = 560;
        break;
    default:
        break;
    }
}

void media_draw(int kind, const struct ui_theme *t, int x, int y, int cw, int ch) {
    switch (kind) {
    case WK_IMGVIEW: iv_draw(t, x, y, cw, ch); break;
    case WK_VIDEO:   vid_tick(); vid_draw(t, x, y, cw, ch); break;
    case WK_BROWSER: web_draw(t, x, y, cw, ch); break;
    default: break;
    }
}

void media_hotspot(int act, int a, int b) {
    (void)b;
    if (act == ACT_IMG_BTN) {
        switch (a) {
        case 1: iv.zoom_idx = -1; break;
        case 2: iv.zoom_idx = 4; break;
        case 3: iv_zoom(-1); break;
        case 4: iv_zoom(+1); break;
        case 5: iv_step(-1); break;
        case 6: iv_step(+1); break;
        }
    } else if (act == ACT_VID_BTN) {
        if (a == 1) vid_toggle();
        else if (a == 2) { vid_seek(0); vid.playing = 1; vid.last_us = mono_us(); }
        else if (a == 3 && vid.plm) vid_seek(plm_get_time(vid.plm) - 10.0);
        else if (a == 4 && vid.plm) vid_seek(plm_get_time(vid.plm) + 10.0);
    } else if (act == ACT_WEB_BTN) {
        if (a == 1) web_back();
        else if (a == 2) { if (web.nhist > 0) web_load(web.hist[web.hcur], 0); }
        else if (a == 3) { web.focus_addr = 0; web_load(web.addr, 1); }
    } else if (act == ACT_WEB_ADDR) {
        web.focus_addr = 1;
    }
}

void media_key(int kind, int k) {
    if (kind == WK_IMGVIEW) {
        if (k == GK_LEFT) iv_step(-1);
        else if (k == GK_RIGHT) iv_step(+1);
        else if (k == '+' || k == '=') iv_zoom(+1);
        else if (k == '-' || k == '_') iv_zoom(-1);
        else if (k == '0' || k == 'f' || k == 'F') iv.zoom_idx = -1;
        else if (k == '1') iv.zoom_idx = 4;
    } else if (kind == WK_VIDEO) {
        if (k == ' ' || k == 'p' || k == 'P') vid_toggle();
        else if (k == 'r' || k == 'R') { vid_seek(0); vid.playing = 1; vid.last_us = mono_us(); }
        else if (k == GK_LEFT && vid.plm) vid_seek(plm_get_time(vid.plm) - 5.0);
        else if (k == GK_RIGHT && vid.plm) vid_seek(plm_get_time(vid.plm) + 5.0);
    } else if (kind == WK_BROWSER) {
        web_init();
        if (web.focus_addr) {
            if (k == 13) { web.focus_addr = 0; web_load(web.addr, 1); }
            else if (k == 27) web.focus_addr = 0;
            else if ((k == 127 || k == 8) && web.alen > 0) web.addr[--web.alen] = 0;
            else if (k >= 32 && k < 127 && web.alen < (int)sizeof web.addr - 1) {
                web.addr[web.alen++] = (char)k;
                web.addr[web.alen] = 0;
            }
            return;
        }
        if (k == GK_UP && web.scroll > 0) web.scroll--;
        else if (k == GK_DOWN && web.scroll < web.nlines - 1) web.scroll++;
        else if (k == GK_PGUP) { web.scroll -= 12; if (web.scroll < 0) web.scroll = 0; }
        else if (k == GK_PGDN) { web.scroll += 12; if (web.scroll >= web.nlines) web.scroll = web.nlines - 1; if (web.scroll < 0) web.scroll = 0; }
        else if (k == GK_HOME) web.scroll = 0;
        else if (k == GK_END) web.scroll = web.nlines > 0 ? web.nlines - 1 : 0;
        else if (k == 'l' || k == 'L' || k == 47 /* / */) { web.focus_addr = 1; web.addr[0] = 0; web.alen = 0; }
    }
}

void media_wheel(int kind, int delta) {
    if (kind == WK_IMGVIEW) {
        iv_zoom(delta > 0 ? -1 : 1);
    } else if (kind == WK_BROWSER) {
        web.scroll += delta * 3;
        if (web.scroll < 0) web.scroll = 0;
        if (web.scroll > web.nlines) web.scroll = web.nlines;
    }
}

int media_needs_tick(int kind) {
    return kind == WK_VIDEO && vid.playing;
}

void media_release(int kind) {
    if (kind == WK_IMGVIEW) { img_free(&iv.src); iv_reset_view(); iv.path[0] = 0; iv.name[0] = 0; }
    else if (kind == WK_VIDEO) vid_close();
    else if (kind == WK_BROWSER) { web.focus_addr = 0; }
}
