/*
 * /programs/videoview - Video Player (MPEG-1 .mpg/.mpeg, video only for now).
 * Usage: videoview <file>   Opens the VaporGlass Video Player window.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("videoview", argc, argv, &ai)) return 0;
    if (argc < 2 || !argv[1][0]) {
        puts("usage: videoview <file.mpg>  (run inside the desktop to open a window)");
        return 1;
    }
    printf("videoview: %s - run inside the desktop to play the video.\n", argv[1]);
    return 0;
}
