/*
 * /programs/calc - Magnesium OS Calculator.
 * Opens the graphical VaporGlass Calculator on the desktop, or evaluates
 * arithmetic expressions directly in console mode.
 * Original code by alxa, MIT licensed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "mg_app.h"

static const char *ep;
static double parse_expr(void);

static void skip_ws(void) { while (*ep == ' ' || *ep == '\t') ep++; }

static double parse_factor(void) {
    skip_ws();
    if (*ep == '+') { ep++; return parse_factor(); }
    if (*ep == '-') { ep++; return -parse_factor(); }
    if (*ep == '(') {
        ep++;
        double v = parse_expr();
        skip_ws();
        if (*ep == ')') ep++;
        return v;
    }
    char *end = NULL;
    double v = strtod(ep, &end);
    if (end && end != ep) ep = end;
    return v;
}

static double parse_term(void) {
    double v = parse_factor();
    for (;;) {
        skip_ws();
        if (*ep == '*') { ep++; v *= parse_factor(); }
        else if (*ep == '/') { ep++; double d = parse_factor(); v = d != 0.0 ? v / d : 0.0; }
        else if (*ep == '%') { ep++; long a = (long)v, b = (long)parse_factor(); v = b ? (double)(a % b) : 0.0; }
        else break;
    }
    return v;
}

static double parse_expr(void) {
    double v = parse_term();
    for (;;) {
        skip_ws();
        if (*ep == '+') { ep++; v += parse_term(); }
        else if (*ep == '-') { ep++; v -= parse_term(); }
        else break;
    }
    return v;
}

int main(int argc, char **argv) {
    int ai = 1;
    if (argc > 1 && (!strcmp(argv[1], "--cli") || !strcmp(argv[1], "-c"))) {
        ai = 2;
    } else if (argc == 1) {
        if (mg_try_desktop_open("calc", argc, argv, &ai)) return 0;
    }
    if (ai >= argc) {
        puts("Magnesium Calculator (type expressions or 'q' to quit)");
        char line[256];
        while (fputs("calc> ", stdout), fflush(stdout), fgets(line, sizeof line, stdin)) {
            if (line[0] == 'q' || line[0] == 'Q') break;
            ep = line;
            printf("  = %.10g\n", parse_expr());
        }
        return 0;
    }
    char joined[512] = "";
    for (int i = ai; i < argc; i++) {
        if (i > ai) strncat(joined, " ", sizeof joined - strlen(joined) - 1);
        strncat(joined, argv[i], sizeof joined - strlen(joined) - 1);
    }
    ep = joined;
    printf("%.10g\n", parse_expr());
    return 0;
}
