/*
 * extapp.h - desktop side of the Magnesium App Protocol (see mgap_proto.h).
 * Starts standalone apps, receives their windows and frames, forwards input.
 * Original code, MIT licensed.
 */
#ifndef MG_EXTAPP_H
#define MG_EXTAPP_H

#include <stddef.h>

#define EXT_MAX 8    /* standalone apps open at once; window kinds WK_EXT0 .. WK_EXT0+7 */

/* Start an app program. Returns its slot (0..EXT_MAX-1) or -1. The window opens
 * when the app says hello. */
int  ext_launch(const char *path);

/* Read messages from all apps. Returns a bit mask of slots that changed (new
 * window, new frame, or exited). Call once per loop iteration. */
int  ext_pump(void);

int  ext_alive(int s);
int  ext_count(void);

/* 1 once the app has said hello; fills its size and title. */
int  ext_info(int s, int *w, int *h, char *title, size_t tn);

/* Copy the app's latest frame into the window content area (clipped). */
void ext_draw(int s, int x, int y, int cw, int ch);

/* Forward input. x,y are relative to the content area; kind is MGAP_PRESS, MGAP_RELEASE or MGAP_MOVE. */
void ext_mouse(int s, int x, int y, int kind, int btn);
void ext_key(int s, int k);

/* Stop the app and free its slot. Safe to call twice. */
void ext_close(int s);

#endif
