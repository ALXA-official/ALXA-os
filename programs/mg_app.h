/*
 * Magnesium OS - helper shared by built-in apps in /programs.
 * If launched inside the Magnesium graphical desktop (unless --cli is passed),
 * asks the desktop compositor to open the app's native VaporGlass window;
 * otherwise runs in standalone console/CLI mode.
 * Original code by alxa, MIT licensed.
 */
#ifndef MG_APP_H
#define MG_APP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define MG_DESKTOP_IPC "/tmp/.mg-desktop.ipc"

static inline int mg_try_desktop_open(const char *app, int argc, char **argv, int *cli_idx) {
    int start = 1;
    if (argc > 1 && (!strcmp(argv[1], "--cli") || !strcmp(argv[1], "-c"))) {
        if (cli_idx) *cli_idx = 2;
        return 0;
    }
    if (cli_idx) *cli_idx = start;
    const char *indesk = getenv("MG_IN_DESKTOP");
    if (!indesk || indesk[0] != '1') return 0;

    int fd = open(MG_DESKTOP_IPC, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd < 0) return 0;
    char line[512];
    int n;
    if (argc > start && argv[start][0]) {
        char absbuf[384];
        if (argv[start][0] != '/' && strstr(argv[start], "://") == NULL) {
            char cwd[256];
            if (getcwd(cwd, sizeof cwd))
                snprintf(absbuf, sizeof absbuf, "%s/%s", cwd, argv[start]);
            else
                snprintf(absbuf, sizeof absbuf, "%s", argv[start]);
            n = snprintf(line, sizeof line, "OPEN %s %s\n", app, absbuf);
        } else {
            n = snprintf(line, sizeof line, "OPEN %s %s\n", app, argv[start]);
        }
    } else {
        n = snprintf(line, sizeof line, "OPEN %s\n", app);
    }
    if (n > 0) {
        ssize_t w = write(fd, line, (size_t)n);
        (void)w;
    }
    close(fd);
    printf("Opened %s on the Magnesium desktop (pass --cli for console mode).\n", app);
    return 1;
}

#endif
