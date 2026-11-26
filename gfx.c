/*
 * Magnesium OS - pixel graphics, KMS GPU driver + Basic Display fallback,
 * 2D drawing primitives, and hardware evdev input.
 * Original code by alxa, MIT licensed.
 *
 * Display architecture (mirrors Linux & Windows):
 *   1) Native GPU Driver (GFX_GPU_DRIVER):
 *      Uses /dev/dri/card* with hardware KMS modesetting, EDID mode list,
 *      live resolution changes, double-buffered scanout page-flipping
 *      (DRM_IOCTL_MODE_PAGE_FLIP + VBlank sync), damage rects, and a
 *      64x64 ARGB hardware cursor plane (DRM_IOCTL_MODE_CURSOR).
 *   2) Basic Display Adapter fallback (GFX_BASIC_DISPLAY):
 *      Used when no hardware GPU driver is present (UEFI GOP or BIOS VESA
 *      linear framebuffer via Linux simpledrm or /dev/fb0 vesafb/efifb).
 *      Renders in a cacheable RAM shadow buffer with dirty-rectangle blits,
 *      16/24/32-bpp pixel conversion, and a composited software cursor.
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
#include <poll.h>
#include <time.h>
#include <termios.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/input.h>
#include "gfx.h"
#include "font8x16.h"

/* ---------- Minimal Linux DRM/KMS UAPI (no libdrm needed) ---------- */

#define MG_DRM_DISPLAY_MODE_LEN     32
#define MG_DRM_CAP_DUMB_BUFFER      0x1
#define MG_DRM_CAP_CURSOR_WIDTH     0x8
#define MG_DRM_CAP_CURSOR_HEIGHT    0x9
#define MG_DRM_MODE_CONNECTED       1
#define MG_DRM_MODE_UNKNOWNCONN     3
#define MG_DRM_MODE_TYPE_PREFERRED  0x08
#define MG_DRM_MODE_PAGE_FLIP_EVENT 0x01
#define MG_DRM_MODE_CURSOR_BO       0x01
#define MG_DRM_MODE_CURSOR_MOVE     0x02
#define MG_DRM_EVENT_FLIP_COMPLETE  0x02

struct mg_drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

struct mg_drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct mg_drm_clip_rect {
    unsigned short x1, y1, x2, y2;
};

struct mg_drm_event {
    uint32_t type;
    uint32_t length;
};

struct mg_drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[MG_DRM_DISPLAY_MODE_LEN];
};

struct mg_drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct mg_drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x, y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct mg_drm_mode_modeinfo mode;
};

struct mg_drm_mode_cursor {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x, y;
    uint32_t width, height;
    uint32_t handle;
};

struct mg_drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct mg_drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width, mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct mg_drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct mg_drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

struct mg_drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

struct mg_drm_mode_create_dumb {
    uint32_t height, width, bpp, flags;
    uint32_t handle, pitch;
    uint64_t size;
};

struct mg_drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct mg_drm_mode_destroy_dumb {
    uint32_t handle;
};

#define MG_DRM_IO(nr)           _IO('d', nr)
#define MG_DRM_IOWR(nr, type)   _IOWR('d', nr, type)

#define MG_DRM_IOCTL_VERSION            MG_DRM_IOWR(0x00, struct mg_drm_version)
#define MG_DRM_IOCTL_GET_CAP            MG_DRM_IOWR(0x0c, struct mg_drm_get_cap)
#define MG_DRM_IOCTL_SET_MASTER         MG_DRM_IO(0x1e)
#define MG_DRM_IOCTL_DROP_MASTER        MG_DRM_IO(0x1f)
#define MG_DRM_IOCTL_MODE_GETRESOURCES  MG_DRM_IOWR(0xa0, struct mg_drm_mode_card_res)
#define MG_DRM_IOCTL_MODE_GETCRTC       MG_DRM_IOWR(0xa1, struct mg_drm_mode_crtc)
#define MG_DRM_IOCTL_MODE_SETCRTC       MG_DRM_IOWR(0xa2, struct mg_drm_mode_crtc)
#define MG_DRM_IOCTL_MODE_CURSOR        MG_DRM_IOWR(0xa3, struct mg_drm_mode_cursor)
#define MG_DRM_IOCTL_MODE_GETENCODER    MG_DRM_IOWR(0xa6, struct mg_drm_mode_get_encoder)
#define MG_DRM_IOCTL_MODE_GETCONNECTOR  MG_DRM_IOWR(0xa7, struct mg_drm_mode_get_connector)
#define MG_DRM_IOCTL_MODE_ADDFB         MG_DRM_IOWR(0xae, struct mg_drm_mode_fb_cmd)
#define MG_DRM_IOCTL_MODE_RMFB          MG_DRM_IOWR(0xaf, unsigned int)
#define MG_DRM_IOCTL_MODE_PAGE_FLIP     MG_DRM_IOWR(0xb0, struct mg_drm_mode_crtc_page_flip)
#define MG_DRM_IOCTL_MODE_DIRTYFB       MG_DRM_IOWR(0xb1, struct mg_drm_mode_fb_dirty_cmd)
#define MG_DRM_IOCTL_MODE_CREATE_DUMB   MG_DRM_IOWR(0xb2, struct mg_drm_mode_create_dumb)
#define MG_DRM_IOCTL_MODE_MAP_DUMB      MG_DRM_IOWR(0xb3, struct mg_drm_mode_map_dumb)
#define MG_DRM_IOCTL_MODE_DESTROY_DUMB  MG_DRM_IOWR(0xb4, struct mg_drm_mode_destroy_dumb)

#if defined(__LP64__)
_Static_assert(sizeof(struct mg_drm_version) == 64, "drm_version ABI");
_Static_assert(sizeof(struct mg_drm_mode_modeinfo) == 68, "drm_mode_modeinfo ABI");
_Static_assert(sizeof(struct mg_drm_mode_card_res) == 64, "drm_mode_card_res ABI");
_Static_assert(sizeof(struct mg_drm_mode_crtc) == 104, "drm_mode_crtc ABI");
_Static_assert(sizeof(struct mg_drm_mode_cursor) == 28, "drm_mode_cursor ABI");
_Static_assert(sizeof(struct mg_drm_mode_get_connector) == 80, "drm_mode_get_connector ABI");
_Static_assert(sizeof(struct mg_drm_mode_fb_cmd) == 28, "drm_mode_fb_cmd ABI");
_Static_assert(sizeof(struct mg_drm_mode_crtc_page_flip) == 24, "drm_mode_crtc_page_flip ABI");
_Static_assert(sizeof(struct mg_drm_mode_fb_dirty_cmd) == 24, "drm_mode_fb_dirty_cmd ABI");
_Static_assert(sizeof(struct mg_drm_mode_create_dumb) == 32, "drm_mode_create_dumb ABI");
#endif

static int drm_ioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    return r;
}

/* ---------- Cursor sprite (20x20 crisp arrow with dark outline) ---------- */

#define ART_W 16
#define ART_H 22
#define CUR_MAX 64   /* largest custom pointer image (hardware cursor limit) */
static const char *const cursor_art[ART_H] = {
    "X...............",
    "XX..............",
    "X.X.............",
    "X..X............",
    "X...X...........",
    "X....X..........",
    "X.....X.........",
    "X......X........",
    "X.......X.......",
    "X........X......",
    "X.........X.....",
    "X..........X....",
    "X......XXXXXX...",
    "X...X..X........",
    "X..XX...X.......",
    "X.X..X..X.......",
    "XX...X...X......",
    "X.....X..X......",
    "......X...X.....",
    ".......X..X.....",
    ".......XXX......",
    "................"
};
/* 'X' = dark outline (0xFF0A0E17), '.' inside arrow = white fill (0xFFFFFFFF), '.' outside = transparent */
static uint32_t cursor_argb[CUR_MAX * CUR_MAX];   /* live sprite, cur_w x cur_h */
static int cursor_ready;
static int cur_w = ART_W, cur_h = ART_H;   /* sprite size */
static int cur_hx = 0, cur_hy = 0;         /* hotspot inside the sprite */
static int cur_custom = 0;                 /* 1 when a user image is loaded */

/* Refresh pacing: the requested refresh rate (Hz). gfx_present() never
 * presents more often than this, and the desktop runs at the matching mode. */
static int pref_hz = 60;
static long long pace_us = 1000000 / 60;
static long long last_present_us = 0;
static int present_pending = 0;

static long long mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Pick the mode with the same resolution whose refresh rate is closest to hz. */
static int closest_hz_index(const struct gfx_mode *modes, int n, int w, int h, int hz) {
    int best = -1, bd = 1 << 30;
    for (int i = 0; i < n; i++) {
        if (modes[i].w != w || modes[i].h != h) continue;
        int d = modes[i].hz > hz ? modes[i].hz - hz : hz - modes[i].hz;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

static void init_cursor_pixels(void) {
    if (cursor_ready) return;
    for (int y = 0; y < ART_H; y++) {
        int inside = 0;
        /* Count spans per row: '.' between 'X' boundaries on the arrow body/stem are white */
        const char *row = cursor_art[y];
        int first_x = -1, last_x = -1;
        for (int x = 0; x < ART_W; x++) if (row[x] == 'X') { if (first_x < 0) first_x = x; last_x = x; }
        (void)inside;
        for (int x = 0; x < ART_W; x++) {
            uint32_t px = 0;
            if (row[x] == 'X') px = 0xFF0B101B;
            else if (y <= 12 && first_x >= 0 && x > first_x && x < last_x) px = 0xFFFFFFFF;
            else if (y == 13 && ((x > 0 && x < 4) || (x > 4 && x < 7))) px = 0xFFFFFFFF;
            else if (y == 14 && ((x > 0 && x < 2) || (x > 5 && x < 8))) px = 0xFFFFFFFF;
            else if (y == 15 && ((x > 0 && x < 1) || (x > 5 && x < 8))) px = 0xFFFFFFFF;
            else if (y == 16 && (x > 5 && x < 9)) px = 0xFFFFFFFF;
            else if (y == 17 && (x > 6 && x < 9)) px = 0xFFFFFFFF;
            else if (y == 18 && (x > 6 && x < 10)) px = 0xFFFFFFFF;
            else if (y == 19 && (x > 7 && x < 10)) px = 0xFFFFFFFF;
            cursor_argb[y * ART_W + x] = px;
        }
    }
    cur_w = ART_W; cur_h = ART_H; cur_hx = cur_hy = 0; cur_custom = 0;
    cursor_ready = 1;
}



/* ---------- Display state ---------- */

struct dumb_buf {
    uint32_t width, height, pitch, handle, fb_id;
    uint64_t size;
    uint8_t *map;
};

static struct {
    int open;
    struct gfx_info info;

    /* Composition backbuffer in fast cacheable RAM (XRGB8888) */
    uint32_t *comp;
    int W, H;
    int clx0, cly0, clx1, cly1;

    /* Dirty rectangle in comp_buf since last present */
    int dx0, dy0, dx1, dy1;
    /* Previous frame's dirty rect (for double-buffered KMS copy sync) */
    int pdx0, pdy0, pdx1, pdy1;

    /* DRM/KMS state (used by GFX_GPU_DRIVER and simpledrm GFX_BASIC_DISPLAY) */
    int drm_fd;
    uint32_t conn_id, enc_id, crtc_id;
    struct mg_drm_mode_crtc saved_crtc;
    int has_saved_crtc;
    struct mg_drm_mode_modeinfo kms_modes[24];
    struct mg_drm_mode_modeinfo cur_mode;
    struct dumb_buf dbuf[2];
    int nbufs, back_idx;
    int can_flip, can_dirtyfb;

    /* Hardware cursor BO (GFX_GPU_DRIVER) */
    uint32_t cur_bo_handle;
    uint32_t cur_bo_w, cur_bo_h, cur_bo_pitch;
    uint64_t cur_bo_size;
    uint32_t *cur_bo_map;

    /* Legacy fbdev state (/dev/fb0 fallback when DRM is unavailable) */
    int fb_fd;
    uint8_t *fb_map;
    size_t fb_map_len;
    uint32_t fb_pitch;
    struct fb_var_screeninfo fb_vinfo;

    /* Offscreen test framebuffer (MG_TEST_FB=/path.ppm) */
    char test_ppm[256];

    /* VT state */
    int vt_fd;
    int vt_graphics_set;
    struct termios tty_orig;
    int tty_raw;

    /* Cursor & evdev input state */
    int cur_x, cur_y;
    int sw_cur_drawn, sw_cur_x, sw_cur_y;
    int buttons;

    int ev_fds[16];
    int ev_abs_min_x[16], ev_abs_max_x[16];
    int ev_abs_min_y[16], ev_abs_max_y[16];
    int ev_has_abs[16];
    int nev;
    time_t last_ev_scan;
    int mod_shift, mod_ctrl, mod_alt, mod_caps;
} G;

/* Replace the pointer with an ARGB image (<= 64x64, hot_x/hot_y = click point).
 * Call with argb == NULL to restore the built-in arrow. Returns 0 or -1. */
static void hw_cursor_destroy(void);
static int  hw_cursor_init(void);
int gfx_cursor_set(const uint32_t *argb, int w, int h, int hot_x, int hot_y) {
    if (!argb) {
        cursor_ready = 0;
        init_cursor_pixels();
    } else {
        if (w <= 0 || h <= 0 || w > CUR_MAX || h > CUR_MAX) return -1;
        if (hot_x < 0 || hot_y < 0 || hot_x >= w || hot_y >= h) { hot_x = 0; hot_y = 0; }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) cursor_argb[y * w + x] = argb[y * w + x];
        cur_w = w; cur_h = h; cur_hx = hot_x; cur_hy = hot_y; cur_custom = 1;
        cursor_ready = 1;
    }
    if (G.open && G.drm_fd >= 0 && G.info.backend == GFX_GPU_DRIVER) {
        hw_cursor_destroy();
        hw_cursor_init();   /* falls back to the software cursor if the GPU refuses */
    }
    if (G.open) gfx_mark_all_dirty();
    return 0;
}
int gfx_cursor_is_custom(void) { return cur_custom; }

/* Set the target refresh rate (24..240 Hz). Switches the display to the
 * matching mode when the hardware offers one at the current resolution. */
int gfx_set_refresh(int hz) {
    if (hz < 24) hz = 24;
    if (hz > 240) hz = 240;
    pref_hz = hz;
    pace_us = 1000000LL / hz;
    if (!G.open || G.test_ppm[0] || G.drm_fd < 0 || !G.info.modeset) return 0;
    return gfx_set_resolution(G.W, G.H);
}
int gfx_get_refresh(void) { return pref_hz; }

static int is_firmware_driver_name(const char *name) {
    static const char *const fw[] = {
        "simpledrm", "simplefb", "vesafb", "efifb", "offb", "vgafb", "ast_fb", NULL
    };
    for (int i = 0; fw[i]; i++)
        if (!strcasecmp(name, fw[i])) return 1;
    return 0;
}

static void describe_driver(const char *drv, int backend, char *out, size_t n) {
    if (backend == GFX_BASIC_DISPLAY) {
        if (!strcasecmp(drv, "simpledrm"))
            snprintf(out, n, "Basic Display Adapter (simpledrm / Firmware GOP-VBE)");
        else
            snprintf(out, n, "Basic Display Adapter (%s / Linear Framebuffer)", drv);
        return;
    }
    if (!strcasecmp(drv, "virtio_gpu")) snprintf(out, n, "VirtIO GPU (Hardware KMS)");
    else if (!strcasecmp(drv, "bochs-drm")) snprintf(out, n, "QEMU Standard VGA / Bochs (Hardware KMS)");
    else if (!strcasecmp(drv, "vmwgfx")) snprintf(out, n, "VMware SVGA II (Hardware KMS)");
    else if (!strcasecmp(drv, "qxl")) snprintf(out, n, "SPICE QXL GPU (Hardware KMS)");
    else if (!strcasecmp(drv, "vboxvideo")) snprintf(out, n, "VirtualBox Graphics Adapter (Hardware KMS)");
    else if (!strcasecmp(drv, "hyperv_drm")) snprintf(out, n, "Microsoft Hyper-V Synthetic GPU (Hardware KMS)");
    else if (!strcasecmp(drv, "i915") || !strcasecmp(drv, "xe")) snprintf(out, n, "Intel Integrated Graphics (%s KMS)", drv);
    else if (!strcasecmp(drv, "amdgpu") || !strcasecmp(drv, "radeon")) snprintf(out, n, "AMD Radeon Graphics (%s KMS)", drv);
    else if (!strcasecmp(drv, "nouveau")) snprintf(out, n, "NVIDIA Graphics (nouveau KMS)");
    else snprintf(out, n, "%s GPU Driver (Hardware KMS)", drv);
}

int gfx_is_local_vt(void) {
    if (getenv("MG_TEST_FB")) return 1;
    long mode = 0;
    if (ioctl(0, KDGETMODE, &mode) == 0) return 1;
    return 0;
}

/* ---------- DRM/KMS helpers ---------- */

static int drm_get_driver(int fd, char *drv, size_t dlen, char *desc, size_t desclen) {
    struct mg_drm_version v;
    memset(&v, 0, sizeof v);
    drv[0] = 0;
    if (desc && desclen) desc[0] = 0;
    v.name = drv;
    v.name_len = dlen - 1;
    v.desc = desc;
    v.desc_len = desc && desclen ? desclen - 1 : 0;
    if (drm_ioctl(fd, MG_DRM_IOCTL_VERSION, &v) < 0) return -1;
    drv[v.name_len < dlen ? v.name_len : dlen - 1] = 0;
    if (desc && desclen) desc[v.desc_len < desclen ? v.desc_len : desclen - 1] = 0;
    return 0;
}

static int drm_has_dumb(int fd) {
    struct mg_drm_get_cap cap = { MG_DRM_CAP_DUMB_BUFFER, 0 };
    if (drm_ioctl(fd, MG_DRM_IOCTL_GET_CAP, &cap) < 0) return 0;
    return cap.value != 0;
}

static int drm_find_output(int fd, uint32_t *out_conn, uint32_t *out_enc, uint32_t *out_crtc,
                           struct mg_drm_mode_modeinfo *modes_out, struct gfx_mode *gmodes_out, int *nmodes_out) {
    struct mg_drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return -1;
    if (res.count_connectors == 0 || res.count_crtcs == 0) return -1;

    uint32_t conns[32], crtcs[32], encs[32], fbs[32];
    if (res.count_connectors > 32) res.count_connectors = 32;
    if (res.count_crtcs > 32) res.count_crtcs = 32;
    if (res.count_encoders > 32) res.count_encoders = 32;
    if (res.count_fbs > 32) res.count_fbs = 32;
    res.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    res.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    res.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) return -1;

    for (uint32_t ci = 0; ci < res.count_connectors; ci++) {
        struct mg_drm_mode_get_connector conn;
        memset(&conn, 0, sizeof conn);
        conn.connector_id = conns[ci];
        if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) continue;
        if (conn.connection != MG_DRM_MODE_CONNECTED && conn.connection != MG_DRM_MODE_UNKNOWNCONN) continue;
        if (conn.count_modes == 0) continue;

        uint32_t nm = conn.count_modes > 64 ? 64 : conn.count_modes;
        uint32_t ne = conn.count_encoders > 16 ? 16 : conn.count_encoders;
        uint32_t np = conn.count_props > 64 ? 64 : conn.count_props;
        struct mg_drm_mode_modeinfo mlist[64];
        uint32_t elist[16], plist[64];
        uint64_t pvlist[64];
        conn.count_modes = nm;
        conn.count_encoders = ne;
        conn.count_props = np;
        conn.modes_ptr = (uint64_t)(uintptr_t)mlist;
        conn.encoders_ptr = (uint64_t)(uintptr_t)elist;
        conn.props_ptr = (uint64_t)(uintptr_t)plist;
        conn.prop_values_ptr = (uint64_t)(uintptr_t)pvlist;
        if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || conn.count_modes == 0) continue;

        uint32_t chosen_crtc = 0, chosen_enc = conn.encoder_id;
        if (chosen_enc) {
            struct mg_drm_mode_get_encoder enc;
            memset(&enc, 0, sizeof enc);
            enc.encoder_id = chosen_enc;
            if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_GETENCODER, &enc) == 0 && enc.crtc_id)
                chosen_crtc = enc.crtc_id;
        }
        if (!chosen_crtc) {
            for (uint32_t ei = 0; ei < conn.count_encoders && !chosen_crtc; ei++) {
                struct mg_drm_mode_get_encoder enc;
                memset(&enc, 0, sizeof enc);
                enc.encoder_id = elist[ei];
                if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_GETENCODER, &enc) < 0) continue;
                for (uint32_t k = 0; k < res.count_crtcs; k++) {
                    if (enc.possible_crtcs & (1u << k)) {
                        chosen_crtc = crtcs[k];
                        chosen_enc = elist[ei];
                        break;
                    }
                }
            }
        }
        if (!chosen_crtc) chosen_crtc = crtcs[0];

        int out_n = 0;
        for (uint32_t m = 0; m < conn.count_modes && out_n < 24; m++) {
            int w = mlist[m].hdisplay, h = mlist[m].vdisplay;
            int hz = mlist[m].vrefresh ? (int)mlist[m].vrefresh : 60;
            int pref = (mlist[m].type & MG_DRM_MODE_TYPE_PREFERRED) != 0;
            if (w < 640 || h < 400) continue;
            int dup = -1;
            for (int j = 0; j < out_n; j++)
                if (gmodes_out[j].w == w && gmodes_out[j].h == h) { dup = j; break; }
            if (dup >= 0) {
                if (pref && !gmodes_out[dup].preferred) {
                    gmodes_out[dup].preferred = 1;
                    gmodes_out[dup].hz = hz;
                    if (modes_out) modes_out[dup] = mlist[m];
                }
                continue;
            }
            gmodes_out[out_n] = (struct gfx_mode){ w, h, hz, pref };
            if (modes_out) modes_out[out_n] = mlist[m];
            out_n++;
        }
        if (out_n == 0) {
            gmodes_out[0] = (struct gfx_mode){ mlist[0].hdisplay, mlist[0].vdisplay,
                                              mlist[0].vrefresh ? (int)mlist[0].vrefresh : 60, 1 };
            if (modes_out) modes_out[0] = mlist[0];
            out_n = 1;
        }
        if (out_conn) *out_conn = conn.connector_id;
        if (out_enc) *out_enc = chosen_enc;
        if (out_crtc) *out_crtc = chosen_crtc;
        *nmodes_out = out_n;
        return 0;
    }
    return -1;
}

static int dumb_create(int fd, uint32_t w, uint32_t h, struct dumb_buf *b) {
    memset(b, 0, sizeof *b);
    struct mg_drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof creq);
    creq.width = w;
    creq.height = h;
    creq.bpp = 32;
    if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;

    struct mg_drm_mode_fb_cmd fb;
    memset(&fb, 0, sizeof fb);
    fb.width = w;
    fb.height = h;
    fb.pitch = creq.pitch;
    fb.bpp = 32;
    fb.depth = 24;
    fb.handle = creq.handle;
    if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_ADDFB, &fb) < 0) {
        struct mg_drm_mode_destroy_dumb dreq = { creq.handle };
        drm_ioctl(fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    struct mg_drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof mreq);
    mreq.handle = creq.handle;
    if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        drm_ioctl(fd, MG_DRM_IOCTL_MODE_RMFB, &fb.fb_id);
        struct mg_drm_mode_destroy_dumb dreq = { creq.handle };
        drm_ioctl(fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }

    void *map = mmap(NULL, (size_t)creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        drm_ioctl(fd, MG_DRM_IOCTL_MODE_RMFB, &fb.fb_id);
        struct mg_drm_mode_destroy_dumb dreq = { creq.handle };
        drm_ioctl(fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        return -1;
    }
    memset(map, 0, (size_t)creq.size);
    b->width = w;
    b->height = h;
    b->pitch = creq.pitch;
    b->handle = creq.handle;
    b->fb_id = fb.fb_id;
    b->size = creq.size;
    b->map = (uint8_t *)map;
    return 0;
}

static void dumb_destroy(int fd, struct dumb_buf *b) {
    if (!b->map) return;
    munmap(b->map, (size_t)b->size);
    if (b->fb_id) drm_ioctl(fd, MG_DRM_IOCTL_MODE_RMFB, &b->fb_id);
    if (b->handle) {
        struct mg_drm_mode_destroy_dumb dreq = { b->handle };
        drm_ioctl(fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    memset(b, 0, sizeof *b);
}

static void hw_cursor_destroy(void) {
    if (!G.cur_bo_map) return;
    struct mg_drm_mode_cursor c = { MG_DRM_MODE_CURSOR_BO, G.crtc_id, 0, 0, 0, 0, 0 };
    drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_CURSOR, &c);
    munmap(G.cur_bo_map, (size_t)G.cur_bo_size);
    struct mg_drm_mode_destroy_dumb d = { G.cur_bo_handle };
    drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    G.cur_bo_map = NULL;
    G.cur_bo_handle = 0;
    G.info.hw_cursor = 0;
}

static int hw_cursor_init(void) {
    init_cursor_pixels();
    uint32_t cw = 64, ch = 64;
    struct mg_drm_get_cap capw = { MG_DRM_CAP_CURSOR_WIDTH, 0 };
    struct mg_drm_get_cap caph = { MG_DRM_CAP_CURSOR_HEIGHT, 0 };
    if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_GET_CAP, &capw) == 0 && capw.value >= 16 && capw.value <= 512)
        cw = (uint32_t)capw.value;
    if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_GET_CAP, &caph) == 0 && caph.value >= 16 && caph.value <= 512)
        ch = (uint32_t)caph.value;

    struct mg_drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof creq);
    creq.width = cw;
    creq.height = ch;
    creq.bpp = 32;
    if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;

    struct mg_drm_mode_map_dumb mreq = { creq.handle, 0, 0 };
    if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        struct mg_drm_mode_destroy_dumb d = { creq.handle };
        drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return -1;
    }
    uint32_t *map = mmap(NULL, (size_t)creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, G.drm_fd, (off_t)mreq.offset);
    if (map == MAP_FAILED) {
        struct mg_drm_mode_destroy_dumb d = { creq.handle };
        drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return -1;
    }
    memset(map, 0, (size_t)creq.size);
    uint32_t stride_px = creq.pitch / 4;
    for (int y = 0; y < cur_h && (uint32_t)y < ch; y++)
        for (int x = 0; x < cur_w && (uint32_t)x < cw; x++)
            map[y * stride_px + x] = cursor_argb[y * cur_w + x];

    struct mg_drm_mode_cursor cur = {
        MG_DRM_MODE_CURSOR_BO | MG_DRM_MODE_CURSOR_MOVE,
        G.crtc_id, G.cur_x - cur_hx, G.cur_y - cur_hy, cw, ch, creq.handle
    };
    if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_CURSOR, &cur) < 0) {
        munmap(map, (size_t)creq.size);
        struct mg_drm_mode_destroy_dumb d = { creq.handle };
        drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        return -1;
    }
    G.cur_bo_handle = creq.handle;
    G.cur_bo_w = cw;
    G.cur_bo_h = ch;
    G.cur_bo_pitch = creq.pitch;
    G.cur_bo_size = creq.size;
    G.cur_bo_map = map;
    G.info.hw_cursor = 1;
    return 0;
}

static void hw_cursor_move(int x, int y) {
    if (!G.info.hw_cursor || G.drm_fd < 0) return;
    struct mg_drm_mode_cursor cur = {
        MG_DRM_MODE_CURSOR_MOVE, G.crtc_id, x - cur_hx, y - cur_hy, G.cur_bo_w, G.cur_bo_h, 0
    };
    if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_CURSOR, &cur) < 0) {
        hw_cursor_destroy();
        gfx_mark_all_dirty();
    }
}

/* ---------- Hardware probe (non-destructive) ---------- */

int gfx_probe(struct gfx_info *out) {
    struct gfx_info best;
    memset(&best, 0, sizeof best);

    const char *tfb = getenv("MG_TEST_FB");
    if (tfb && tfb[0]) {
        const char *tm = getenv("MG_TEST_MODE");
        const char *tr = getenv("MG_TEST_RES");
        int tw = 1280, th = 800;
        if (tr) sscanf(tr, "%dx%d", &tw, &th);
        best.backend = (tm && !strcasecmp(tm, "basic")) ? GFX_BASIC_DISPLAY : GFX_GPU_DRIVER;
        snprintf(best.driver, sizeof best.driver, "%s", best.backend == GFX_GPU_DRIVER ? "virtio_gpu" : "simpledrm");
        describe_driver(best.driver, best.backend, best.desc, sizeof best.desc);
        snprintf(best.devnode, sizeof best.devnode, "%s", "/dev/dri/card0");
        best.width = tw; best.height = th; best.hz = 60; best.bpp = 32;
        best.hw_cursor = best.backend == GFX_GPU_DRIVER;
        best.page_flip = best.backend == GFX_GPU_DRIVER;
        best.modeset = best.backend == GFX_GPU_DRIVER;
        if (best.modeset) {
            best.modes[0] = (struct gfx_mode){ 1920, 1080, 60, 0 };
            best.modes[1] = (struct gfx_mode){ 1600, 900, 60, 0 };
            best.modes[2] = (struct gfx_mode){ 1280, 800, 60, 1 };
            best.modes[3] = (struct gfx_mode){ 1024, 768, 60, 0 };
            best.modes[4] = (struct gfx_mode){ 800, 600, 60, 0 };
            best.nmodes = 5;
        } else {
            best.modes[0] = (struct gfx_mode){ tw, th, 60, 1 };
            best.nmodes = 1;
        }
        if (out) *out = best;
        return best.backend;
    }

    /* Pass 1 & 2: scan /dev/dri/card0..7, preferring native GPU drivers over firmware simpledrm */
    for (int pass = 0; pass < 2 && best.backend == GFX_NONE; pass++) {
        for (int i = 0; i < 8; i++) {
            char path[32], drv[32];
            snprintf(path, sizeof path, "/dev/dri/card%d", i);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            if (drm_get_driver(fd, drv, sizeof drv, NULL, 0) < 0 || !drm_has_dumb(fd)) {
                close(fd);
                continue;
            }
            int fw = is_firmware_driver_name(drv);
            if ((pass == 0 && fw) || (pass == 1 && !fw)) {
                close(fd);
                continue;
            }
            struct gfx_mode mlist[24];
            int nm = 0;
            if (drm_find_output(fd, NULL, NULL, NULL, NULL, mlist, &nm) == 0 && nm > 0) {
                best.backend = fw ? GFX_BASIC_DISPLAY : GFX_GPU_DRIVER;
                snprintf(best.driver, sizeof best.driver, "%s", drv);
                describe_driver(drv, best.backend, best.desc, sizeof best.desc);
                snprintf(best.devnode, sizeof best.devnode, "%s", path);
                int pick = 0;
                for (int k = 0; k < nm; k++) if (mlist[k].preferred) { pick = k; break; }
                best.width = mlist[pick].w;
                best.height = mlist[pick].h;
                best.hz = mlist[pick].hz;
                best.bpp = 32;
                best.modeset = !fw && nm > 1;
                best.page_flip = !fw;
                best.hw_cursor = !fw;
                best.nmodes = nm;
                memcpy(best.modes, mlist, (size_t)nm * sizeof(struct gfx_mode));
                close(fd);
                break;
            }
            close(fd);
        }
    }

    /* Pass 3: legacy /dev/fb0 fallback (vesafb / efifb / simplefb) */
    if (best.backend == GFX_NONE) {
        int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            struct fb_var_screeninfo vi;
            struct fb_fix_screeninfo fi;
            if (ioctl(fd, FBIOGET_VSCREENINFO, &vi) == 0 && ioctl(fd, FBIOGET_FSCREENINFO, &fi) == 0 &&
                vi.xres >= 320 && vi.yres >= 200) {
                best.backend = GFX_BASIC_DISPLAY;
                snprintf(best.driver, sizeof best.driver, "%s", fi.id[0] ? fi.id : "fbdev");
                describe_driver(best.driver, GFX_BASIC_DISPLAY, best.desc, sizeof best.desc);
                snprintf(best.devnode, sizeof best.devnode, "/dev/fb0");
                best.width = (int)vi.xres;
                best.height = (int)vi.yres;
                best.hz = 60;
                best.bpp = (int)vi.bits_per_pixel;
                best.nmodes = 1;
                best.modes[0] = (struct gfx_mode){ (int)vi.xres, (int)vi.yres, 60, 1 };
            }
            close(fd);
        }
    }

    if (out) *out = best;
    return best.backend;
}

/* ---------- Input (evdev + tty fallback) ---------- */

static void ev_close_all(void) {
    for (int i = 0; i < G.nev; i++) if (G.ev_fds[i] >= 0) close(G.ev_fds[i]);
    G.nev = 0;
}

#define BITS_LONGS(n) (((n) + 8 * (int)sizeof(unsigned long) - 1) / (8 * (int)sizeof(unsigned long)))
static int test_bit_ul(int bit, const unsigned long *arr) {
    return (arr[bit / (8 * (int)sizeof(unsigned long))] >> (bit % (8 * (int)sizeof(unsigned long)))) & 1UL;
}

static void ev_scan(void) {
    time_t now = time(NULL);
    if (G.nev > 0 && now - G.last_ev_scan < 3) return;
    G.last_ev_scan = now;
    ev_close_all();
    for (int i = 0; i < 16 && G.nev < 16; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned long evbits[BITS_LONGS(EV_MAX + 1)] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0 ||
            (!test_bit_ul(EV_KEY, evbits) && !test_bit_ul(EV_REL, evbits) && !test_bit_ul(EV_ABS, evbits))) {
            close(fd);
            continue;
        }
        int idx = G.nev++;
        G.ev_fds[idx] = fd;
        G.ev_has_abs[idx] = 0;
        if (test_bit_ul(EV_ABS, evbits)) {
            struct input_absinfo ax, ay;
            if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0 && ioctl(fd, EVIOCGABS(ABS_Y), &ay) == 0 &&
                ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
                G.ev_has_abs[idx] = 1;
                G.ev_abs_min_x[idx] = ax.minimum;
                G.ev_abs_max_x[idx] = ax.maximum;
                G.ev_abs_min_y[idx] = ay.minimum;
                G.ev_abs_max_y[idx] = ay.maximum;
            }
        }
    }
}

static int evdev_translate_key(uint16_t code) {
    switch (code) {
    case KEY_UP:         return GK_UP;
    case KEY_DOWN:       return GK_DOWN;
    case KEY_LEFT:       return GK_LEFT;
    case KEY_RIGHT:      return GK_RIGHT;
    case KEY_HOME:       return GK_HOME;
    case KEY_END:        return GK_END;
    case KEY_DELETE:     return GK_DEL;
    case KEY_PAGEUP:     return GK_PGUP;
    case KEY_PAGEDOWN:   return GK_PGDN;
    case KEY_F1:         return GK_F1;
    case KEY_F2:         return GK_F2;
    case KEY_F3:         return GK_F3;
    case KEY_F4:         return GK_F4;
    case KEY_F5:         return GK_F5;
    case KEY_F6:         return GK_F6;
    case KEY_F7:         return GK_F7;
    case KEY_F10:        return GK_F10;
    case KEY_ESC:        return 27;
    case KEY_ENTER:
    case KEY_KPENTER:    return 13;
    case KEY_BACKSPACE:  return 127;
    case KEY_TAB:        return G.mod_shift ? GK_STAB : 9;
    case KEY_SPACE:      return ' ';
    }

    static const struct { uint16_t k; char lo, hi; } map[] = {
        { KEY_A, 'a', 'A' }, { KEY_B, 'b', 'B' }, { KEY_C, 'c', 'C' }, { KEY_D, 'd', 'D' },
        { KEY_E, 'e', 'E' }, { KEY_F, 'f', 'F' }, { KEY_G, 'g', 'G' }, { KEY_H, 'h', 'H' },
        { KEY_I, 'i', 'I' }, { KEY_J, 'j', 'J' }, { KEY_K, 'k', 'K' }, { KEY_L, 'l', 'L' },
        { KEY_M, 'm', 'M' }, { KEY_N, 'n', 'N' }, { KEY_O, 'o', 'O' }, { KEY_P, 'p', 'P' },
        { KEY_Q, 'q', 'Q' }, { KEY_R, 'r', 'R' }, { KEY_S, 's', 'S' }, { KEY_T, 't', 'T' },
        { KEY_U, 'u', 'U' }, { KEY_V, 'v', 'V' }, { KEY_W, 'w', 'W' }, { KEY_X, 'x', 'X' },
        { KEY_Y, 'y', 'Y' }, { KEY_Z, 'z', 'Z' },
        { KEY_1, '1', '!' }, { KEY_2, '2', '@' }, { KEY_3, '3', '#' }, { KEY_4, '4', '$' },
        { KEY_5, '5', '%' }, { KEY_6, '6', '^' }, { KEY_7, '7', '&' }, { KEY_8, '8', '*' },
        { KEY_9, '9', '(' }, { KEY_0, '0', ')' },
        { KEY_MINUS, '-', '_' }, { KEY_EQUAL, '=', '+' },
        { KEY_LEFTBRACE, '[', '{' }, { KEY_RIGHTBRACE, ']', '}' },
        { KEY_SEMICOLON, ';', ':' }, { KEY_APOSTROPHE, '\'', '"' },
        { KEY_GRAVE, '`', '~' }, { KEY_BACKSLASH, '\\', '|' },
        { KEY_COMMA, ',', '<' }, { KEY_DOT, '.', '>' }, { KEY_SLASH, '/', '?' },
        { KEY_KP0, '0', '0' }, { KEY_KP1, '1', '1' }, { KEY_KP2, '2', '2' },
        { KEY_KP3, '3', '3' }, { KEY_KP4, '4', '4' }, { KEY_KP5, '5', '5' },
        { KEY_KP6, '6', '6' }, { KEY_KP7, '7', '7' }, { KEY_KP8, '8', '8' },
        { KEY_KP9, '9', '9' }, { KEY_KPDOT, '.', '.' }, { KEY_KPSLASH, '/', '/' },
        { KEY_KPASTERISK, '*', '*' }, { KEY_KPMINUS, '-', '-' }, { KEY_KPPLUS, '+', '+' },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (map[i].k == code) {
            int alpha = map[i].lo >= 'a' && map[i].lo <= 'z';
            int upper = alpha ? (G.mod_shift ^ G.mod_caps) : G.mod_shift;
            int ch = (unsigned char)(upper ? map[i].hi : map[i].lo);
            if (G.mod_ctrl && alpha) return ch & 0x1F;
            return ch;
        }
    }
    return GK_NONE;
}

static void tty_enter_raw(void) {
    if (!isatty(0) || G.tty_raw) return;
    if (tcgetattr(0, &G.tty_orig) < 0) return;
    struct termios t = G.tty_orig;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    G.tty_raw = 1;
}

static void tty_leave_raw(void) {
    if (!G.tty_raw) return;
    tcsetattr(0, TCSANOW, &G.tty_orig);
    G.tty_raw = 0;
}

static void vt_enter_graphics(void) {
    G.vt_fd = -1;
    if (getenv("MG_TEST_FB")) return;
    const char *vts[] = { "/dev/tty0", "/dev/tty1", NULL };
    for (int i = 0; vts[i]; i++) {
        int fd = open(vts[i], O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            if (ioctl(fd, KDSETMODE, KD_GRAPHICS) == 0) {
                G.vt_fd = fd;
                G.vt_graphics_set = 1;
                return;
            }
            close(fd);
        }
    }
    if (isatty(0) && ioctl(0, KDSETMODE, KD_GRAPHICS) == 0) {
        G.vt_fd = 0;
        G.vt_graphics_set = 1;
    }
}

static void vt_leave_graphics(void) {
    if (G.vt_graphics_set && G.vt_fd >= 0) {
        ioctl(G.vt_fd, KDSETMODE, KD_TEXT);
        if (G.vt_fd > 2) close(G.vt_fd);
    }
    G.vt_fd = -1;
    G.vt_graphics_set = 0;
}

/* ---------- Open / Close / Modeset ---------- */

static int alloc_comp(int w, int h) {
    uint32_t *nb = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (!nb) return -1;
    free(G.comp);
    G.comp = nb;
    G.W = w;
    G.H = h;
    G.clx0 = 0; G.cly0 = 0; G.clx1 = w; G.cly1 = h;
    G.dx0 = 0; G.dy0 = 0; G.dx1 = w; G.dy1 = h;
    G.pdx0 = 0; G.pdy0 = 0; G.pdx1 = w; G.pdy1 = h;
    if (G.cur_x <= 0 || G.cur_x >= w) G.cur_x = w / 2;
    if (G.cur_y <= 0 || G.cur_y >= h) G.cur_y = h / 2;
    G.sw_cur_drawn = 0;
    return 0;
}

static int apply_kms_mode(int mode_idx) {
    struct mg_drm_mode_modeinfo m = G.kms_modes[mode_idx];
    int w = m.hdisplay, h = m.vdisplay;
    struct dumb_buf nb0, nb1;
    memset(&nb0, 0, sizeof nb0);
    memset(&nb1, 0, sizeof nb1);

    int want_double = (G.info.backend == GFX_GPU_DRIVER);
    if (dumb_create(G.drm_fd, (uint32_t)w, (uint32_t)h, &nb0) < 0) return -1;
    if (want_double && dumb_create(G.drm_fd, (uint32_t)w, (uint32_t)h, &nb1) < 0)
        want_double = 0;

    if (alloc_comp(w, h) < 0) {
        dumb_destroy(G.drm_fd, &nb0);
        if (want_double) dumb_destroy(G.drm_fd, &nb1);
        return -1;
    }

    struct mg_drm_mode_crtc crtc;
    memset(&crtc, 0, sizeof crtc);
    crtc.crtc_id = G.crtc_id;
    crtc.fb_id = nb0.fb_id;
    crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&G.conn_id;
    crtc.count_connectors = 1;
    crtc.mode = m;
    crtc.mode_valid = 1;
    if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
        dumb_destroy(G.drm_fd, &nb0);
        if (want_double) dumb_destroy(G.drm_fd, &nb1);
        return -1;
    }

    hw_cursor_destroy();
    for (int i = 0; i < G.nbufs; i++) dumb_destroy(G.drm_fd, &G.dbuf[i]);

    G.dbuf[0] = nb0;
    G.dbuf[1] = nb1;
    G.nbufs = want_double ? 2 : 1;
    G.back_idx = want_double ? 1 : 0;
    G.can_flip = want_double;
    G.can_dirtyfb = 1;
    G.cur_mode = m;
    G.info.width = w;
    G.info.height = h;
    G.info.hz = m.vrefresh ? (int)m.vrefresh : 60;
    G.info.page_flip = want_double;

    if (G.info.backend == GFX_GPU_DRIVER) hw_cursor_init();
    return 0;
}

int gfx_open(int pref_w, int pref_h) {
    if (G.open) return 0;
    memset(&G, 0, sizeof G);
    G.drm_fd = -1;
    G.fb_fd = -1;
    G.vt_fd = -1;
    init_cursor_pixels();

    /* Offscreen test framebuffer mode for automated testing */
    const char *tfb = getenv("MG_TEST_FB");
    if (tfb && tfb[0]) {
        gfx_probe(&G.info);
        snprintf(G.test_ppm, sizeof G.test_ppm, "%s", tfb);
        int w = pref_w > 0 ? pref_w : G.info.width;
        int h = pref_h > 0 ? pref_h : G.info.height;
        if (alloc_comp(w, h) < 0) return -1;
        G.info.width = w;
        G.info.height = h;
        G.open = 1;
        tty_enter_raw();
        return 0;
    }

    vt_enter_graphics();
    tty_enter_raw();
    ev_scan();

    /* Pass 1: Hardware KMS GPU driver (/dev/dri/card*); Pass 2: simpledrm firmware fallback */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 8; i++) {
            char path[32], drv[32];
            snprintf(path, sizeof path, "/dev/dri/card%d", i);
            int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) continue;
            if (drm_get_driver(fd, drv, sizeof drv, NULL, 0) < 0 || !drm_has_dumb(fd)) {
                close(fd);
                continue;
            }
            int fw = is_firmware_driver_name(drv);
            if ((pass == 0 && fw) || (pass == 1 && !fw)) {
                close(fd);
                continue;
            }
            drm_ioctl(fd, MG_DRM_IOCTL_SET_MASTER, NULL);

            struct gfx_mode gmodes[24];
            int nm = 0;
            if (drm_find_output(fd, &G.conn_id, &G.enc_id, &G.crtc_id, G.kms_modes, gmodes, &nm) == 0 && nm > 0) {
                G.drm_fd = fd;
                G.info.backend = fw ? GFX_BASIC_DISPLAY : GFX_GPU_DRIVER;
                snprintf(G.info.driver, sizeof G.info.driver, "%s", drv);
                describe_driver(drv, G.info.backend, G.info.desc, sizeof G.info.desc);
                snprintf(G.info.devnode, sizeof G.info.devnode, "%s", path);
                G.info.bpp = 32;
                G.info.modeset = !fw && nm > 1;
                G.info.nmodes = nm;
                memcpy(G.info.modes, gmodes, (size_t)nm * sizeof(struct gfx_mode));

                memset(&G.saved_crtc, 0, sizeof G.saved_crtc);
                G.saved_crtc.crtc_id = G.crtc_id;
                if (drm_ioctl(fd, MG_DRM_IOCTL_MODE_GETCRTC, &G.saved_crtc) == 0)
                    G.has_saved_crtc = 1;

                int pick = -1;
                if (pref_w > 0 && pref_h > 0) pick = closest_hz_index(gmodes, nm, pref_w, pref_h, pref_hz);
                if (pick < 0) {
                    for (int k = 0; k < nm; k++)
                        if (gmodes[k].preferred) { pick = k; break; }
                }
                if (pick < 0) pick = 0;

                if (apply_kms_mode(pick) == 0) {
                    G.open = 1;
                    return 0;
                }
            }
            drm_ioctl(fd, MG_DRM_IOCTL_DROP_MASTER, NULL);
            close(fd);
            G.drm_fd = -1;
        }
    }

    /* Pass 3: Legacy /dev/fb0 fallback (vesafb / efifb / simplefb when DRM is absent) */
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        struct fb_fix_screeninfo fi;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &G.fb_vinfo) == 0 &&
            ioctl(fd, FBIOGET_FSCREENINFO, &fi) == 0 &&
            G.fb_vinfo.xres >= 320 && G.fb_vinfo.yres >= 200 && fi.smem_len > 0) {
            uint8_t *map = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map != MAP_FAILED) {
                if (alloc_comp((int)G.fb_vinfo.xres, (int)G.fb_vinfo.yres) == 0) {
                    G.fb_fd = fd;
                    G.fb_map = map;
                    G.fb_map_len = fi.smem_len;
                    G.fb_pitch = fi.line_length;
                    G.info.backend = GFX_BASIC_DISPLAY;
                    snprintf(G.info.driver, sizeof G.info.driver, "%s", fi.id[0] ? fi.id : "fbdev");
                    describe_driver(G.info.driver, GFX_BASIC_DISPLAY, G.info.desc, sizeof G.info.desc);
                    snprintf(G.info.devnode, sizeof G.info.devnode, "/dev/fb0");
                    G.info.width = G.W;
                    G.info.height = G.H;
                    G.info.hz = 60;
                    G.info.bpp = (int)G.fb_vinfo.bits_per_pixel;
                    G.info.nmodes = 1;
                    G.info.modes[0] = (struct gfx_mode){ G.W, G.H, 60, 1 };
                    G.open = 1;
                    return 0;
                }
                munmap(map, fi.smem_len);
            }
        }
        close(fd);
    }

    ev_close_all();
    tty_leave_raw();
    vt_leave_graphics();
    return -1;
}

void gfx_close(void) {
    if (!G.open) return;
    if (G.drm_fd >= 0) {
        hw_cursor_destroy();
        if (G.has_saved_crtc) {
            G.saved_crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&G.conn_id;
            G.saved_crtc.count_connectors = 1;
            drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_SETCRTC, &G.saved_crtc);
        }
        for (int i = 0; i < G.nbufs; i++) dumb_destroy(G.drm_fd, &G.dbuf[i]);
        drm_ioctl(G.drm_fd, MG_DRM_IOCTL_DROP_MASTER, NULL);
        close(G.drm_fd);
        G.drm_fd = -1;
    }
    if (G.fb_fd >= 0) {
        if (G.fb_map) munmap(G.fb_map, G.fb_map_len);
        close(G.fb_fd);
        G.fb_fd = -1;
    }
    ev_close_all();
    tty_leave_raw();
    vt_leave_graphics();
    free(G.comp);
    G.comp = NULL;
    G.open = 0;
}

void gfx_suspend(void) {
    if (!G.open) return;
    if (G.drm_fd >= 0) {
        hw_cursor_destroy();
        if (G.has_saved_crtc) {
            G.saved_crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&G.conn_id;
            G.saved_crtc.count_connectors = 1;
            drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_SETCRTC, &G.saved_crtc);
        }
        drm_ioctl(G.drm_fd, MG_DRM_IOCTL_DROP_MASTER, NULL);
    }
    ev_close_all();
    tty_leave_raw();
    vt_leave_graphics();
}

int gfx_resume(void) {
    if (!G.open) return -1;
    vt_enter_graphics();
    tty_enter_raw();
    G.last_ev_scan = 0;
    ev_scan();
    if (G.drm_fd >= 0) {
        drm_ioctl(G.drm_fd, MG_DRM_IOCTL_SET_MASTER, NULL);
        struct mg_drm_mode_crtc crtc;
        memset(&crtc, 0, sizeof crtc);
        crtc.crtc_id = G.crtc_id;
        crtc.fb_id = G.dbuf[0].fb_id;
        crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&G.conn_id;
        crtc.count_connectors = 1;
        crtc.mode = G.cur_mode;
        crtc.mode_valid = 1;
        drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_SETCRTC, &crtc);
        G.back_idx = G.nbufs > 1 ? 1 : 0;
        if (G.info.backend == GFX_GPU_DRIVER) hw_cursor_init();
    }
    gfx_mark_all_dirty();
    return 0;
}

const struct gfx_info *gfx_get_info(void) { return &G.info; }
int gfx_width(void) { return G.W; }
int gfx_height(void) { return G.H; }
void gfx_get_cursor(int *x, int *y) { if (x) *x = G.cur_x; if (y) *y = G.cur_y; }

int gfx_set_resolution(int w, int h) {
    if (!G.open) return -1;
    if (G.test_ppm[0]) {
        if (w <= 0 || h <= 0) { w = 1280; h = 800; }
        if (w == G.W && h == G.H) return 0;
        if (alloc_comp(w, h) < 0) return -1;
        G.info.width = w; G.info.height = h;
        return 0;
    }
    if (G.drm_fd < 0 || !G.info.modeset) return -1;
    int pick = -1;
    if (w <= 0 || h <= 0) {
        for (int i = 0; i < G.info.nmodes; i++)
            if (G.info.modes[i].preferred) { pick = i; break; }
        if (pick < 0) pick = 0;
    } else {
        pick = closest_hz_index(G.info.modes, G.info.nmodes, w, h, pref_hz);
    }
    if (pick < 0) return -1;
    if (G.info.modes[pick].w == G.W && G.info.modes[pick].h == G.H && G.info.modes[pick].hz == G.info.hz)
        return 0;
    return apply_kms_mode(pick);
}

int gfx_check_hotplug(int auto_res) {
    if (!G.open || G.drm_fd < 0 || G.info.backend != GFX_GPU_DRIVER) return 0;
    struct gfx_mode gmodes[24];
    struct mg_drm_mode_modeinfo kmodes[24];
    int nm = 0;
    if (drm_find_output(G.drm_fd, &G.conn_id, &G.enc_id, &G.crtc_id, kmodes, gmodes, &nm) < 0 || nm <= 0)
        return 0;
    memcpy(G.kms_modes, kmodes, (size_t)nm * sizeof(kmodes[0]));
    memcpy(G.info.modes, gmodes, (size_t)nm * sizeof(gmodes[0]));
    G.info.nmodes = nm;
    if (!auto_res) return 0;
    int pick = 0;
    for (int i = 0; i < nm; i++) if (gmodes[i].preferred) { pick = i; break; }
    if (gmodes[pick].w != G.W || gmodes[pick].h != G.H) {
        if (apply_kms_mode(pick) == 0) return 1;
    }
    return 0;
}

/* ---------- 2D rasterizer ---------- */

static void mark_dirty(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G.W) x1 = G.W;
    if (y1 > G.H) y1 = G.H;
    if (x0 >= x1 || y0 >= y1) return;
    if (G.dx0 >= G.dx1 || G.dy0 >= G.dy1) {
        G.dx0 = x0; G.dy0 = y0; G.dx1 = x1; G.dy1 = y1;
    } else {
        if (x0 < G.dx0) G.dx0 = x0;
        if (y0 < G.dy0) G.dy0 = y0;
        if (x1 > G.dx1) G.dx1 = x1;
        if (y1 > G.dy1) G.dy1 = y1;
    }
}

void gfx_mark_all_dirty(void) {
    G.dx0 = 0; G.dy0 = 0; G.dx1 = G.W; G.dy1 = G.H;
}

void gfx_clip(int x, int y, int w, int h) {
    G.clx0 = x < 0 ? 0 : x;
    G.cly0 = y < 0 ? 0 : y;
    G.clx1 = x + w > G.W ? G.W : x + w;
    G.cly1 = y + h > G.H ? G.H : y + h;
    if (G.clx1 < G.clx0) G.clx1 = G.clx0;
    if (G.cly1 < G.cly0) G.cly1 = G.cly0;
}

void gfx_clip_reset(void) {
    G.clx0 = 0; G.cly0 = 0; G.clx1 = G.W; G.cly1 = G.H;
}

void gfx_put(int x, int y, uint32_t rgb) {
    if (x < G.clx0 || x >= G.clx1 || y < G.cly0 || y >= G.cly1) return;
    G.comp[y * G.W + x] = rgb;
    mark_dirty(x, y, x + 1, y + 1);
}

/* Copy a block of 0x00RRGGBB (or 0xAARRGGBB, alpha ignored) pixels, clipped. */
void gfx_blit(int x, int y, int w, int h, const uint32_t *src, int src_stride) {
    if (!src || w <= 0 || h <= 0) return;
    int x0 = x < G.clx0 ? G.clx0 : x, y0 = y < G.cly0 ? G.cly0 : y;
    int x1 = x + w > G.clx1 ? G.clx1 : x + w, y1 = y + h > G.cly1 ? G.cly1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        const uint32_t *sp = src + (size_t)(j - y) * src_stride + (x0 - x);
        uint32_t *dp = &G.comp[j * G.W + x0];
        for (int i = 0; i < x1 - x0; i++) dp[i] = sp[i] & 0x00FFFFFFu;
    }
    mark_dirty(x0, y0, x1, y1);
}

/* Same as gfx_blit but honours the source alpha (0..255 per pixel). */
void gfx_blit_alpha(int x, int y, int w, int h, const uint32_t *src, int src_stride) {
    if (!src || w <= 0 || h <= 0) return;
    int x0 = x < G.clx0 ? G.clx0 : x, y0 = y < G.cly0 ? G.cly0 : y;
    int x1 = x + w > G.clx1 ? G.clx1 : x + w, y1 = y + h > G.cly1 ? G.cly1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        const uint32_t *sp = src + (size_t)(j - y) * src_stride + (x0 - x);
        uint32_t *dp = &G.comp[j * G.W + x0];
        for (int i = 0; i < x1 - x0; i++) {
            uint32_t c = sp[i];
            unsigned a = c >> 24;
            if (a == 255) { dp[i] = c & 0x00FFFFFFu; continue; }
            if (a == 0) continue;
            uint32_t d = dp[i];
            unsigned ia = 255 - a;
            unsigned r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * ia) / 255;
            unsigned g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * ia) / 255;
            unsigned b = ((c & 255) * a + (d & 255) * ia) / 255;
            dp[i] = (r << 16) | (g << 8) | b;
        }
    }
    mark_dirty(x0, y0, x1, y1);
}

/* Scale a source image (sw x sh) into the rectangle dw x dh (nearest neighbour). */
void gfx_blit_scaled(int x, int y, int dw, int dh, const uint32_t *src, int sw, int sh) {
    if (!src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int x0 = x < G.clx0 ? G.clx0 : x, y0 = y < G.cly0 ? G.cly0 : y;
    int x1 = x + dw > G.clx1 ? G.clx1 : x + dw, y1 = y + dh > G.cly1 ? G.cly1 : y + dh;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        long long sy = (long long)(j - y) * sh / dh;
        const uint32_t *srow = src + sy * sw;
        uint32_t *dp = &G.comp[j * G.W + x0];
        for (int i = x0; i < x1; i++) {
            long long sx = (long long)(i - x) * sw / dw;
            *dp++ = srow[sx] & 0x00FFFFFFu;
        }
    }
    mark_dirty(x0, y0, x1, y1);
}

void gfx_rect(int x, int y, int w, int h, uint32_t rgb) {
    int x0 = x < G.clx0 ? G.clx0 : x;
    int y0 = y < G.cly0 ? G.cly0 : y;
    int x1 = x + w > G.clx1 ? G.clx1 : x + w;
    int y1 = y + h > G.cly1 ? G.cly1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &G.comp[j * G.W + x0];
        int cnt = x1 - x0;
        for (int i = 0; i < cnt; i++) row[i] = rgb;
    }
    mark_dirty(x0, y0, x1, y1);
}

static inline uint32_t blend_px(uint32_t dst, uint32_t src, int a) {
    int ia = 256 - a;
    uint32_t rb = (((src & 0xFF00FFu) * (uint32_t)a + (dst & 0xFF00FFu) * (uint32_t)ia) >> 8) & 0xFF00FFu;
    uint32_t g  = (((src & 0x00FF00u) * (uint32_t)a + (dst & 0x00FF00u) * (uint32_t)ia) >> 8) & 0x00FF00u;
    return rb | g;
}

void gfx_rect_alpha(int x, int y, int w, int h, uint32_t rgb, int alpha) {
    if (alpha <= 0) return;
    if (alpha >= 255) { gfx_rect(x, y, w, h, rgb); return; }
    int x0 = x < G.clx0 ? G.clx0 : x;
    int y0 = y < G.cly0 ? G.cly0 : y;
    int x1 = x + w > G.clx1 ? G.clx1 : x + w;
    int y1 = y + h > G.cly1 ? G.cly1 : y + h;
    if (x0 >= x1 || y0 >= y1) return;
    int a = alpha + 1;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = &G.comp[j * G.W + x0];
        int cnt = x1 - x0;
        for (int i = 0; i < cnt; i++) row[i] = blend_px(row[i], rgb, a);
    }
    mark_dirty(x0, y0, x1, y1);
}

void gfx_hline(int x, int y, int w, uint32_t rgb) { gfx_rect(x, y, w, 1, rgb); }
void gfx_vline(int x, int y, int h, uint32_t rgb) { gfx_rect(x, y, 1, h, rgb); }

void gfx_box(int x, int y, int w, int h, uint32_t rgb, int thick) {
    if (w <= 0 || h <= 0 || thick <= 0) return;
    gfx_rect(x, y, w, thick, rgb);
    gfx_rect(x, y + h - thick, w, thick, rgb);
    gfx_rect(x, y + thick, thick, h - 2 * thick, rgb);
    gfx_rect(x + w - thick, y + thick, thick, h - 2 * thick, rgb);
}

void gfx_line(int x0, int y0, int x1, int y1, uint32_t rgb, int thick) {
    if (thick < 1) thick = 1;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int half = thick / 2;
    for (;;) {
        if (thick == 1) gfx_put(x0, y0, rgb);
        else gfx_rect(x0 - half, y0 - half, thick, thick, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(int cx, int cy, int r, uint32_t rgb, int fill) {
    if (r <= 0) { gfx_put(cx, cy, rgb); return; }
    if (fill) {
        int r2 = r * r;
        for (int dy = -r; dy <= r; dy++) {
            int dx = 0;
            while ((dx + 1) * (dx + 1) + dy * dy <= r2) dx++;
            gfx_rect(cx - dx, cy + dy, 2 * dx + 1, 1, rgb);
        }
        return;
    }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_put(cx + x, cy + y, rgb); gfx_put(cx + y, cy + x, rgb);
        gfx_put(cx - y, cy + x, rgb); gfx_put(cx - x, cy + y, rgb);
        gfx_put(cx - x, cy - y, rgb); gfx_put(cx - y, cy - x, rgb);
        gfx_put(cx + y, cy - x, rgb); gfx_put(cx + x, cy - y, rgb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* 4x4 subpixel corner coverage for anti-aliased rounded rectangles */
static int rrect_corner_cov(int r, int lx, int ly) {
    /* lx, ly in [0 .. r-1] measured from outer corner (0,0); arc center is (r, r) in pixel coords */
    int r4 = r * 4;
    int r2 = r4 * r4;
    int hits = 0;
    for (int sy = 0; sy < 4; sy++) {
        int dy = r4 - (ly * 4 + sy + 1);
        for (int sx = 0; sx < 4; sx++) {
            int dx = r4 - (lx * 4 + sx + 1);
            if (dx * dx + dy * dy <= r2) hits++;
        }
    }
    return (hits * 255) / 16;
}

static void put_alpha(int x, int y, uint32_t rgb, int a) {
    if (a <= 0 || x < G.clx0 || x >= G.clx1 || y < G.cly0 || y >= G.cly1) return;
    uint32_t *p = &G.comp[y * G.W + x];
    *p = a >= 255 ? rgb : blend_px(*p, rgb, a + 1);
}

void gfx_rrect_alpha(int x, int y, int w, int h, int r, uint32_t rgb, int alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0) return;
    if (r <= 0) { gfx_rect_alpha(x, y, w, h, rgb, alpha); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_rect_alpha(x, y + r, w, h - 2 * r, rgb, alpha);
    for (int ly = 0; ly < r; ly++) {
        int full_x = r;
        for (int lx = 0; lx < r; lx++) {
            int cov = rrect_corner_cov(r, lx, ly);
            if (cov >= 255) { full_x = lx; break; }
            if (cov > 0) {
                int a = (cov * alpha) / 255;
                put_alpha(x + lx, y + ly, rgb, a);
                put_alpha(x + w - 1 - lx, y + ly, rgb, a);
                put_alpha(x + lx, y + h - 1 - ly, rgb, a);
                put_alpha(x + w - 1 - lx, y + h - 1 - ly, rgb, a);
            }
        }
        if (w - 2 * full_x > 0) {
            gfx_rect_alpha(x + full_x, y + ly, w - 2 * full_x, 1, rgb, alpha);
            gfx_rect_alpha(x + full_x, y + h - 1 - ly, w - 2 * full_x, 1, rgb, alpha);
        }
    }
    mark_dirty(x, y, x + w, y + h);
}

void gfx_rrect(int x, int y, int w, int h, int r, uint32_t rgb) {
    gfx_rrect_alpha(x, y, w, h, r, rgb, 255);
}

void gfx_rrect_outline(int x, int y, int w, int h, int r, uint32_t rgb) {
    if (w <= 2 || h <= 2) return;
    if (r <= 0) { gfx_box(x, y, w, h, rgb, 1); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_hline(x + r, y, w - 2 * r, rgb);
    gfx_hline(x + r, y + h - 1, w - 2 * r, rgb);
    gfx_vline(x, y + r, h - 2 * r, rgb);
    gfx_vline(x + w - 1, y + r, h - 2 * r, rgb);
    int r4 = r * 4, ro2 = r4 * r4, ri2 = (r4 - 4) * (r4 - 4);
    for (int ly = 0; ly < r; ly++) {
        for (int lx = 0; lx < r; lx++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++) {
                int dy = r4 - (ly * 4 + sy + 1);
                for (int sx = 0; sx < 4; sx++) {
                    int dx = r4 - (lx * 4 + sx + 1);
                    int d2 = dx * dx + dy * dy;
                    if (d2 <= ro2 && d2 >= ri2) hits++;
                }
            }
            if (hits > 0) {
                int a = (hits * 255) / 16;
                put_alpha(x + lx, y + ly, rgb, a);
                put_alpha(x + w - 1 - lx, y + ly, rgb, a);
                put_alpha(x + lx, y + h - 1 - ly, rgb, a);
                put_alpha(x + w - 1 - lx, y + h - 1 - ly, rgb, a);
            }
        }
    }
    mark_dirty(x, y, x + w, y + h);
}

/* Fast O(1)-per-pixel 2-pass separable running-sum box blur for VaporGlass */
void gfx_blur_rect(int x, int y, int w, int h, int radius) {
    if (radius <= 0 || w <= 2 || h <= 2) return;
    if (radius > 16) radius = 16;
    int x0 = x < G.clx0 ? G.clx0 : x;
    int y0 = y < G.cly0 ? G.cly0 : y;
    int x1 = x + w > G.clx1 ? G.clx1 : x + w;
    int y1 = y + h > G.cly1 ? G.cly1 : y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G.W) x1 = G.W;
    if (y1 > G.H) y1 = G.H;
    int bw = x1 - x0, bh = y1 - y0;
    if (bw <= 2 || bh <= 2 || bw > 4096 || bh > 4096) return;

    static uint32_t line[4096];
    int div = 2 * radius + 1;

    for (int pass = 0; pass < 2; pass++) {
        /* Horizontal running-sum pass */
        for (int yy = y0; yy < y1; yy++) {
            uint32_t *row = &G.comp[yy * G.W + x0];
            memcpy(line, row, (size_t)bw * sizeof(uint32_t));
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int k = -radius; k <= radius; k++) {
                int idx = k < 0 ? 0 : (k >= bw ? bw - 1 : k);
                uint32_t c = line[idx];
                sr += (c >> 16) & 255u; sg += (c >> 8) & 255u; sb += c & 255u;
            }
            for (int i = 0; i < bw; i++) {
                row[i] = ((sr / (uint32_t)div) << 16) | ((sg / (uint32_t)div) << 8) | (sb / (uint32_t)div);
                int out_i = i - radius; if (out_i < 0) out_i = 0;
                int in_i  = i + radius + 1; if (in_i >= bw) in_i = bw - 1;
                uint32_t co = line[out_i], ci = line[in_i];
                sr += ((ci >> 16) & 255u) - ((co >> 16) & 255u);
                sg += ((ci >> 8) & 255u)  - ((co >> 8) & 255u);
                sb += (ci & 255u)         - (co & 255u);
            }
        }
        /* Vertical running-sum pass */
        for (int xx = x0; xx < x1; xx++) {
            for (int j = 0; j < bh; j++) line[j] = G.comp[(y0 + j) * G.W + xx];
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int k = -radius; k <= radius; k++) {
                int idx = k < 0 ? 0 : (k >= bh ? bh - 1 : k);
                uint32_t c = line[idx];
                sr += (c >> 16) & 255u; sg += (c >> 8) & 255u; sb += c & 255u;
            }
            for (int j = 0; j < bh; j++) {
                G.comp[(y0 + j) * G.W + xx] =
                    ((sr / (uint32_t)div) << 16) | ((sg / (uint32_t)div) << 8) | (sb / (uint32_t)div);
                int out_j = j - radius; if (out_j < 0) out_j = 0;
                int in_j  = j + radius + 1; if (in_j >= bh) in_j = bh - 1;
                uint32_t co = line[out_j], ci = line[in_j];
                sr += ((ci >> 16) & 255u) - ((co >> 16) & 255u);
                sg += ((ci >> 8) & 255u)  - ((co >> 8) & 255u);
                sb += (ci & 255u)         - (co & 255u);
            }
        }
    }
    mark_dirty(x0, y0, x1, y1);
}

/*
 * Magnesium VaporGlass panel:
 * Frosted live backdrop blur + mineral tint + diagonal crystalline ion-sheen +
 * inner top specular highlight + anti-aliased luminous rim.
 */
void gfx_glass_panel(int x, int y, int w, int h, int r, uint32_t tint_rgb,
                     int alpha, int blur_r, uint32_t rim_rgb, int sheen) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    if (blur_r > 0 && alpha < 248 && w > 4 && h > 4) {
        int inset = r > 4 ? 2 : 1;
        gfx_blur_rect(x + inset, y + inset, w - 2 * inset, h - 2 * inset, blur_r);
    }

    gfx_rrect_alpha(x, y, w, h, r, tint_rgb, alpha);

    /* Subtle crystalline ion-sheen (angled mineral light facets across top surface) */
    if (sheen > 0 && h > 8 && w > 12) {
        int sh_h = h < 64 ? h / 2 : 34;
        int x0 = x < G.clx0 ? G.clx0 : x;
        int x1 = x + w > G.clx1 ? G.clx1 : x + w;
        for (int j = 1; j < sh_h; j++) {
            int py = y + j;
            if (py < G.cly0 || py >= G.cly1) continue;
            int ins = j < r ? (r - 1) : 1;
            int lx0 = x + ins < x0 ? x0 : x + ins;
            int lx1 = x + w - ins > x1 ? x1 : x + w - ins;
            if (lx0 >= lx1) continue;
            int vfade = ((sh_h - j) * sheen) / sh_h;
            uint32_t *row = &G.comp[py * G.W];
            for (int px = lx0; px < lx1; px++) {
                /* Gentle crystalline facet wave (uniquely Magnesium mineral-glass) */
                int u = (px - x) + j * 2;
                int facet = ((u % 180) < 46) ? 10 : (((u % 180) > 110 && (u % 180) < 132) ? 6 : 0);
                int a = vfade + (facet * (sh_h - j)) / sh_h;
                if (a > 0) row[px] = blend_px(row[px], 0xFFFFFFu, a + 1);
            }
        }
        /* Crisp 1px top specular highlight inside the rounded corners */
        int hx = x + (r > 2 ? r : 2);
        int hw = w - 2 * (r > 2 ? r : 2);
        if (hw > 0) gfx_rect_alpha(hx, y + 1, hw, 1, 0xFFFFFFu, sheen + 26 > 110 ? 110 : sheen + 26);
        mark_dirty(x, y, x + w, y + sh_h + 2);
    }

    gfx_rrect_outline(x, y, w, h, r, rim_rgb);
}

void gfx_shadow(int x, int y, int w, int h, int r) {
    /* Soft multi-step feathered ambient drop shadow */
    static const int alphas[5] = { 34, 24, 16, 10, 5 };
    for (int i = 0; i < 5; i++) {
        int pad = i * 2 + 1;
        int sx = x - pad + 2, sy = y + 3 - i;
        int sw = w + pad * 2, sh = h + pad + i + 2;
        int sr = r + pad;
        /* Draw outer shadow band around window bounds so translucent glass stays clean */
        gfx_rect_alpha(sx + sr / 2, sy + sh - 2, sw - sr, 2, 0x000000, alphas[i]);
        gfx_rect_alpha(sx + sw - 2, sy + sr / 2, 2, sh - sr, 0x000000, alphas[i]);
        gfx_rect_alpha(sx, sy + sr / 2, 2, sh - sr, 0x000000, alphas[i] / 2);
    }
}

void gfx_gradient_v(int x, int y, int w, int h, uint32_t top_rgb, uint32_t bot_rgb) {
    if (w <= 0 || h <= 0) return;
    int r0 = (top_rgb >> 16) & 255, g0 = (top_rgb >> 8) & 255, b0 = top_rgb & 255;
    int r1 = (bot_rgb >> 16) & 255, g1 = (bot_rgb >> 8) & 255, b1 = bot_rgb & 255;
    int denom = h > 1 ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        uint32_t c = ((uint32_t)(r0 + (r1 - r0) * j / denom) << 16) |
                     ((uint32_t)(g0 + (g1 - g0) * j / denom) << 8) |
                     (uint32_t)(b0 + (b1 - b0) * j / denom);
        gfx_rect(x, y + j, w, 1, c);
    }
}

/* ---------- Smooth Anti-Aliased Typography Engine ---------- */

#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1)
static int font_mode = GFX_FONT_SMOOTH;
static int font_aa_ready = 0;
static uint8_t font_aa1[FONT_COUNT][16][8];   /* 1x (8x16) smooth hinted coverage */
static uint8_t font_aa2[FONT_COUNT][32][16];  /* 2x (16x32) native smooth vector coverage */

void gfx_set_font_mode(int mode) {
    if (mode < GFX_FONT_SMOOTH || mode > GFX_FONT_CRISP) mode = GFX_FONT_SMOOTH;
    font_mode = mode;
}
int gfx_get_font_mode(void) { return font_mode; }

static inline int glyph_bit(int g, int x, int y) {
    if (x < 0 || x >= FONT_W || y < 0 || y >= FONT_H) return 0;
    return (mg_font8x16[g][y] & (0x80u >> x)) ? 1 : 0;
}

struct fseg { int16_t x0, y0, x1, y1, r2; };

/*
 * Reconstructs continuous vector stroke capsules (stems, bars, curves & diagonals)
 * from each 8x16 glyph and rasterizes 16x-supersampled anti-aliased coverage
 * atlases for both 1x (8x16) and native 2x (16x32) typography.
 */
static void ensure_font_atlases(void) {
    if (font_aa_ready) return;
    font_aa_ready = 1;

    for (int g = 0; g < FONT_COUNT; g++) {
        struct fseg segs[256];
        int nseg = 0;
        for (int y = 0; y < FONT_H; y++) {
            for (int x = 0; x < FONT_W; x++) {
                if (!glyph_bit(g, x, y)) continue;
                int cx = x * 16 + 8, cy = y * 16 + 8;
                int has_nbr = 0;
                /* Orthogonal stems & bars: crisp pixel-aligned radius (8.0/16 = 0.5px) */
                if (glyph_bit(g, x + 1, y) && nseg < 256) {
                    segs[nseg++] = (struct fseg){ (int16_t)cx, (int16_t)cy, (int16_t)(cx + 16), (int16_t)cy, 64 };
                    has_nbr = 1;
                }
                if (glyph_bit(g, x, y + 1) && nseg < 256) {
                    segs[nseg++] = (struct fseg){ (int16_t)cx, (int16_t)cy, (int16_t)cx, (int16_t)(cy + 16), 64 };
                    has_nbr = 1;
                }
                /* Diagonal strokes & curve arcs: connect across empty saddles into smooth 45-deg capsules */
                if (glyph_bit(g, x + 1, y + 1) &&
                    !(glyph_bit(g, x + 1, y) && glyph_bit(g, x, y + 1)) && nseg < 256) {
                    segs[nseg++] = (struct fseg){ (int16_t)cx, (int16_t)cy, (int16_t)(cx + 16), (int16_t)(cy + 16), 54 };
                    has_nbr = 1;
                }
                if (glyph_bit(g, x - 1, y + 1) &&
                    !(glyph_bit(g, x - 1, y) && glyph_bit(g, x, y + 1)) && nseg < 256) {
                    segs[nseg++] = (struct fseg){ (int16_t)cx, (int16_t)cy, (int16_t)(cx - 16), (int16_t)(cy + 16), 54 };
                    has_nbr = 1;
                }
                /* Isolated dot (e.g. 'i', ':', '.') */
                if (!has_nbr && !glyph_bit(g, x - 1, y) && !glyph_bit(g, x, y - 1) &&
                    !glyph_bit(g, x - 1, y - 1) && !glyph_bit(g, x + 1, y - 1) && nseg < 256) {
                    segs[nseg++] = (struct fseg){ (int16_t)cx, (int16_t)cy, (int16_t)cx, (int16_t)cy, 68 };
                }
            }
        }
        if (nseg == 0) continue;

        /* 1x (8x16) hinted smooth coverage: 4x4 subsamples per pixel */
        for (int py = 0; py < 16; py++) {
            for (int px = 0; px < 8; px++) {
                int hits = 0;
                for (int sy = 0; sy < 4; sy++) {
                    int fy = py * 16 + sy * 4 + 2;
                    for (int sx = 0; sx < 4; sx++) {
                        int fx = px * 16 + sx * 4 + 2;
                        for (int k = 0; k < nseg; k++) {
                            int dx = segs[k].x1 - segs[k].x0, dy = segs[k].y1 - segs[k].y0;
                            int l2 = dx * dx + dy * dy;
                            int t = l2 ? (((fx - segs[k].x0) * dx + (fy - segs[k].y0) * dy) * 256) / l2 : 0;
                            if (t < 0) t = 0; else if (t > 256) t = 256;
                            int qx = segs[k].x0 + (dx * t) / 256;
                            int qy = segs[k].y0 + (dy * t) / 256;
                            int d2 = (fx - qx) * (fx - qx) + (fy - qy) * (fy - qy);
                            if (d2 <= segs[k].r2) { hits++; break; }
                        }
                    }
                }
                int a = (hits * 255) / 16;
                /* Slight perceptual contrast lift so 1x stems stay crisp and smooth edges pop */
                a = a + ((a * (255 - a)) >> 9);
                if (a > 255) a = 255;
                font_aa1[g][py][px] = (uint8_t)a;
            }
        }

        /* 2x (16x32) native smooth vector coverage: 4x4 subsamples per 2x pixel */
        for (int py = 0; py < 32; py++) {
            for (int px = 0; px < 16; px++) {
                int hits = 0;
                for (int sy = 0; sy < 4; sy++) {
                    int fy = py * 8 + sy * 2 + 1;
                    for (int sx = 0; sx < 4; sx++) {
                        int fx = px * 8 + sx * 2 + 1;
                        for (int k = 0; k < nseg; k++) {
                            int dx = segs[k].x1 - segs[k].x0, dy = segs[k].y1 - segs[k].y0;
                            int l2 = dx * dx + dy * dy;
                            int t = l2 ? (((fx - segs[k].x0) * dx + (fy - segs[k].y0) * dy) * 256) / l2 : 0;
                            if (t < 0) t = 0; else if (t > 256) t = 256;
                            int qx = segs[k].x0 + (dx * t) / 256;
                            int qy = segs[k].y0 + (dy * t) / 256;
                            int d2 = (fx - qx) * (fx - qx) + (fy - qy) * (fy - qy);
                            if (d2 <= segs[k].r2) { hits++; break; }
                        }
                    }
                }
                int a = (hits * 255) / 16;
                a = a + ((a * (255 - a)) >> 9);
                if (a > 255) a = 255;
                font_aa2[g][py][px] = (uint8_t)a;
            }
        }
    }
}

static uint32_t decode_utf8(const char **sp) {
    const unsigned char *s = (const unsigned char *)*sp;
    uint32_t c = *s;
    if (c < 0x80) { *sp += 1; return c; }
    int n = 0;
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

static int map_glyph(uint32_t cp) {
    if (cp >= 32 && cp <= 126) return (int)cp;
    switch (cp) {
    case 0xB7: case 0x2022: return 127; /* bullet */
    case 0x25B8: case 0x25B6: return 128; /* right triangle */
    case 0x25BE: case 0x25BC: return 129; /* down triangle */
    case 0x2039: case 0xAB: return 130;   /* left chevron */
    case 0x203A: case 0xBB: return 131;   /* right chevron */
    case 0xD7: case 0x2715: return 132;   /* close X */
    case 0x2014: case 0x2013: return 133; /* em-dash */
    default: return '?';
    }
}

int gfx_char_w(int scale) { return FONT_W * (scale < 1 ? 1 : scale); }
int gfx_char_h(int scale) { return FONT_H * (scale < 1 ? 1 : scale); }

int gfx_text_width(const char *s, int scale) {
    if (!s) return 0;
    int cw = gfx_char_w(scale), n = 0;
    while (*s) { decode_utf8(&s); n++; }
    return n * cw;
}

static inline uint32_t blend_subpixel(uint32_t dst, uint32_t fg, int ar, int ag, int ab) {
    uint32_t dr = (dst >> 16) & 255u, dg = (dst >> 8) & 255u, db = dst & 255u;
    uint32_t sr = (fg >> 16) & 255u,  sg = (fg >> 8) & 255u,  sb = fg & 255u;
    uint32_t r = (sr * (uint32_t)(ar + 1) + dr * (uint32_t)(256 - (ar + 1))) >> 8;
    uint32_t g = (sg * (uint32_t)(ag + 1) + dg * (uint32_t)(256 - (ag + 1))) >> 8;
    uint32_t b = (sb * (uint32_t)(ab + 1) + db * (uint32_t)(256 - (ab + 1))) >> 8;
    return (r << 16) | (g << 8) | b;
}

int gfx_text(int x, int y, const char *s, uint32_t fg, int bold, int scale, int max_px) {
    if (!s || !*s) return 0;
    if (scale < 1) scale = 1;
    int cw = FONT_W * scale, ch = FONT_H * scale;
    int drawn = 0;
    int start_x = x;

    if (font_mode == GFX_FONT_CRISP) {
        while (*s) {
            if (max_px >= 0 && drawn + cw > max_px) break;
            int g = map_glyph(decode_utf8(&s)) - FONT_FIRST;
            const uint8_t *bmp = mg_font8x16[g];
            if (x + cw > G.clx0 && x < G.clx1 && y + ch > G.cly0 && y < G.cly1) {
                for (int r = 0; r < FONT_H; r++) {
                    uint8_t bits = bmp[r];
                    if (bold) bits |= (uint8_t)(bits >> 1);
                    if (!bits) continue;
                    for (int sy = 0; sy < scale; sy++) {
                        int py = y + r * scale + sy;
                        if (py < G.cly0 || py >= G.cly1) continue;
                        uint32_t *row = &G.comp[py * G.W];
                        for (int c = 0; c < FONT_W; c++) {
                            if (!(bits & (0x80u >> c))) continue;
                            for (int sx = 0; sx < scale; sx++) {
                                int px = x + c * scale + sx;
                                if (px >= G.clx0 && px < G.clx1) row[px] = fg;
                            }
                        }
                    }
                }
            }
            x += cw;
            drawn += cw;
        }
        if (drawn > 0) mark_dirty(start_x, y, start_x + drawn + (bold ? scale : 0), y + ch);
        return drawn;
    }

    ensure_font_atlases();
    int subpx = (font_mode == GFX_FONT_SUBPIXEL);

    while (*s) {
        if (max_px >= 0 && drawn + cw > max_px) break;
        int g = map_glyph(decode_utf8(&s)) - FONT_FIRST;
        if (x + cw + 2 > G.clx0 && x < G.clx1 && y + ch > G.cly0 && y < G.cly1) {
            if (scale == 1) {
                for (int r = 0; r < 16; r++) {
                    int py = y + r;
                    if (py < G.cly0 || py >= G.cly1) continue;
                    uint32_t *row = &G.comp[py * G.W];
                    for (int c = 0; c < 8; c++) {
                        int px = x + c;
                        if (px < G.clx0 || px >= G.clx1) continue;
                        int a = font_aa1[g][r][c];
                        if (bold && c > 0) {
                            int bprev = (font_aa1[g][r][c - 1] * 3) >> 2;
                            if (bprev > a) a = bprev;
                        }
                        if (a <= 0) continue;
                        if (!subpx) {
                            row[px] = a >= 254 ? fg : blend_px(row[px], fg, a + 1);
                        } else {
                            int al = c > 0 ? font_aa1[g][r][c - 1] : 0;
                            int ar_n = c < 7 ? font_aa1[g][r][c + 1] : 0;
                            int cr = (al + a * 2) / 3, cg = a, cb = (a * 2 + ar_n) / 3;
                            row[px] = blend_subpixel(row[px], fg, cr, cg, cb);
                        }
                    }
                }
            } else {
                int mult = scale / 2;
                if (mult < 1) mult = 1;
                for (int r = 0; r < 32; r++) {
                    for (int sy = 0; sy < mult; sy++) {
                        int py = y + r * mult + sy;
                        if (py < G.cly0 || py >= G.cly1) continue;
                        uint32_t *row = &G.comp[py * G.W];
                        for (int c = 0; c < 16; c++) {
                            int a = font_aa2[g][r][c];
                            if (bold && c > 0) {
                                int bp = font_aa2[g][r][c - 1];
                                if (bp > a) a = bp;
                            }
                            if (a <= 0) continue;
                            for (int sx = 0; sx < mult; sx++) {
                                int px = x + c * mult + sx;
                                if (px >= G.clx0 && px < G.clx1)
                                    row[px] = a >= 254 ? fg : blend_px(row[px], fg, a + 1);
                            }
                        }
                    }
                }
            }
        }
        x += cw;
        drawn += cw;
    }
    if (drawn > 0) mark_dirty(start_x, y, start_x + drawn + (bold ? scale : 0), y + ch);
    return drawn;
}

int gfx_textf(int x, int y, uint32_t fg, int bold, int scale, int max_px, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return gfx_text(x, y, buf, fg, bold, scale, max_px);
}

/* ---------- Present (GPU Page-Flip vs Basic Display Dirty-Rect Blit) ---------- */

static inline uint32_t pack_fb_pixel(uint32_t xrgb, const struct fb_var_screeninfo *v) {
    uint32_t r = (xrgb >> 16) & 255u;
    uint32_t g = (xrgb >> 8) & 255u;
    uint32_t b = xrgb & 255u;
    r >>= (8u - (v->red.length ? v->red.length : 8u));
    g >>= (8u - (v->green.length ? v->green.length : 8u));
    b >>= (8u - (v->blue.length ? v->blue.length : 8u));
    return (r << v->red.offset) | (g << v->green.offset) | (b << v->blue.offset);
}

static void blit_region_to_target(uint8_t *dst_base, uint32_t dst_pitch, int is_fbdev,
                                  int x0, int y0, int x1, int y1, int draw_sw_cursor) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > G.W) x1 = G.W;
    if (y1 > G.H) y1 = G.H;
    if (x0 >= x1 || y0 >= y1) return;

    int bpp = is_fbdev ? (int)G.fb_vinfo.bits_per_pixel : 32;
    int std_xrgb = !is_fbdev || (bpp == 32 && G.fb_vinfo.red.offset == 16 &&
                                 G.fb_vinfo.green.offset == 8 && G.fb_vinfo.blue.offset == 0);

    for (int y = y0; y < y1; y++) {
        const uint32_t *src = &G.comp[y * G.W + x0];
        uint8_t *drow = dst_base + (size_t)y * dst_pitch;
        int cur_row = draw_sw_cursor && y >= (G.cur_y - cur_hy) && y < (G.cur_y - cur_hy) + cur_h;

        if (std_xrgb && !cur_row) {
            memcpy(drow + (size_t)x0 * 4, src, (size_t)(x1 - x0) * 4);
            continue;
        }
        for (int x = x0; x < x1; x++) {
            uint32_t px = G.comp[y * G.W + x];
            if (cur_row && x >= (G.cur_x - cur_hx) && x < (G.cur_x - cur_hx) + cur_w) {
                uint32_t cpx = cursor_argb[(y - (G.cur_y - cur_hy)) * cur_w + (x - (G.cur_x - cur_hx))];
                if (cpx >> 24) px = cpx & 0xFFFFFFu;
            }
            if (std_xrgb) {
                ((uint32_t *)drow)[x] = px;
            } else if (bpp == 32) {
                ((uint32_t *)drow)[x] = pack_fb_pixel(px, &G.fb_vinfo);
            } else if (bpp == 24) {
                uint32_t v = pack_fb_pixel(px, &G.fb_vinfo);
                drow[x * 3 + 0] = (uint8_t)(v & 0xFF);
                drow[x * 3 + 1] = (uint8_t)((v >> 8) & 0xFF);
                drow[x * 3 + 2] = (uint8_t)((v >> 16) & 0xFF);
            } else if (bpp == 16) {
                ((uint16_t *)drow)[x] = (uint16_t)pack_fb_pixel(px, &G.fb_vinfo);
            }
        }
    }
}

static void write_test_ppm(void) {
    FILE *f = fopen(G.test_ppm, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", G.W, G.H);
    uint8_t *row = malloc((size_t)G.W * 3);
    if (row) {
        for (int y = 0; y < G.H; y++) {
            for (int x = 0; x < G.W; x++) {
                uint32_t px = G.comp[y * G.W + x];
                if (y >= (G.cur_y - cur_hy) && y < (G.cur_y - cur_hy) + cur_h && x >= (G.cur_x - cur_hx) && x < (G.cur_x - cur_hx) + cur_w) {
                    uint32_t cpx = cursor_argb[(y - (G.cur_y - cur_hy)) * cur_w + (x - (G.cur_x - cur_hx))];
                    if (cpx >> 24) px = cpx & 0xFFFFFFu;
                }
                row[x * 3 + 0] = (uint8_t)((px >> 16) & 255);
                row[x * 3 + 1] = (uint8_t)((px >> 8) & 255);
                row[x * 3 + 2] = (uint8_t)(px & 255);
            }
            fwrite(row, 1, (size_t)G.W * 3, f);
        }
        free(row);
    }
    fclose(f);
}

void gfx_present(void) {
    if (!G.open) return;
    /* Frame limiter: present at most once per refresh interval. */
    long long now_us = mono_us();
    if (now_us - last_present_us < pace_us) { present_pending = 1; return; }
    last_present_us = now_us;
    present_pending = 0;

    int sw_cur = !G.info.hw_cursor;
    if (sw_cur) {
        if (!G.sw_cur_drawn || G.sw_cur_x != G.cur_x - cur_hx || G.sw_cur_y != G.cur_y - cur_hy) {
            if (G.sw_cur_drawn)
                mark_dirty(G.sw_cur_x, G.sw_cur_y, G.sw_cur_x + cur_w, G.sw_cur_y + cur_h);
            mark_dirty(G.cur_x - cur_hx, G.cur_y - cur_hy, G.cur_x - cur_hx + cur_w, G.cur_y - cur_hy + cur_h);
            G.sw_cur_x = G.cur_x - cur_hx;
            G.sw_cur_y = G.cur_y - cur_hy;
            G.sw_cur_drawn = 1;
        }
    }

    if (G.dx0 >= G.dx1 || G.dy0 >= G.dy1) return;

    if (G.test_ppm[0]) {
        write_test_ppm();
        G.dx0 = G.dy0 = G.dx1 = G.dy1 = 0;
        return;
    }

    /* Path A: Native GPU Driver with double-buffered KMS page-flipping */
    if (G.drm_fd >= 0 && G.nbufs == 2 && G.can_flip) {
        int ux0 = G.dx0 < G.pdx0 ? G.dx0 : G.pdx0;
        int uy0 = G.dy0 < G.pdy0 ? G.dy0 : G.pdy0;
        int ux1 = G.dx1 > G.pdx1 ? G.dx1 : G.pdx1;
        int uy1 = G.dy1 > G.pdy1 ? G.dy1 : G.pdy1;
        if (G.pdx0 >= G.pdx1) { ux0 = G.dx0; uy0 = G.dy0; ux1 = G.dx1; uy1 = G.dy1; }

        struct dumb_buf *back = &G.dbuf[G.back_idx];
        blit_region_to_target(back->map, back->pitch, 0, ux0, uy0, ux1, uy1, sw_cur);

        if (G.can_dirtyfb) {
            struct mg_drm_clip_rect clip = {
                (unsigned short)G.dx0, (unsigned short)G.dy0,
                (unsigned short)G.dx1, (unsigned short)G.dy1
            };
            struct mg_drm_mode_fb_dirty_cmd dcmd = {
                back->fb_id, 0, 0, 1, (uint64_t)(uintptr_t)&clip
            };
            if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_DIRTYFB, &dcmd) < 0 && errno == ENOSYS)
                G.can_dirtyfb = 0;
        }

        struct mg_drm_mode_crtc_page_flip flip = {
            G.crtc_id, back->fb_id, MG_DRM_MODE_PAGE_FLIP_EVENT, 0, 0
        };
        if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_PAGE_FLIP, &flip) == 0) {
            struct pollfd pfd = { .fd = G.drm_fd, .events = POLLIN };
            if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                char evbuf[128];
                ssize_t r = read(G.drm_fd, evbuf, sizeof evbuf);
                (void)r;
            }
            G.back_idx ^= 1;
            G.pdx0 = G.dx0; G.pdy0 = G.dy0; G.pdx1 = G.dx1; G.pdy1 = G.dy1;
            G.dx0 = G.dy0 = G.dx1 = G.dy1 = 0;
            return;
        }
        /* Driver rejected async PAGE_FLIP -> fall back cleanly to single-buffer KMS + DIRTYFB */
        G.can_flip = 0;
        G.info.page_flip = 0;
        G.dx0 = 0; G.dy0 = 0; G.dx1 = G.W; G.dy1 = G.H;
    }

    /* Path B1: Single-buffer DRM (simpledrm firmware GOP/VBE or non-flip KMS) */
    if (G.drm_fd >= 0) {
        struct dumb_buf *front = &G.dbuf[0];
        blit_region_to_target(front->map, front->pitch, 0, G.dx0, G.dy0, G.dx1, G.dy1, sw_cur);
        if (G.can_dirtyfb) {
            struct mg_drm_clip_rect clip = {
                (unsigned short)G.dx0, (unsigned short)G.dy0,
                (unsigned short)G.dx1, (unsigned short)G.dy1
            };
            struct mg_drm_mode_fb_dirty_cmd dcmd = {
                front->fb_id, 0, 0, 1, (uint64_t)(uintptr_t)&clip
            };
            if (drm_ioctl(G.drm_fd, MG_DRM_IOCTL_MODE_DIRTYFB, &dcmd) < 0) G.can_dirtyfb = 0;
        }
        G.dx0 = G.dy0 = G.dx1 = G.dy1 = 0;
        return;
    }

    /* Path B2: Legacy /dev/fb0 (vesafb / efifb / simplefb) */
    if (G.fb_fd >= 0 && G.fb_map) {
        blit_region_to_target(G.fb_map, G.fb_pitch, 1, G.dx0, G.dy0, G.dx1, G.dy1, sw_cur);
        G.dx0 = G.dy0 = G.dx1 = G.dy1 = 0;
    }
}

/* ---------- Event polling ---------- */

static int read_tty_escape(void) {
    unsigned char seq[16];
    struct pollfd p = { .fd = 0, .events = POLLIN };
    if (poll(&p, 1, 25) <= 0) return 27;
    ssize_t n = read(0, seq, 2);
    if (n < 2) return 27;
    if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'P': return GK_F1; case 'Q': return GK_F2; case 'R': return GK_F3; case 'S': return GK_F4;
        case 'H': return GK_HOME; case 'F': return GK_END;
        }
        return 27;
    }
    if (seq[0] != '[') return 27;
    if (seq[1] >= '0' && seq[1] <= '9') {
        int num = seq[1] - '0';
        unsigned char d = 0;
        while (poll(&p, 1, 20) > 0 && read(0, &d, 1) == 1) {
            if (d >= '0' && d <= '9') num = num * 10 + (d - '0');
            else break;
        }
        if (d == '~') {
            switch (num) {
            case 1: case 7: return GK_HOME; case 4: case 8: return GK_END; case 3: return GK_DEL;
            case 5: return GK_PGUP; case 6: return GK_PGDN;
            case 11: return GK_F1; case 12: return GK_F2; case 13: return GK_F3; case 14: return GK_F4;
            case 15: return GK_F5; case 17: return GK_F6; case 18: return GK_F7; case 21: return GK_F10;
            }
        }
        return 27;
    }
    switch (seq[1]) {
    case 'A': return GK_UP; case 'B': return GK_DOWN; case 'C': return GK_RIGHT; case 'D': return GK_LEFT;
    case 'H': return GK_HOME; case 'F': return GK_END; case 'Z': return GK_STAB;
    }
    return 27;
}

int gfx_poll_event(int timeout_ms, int extra_fd, int *extra_ready, struct gfx_mouse *mev) {
    if (extra_ready) *extra_ready = 0;
    if (!G.test_ppm[0]) ev_scan();
    /* A frame held back by the refresh limiter goes out as soon as its slot
     * arrives, even if nothing else wakes the loop. */
    if (present_pending) {
        long long wait_us = pace_us - (mono_us() - last_present_us);
        if (wait_us <= 0) {
            gfx_present();
        } else {
            long long wait_ms = (wait_us + 999) / 1000;
            if (timeout_ms < 0 || timeout_ms > wait_ms) timeout_ms = (int)wait_ms;
        }
    }

    struct pollfd pfds[20];
    int nfds = 0, tty_slot = -1, extra_slot = -1;

    for (int i = 0; i < G.nev; i++) {
        pfds[nfds++] = (struct pollfd){ .fd = G.ev_fds[i], .events = POLLIN };
    }
    /* When evdev devices are open, hardware keyboards also feed the VT tty; only fall back to stdin
     * when no evdev keyboard is present or in offscreen test mode. */
    if (G.nev == 0 || G.test_ppm[0]) {
        tty_slot = nfds;
        pfds[nfds++] = (struct pollfd){ .fd = 0, .events = POLLIN };
    } else {
        /* Drain any echoed VT input buffer silently so returning to shell stays clean */
        unsigned char drain[64];
        while (read(0, drain, sizeof drain) > 0) ;
    }
    if (extra_fd >= 0) {
        extra_slot = nfds;
        pfds[nfds++] = (struct pollfd){ .fd = extra_fd, .events = POLLIN };
    }

    int r = poll(pfds, (nfds_t)nfds, timeout_ms);
    if (r < 0) return errno == EINTR ? GK_TIMEOUT : GK_EOF;
    if (r == 0) return GK_TIMEOUT;

    if (extra_slot >= 0 && (pfds[extra_slot].revents & (POLLIN | POLLHUP))) {
        if (extra_ready) *extra_ready = 1;
    }

    /* Process evdev events */
    int moved = 0;
    for (int i = 0; i < G.nev; i++) {
        if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP))) continue;
        struct input_event ev[32];
        ssize_t n = read(G.ev_fds[i], ev, sizeof ev);
        if (n < (ssize_t)sizeof(struct input_event)) {
            if (n < 0 && errno != EAGAIN && errno != EINTR) { G.last_ev_scan = 0; }
            continue;
        }
        int cnt = (int)(n / (ssize_t)sizeof(struct input_event));
        for (int k = 0; k < cnt; k++) {
            uint16_t t = ev[k].type, c = ev[k].code;
            int32_t v = ev[k].value;
            if (t == EV_REL) {
                if (c == REL_X) { G.cur_x += v; moved = 1; }
                else if (c == REL_Y) { G.cur_y += v; moved = 1; }
                else if (c == REL_WHEEL && v != 0 && mev) {
                    if (G.cur_x < 0) G.cur_x = 0;
                    if (G.cur_x >= G.W) G.cur_x = G.W - 1;
                    if (G.cur_y < 0) G.cur_y = 0;
                    if (G.cur_y >= G.H) G.cur_y = G.H - 1;
                    *mev = (struct gfx_mouse){ G.cur_x, G.cur_y, 0, GM_WHEEL, v > 0 ? -1 : 1, G.buttons };
                    return GK_MOUSE;
                }
            } else if (t == EV_ABS && G.ev_has_abs[i]) {
                if (c == ABS_X) {
                    int span = G.ev_abs_max_x[i] - G.ev_abs_min_x[i];
                    G.cur_x = (int)(((long long)(v - G.ev_abs_min_x[i]) * (G.W - 1)) / span);
                    moved = 1;
                } else if (c == ABS_Y) {
                    int span = G.ev_abs_max_y[i] - G.ev_abs_min_y[i];
                    G.cur_y = (int)(((long long)(v - G.ev_abs_min_y[i]) * (G.H - 1)) / span);
                    moved = 1;
                }
            } else if (t == EV_KEY) {
                if (c == BTN_LEFT || c == BTN_RIGHT || c == BTN_MIDDLE) {
                    int b = c == BTN_LEFT ? 0 : c == BTN_MIDDLE ? 1 : 2;
                    if (v) G.buttons |= (1 << b); else G.buttons &= ~(1 << b);
                    if (G.cur_x < 0) G.cur_x = 0;
                    if (G.cur_x >= G.W) G.cur_x = G.W - 1;
                    if (G.cur_y < 0) G.cur_y = 0;
                    if (G.cur_y >= G.H) G.cur_y = G.H - 1;
                    hw_cursor_move(G.cur_x, G.cur_y);
                    if (mev) *mev = (struct gfx_mouse){ G.cur_x, G.cur_y, b, v ? GM_PRESS : GM_RELEASE, 0, G.buttons };
                    return GK_MOUSE;
                }
                if (c == KEY_LEFTSHIFT || c == KEY_RIGHTSHIFT) { G.mod_shift = v != 0; continue; }
                if (c == KEY_LEFTCTRL || c == KEY_RIGHTCTRL) { G.mod_ctrl = v != 0; continue; }
                if (c == KEY_LEFTALT || c == KEY_RIGHTALT) { G.mod_alt = v != 0; continue; }
                if (c == KEY_CAPSLOCK && v == 1) { G.mod_caps = !G.mod_caps; continue; }
                if (v == 1 || v == 2) {
                    int key = evdev_translate_key(c);
                    if (key != GK_NONE) return key;
                }
            }
        }
    }

    if (moved) {
        if (G.cur_x < 0) G.cur_x = 0;
        if (G.cur_x >= G.W) G.cur_x = G.W - 1;
        if (G.cur_y < 0) G.cur_y = 0;
        if (G.cur_y >= G.H) G.cur_y = G.H - 1;
        hw_cursor_move(G.cur_x, G.cur_y);
        if (mev) *mev = (struct gfx_mouse){ G.cur_x, G.cur_y, 0, GM_MOVE, 0, G.buttons };
        return GK_MOUSE;
    }

    if (tty_slot >= 0 && (pfds[tty_slot].revents & (POLLIN | POLLHUP))) {
        unsigned char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) return GK_EOF;
        if (c == 27) return read_tty_escape();
        return (int)c;
    }

    return GK_TIMEOUT;
}

/* ---------- `display` / `gpu` shell diagnostic command ---------- */

int cmd_display(int argc, char **argv) {
    (void)argc; (void)argv;
    struct gfx_info gi;
    if (G.open) gi = G.info;
    else gfx_probe(&gi);

    if (gi.backend == GFX_NONE) {
        puts("Display: none (serial / headless mode; boot without -nographic for pixel graphics)");
        return 0;
    }
    printf("Adapter:      %s\n", gi.desc);
    printf("Driver path:  %s\n", gi.backend == GFX_GPU_DRIVER
           ? "Hardware GPU Driver (DRM/KMS)"
           : "Basic Display Adapter (Firmware Framebuffer Fallback)");
    printf("Kernel drv:   %s (%s)\n", gi.driver, gi.devnode);
    printf("Active mode:  %dx%d @ %d Hz (%d-bit)\n", gi.width, gi.height, gi.hz, gi.bpp);
    printf("Features:     Page-flip=%s  HW-cursor=%s  Live-modeset=%s\n",
           gi.page_flip ? "yes" : "no (dirty-rect shadow blit)",
           gi.hw_cursor ? "yes" : "no (software cursor)",
           gi.modeset ? "yes" : "no (fixed firmware mode)");
    if (gi.nmodes > 0) {
        printf("Modes:       ");
        for (int i = 0; i < gi.nmodes; i++)
            printf(" %dx%d%s", gi.modes[i].w, gi.modes[i].h, gi.modes[i].preferred ? "*" : "");
        putchar('\n');
    }
    return 0;
}
