/*
 * /programs/canvas - Magnesium OS Pixel Paint & Image Studio.
 * Opens the graphical VaporGlass Canvas paint studio on the desktop, or
 * generates a sample gradient PPM artwork in console mode.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("canvas", argc, argv, &ai)) return 0;

    const char *out = (ai < argc) ? argv[ai] : NULL;
    if (!out) {
        puts("Magnesium Canvas requires the graphical desktop (run 'desktop'),");
        puts("or pass an output filename (`canvas art.ppm`) to generate a test PPM.");
        return 0;
    }
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    int W = 320, H = 200;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            unsigned char px[3] = {
                (unsigned char)((x * 255) / W),
                (unsigned char)((y * 200) / H + 30),
                (unsigned char)(220 - (x * 140) / W)
            };
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("Saved %dx%d PPM image to %s\n", W, H, out);
    return 0;
}
