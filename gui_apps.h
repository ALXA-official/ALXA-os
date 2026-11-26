/*
 * Magnesium OS - built-in VaporGlass desktop applications (backed by /programs):
 *   Files, Notes, Calculator, Canvas, System & Driver Monitor,
 *   Network Center & Web Reader, and Mg Mines.
 * Original code by alxa, MIT licensed.
 */
#ifndef MG_GUI_APPS_H
#define MG_GUI_APPS_H

#include <stdint.h>
#include "gfx.h"

/* Window kinds shared between gui.c and gui_apps.c */
enum {
    WK_SETTINGS  = 1,
    WK_REGISTRY  = 2,
    WK_TERMINAL  = 3,
    WK_FILES     = 4,
    WK_NOTES     = 5,
    WK_CALC      = 6,
    WK_CANVAS    = 7,
    WK_SYSMON    = 8,
    WK_NETCENTER = 9,
    WK_MINES     = 10,
    WK_IMGVIEW   = 11,   /* Picture Viewer (media_apps.c) */
    WK_VIDEO     = 12,   /* Video Player (media_apps.c) */
    WK_BROWSER   = 13    /* Web Browser (media_apps.c) */
};

/* Shared desktop theme snapshot passed from gui.c to gui_apps.c */
struct ui_theme {
    uint32_t win, win_alt, winfg, titleoff, border, muted, sel, input_bg, ok, bad;
    uint32_t acc, accfg;
    int radius;
};

/* Callbacks exported by gui.c for gui_apps.c */
void ui_get_theme(struct ui_theme *out);
void ui_set_clip(int x, int y, int w, int h);
void ui_hot_add(int x, int y, int w, int h, int act, int a, int b);
int  ui_hover_in(int x, int y, int w, int h);
void ui_toast(const char *fmt, ...);
void ui_open_app(int kind, const char *arg);
void ui_run_terminal_cmd(const char *cmd);
void ui_reload_manual_cfg(void);

/* Action IDs reserved for gui_apps.c (start at 200 to avoid colliding with gui.c) */
enum {
    ACT_APP_BASE   = 200,
    ACT_FILES_NAV  = 201,
    ACT_FILES_TOOL = 202,
    ACT_FILES_ITEM = 203,
    ACT_NOTES_TOOL = 210,
    ACT_NOTES_AREA = 211,
    ACT_CALC_BTN   = 220,
    ACT_CANV_TOOL  = 230,
    ACT_CANV_COL   = 231,
    ACT_CANV_SIZE  = 232,
    ACT_CANV_PAD   = 233,
    ACT_SMON_TAB   = 240,
    ACT_SMON_ROW   = 241,
    ACT_SMON_BTN   = 242,
    ACT_NET_BTN    = 250,
    ACT_NET_URL    = 251,
    ACT_MINE_CELL  = 260,
    ACT_MINE_BTN   = 261,
    ACT_IMG_BTN    = 270,
    ACT_VID_BTN    = 271,
    ACT_WEB_BTN    = 272,
    ACT_WEB_ADDR   = 273
};

void apps_init_window(int kind, const char *arg, char *title_out, int *def_w, int *def_h);
void apps_draw_window(int kind, int x, int y, int cw, int ch, int active);
void apps_on_hotspot(int act, int a, int b, int mx, int my, int mouse_btn);
int  apps_on_mouse_drag(int kind, int x, int y, int cw, int ch, const struct gfx_mouse *mev);
void apps_on_wheel(int kind, int delta);
void apps_on_key(int kind, int k);
int  apps_needs_tick(int kind);

/* Picture Viewer, Video Player and Web Browser (media_apps.c) */
void media_init_window(int kind, const char *arg, char *title_out, int *def_w, int *def_h);
void media_draw(int kind, const struct ui_theme *t, int x, int y, int cw, int ch);
void media_hotspot(int act, int a, int b);
void media_key(int kind, int k);
void media_wheel(int kind, int delta);
int  media_needs_tick(int kind);
void media_release(int kind);

/* Map app command/name (e.g. "files", "/programs/calc") to WK_* or 0 */
int  apps_kind_from_name(const char *name, const char **sym_out, const char **label_out);

#endif
