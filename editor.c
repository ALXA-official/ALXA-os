/*
 * ALXA OS - "edit": a small full-screen, colored terminal text editor.
 * Original code by alxa, MIT licensed.
 *
 * Keys: arrows/Home/End/PgUp/PgDn move, Delete/Backspace erase,
 *       Ctrl+O or Ctrl+S save, Ctrl+X or Ctrl+Q exit.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "editor.h"

#define MAXL 1000
#define MAXC 240
#define GUTTER 5
#define KCTRL(k) ((k) & 0x1f)

enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL, K_PGUP, K_PGDN };

static char lines[MAXL][MAXC];
static int nl, cx, cy, rowoff, coloff, rows = 24, cols = 80, dirty, quit_confirm;
static char msg[100];
static const char *fname;
static struct termios orig;

/* ---------- output buffer ---------- */
static char ob[131072];
static size_t ol;

static void oan(const char *s, size_t n) {
    if (ol + n < sizeof ob) { memcpy(ob + ol, s, n); ol += n; }
}
static void oa(const char *s) { oan(s, strlen(s)); }
static void oaf(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) oan(tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
}
static void flush_out(void) {
    size_t off = 0;
    while (off < ol) {
        ssize_t w = write(1, ob + off, ol - off);
        if (w <= 0) { if (errno == EINTR) continue; break; }
        off += (size_t)w;
    }
    ol = 0;
}

/* ---------- terminal ---------- */
static void raw_on(void) {
    struct termios t;
    tcgetattr(0, &orig);
    t = orig;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~(OPOST);
    t.c_cflag |= CS8;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
}
static void raw_off(void) { tcsetattr(0, TCSAFLUSH, &orig); }

/* returns byte, -1 on timeout, -2 on end of input */
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

static void get_size(void) {
    struct winsize ws;
    rows = 24; cols = 80;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row; cols = ws.ws_col;
    } else {
        /* serial consoles report no size: ask the terminal where the far corner is */
        const char *q = "\x1b[999C\x1b[999B\x1b[6n";
        if (write(1, q, strlen(q)) < 0) {}
        char buf[32];
        int i = 0, c;
        while (i < (int)sizeof buf - 1 && (c = read_byte(300)) >= 0) {
            buf[i++] = (char)c;
            if (c == 'R') break;
        }
        buf[i] = 0;
        int r, cc;
        if (sscanf(buf, "\x1b[%d;%dR", &r, &cc) == 2 && r >= 5 && cc >= 20) { rows = r; cols = cc; }
    }
    if (rows > 100) rows = 100;
    if (cols > 250) cols = 250;
    if (rows < 5) rows = 5;
    if (cols < 20) cols = 20;
}

static int read_key(void) {
    int c = read_byte(-1);
    if (c < 0) return -1;
    if (c != 27) return c;
    int a = read_byte(50);
    if (a < 0) return 27;
    int b = read_byte(50);
    if (b < 0) return 27;
    if (a == '[') {
        if (b >= '0' && b <= '9') {
            int d = read_byte(50);
            if (d == '~') {
                switch (b) {
                case '1': case '7': return K_HOME;
                case '3': return K_DEL;
                case '4': case '8': return K_END;
                case '5': return K_PGUP;
                case '6': return K_PGDN;
                }
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

/* ---------- editing ---------- */
static void ins(int c) {
    int l = (int)strlen(lines[cy]);
    if (l >= MAXC - 1) { snprintf(msg, sizeof msg, "Line is full (max %d chars)", MAXC - 1); return; }
    memmove(&lines[cy][cx + 1], &lines[cy][cx], (size_t)(l - cx + 1));
    lines[cy][cx++] = (char)c;
    dirty = 1;
}

static void newline(void) {
    if (nl >= MAXL) { snprintf(msg, sizeof msg, "Too many lines (max %d)", MAXL); return; }
    for (int i = nl; i > cy + 1; i--) memcpy(lines[i], lines[i - 1], MAXC);
    size_t tl = strlen(&lines[cy][cx]);
    memcpy(lines[cy + 1], &lines[cy][cx], tl + 1);
    lines[cy][cx] = 0;
    nl++; cy++; cx = 0; dirty = 1;
}

static void remove_line(int at) {
    for (int i = at; i < nl - 1; i++) memcpy(lines[i], lines[i + 1], MAXC);
    nl--;
    lines[nl][0] = 0;
}

static void backspace(void) {
    if (cx > 0) {
        memmove(&lines[cy][cx - 1], &lines[cy][cx], strlen(lines[cy]) - (size_t)cx + 1);
        cx--; dirty = 1;
    } else if (cy > 0) {
        int pl = (int)strlen(lines[cy - 1]), l = (int)strlen(lines[cy]);
        if (pl + l >= MAXC) { snprintf(msg, sizeof msg, "Cannot join: line too long"); return; }
        memcpy(lines[cy - 1] + pl, lines[cy], (size_t)l + 1);
        remove_line(cy);
        cy--; cx = pl; dirty = 1;
    }
}

static void del_char(void) {
    int l = (int)strlen(lines[cy]);
    if (cx < l) {
        memmove(&lines[cy][cx], &lines[cy][cx + 1], (size_t)(l - cx));
        dirty = 1;
    } else if (cy < nl - 1) {
        if (l + (int)strlen(lines[cy + 1]) >= MAXC) { snprintf(msg, sizeof msg, "Cannot join: line too long"); return; }
        memcpy(lines[cy] + l, lines[cy + 1], strlen(lines[cy + 1]) + 1);
        remove_line(cy + 1);
        dirty = 1;
    }
}

static void clamp_cx(void) {
    int l = (int)strlen(lines[cy]);
    if (cx > l) cx = l;
}

/* ---------- file I/O ---------- */
static int load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) { nl = 1; lines[0][0] = 0; snprintf(msg, sizeof msg, "New file"); return 0; }
        return -1;
    }
    int c, len = 0;
    nl = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == 0) { fclose(f); return -4; }
        if (c == '\n') {
            lines[nl][len] = 0; nl++; len = 0;
            if (nl >= MAXL) { fclose(f); return -2; }
            continue;
        }
        if (c == '\r') continue;
        int reps = (c == '\t') ? 4 : 1;
        for (int k = 0; k < reps; k++) {
            if (len >= MAXC - 1) { fclose(f); return -3; }
            lines[nl][len++] = (c == '\t') ? ' ' : (char)c;
        }
    }
    if (len > 0 || nl == 0) { lines[nl][len] = 0; nl++; }
    fclose(f);
    return 0;
}

static void save(void) {
    FILE *f = fopen(fname, "w");
    if (!f) { snprintf(msg, sizeof msg, "Save failed: %s", strerror(errno)); return; }
    for (int i = 0; i < nl; i++) { fputs(lines[i], f); fputc('\n', f); }
    if (fclose(f) != 0) { snprintf(msg, sizeof msg, "Save failed: %s", strerror(errno)); return; }
    dirty = 0;
    snprintf(msg, sizeof msg, "Saved %d lines", nl);
}

/* ---------- drawing ---------- */
static int is_comment(const char *s) {
    while (*s == ' ') s++;
    return s[0] == '#' || (s[0] == '/' && s[1] == '/');
}

static void draw(void) {
    int textrows = rows - 2, textw = cols - GUTTER;
    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + textrows) rowoff = cy - textrows + 1;
    if (cx < coloff) coloff = cx;
    if (cx >= coloff + textw) coloff = cx - textw + 1;

    ol = 0;
    oa("\x1b[?25l\x1b[H");

    char t[300];
    snprintf(t, sizeof t, " ALXA edit  %s%s", fname, dirty ? "  [modified]" : "");
    oa("\x1b[44;97m");
    oaf("%-*.*s", cols, cols, t);
    oa("\x1b[0m\r\n");

    for (int y = 0; y < textrows; y++) {
        int fr = rowoff + y;
        if (fr < nl) {
            oaf("\x1b[36m%4d \x1b[0m", fr + 1);
            const char *s = lines[fr];
            int len = (int)strlen(s), cm = is_comment(s);
            if (cm) oa("\x1b[32m");
            if (len > coloff) {
                int n = len - coloff;
                if (n > textw) n = textw;
                oan(s + coloff, (size_t)n);
            }
            if (cm) oa("\x1b[0m");
        } else {
            oa("\x1b[34m   ~\x1b[0m");
        }
        oa("\x1b[K\r\n");
    }

    char st[300];
    snprintf(st, sizeof st, " ^O Save  ^X Exit | Ln %d/%d Col %d  %s", cy + 1, nl, cx + 1, msg);
    oa("\x1b[7m");
    oaf("%-*.*s", cols, cols, st);
    oa("\x1b[0m");
    oaf("\x1b[%d;%dH\x1b[?25h", cy - rowoff + 2, GUTTER + cx - coloff + 1);
    flush_out();
}

/* ---------- main entry ---------- */
int editor_run(const char *path) {
    if (!isatty(0)) { puts("edit: needs a terminal"); return 1; }
    fname = path;
    cx = cy = rowoff = coloff = dirty = quit_confirm = 0;
    msg[0] = 0;

    int r = load(path);
    if (r < 0) {
        const char *why = r == -1 ? strerror(errno)
                        : r == -2 ? "too many lines (max 1000)"
                        : r == -3 ? "a line is too long (max 239 chars)"
                        : "binary file";
        printf("edit: cannot open %s: %s\n", path, why);
        return 1;
    }

    raw_on();
    get_size();

    for (;;) {
        draw();
        int k = read_key();
        if (k < 0) break;
        msg[0] = 0;
        if (k != KCTRL('X') && k != KCTRL('Q')) quit_confirm = 0;
        int textrows = rows - 2;

        switch (k) {
        case K_LEFT:  if (cx > 0) cx--; else if (cy > 0) { cy--; cx = (int)strlen(lines[cy]); } break;
        case K_RIGHT: if (cx < (int)strlen(lines[cy])) cx++; else if (cy < nl - 1) { cy++; cx = 0; } break;
        case K_UP:    if (cy > 0) cy--; clamp_cx(); break;
        case K_DOWN:  if (cy < nl - 1) cy++; clamp_cx(); break;
        case K_HOME:  cx = 0; break;
        case K_END:   cx = (int)strlen(lines[cy]); break;
        case K_PGUP:  cy -= textrows; if (cy < 0) cy = 0; clamp_cx(); break;
        case K_PGDN:  cy += textrows; if (cy > nl - 1) cy = nl - 1; clamp_cx(); break;
        case K_DEL:   del_char(); break;
        case 127: case 8: backspace(); break;
        case 13: case 10: newline(); break;
        case 9: for (int i = 0; i < 4; i++) ins(' '); break;
        case KCTRL('S'): case KCTRL('O'): save(); break;
        case KCTRL('X'): case KCTRL('Q'):
            if (dirty && !quit_confirm) {
                quit_confirm = 1;
                snprintf(msg, sizeof msg, "Unsaved changes! Press ^X again to exit without saving");
                break;
            }
            goto done;
        default:
            if (k >= 32 && k != 127 && k < 256) ins(k);
        }
    }
done:
    raw_off();
    oa("\x1b[0m\x1b[2J\x1b[H");
    flush_out();
    return 0;
}
