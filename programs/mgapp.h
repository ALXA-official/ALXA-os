/*
 * mgapp.h - client library for standalone Magnesium OS apps.
 *
 * Use it from a /programs app that should run as its own program, separate from
 * the desktop. mgapp_open() connects to the desktop; if the app was not started
 * by the desktop it returns -1 and the app can fall back to console mode.
 * Drawing is into a shared buffer; call mgapp_present() to show it.
 * Protocol: ../mgap_proto.h. Original code, MIT licensed.
 */
#ifndef MGAPP_H
#define MGAPP_H

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include "../mgap_proto.h"
#include "../font8x16.h"

static int mgapp_fd = -1;
static int mgapp_w, mgapp_h;
static uint32_t *mgapp_px;
static size_t mgapp_bytes;

/* Connect to the desktop and create the shared buffer. Returns 0, or -1 when the
 * app is not running under the desktop (or the buffer could not be made). */
static int mgapp_open(const char *title, int w, int h) {
    const char *e = getenv("MG_APP_FD");
    if (!e || atoi(e) != MGAP_FD || w < 1 || h < 1 || w > 2048 || h > 2048) return -1;
    int mfd = memfd_create("mgapp", 0);
    if (mfd < 0) return -1;
    mgapp_bytes = (size_t)w * (size_t)h * 4u;
    if (ftruncate(mfd, (off_t)mgapp_bytes) != 0) { close(mfd); return -1; }
    void *p = mmap(NULL, mgapp_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (p == MAP_FAILED) { close(mfd); return -1; }
    mgapp_px = (uint32_t *)p;
    mgapp_w = w;
    mgapp_h = h;
    mgapp_fd = MGAP_FD;

    struct mgap_msg m;
    memset(&m, 0, sizeof m);
    m.type = MGAP_HELLO;
    m.a = w;
    m.b = h;
    snprintf(m.text, sizeof m.text, "%s", title ? title : "App");
    struct iovec iov = { &m, sizeof m };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &mfd, sizeof(int));
    ssize_t r = sendmsg(mgapp_fd, &mh, MSG_NOSIGNAL);
    close(mfd);                       /* the desktop holds its own reference */
    return r < 0 ? -1 : 0;
}

/* Show the current buffer contents on the desktop. */
static void mgapp_present(void) {
    if (mgapp_fd < 0) return;
    struct mgap_msg m;
    memset(&m, 0, sizeof m);
    m.type = MGAP_FRAME;
    m.a = mgapp_w;
    m.b = mgapp_h;
    (void)send(mgapp_fd, &m, sizeof m, MSG_NOSIGNAL);
}

/* Wait up to timeout_ms (-1 = forever, 0 = just look) for one message.
 * Returns 1 when m was filled, 0 on timeout, -1 when the desktop went away. */
static int mgapp_next(struct mgap_msg *m, int timeout_ms) {
    if (mgapp_fd < 0) return -1;
    struct pollfd pfd = { mgapp_fd, POLLIN, 0 };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return pr;
    ssize_t r = recv(mgapp_fd, m, sizeof *m, 0);
    if (r == (ssize_t)sizeof *m) return 1;
    return r == 0 || (pfd.revents & (POLLHUP | POLLERR)) ? -1 : 0;
}

/* ---- drawing into the shared buffer (clipped) ---- */

static void mgapp_rect(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > mgapp_w ? mgapp_w : x + w, y1 = y + h > mgapp_h ? mgapp_h : y + h;
    if (!mgapp_px || x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++) mgapp_px[(size_t)j * mgapp_w + i] = c;
}

static void mgapp_fill(uint32_t c) { mgapp_rect(0, 0, mgapp_w, mgapp_h, c); }

static int mgapp_text_width(const char *s, int scale) {
    return (int)strlen(s) * FONT_W * scale;
}

/* Draw text with the 8x16 font. scale 1 = normal, 2 = double size. */
static void mgapp_text(int x, int y, const char *s, uint32_t c, int scale) {
    for (; *s; s++, x += FONT_W * scale) {
        int g = (unsigned char)*s - FONT_FIRST;
        if (g < 0 || g > FONT_LAST - FONT_FIRST) g = '?' - FONT_FIRST;
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = mg_font8x16[g][row];
            for (int col = 0; col < FONT_W; col++)
                if (bits & (0x80u >> col))
                    mgapp_rect(x + col * scale, y + row * scale, scale, scale, c);
        }
    }
}

#endif
