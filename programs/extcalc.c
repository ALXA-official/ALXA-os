/*
 * /programs/extcalc - Calculator as a standalone app (Magnesium App Protocol).
 * Runs as its own program and draws into a window the desktop shows. It does not
 * depend on the desktop's built-in apps. Start it from the Start menu
 * ("Calc (standalone)") or run it from a shell inside the desktop.
 * Original code, MIT licensed.
 */
#include "mgapp.h"

#define WIN_W 260
#define WIN_H 340
#define DISP_H 58
#define BTN_TOP 64
#define GAP 8

static const uint32_t C_BG = 0x0B1322, C_DISP = 0x132B4C, C_BTN = 0x1B2B48;
static const uint32_t C_OP = 0x22385E, C_EQ = 0x34D399, C_TXT = 0xE2ECF9, C_ACC = 0x38BDF8;
static const uint32_t C_ERR = 0xF06565;

static const char *labels[5][4] = {
    { "C", "DEL", "/", "*" },
    { "7", "8", "9", "-" },
    { "4", "5", "6", "+" },
    { "1", "2", "3", "=" },
    { "0", ".", "", "" },
};

static double acc, cur;
static char op;              /* 0, '+', '-', '*' or '/' */
static char disp[32];
static int entering, after_eq, errflag;

static void show_number(double v) {
    snprintf(disp, sizeof disp, "%.10g", v);
}

static void clear_all(void) {
    acc = cur = 0;
    op = 0;
    entering = after_eq = errflag = 0;
    snprintf(disp, sizeof disp, "0");
}

static double calc(double a, char o, double b) {
    switch (o) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/':
        if (b == 0) { errflag = 1; return 0; }
        return a / b;
    }
    return b;
}

static void press_digit(char d) {
    if (errflag) return;
    if (!entering || after_eq) {
        disp[0] = 0;
        entering = 1;
        after_eq = 0;
    }
    if (d == '.') {
        if (strchr(disp, '.')) return;
        if (!disp[0]) snprintf(disp, sizeof disp, "0");
    }
    size_t l = strlen(disp);
    if (l < 14) { disp[l] = d; disp[l + 1] = 0; }
    cur = strtod(disp, NULL);
}

static void press_backspace(void) {
    if (errflag || !entering) return;
    size_t l = strlen(disp);
    if (l) disp[--l] = 0;
    if (!l) snprintf(disp, sizeof disp, "0");
    cur = strtod(disp, NULL);
}

static void press_op(char o) {
    if (errflag) return;
    if (op && !after_eq && entering) {
        acc = calc(acc, op, cur);
    } else if (!op || after_eq) {
        acc = cur;
    }
    if (errflag) { show_number(0); snprintf(disp, sizeof disp, "Error"); return; }
    op = o;
    entering = 0;
    after_eq = 0;
    cur = acc;
    show_number(acc);
}

static void press_equals(void) {
    if (errflag || !op) return;
    double r = calc(acc, op, cur);
    if (errflag) { snprintf(disp, sizeof disp, "Error"); return; }
    acc = cur = r;
    op = 0;
    entering = 0;
    after_eq = 1;
    show_number(r);
}

static void press_label(const char *l) {
    if (!strcmp(l, "C")) clear_all();
    else if (!strcmp(l, "DEL")) press_backspace();
    else if (!strcmp(l, "=")) press_equals();
    else if (l[0] == '+' || l[0] == '-' || l[0] == '*' || l[0] == '/') press_op(l[0]);
    else press_digit(l[0]);
}

static void hit_button(int x, int y) {
    int bw = (WIN_W - GAP * 5) / 4, bh = (WIN_H - BTN_TOP - GAP * 6) / 5;
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            if (!*labels[r][c]) continue;
            int bx = GAP + c * (bw + GAP), by = BTN_TOP + r * (bh + GAP);
            if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                press_label(labels[r][c]);
                return;
            }
        }
}

static void press_key(int k) {
    if ((k >= '0' && k <= '9') || k == '.') press_digit((char)k);
    else if (k == '+' || k == '-' || k == '*' || k == '/') press_op((char)k);
    else if (k == 13 || k == '=') press_equals();
    else if (k == 8 || k == 127) press_backspace();
    else if (k == 27 || k == 'c' || k == 'C') clear_all();
}

static void draw(void) {
    int bw = (WIN_W - GAP * 5) / 4, bh = (WIN_H - BTN_TOP - GAP * 6) / 5;
    mgapp_fill(C_BG);
    mgapp_rect(GAP, GAP, WIN_W - GAP * 2, DISP_H, C_DISP);
    uint32_t tc = errflag ? C_ERR : C_TXT;
    int tw = mgapp_text_width(disp, 2);
    int tx = WIN_W - GAP * 2 - tw;
    if (tx < GAP * 2) tx = GAP * 2;
    mgapp_text(tx, GAP + (DISP_H - 32) / 2, disp, tc, 2);
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 4; c++) {
            const char *l = labels[r][c];
            if (!*l) continue;
            int bx = GAP + c * (bw + GAP), by = BTN_TOP + r * (bh + GAP);
            uint32_t bg = C_BTN;
            if (!strcmp(l, "=")) bg = C_EQ;
            else if (!strcmp(l, "C") || !strcmp(l, "DEL")) bg = C_OP;
            else if (l[0] == '+' || l[0] == '-' || l[0] == '*' || l[0] == '/') bg = C_OP;
            if (l[0] == op && op && !entering) bg = C_ACC;   /* show the pending operator */
            mgapp_rect(bx, by, bw, bh, bg);
            int lw = mgapp_text_width(l, 2);
            mgapp_text(bx + (bw - lw) / 2, by + (bh - 32) / 2, l,
                       bg == C_EQ || bg == C_ACC ? 0x0B1322 : C_TXT, 2);
        }
}

int main(void) {
    clear_all();
    if (mgapp_open("Calc (standalone)", WIN_W, WIN_H) != 0) {
        puts("extcalc: this is a standalone window app. Start it from the desktop");
        puts("(Start menu > Calc (standalone)); from a shell, use the calc command.");
        return 1;
    }
    draw();
    mgapp_present();
    for (;;) {
        struct mgap_msg m;
        int r = mgapp_next(&m, -1);
        if (r < 0 || m.type == MGAP_QUIT) break;
        if (r == 0) continue;
        if (m.type == MGAP_MOUSE && m.c == MGAP_PRESS && m.d == 0) hit_button(m.a, m.b);
        else if (m.type == MGAP_KEY) press_key(m.a);
        else continue;
        draw();
        mgapp_present();
    }
    return 0;
}
