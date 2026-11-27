/*
 * Magnesium OS - image decoding and resampling.
 * Decoding uses stb_image (third_party/): PNG, JPEG, BMP, GIF, TGA, PSD,
 * PIC, PNM and HDR (HDR needs libm, which the build links).
 * Pixels are 0xAARRGGBB, row-major, top row first.
 * Original code by alxa, MIT licensed.
 */
#ifndef MG_IMGCODEC_H
#define MG_IMGCODEC_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int w, h;
    uint32_t *px;            /* 0xAARRGGBB, w*h entries; NULL when empty */
} mg_image;

/* Limits protect the 256 MB default RAM: files above 64 MB or images above
 * 40 megapixels are refused with a message in err. */
#define IMG_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define IMG_MAX_PIXELS     (40u * 1000u * 1000u)

/* Load any supported picture file. Returns 0 on success, -1 on error (err filled). */
int  img_load_file(const char *path, mg_image *out, char *err, size_t errlen);
void img_free(mg_image *im);

/* Decode a picture held in memory (PNG, BMP, ... as above). */
int  img_load_mem(const unsigned char *buf, size_t len, mg_image *out, char *err, size_t errlen);

/* Resample to dw x dh (box filter, good for both shrinking and enlarging). */
int  img_resample(const mg_image *src, int dw, int dh, mg_image *out);

/* Human-readable format name from the file extension, e.g. "PNG", "JPEG". */
const char *img_format_name(const char *path);

/* True when the file name has an extension this build can decode. */
int  img_is_supported(const char *path);

#endif
