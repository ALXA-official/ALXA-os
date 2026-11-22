/*
 * Magnesium OS - interactive line editor for the shell: cursor keys, history
 * (Up/Down), Tab completion, Ctrl+U/K/W/L/C/D.
 * Original code by alxa, MIT licensed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include "line.h"

#define HIST_MAX 100
#define MAXL 512

enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL };

static char *hist[HIST_MAX];
static int nhist;
static line_completer completer;

void line_set_completer(line_completer fn) { completer = fn; }
int line_history_count(void) { return nhist; }
const char *line_history_get(int i) { return (i >= 0 && i < nhist) ? hist[i] : NULL; }

void line_history_add(const char *l) {
    if (!*l) return;
    if (nhist && !strcmp(hist[nhist - 1], l)) return;
    if (nhist == HIST_MAX) {
        free(hist[0]);
        memmove(hist, hist + 1, (HIST_MAX - 1) * sizeof *hist);
        nhist--;
    }
    hist[nhist++] = strdup(l);
}

static void out(const char *s, size_t n) {
    while (n > 0) {
        ssize_t w = write(1, s, n);
        if (w <= 0) { if (errno == EINTR) continue; return; }
        s += w; n -= (size_t)w;
    }
}
static void outs(const char *s) { out(s, strlen(s)); }

static int read_byte(int timeout_ms) {
    struct pollfd p = { .fd = 0, .events = POLLIN };
    for (;;) {
        int r = poll(&p, 1, timeout_ms);
        if (r < 0) { if (errno == EINTR) continue; return -2; }
        if (r == 0) return -1;
        unsigned char c;
        ssize_t n = read(0, &c, 1);
        if (n == 1) return c;
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return -2;
    }
}

static int read_key(void) {
    int c = read_byte(-1);
    if (c < 0) return -2;
    if (c != 27) return c;
    int a = read_byte(50);
    if (a < 0) return 27;
    int b = read_byte(50);
    if (b < 0) return 27;
    if (a == '[') {
        if (b >= '0' && b <= '9') {
            int d = read_byte(50);
            if (d == '~') {
                if (b == '1' || b == '7') return K_HOME;
                if (b == '4' || b == '8') return K_END;
                if (b == '3') return K_DEL;
            }
            return 27;
        }
        switch (b) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        }
    } else if (a == 'O') {
        if (b == 'H') return K_HOME;
        if (b == 'F') return K_END;
    }
    return 27;
}

static void redraw(const char *prompt, const char *buf, size_t len, size_t pos) {
    outs("\r\033[K");
    outs(prompt);
    out(buf, len);
    if (len > pos) {
        char t[24];
        int n = snprintf(t, sizeof t, "\033[%zuD", len - pos);
        out(t, (size_t)n);
    }
}

static int insert_str(char *buf, size_t *len, size_t *pos, size_t size, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > size) return 0;
    memmove(buf + *pos + n, buf + *pos, *len - *pos + 1);
    memcpy(buf + *pos, s, n);
    *len += n; *pos += n;
    return 1;
}

static void do_tab(char *buf, size_t *len, size_t *pos, size_t size, const char *prompt) {
    if (!completer) return;
    size_t ws = *pos;
    while (ws > 0 && buf[ws - 1] != ' ' && buf[ws - 1] != '\t') ws--;
    int first = 1;
    size_t k = ws;
    while (k > 0 && (buf[k - 1] == ' ' || buf[k - 1] == '\t')) k--;
    if (k > 0 && buf[k - 1] != ';' && buf[k - 1] != '|' && buf[k - 1] != '&') first = 0;
    char word[256];
    size_t wl = *pos - ws;
    if (wl >= sizeof word) return;
    memcpy(word, buf + ws, wl);
    word[wl] = 0;

    char **m = NULL;
    int n = completer(word, first, &m);
    if (n <= 0) { out("\a", 1); free(m); return; }

    char cp[256];
    snprintf(cp, sizeof cp, "%s", m[0]);
    for (int i = 1; i < n; i++) {
        size_t j = 0;
        while (cp[j] && cp[j] == m[i][j]) j++;
        cp[j] = 0;
    }
    if (strlen(cp) > wl) {
        insert_str(buf, len, pos, size, cp + wl);
        if (n == 1 && cp[strlen(cp) - 1] != '/') insert_str(buf, len, pos, size, " ");
        redraw(prompt, buf, *len, *pos);
    } else if (n > 1) {
        outs("\r\n");
        for (int i = 0; i < n; i++) { outs(m[i]); outs("  "); }
        outs("\r\n");
        redraw(prompt, buf, *len, *pos);
    }
    for (int i = 0; i < n; i++) free(m[i]);
    free(m);
}

int line_edit(const char *prompt, char *buf, size_t size) {
    struct termios orig, raw;
    if (tcgetattr(0, &orig) < 0) return -1;
    raw = orig;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(0, TCSADRAIN, &raw);

    size_t len = 0, pos = 0;
    int hi = nhist, result = 1;
    char saved[MAXL] = "";
    buf[0] = 0;
    redraw(prompt, buf, len, pos);

    for (;;) {
        int k = read_key();
        if (k == -2) { result = 0; break; }
        if (k == 13 || k == 10) { outs("\n"); break; }
        if (k == 3) { outs("^C\n"); buf[0] = 0; break; }
        if (k == 4) {
            if (len == 0) { outs("\n"); result = 0; break; }
            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); len--; }
        } else if (k == 127 || k == 8) {
            if (pos > 0) { memmove(buf + pos - 1, buf + pos, len - pos + 1); pos--; len--; }
        } else if (k == K_DEL) {
            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); len--; }
        } else if (k == K_LEFT) { if (pos > 0) pos--; }
        else if (k == K_RIGHT) { if (pos < len) pos++; }
        else if (k == K_HOME) pos = 0;
        else if (k == K_END) pos = len;
        else if (k == K_UP || k == K_DOWN) {
            if (k == K_UP && hi > 0) {
                if (hi == nhist) snprintf(saved, sizeof saved, "%s", buf);
                hi--;
                snprintf(buf, size, "%s", hist[hi]);
            } else if (k == K_DOWN && hi < nhist) {
                hi++;
                snprintf(buf, size, "%s", hi == nhist ? saved : hist[hi]);
            } else continue;
            len = pos = strlen(buf);
        } else if (k == 21) {                      /* Ctrl+U */
            memmove(buf, buf + pos, len - pos + 1);
            len -= pos; pos = 0;
        } else if (k == 11) {                      /* Ctrl+K */
            buf[pos] = 0; len = pos;
        } else if (k == 23) {                      /* Ctrl+W */
            size_t s = pos;
            while (s > 0 && buf[s - 1] == ' ') s--;
            while (s > 0 && buf[s - 1] != ' ') s--;
            memmove(buf + s, buf + pos, len - pos + 1);
            len -= pos - s; pos = s;
        } else if (k == 12) {                      /* Ctrl+L */
            outs("\033[2J\033[H");
        } else if (k == 9) {
            do_tab(buf, &len, &pos, size, prompt);
            continue;
        } else if (k >= 32 && k < 256 && k != 127) {
            if (len + 1 < size) {
                memmove(buf + pos + 1, buf + pos, len - pos + 1);
                buf[pos++] = (char)k; len++;
            }
        } else continue;
        redraw(prompt, buf, len, pos);
    }
    tcsetattr(0, TCSADRAIN, &orig);
    return result;
}
