/*
 * /programs/mines - Magnesium Mines (Mg Minesweeper).
 * Opens the graphical VaporGlass Mg Mines puzzle game on the desktop, or plays
 * a compact coordinate version in console mode.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mg_app.h"

int main(int argc, char **argv) {
    int ai = 1;
    if (mg_try_desktop_open("mines", argc, argv, &ai)) return 0;

    puts("Mg Mines (console mode) - run 'desktop' to play with mouse & VaporGlass tiles!");
    int W = 8, H = 8, M = 10;
    unsigned char mine[8][8] = {{0}}, open_c[8][8] = {{0}};
    srand((unsigned)time(NULL));
    for (int placed = 0; placed < M; ) {
        int r = rand() % H, c = rand() % W;
        if (!mine[r][c]) { mine[r][c] = 1; placed++; }
    }
    int safe_left = W * H - M;
    while (safe_left > 0) {
        puts("\n    0 1 2 3 4 5 6 7");
        for (int r = 0; r < H; r++) {
            printf(" %d  ", r);
            for (int c = 0; c < W; c++) {
                if (!open_c[r][c]) { fputs(". ", stdout); continue; }
                int cnt = 0;
                for (int dr = -1; dr <= 1; dr++)
                    for (int dc = -1; dc <= 1; dc++)
                        if (r + dr >= 0 && r + dr < H && c + dc >= 0 && c + dc < W && mine[r + dr][c + dc]) cnt++;
                printf("%c ", cnt ? ('0' + cnt) : ' ');
            }
            putchar('\n');
        }
        printf("Enter 'row col' (or q to quit): ");
        fflush(stdout);
        char buf[64];
        if (!fgets(buf, sizeof buf, stdin) || buf[0] == 'q') break;
        int r = -1, c = -1;
        if (sscanf(buf, "%d %d", &r, &c) != 2 || r < 0 || r >= H || c < 0 || c >= W) continue;
        if (mine[r][c]) { puts("BOOM! You hit a magnesium mine!"); return 0; }
        if (!open_c[r][c]) { open_c[r][c] = 1; safe_left--; }
    }
    if (safe_left == 0) puts("Congratulations! Board cleared!");
    return 0;
}
