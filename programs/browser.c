/*
 * /programs/browser - Web Browser (simple HTTP page reader).
 * Usage: browser [http://address]   Opens the VaporGlass Web Browser window.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("browser", argc, argv, &ai)) return 0;
    puts("browser: run inside the desktop to open the Web Browser window.");
    return 0;
}
