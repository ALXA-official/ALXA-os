/*
 * Magnesium OS - image decoding and resampling (see imgcodec.h).
 * Original code by alxa, MIT licensed. Uses stb_image (public domain / MIT).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* stb_image is compiled here only, with its warnings silenced. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_FAILURE_LOG
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_HDR
#define STBI_ONLY_PIC
#define STBI_ONLY_PNM
#include "third_party/stb_image.h"

#include "imgcodec.h"

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

const char *img_format_name(const char *path) {
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return "image";
    if (!strcasecmp(dot, ".png")) return "PNG";
    if (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".jpe")) return "JPEG";
    if (!strcasecmp(dot, ".bmp") || !strcasecmp(dot, ".dib")) return "BMP";
    if (!strcasecmp(dot, ".gif")) return "GIF";
    if (!strcasecmp(dot, ".tga")) return "TGA";
    if (!strcasecmp(dot, ".psd")) return "PSD";
    if (!strcasecmp(dot, ".hdr")) return "HDR";
    if (!strcasecmp(dot, ".pic")) return "PIC";
    if (!strcasecmp(dot, ".pgm") || !strcasecmp(dot, ".ppm") || !strcasecmp(dot, ".pnm")) return "PNM";
    return "image";
}

int img_is_supported(const char *path) {
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot) return 0;
    static const char *const exts[] = {
        ".png", ".jpg", ".jpeg", ".jpe", ".bmp", ".dib", ".gif", ".tga",
        ".psd", ".hdr", ".pic", ".pgm", ".ppm", ".pnm"
    };
    for (size_t i = 0; i < sizeof exts / sizeof exts[0]; i++)
        if (!strcasecmp(dot, exts[i])) return 1;
    return 0;
}

void img_free(mg_image *im) {
    if (!im) return;
    free(im->px);
    im->px = NULL;
    im->w = im->h = 0;
}

/* Turn stb's RGBA bytes into 0xAARRGGBB and take ownership of the stb buffer. */
static int img_convert_rgba(stbi_uc *pix, int w, int h, mg_image *out, char *err, size_t errlen) {
    if (w <= 0 || h <= 0 || (unsigned long long)w * (unsigned long long)h > IMG_MAX_PIXELS) {
        stbi_image_free(pix);
        set_err(err, errlen, "image is too large to display");
        return -1;
    }
    size_t n = (size_t)w * (size_t)h;
    uint32_t *dst = malloc(n * sizeof(uint32_t));
    if (!dst) { stbi_image_free(pix); set_err(err, errlen, "out of memory"); return -1; }
    for (size_t i = 0; i < n; i++) {
        const stbi_uc *p = pix + i * 4;
        dst[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
                 ((uint32_t)p[1] << 8) | (uint32_t)p[2];
    }
    stbi_image_free(pix);
    out->w = w;
    out->h = h;
    out->px = dst;
    return 0;
}

int img_load_file(const char *path, mg_image *out, char *err, size_t errlen) {
    out->w = out->h = 0;
    out->px = NULL;
    if (!path || !path[0]) { set_err(err, errlen, "no file given"); return -1; }

    struct stat st;
    if (stat(path, &st) != 0) { set_err(err, errlen, "file not found"); return -1; }
    if (!S_ISREG(st.st_mode)) { set_err(err, errlen, "not a regular file"); return -1; }
    if ((unsigned long long)st.st_size > IMG_MAX_FILE_BYTES) {
        set_err(err, errlen, "file is larger than 64 MB");
        return -1;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc *pix = stbi_load(path, &w, &h, &comp, 4);   /* always RGBA */
    if (!pix) {
        const char *why = stbi_failure_reason();
        char buf[160];
        snprintf(buf, sizeof buf, "cannot decode %s file (%s)",
                 img_format_name(path), why ? why : "unknown error");
        set_err(err, errlen, buf);
        return -1;
    }
    if (w <= 0 || h <= 0 || (unsigned long long)w * (unsigned long long)h > IMG_MAX_PIXELS) {
        stbi_image_free(pix);
        set_err(err, errlen, "image is too large to display");
        return -1;
    }

    return img_convert_rgba(pix, w, h, out, err, errlen);
}

int img_load_mem(const unsigned char *buf, size_t len, mg_image *out, char *err, size_t errlen) {
    out->w = out->h = 0;
    out->px = NULL;
    if (!buf || len == 0) { set_err(err, errlen, "no picture data"); return -1; }
    if (len > IMG_MAX_FILE_BYTES) { set_err(err, errlen, "picture data is larger than 64 MB"); return -1; }
    int w = 0, h = 0, comp = 0;
    stbi_uc *pix = stbi_load_from_memory(buf, (int)len, &w, &h, &comp, 4);
    if (!pix) { set_err(err, errlen, "cannot decode embedded picture"); return -1; }
    return img_convert_rgba(pix, w, h, out, err, errlen);
}

/* Box-filter resample: each destination pixel averages the source area it covers.
 * Works for shrinking (smooth) and enlarging (each source pixel covers several). */
int img_resample(const mg_image *src, int dw, int dh, mg_image *out) {
    out->w = out->h = 0;
    out->px = NULL;
    if (!src || !src->px || src->w <= 0 || src->h <= 0 || dw <= 0 || dh <= 0) return -1;
    if ((unsigned long long)dw * (unsigned long long)dh > IMG_MAX_PIXELS) return -1;

    uint32_t *dst = malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
    if (!dst) return -1;

    for (int y = 0; y < dh; y++) {
        long y0 = (long)y * src->h / dh;
        long y1 = (long)(y + 1) * src->h / dh;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > src->h) y1 = src->h;
        for (int x = 0; x < dw; x++) {
            long x0 = (long)x * src->w / dw;
            long x1 = (long)(x + 1) * src->w / dw;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > src->w) x1 = src->w;
            unsigned long long sa = 0, sr = 0, sg = 0, sb = 0;
            unsigned long long cnt = 0;
            for (long yy = y0; yy < y1; yy++) {
                const uint32_t *row = src->px + yy * src->w;
                for (long xx = x0; xx < x1; xx++) {
                    uint32_t p = row[xx];
                    /* Premultiply so transparent pixels don't darken edges. */
                    unsigned a = p >> 24;
                    sa += a;
                    sr += (unsigned long long)((p >> 16) & 255) * a;
                    sg += (unsigned long long)((p >> 8) & 255) * a;
                    sb += (unsigned long long)(p & 255) * a;
                    cnt++;
                }
            }
            uint32_t r = 0, g = 0, b = 0, a = 0;
            if (cnt) {
                a = (uint32_t)(sa / cnt);
                if (sa) {
                    r = (uint32_t)(sr / sa);
                    g = (uint32_t)(sg / sa);
                    b = (uint32_t)(sb / sa);
                }
            }
            dst[(size_t)y * dw + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    out->w = dw;
    out->h = dh;
    out->px = dst;
    return 0;
}
