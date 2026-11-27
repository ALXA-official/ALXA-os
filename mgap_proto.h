/*
 * mgap_proto.h - Magnesium App Protocol (MAP) v1, shared by the desktop and
 * standalone apps.
 *
 * A standalone app is an ordinary static program. The desktop starts it with a
 * SOCK_SEQPACKET socket on file descriptor MGAP_FD (environment MG_APP_FD=3).
 * The app creates a shared pixel buffer (memfd), sends it to the desktop in a
 * HELLO message, draws into it and sends FRAME messages to show it. The desktop
 * sends MOUSE and KEY events back. Coordinates are relative to the window's
 * content area. Original code, MIT licensed.
 */
#ifndef MG_APP_PROTO_H
#define MG_APP_PROTO_H

#include <stdint.h>

#define MGAP_FD 3

enum {
    MGAP_HELLO = 1,    /* app -> desk: a=width, b=height, text=title, fd=shared buffer (0xAARRGGBB, top-left first) */
    MGAP_FRAME = 2,    /* app -> desk: buffer updated, show it */
    MGAP_MOUSE = 10,   /* desk -> app: a=x, b=y, c=MGAP_PRESS/RELEASE/MOVE, d=button (0 left, 1 middle, 2 right) */
    MGAP_KEY   = 11,   /* desk -> app: a=key (ASCII, or Enter 13, Backspace 8/127, Esc 27) */
    MGAP_QUIT  = 12    /* desk -> app: window was closed, exit */
};

enum { MGAP_PRESS = 0, MGAP_RELEASE = 1, MGAP_MOVE = 2 };

struct mgap_msg {
    int32_t type, a, b, c, d;
    char text[44];
};

#endif
