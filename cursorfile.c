/*
 * Magnesium OS - pointer cursor files: .cur, .ani and PNG.
 * Original code, MIT licensed. See cursorfile.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cursorfile.h"

#define CUR_MAX_FILE (4u * 1024u * 1024u)
#define CUR_MAX_DIM  64      /* pointer images larger than this are scaled down by the desktop */

static void set_err(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static uint32_t rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void cur_anim_free(struct cur_anim *a) {
    for (int i = 0; i < a->nicons; i++) img_free(&a->icon[i]);
    memset(a, 0, sizeof *a);
}

/* Decode one image entry (PNG or DIB with AND mask) found at d[off .. off+sz). */
static int decode_entry(const unsigned char *d, size_t len, size_t off, size_t sz,
                        mg_image *out, char *err, size_t errlen) {
    out->w = out->h = 0;
    out->px = NULL;
    if (off > len || sz > len - off || sz < 16) { set_err(err, errlen, "cursor entry is outside the file"); return -1; }
    const unsigned char *e = d + off;
    if (sz >= 8 && e[0] == 0x89 && e[1] == 'P' && e[2] == 'N' && e[3] == 'G')
        return img_load_mem(e, sz, out, err, errlen);

    uint32_t bisz = rd32(e);
    if (bisz < 40 || bisz > sz) { set_err(err, errlen, "unsupported cursor image header"); return -1; }
    int32_t w = (int32_t)rd32(e + 4);
    int32_t hh = (int32_t)rd32(e + 8);
    uint32_t bpp = rd16(e + 14);
    uint32_t comp = rd32(e + 16);
    uint32_t clr = rd32(e + 32);
    int h = hh / 2;                           /* DIB height covers XOR + AND masks */
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || (comp != 0 && comp != 3)) {
        set_err(err, errlen, "unsupported cursor image size or compression"); return -1;
    }
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) {
        set_err(err, errlen, "unsupported cursor bit depth"); return -1;
    }
    size_t ncol = 0;
    if (bpp <= 8) ncol = clr ? clr : ((size_t)1 << bpp);
    if (ncol > 256) { set_err(err, errlen, "bad cursor palette"); return -1; }
    size_t pal = off + bisz;
    size_t xor_start = pal + ncol * 4;
    size_t xor_stride = (((size_t)w * bpp + 31) / 32) * 4;
    size_t and_stride = (((size_t)w + 31) / 32) * 4;
    size_t and_start = xor_start + xor_stride * (size_t)h;
    size_t end = off + sz;
    if (and_start + and_stride * (size_t)h > end || (xor_start > len)) {
        set_err(err, errlen, "cursor image data is truncated"); return -1;
    }

    size_t n = (size_t)w * (size_t)h;
    uint32_t *px = malloc(n * sizeof *px);
    if (!px) { set_err(err, errlen, "out of memory"); return -1; }
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {                     /* DIB rows are stored bottom-up */
        int sy = h - 1 - y;
        const unsigned char *row = d + xor_start + xor_stride * (size_t)sy;
        const unsigned char *arow = d + and_start + and_stride * (size_t)sy;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            uint32_t a = 0xFF;
            int and_bit = (arow[x >> 3] >> (7 - (x & 7))) & 1;
            if (bpp == 32) {
                const unsigned char *p = row + x * 4;
                c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                a = p[3];
                if (a) any_alpha = 1;
            } else if (bpp == 24) {
                const unsigned char *p = row + x * 3;
                c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                a = and_bit ? 0 : 0xFF;
            } else {
                uint32_t idx;
                if (bpp == 8) idx = row[x];
                else if (bpp == 4) idx = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0x0F;
                else idx = (row[x >> 3] >> (7 - (x & 7))) & 1;
                if (idx >= ncol) idx = 0;
                const unsigned char *p = d + pal + idx * 4;
                c = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
                a = and_bit ? 0 : 0xFF;
            }
            px[(size_t)y * w + x] = (a << 24) | c;
        }
    }
    if (bpp == 32 && !any_alpha) {                    /* old 32-bit cursors: alpha lives in the AND mask */
        for (int y = 0; y < h; y++) {
            int sy = h - 1 - y;
            const unsigned char *arow = d + and_start + and_stride * (size_t)sy;
            for (int x = 0; x < w; x++) {
                int and_bit = (arow[x >> 3] >> (7 - (x & 7))) & 1;
                px[(size_t)y * w + x] = (and_bit ? 0 : 0xFF000000u) | (px[(size_t)y * w + x] & 0xFFFFFF);
            }
        }
    }
    out->w = w;
    out->h = h;
    out->px = px;
    return 0;
}

/* Read a .cur/.ico file image (ICONDIR) that lives at d[off .. off+sz). Picks the
 * largest entry that fits CUR_MAX_DIM. hx/hy come from the entry (cursor hot spot). */
static int decode_cur_blob(const unsigned char *d, size_t len, size_t off, size_t sz,
                           mg_image *out, int *hx, int *hy, char *err, size_t errlen) {
    if (off > len || sz > len - off || sz < 6) { set_err(err, errlen, "cursor data is truncated"); return -1; }
    const unsigned char *b = d + off;
    if (rd16(b) != 0 || (rd16(b + 2) != 1 && rd16(b + 2) != 2)) {
        set_err(err, errlen, "not a cursor or icon file"); return -1;
    }
    uint32_t count = rd16(b + 4);
    if (count == 0 || 6 + count * 16 > sz) { set_err(err, errlen, "bad cursor entry table"); return -1; }
    int best = -1, best_w = -1;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *e = b + 6 + i * 16;
        int w = e[0] ? e[0] : 256;
        int fits = w <= CUR_MAX_DIM;
        int bw = best < 0 ? -1 : best_w;
        int better = best < 0 || (fits && (bw > CUR_MAX_DIM || w > bw)) || (!fits && bw > CUR_MAX_DIM && w < bw);
        if (better) { best = (int)i; best_w = w; }
    }
    const unsigned char *e = b + 6 + (size_t)best * 16;
    size_t esz = rd32(e + 8), eoff = rd32(e + 12);
    if (rd16(b + 2) == 2) { *hx = rd16(e + 4); *hy = rd16(e + 6); }
    else { *hx = 0; *hy = 0; }
    if (eoff > sz || esz > sz - eoff) { set_err(err, errlen, "cursor image is outside the file"); return -1; }
    return decode_entry(d, len, off + eoff, esz, out, err, errlen);
}

static int load_ani(const unsigned char *d, size_t len, struct cur_anim *a, char *err, size_t errlen) {
    size_t p = 12;
    uint32_t nframes = 0, nsteps = 0, rate_def = 10;
    const unsigned char *seq = NULL, *rate = NULL;
    size_t seq_n = 0, rate_n = 0;
    int have_anih = 0;
    while (p + 8 <= len) {
        uint32_t csz = rd32(d + p + 4);
        const unsigned char *c = d + p + 8;
        size_t avail = len - (p + 8);
        if (csz > avail) break;
        if (!memcmp(d + p, "anih", 4) && csz >= 36) {
            nframes = rd32(c + 4);
            nsteps = rd32(c + 8);
            rate_def = rd32(c + 28);
            have_anih = 1;
        } else if (!memcmp(d + p, "seq ", 4)) {
            seq = c; seq_n = csz / 4;
        } else if (!memcmp(d + p, "rate", 4)) {
            rate = c; rate_n = csz / 4;
        } else if (!memcmp(d + p, "LIST", 4) && csz >= 4 && !memcmp(c, "fram", 4)) {
            size_t q = 4;
            while (q + 8 <= csz) {
                uint32_t isz = rd32(c + q + 4);
                if (isz > csz - q - 8) break;
                if (!memcmp(c + q, "icon", 4)) {
                    if (a->nicons >= CUR_MAX_ICONS) break;
                    int hx = 0, hy = 0;
                    mg_image im = { 0, 0, NULL };
                    if (decode_cur_blob(d, len, (size_t)(c + q + 8 - d), isz, &im, &hx, &hy, err, errlen) != 0)
                        return -1;
                    a->icon[a->nicons] = im;
                    a->hx[a->nicons] = hx;
                    a->hy[a->nicons] = hy;
                    a->nicons++;
                }
                q += 8 + isz + (isz & 1);
            }
        }
        p += 8 + csz + (csz & 1);
    }
    if (!have_anih || a->nicons == 0) { set_err(err, errlen, "animated cursor has no frames"); return -1; }
    if (nsteps == 0) nsteps = nframes;
    if (nsteps > CUR_MAX_STEPS) nsteps = CUR_MAX_STEPS;
    if (nsteps == 0) { set_err(err, errlen, "animated cursor has no steps"); return -1; }
    a->nsteps = (int)nsteps;
    for (uint32_t s = 0; s < nsteps; s++) {
        uint32_t icon = (seq && s < seq_n) ? rd32(seq + s * 4) : s;
        if (icon >= (uint32_t)a->nicons) icon = icon % (uint32_t)a->nicons;
        uint32_t jiffies = (rate && s < rate_n) ? rd32(rate + s * 4) : rate_def;
        int ms = (int)((unsigned long)jiffies * 1000u / 60u);   /* a jiffy is 1/60 s */
        if (ms < 10) ms = 10;
        a->step_icon[s] = (int)icon;
        a->step_ms[s] = ms;
        a->total_ms += ms;
    }
    return 0;
}

int cur_anim_icon_at(const struct cur_anim *a, long now_ms) {
    if (!a || a->nsteps <= 0) return 0;
    if (a->nsteps == 1 || a->total_ms <= 0) return a->step_icon[0];
    long t = now_ms % a->total_ms;
    for (int s = 0; s < a->nsteps; s++) {
        if (t < a->step_ms[s]) return a->step_icon[s];
        t -= a->step_ms[s];
    }
    return a->step_icon[a->nsteps - 1];
}

int cur_load(const char *path, struct cur_anim *out, char *err, size_t errlen) {
    memset(out, 0, sizeof *out);
    if (!path || !path[0]) { set_err(err, errlen, "no file given"); return -1; }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { set_err(err, errlen, "file not found"); return -1; }
    if ((unsigned long long)st.st_size > CUR_MAX_FILE) { set_err(err, errlen, "cursor file is larger than 4 MB"); return -1; }
    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errlen, "cannot open cursor file"); return -1; }
    size_t len = (size_t)st.st_size;
    unsigned char *d = malloc(len ? len : 1);
    if (!d || fread(d, 1, len, f) != len) { fclose(f); free(d); set_err(err, errlen, "cannot read cursor file"); return -1; }
    fclose(f);

    int rc;
    size_t ext = 0;
    for (size_t i = 0; path[i]; i++) if (path[i] == '.') ext = i;
    int is_ani = len >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "ACON", 4);
    if (is_ani) {
        rc = load_ani(d, len, out, err, errlen);
    } else if (len >= 6 && rd16(d) == 0 && (rd16(d + 2) == 1 || rd16(d + 2) == 2)) {
        int hx = 0, hy = 0;
        mg_image im = { 0, 0, NULL };
        rc = decode_cur_blob(d, len, 0, len, &im, &hx, &hy, err, errlen);
        if (rc == 0) {
            out->icon[0] = im; out->hx[0] = hx; out->hy[0] = hy; out->nicons = 1;
            out->nsteps = 1; out->step_icon[0] = 0; out->step_ms[0] = 0;
        }
    } else {
        /* Anything else is treated as a picture (PNG, BMP, JPG, ...). */
        mg_image im = { 0, 0, NULL };
        rc = img_load_mem(d, len, &im, err, errlen);
        if (rc == 0) {
            out->icon[0] = im; out->hx[0] = 0; out->hy[0] = 0; out->nicons = 1;
            out->nsteps = 1; out->step_icon[0] = 0; out->step_ms[0] = 0;
        }
    }
    (void)ext;
    free(d);
    if (rc != 0) { cur_anim_free(out); return -1; }
    /* Oversized images are rejected; the desktop expects <= 64 px. */
    for (int i = 0; i < out->nicons; i++) {
        if (out->icon[i].w > CUR_MAX_DIM || out->icon[i].h > CUR_MAX_DIM) {
            cur_anim_free(out);
            set_err(err, errlen, "cursor image is larger than 64 x 64 px");
            return -1;
        }
    }
    if (out->nsteps == 0) { cur_anim_free(out); set_err(err, errlen, "empty cursor"); return -1; }
    return 0;
}
