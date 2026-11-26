/*
 * /programs/imgview - Picture Viewer.
 * Usage: imgview <file>   (PNG, JPEG, BMP, GIF, TGA, PSD, PIC, PNM, HDR)
 * Opens the VaporGlass Picture Viewer window on the desktop.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("imgview", argc, argv, &ai)) return 0;
    if (argc < 2 || !argv[1][0]) {
        puts("usage: imgview <file>  (run inside the desktop to open a window)");
        return 1;
    }
    printf("imgview: %s - run inside the desktop to view the picture.\n", argv[1]);
    return 0;
}
