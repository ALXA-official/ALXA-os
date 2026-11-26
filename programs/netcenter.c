/*
 * /programs/netcenter - Magnesium OS Network Center & Web Reader.
 * Opens the graphical VaporGlass Network Center & HTTP Reader on the desktop,
 * or prints network interface status in console mode.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("netcenter", argc, argv, &ai)) return 0;

    puts("Magnesium Network Center (console mode)");
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) { puts("Networking not active in kernel."); return 1; }
    char line[256];
    while (fgets(line, sizeof line, f)) fputs(line, stdout);
    fclose(f);
    puts("\nTip: use 'ip', 'dhcp', 'ping HOST', 'dns HOST', or 'fetch -t http://example.com'.");
    return 0;
}
