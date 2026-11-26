/*
 * Magnesium OS - pixel graphics, KMS GPU driver + Basic Display fallback,
 * 2D drawing primitives, and hardware evdev input.
 * Original code by alxa, MIT licensed.
 */
#ifndef MG_GFX_H
#define MG_GFX_H

#include <stdint.h>
#include <stddef.h>

enum {
    GFX_NONE          = 0,
    GFX_BASIC_DISPLAY = 1,   /* Firmware VESA VBE / UEFI GOP (simpledrm or /dev/fb0) */
    GFX_GPU_DRIVER    = 2    /* Native hardware/VM DRM/KMS GPU driver */
};

struct gfx_mode {
    int w, h, hz;
    int preferred;
};

struct gfx_info {
    int backend;             /* GFX_NONE, GFX_BASIC_DISPLAY, GFX_GPU_DRIVER */
    char driver[32];         /* e.g. "virtio_gpu", "bochs-drm", "simpledrm", "vesafb" */
    char desc[96];           /* human-readable adapter / driver description */
    char devnode[32];        /* "/dev/dri/card0" or "/dev/fb0" */
    int width, height, hz, bpp;
    int hw_cursor;           /* 1 if GPU hardware cursor plane is active */
    int page_flip;           /* 1 if double-buffered KMS page flipping is active */
    int modeset;             /* 1 if live hardware resolution switching is supported */
    int nmodes;
    struct gfx_mode modes[24];
};

/* Input event & key codes (shared with gui.c) */
enum {
    GK_NONE = 0,
    GK_UP = 2000, GK_DOWN, GK_LEFT, GK_RIGHT,
    GK_HOME, GK_END, GK_DEL, GK_PGUP, GK_PGDN, GK_STAB,
    GK_F1, GK_F2, GK_F3, GK_F4, GK_F5, GK_F6, GK_F7, GK_F10,
    GK_MOUSE, GK_TIMEOUT, GK_EOF, GK_RESIZE
};

/* Mouse event state when gfx_poll_event returns GK_MOUSE */
enum { GM_RELEASE = 0, GM_PRESS = 1, GM_MOVE = 2, GM_WHEEL = 3 };

struct gfx_mouse {
    int x, y;
    int btn;                 /* 0 = left, 1 = middle, 2 = right */
    int kind;                /* GM_RELEASE, GM_PRESS, GM_MOVE, GM_WHEEL */
    int wheel;               /* -1 up, +1 down when kind == GM_WHEEL */
    int buttons_down;        /* bitmask of held buttons */
};

/* Probe graphics hardware without taking over the screen (returns backend type). */
int gfx_probe(struct gfx_info *out);

/* Returns 1 if stdin/console is a local virtual terminal (not a serial port). */
int gfx_is_local_vt(void);

/* Initialize pixel graphics mode. pref_w/pref_h = 0 selects native/preferred mode. */
int gfx_open(int pref_w, int pref_h);
void gfx_close(void);

/* Temporarily release/re-acquire master & VT for external console commands. */
void gfx_suspend(void);
int gfx_resume(void);

/* Query current state and change resolution live (returns 0 on success). */
const struct gfx_info *gfx_get_info(void);
int gfx_width(void);
int gfx_height(void);
int gfx_set_resolution(int w, int h);
int gfx_check_hotplug(int auto_res);

/* Clipping & 2D pixel drawing into the 32-bit XRGB8888 backbuffer */
enum {
    GFX_FONT_SMOOTH   = 0,   /* Anti-aliased smooth vector-reconstructed glyphs */
    GFX_FONT_SUBPIXEL = 1,   /* LCD RGB horizontal subpixel anti-aliased glyphs */
    GFX_FONT_CRISP    = 2    /* Classic 1-bit pixel bitmap */
};

void gfx_set_font_mode(int mode);
int  gfx_get_font_mode(void);

void gfx_clip(int x, int y, int w, int h);
void gfx_clip_reset(void);
void gfx_mark_all_dirty(void);

void gfx_put(int x, int y, uint32_t rgb);
void gfx_rect(int x, int y, int w, int h, uint32_t rgb);
void gfx_rect_alpha(int x, int y, int w, int h, uint32_t rgb, int alpha);
void gfx_hline(int x, int y, int w, uint32_t rgb);
void gfx_vline(int x, int y, int h, uint32_t rgb);
void gfx_line(int x0, int y0, int x1, int y1, uint32_t rgb, int thick);
void gfx_circle(int cx, int cy, int r, uint32_t rgb, int fill);
void gfx_box(int x, int y, int w, int h, uint32_t rgb, int thick);
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t rgb);
void gfx_rrect_alpha(int x, int y, int w, int h, int r, uint32_t rgb, int alpha);
void gfx_rrect_outline(int x, int y, int w, int h, int r, uint32_t rgb);
void gfx_blur_rect(int x, int y, int w, int h, int radius);
void gfx_glass_panel(int x, int y, int w, int h, int r, uint32_t tint_rgb,
                     int alpha, int blur_r, uint32_t rim_rgb, int sheen);
void gfx_shadow(int x, int y, int w, int h, int r);
void gfx_gradient_v(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bot_rgb);

/* Text rendering (scale=1: 8x16 crisp font; scale=2: 16x32 display header font) */
int gfx_char_w(int scale);
int gfx_char_h(int scale);
int gfx_text_width(const char *s, int scale);
int gfx_text(int x, int y, const char *s, uint32_t fg, int bold, int scale, int max_px);
int gfx_textf(int x, int y, uint32_t fg, int bold, int scale, int max_px, const char *fmt, ...);

/* Present backbuffer to GPU/framebuffer (VSync page-flip or dirty-rect blit). */
void gfx_present(void);

/* Poll keyboard, mouse, tablet, and optional extra fd (e.g. PTY master). */
int gfx_poll_event(int timeout_ms, int extra_fd, int *extra_ready, struct gfx_mouse *mev);
void gfx_get_cursor(int *x, int *y);

/* Shell diagnostic command: `display` / `gpu` */
int cmd_display(int argc, char **argv);

#endif
