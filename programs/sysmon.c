/*
 * /programs/sysmon - Magnesium OS System & Hardware Monitor.
 * Opens the graphical VaporGlass System & Driver Monitor on the desktop, or
 * prints live CPU/RAM/process summary in console mode.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("sysmon", argc, argv, &ai)) return 0;

    struct utsname u;
    struct sysinfo si;
    if (uname(&u) == 0) printf("Kernel:  %s %s (%s)\n", u.sysname, u.release, u.machine);
    if (sysinfo(&si) == 0) {
        unsigned long long unit = si.mem_unit ? si.mem_unit : 1;
        unsigned long long tot = (si.totalram * unit) >> 20;
        unsigned long long fre = (si.freeram * unit) >> 20;
        printf("Memory:  %llu MB used / %llu MB total (%llu MB free)\n", tot - fre, tot, fre);
        printf("Uptime:  %ldd %02ld:%02ld:%02ld   Procs: %u\n\n",
               si.uptime / 86400, (si.uptime / 3600) % 24, (si.uptime / 60) % 60, si.uptime % 60,
               (unsigned)si.procs);
    }
    puts("  PID  STATE  COMMAND");
    puts("------------------------------");
    DIR *dp = opendir("/proc");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) != NULL) {
            if (!isdigit((unsigned char)e->d_name[0])) continue;
            char p[300], line[160], name[64] = "?", st = '?';
            snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
            FILE *f = fopen(p, "r");
            if (!f) continue;
            if (fgets(line, sizeof line, f)) {
                char *lp = strchr(line, '('), *rp = strrchr(line, ')');
                if (lp && rp && rp > lp) {
                    size_t l = (size_t)(rp - lp - 1);
                    if (l >= sizeof name) l = sizeof name - 1;
                    memcpy(name, lp + 1, l); name[l] = 0;
                    if (rp[1] == ' ' && rp[2]) st = rp[2];
                }
            }
            fclose(f);
            printf("%5s    %c    %s\n", e->d_name, st, name);
        }
        closedir(dp);
    }
    return 0;
}
