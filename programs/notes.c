/*
 * /programs/notes - Magnesium OS Notes (Text & Config Editor).
 * Opens the graphical VaporGlass Notes editor on the desktop, or views/appends
 * notes in console mode.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("notes", argc, argv, &ai)) return 0;

    if (ai >= argc) {
        puts("Magnesium Notes (console mode)");
        puts("usage: notes FILE           view a text file");
        puts("       notes FILE TEXT...   append a note line to FILE");
        puts("       (or use 'edit FILE' for the full-screen terminal editor)");
        return 0;
    }
    const char *file = argv[ai];
    if (ai + 1 < argc) {
        FILE *f = fopen(file, "a");
        if (!f) { perror(file); return 1; }
        for (int i = ai + 1; i < argc; i++)
            fprintf(f, "%s%s", i > ai + 1 ? " " : "", argv[i]);
        fputc('\n', f);
        fclose(f);
        printf("Saved note to %s\n", file);
        return 0;
    }
    FILE *f = fopen(file, "r");
    if (!f) { perror(file); return 1; }
    char buf[256];
    int ln = 1;
    while (fgets(buf, sizeof buf, f)) printf("%4d | %s", ln++, buf);
    fclose(f);
    return 0;
}
