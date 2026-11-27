/*
 * extapp.c - desktop side of the Magnesium App Protocol (mgap_proto.h).
 * A standalone app runs as its own process and draws into a shared buffer; the
 * desktop shows that buffer in a window and forwards mouse and keyboard input.
 * Original code, MIT licensed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "extapp.h"
#include "gfx.h"
#include "mgap_proto.h"

#define EXT_MAX_DIM 2048

struct ext_slot {
    int used, dead, ready, fd, pid;
    int w, h;
    size_t bytes;
    uint32_t *px;
    char title[44];
};
static struct ext_slot ext[EXT_MAX];

int ext_launch(const char *path) {
    int s = -1;
    for (int i = 0; i < EXT_MAX; i++) if (!ext[i].used) { s = i; break; }
    if (s < 0 || !path || access(path, X_OK) != 0) return -1;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(sv[0]); close(sv[1]); return -1; }
    if (pid == 0) {
        /* Child: the app's end of the connection becomes MGAP_FD. */
        if (sv[1] != MGAP_FD) dup2(sv[1], MGAP_FD);
        fcntl(MGAP_FD, F_SETFD, 0);
        setenv("MG_APP_FD", "3", 1);
        char *argv[] = { (char *)path, NULL };
        execv(path, argv);
        _exit(127);
    }
    close(sv[1]);
    fcntl(sv[0], F_SETFL, O_NONBLOCK);
    memset(&ext[s], 0, sizeof ext[s]);
    ext[s].used = 1;
    ext[s].fd = sv[0];
    ext[s].pid = pid;
    return s;
}

static void slot_mark_dead(struct ext_slot *e) {
    e->dead = 1;
}

/* Take the shared buffer from a HELLO message. */
static void slot_hello(struct ext_slot *e, struct msghdr *mh, const struct mgap_msg *m) {
    int fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(mh); c; c = CMSG_NXTHDR(mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(int)))
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
    if (e->ready || fd < 0) { if (fd >= 0) close(fd); return; }
    if (m->a < 1 || m->b < 1 || m->a > EXT_MAX_DIM || m->b > EXT_MAX_DIM) { close(fd); slot_mark_dead(e); return; }
    size_t bytes = (size_t)m->a * (size_t)m->b * 4u;
    void *p = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { slot_mark_dead(e); return; }
    e->px = p;
    e->bytes = bytes;
    e->w = m->a;
    e->h = m->b;
    snprintf(e->title, sizeof e->title, "%.43s", m->text);
    e->ready = 1;
}

int ext_pump(void) {
    int mask = 0;
    for (int s = 0; s < EXT_MAX; s++) {
        struct ext_slot *e = &ext[s];
        if (!e->used || e->dead) continue;
        for (;;) {
            struct mgap_msg m;
            char cbuf[CMSG_SPACE(sizeof(int))];
            struct iovec iov = { &m, sizeof m };
            struct msghdr mh;
            memset(&mh, 0, sizeof mh);
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = cbuf;
            mh.msg_controllen = sizeof cbuf;
            ssize_t r = recvmsg(e->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (r == 0) { slot_mark_dead(e); mask |= 1 << s; break; }
            if (r < 0) {
                if (errno != EAGAIN && errno != EINTR) { slot_mark_dead(e); mask |= 1 << s; }
                break;
            }
            if (r != (ssize_t)sizeof m) continue;     /* ignore malformed messages */
            if (m.type == MGAP_HELLO) {
                int was = e->ready;
                slot_hello(e, &mh, &m);
                if (!was && e->ready) mask |= 1 << s;
                if (e->dead) { mask |= 1 << s; break; }
            } else if (m.type == MGAP_FRAME) {
                if (e->ready) mask |= 1 << s;
            }
        }
        if (!e->dead) {
            int st;
            if (waitpid(e->pid, &st, WNOHANG) > 0) { slot_mark_dead(e); mask |= 1 << s; }
        }
    }
    return mask;
}

int ext_alive(int s) {
    return s >= 0 && s < EXT_MAX && ext[s].used && !ext[s].dead;
}

int ext_count(void) {
    int n = 0;
    for (int s = 0; s < EXT_MAX; s++) if (ext_alive(s)) n++;
    return n;
}

int ext_info(int s, int *w, int *h, char *title, size_t tn) {
    if (!ext_alive(s) || !ext[s].ready) return 0;
    if (w) *w = ext[s].w;
    if (h) *h = ext[s].h;
    if (title && tn) snprintf(title, tn, "%s", ext[s].title);
    return 1;
}

void ext_draw(int s, int x, int y, int cw, int ch) {
    if (!ext_alive(s) || !ext[s].ready || !ext[s].px) return;
    int dw = ext[s].w < cw ? ext[s].w : cw;
    int dh = ext[s].h < ch ? ext[s].h : ch;
    gfx_blit(x, y, dw, dh, ext[s].px, ext[s].w);
}

static void send_msg(struct ext_slot *e, const struct mgap_msg *m) {
    (void)send(e->fd, m, sizeof *m, MSG_NOSIGNAL | MSG_DONTWAIT);
}

void ext_mouse(int s, int x, int y, int kind, int btn) {
    if (!ext_alive(s) || !ext[s].ready) return;
    struct mgap_msg m;
    memset(&m, 0, sizeof m);
    m.type = MGAP_MOUSE;
    m.a = x;
    m.b = y;
    m.c = kind;
    m.d = btn;
    send_msg(&ext[s], &m);
}

void ext_key(int s, int k) {
    if (!ext_alive(s) || !ext[s].ready) return;
    struct mgap_msg m;
    memset(&m, 0, sizeof m);
    m.type = MGAP_KEY;
    m.a = k;
    send_msg(&ext[s], &m);
}

void ext_close(int s) {
    if (s < 0 || s >= EXT_MAX || !ext[s].used) return;
    struct ext_slot *e = &ext[s];
    if (!e->dead) {
        struct mgap_msg m;
        memset(&m, 0, sizeof m);
        m.type = MGAP_QUIT;
        send_msg(e, &m);
    }
    kill(e->pid, SIGKILL);
    waitpid(e->pid, NULL, 0);
    if (e->px) munmap(e->px, e->bytes);
    close(e->fd);
    memset(e, 0, sizeof *e);
}
