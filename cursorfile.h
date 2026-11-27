/*
 * Magnesium OS - pointer cursor files: .cur, .ani and PNG.
 * .cur and .ani follow the public Windows cursor/animation file layout
 * (ICONDIR entries, RIFF "ACON" chunks). Original code, MIT licensed.
 * Pictures are decoded by imgcodec.c, so PNG payloads work too.
 */
#ifndef MG_CURSORFILE_H
#define MG_CURSORFILE_H
#include <stddef.h>
#include "imgcodec.h"

#define CUR_MAX_ICONS 64     /* distinct frames in one animation */
#define CUR_MAX_STEPS 256    /* playback steps (a frame can repeat) */

struct cur_anim {
    int nicons;
    mg_image icon[CUR_MAX_ICONS];
    int hx[CUR_MAX_ICONS], hy[CUR_MAX_ICONS];
    int nsteps;
    int step_icon[CUR_MAX_STEPS];
    int step_ms[CUR_MAX_STEPS];       /* how long each step shows, in ms */
    int total_ms;                     /* one full cycle; 0 when static */
};

/* Load .cur, .ani or a picture file as a cursor. Returns 0 or -1 (err filled). */
int  cur_load(const char *path, struct cur_anim *out, char *err, size_t errlen);
void cur_anim_free(struct cur_anim *a);

/* Which icon to show at time now_ms (monotonic). Returns an icon index. */
int  cur_anim_icon_at(const struct cur_anim *a, long now_ms);

#endif
