/*
 * /programs/files - Magnesium OS File & Program Manager.
 * Opens the graphical VaporGlass File Manager on the desktop, or lists/explores
 * folders in console mode.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("files", argc, argv, &ai)) return 0;

    const char *path = (ai < argc) ? argv[ai] : "/programs";
    DIR *dp = opendir(path);
    if (!dp) { perror(path); return 1; }

    printf("Magnesium Files - %s\n", path);
    puts("TYPE      SIZE  NAME");
    puts("--------------------------------------------");
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[512];
        snprintf(full, sizeof full, "%s/%s", !strcmp(path, "/") ? "" : path, e->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        const char *t = S_ISDIR(st.st_mode) ? "[DIR]" :
                        (access(full, X_OK) == 0 ? "[APP]" : "[FILE]");
        printf("%-6s %7lld  %s\n", t, (long long)st.st_size, e->d_name);
    }
    closedir(dp);
    return 0;
}
