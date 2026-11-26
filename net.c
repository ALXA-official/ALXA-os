/*
 * Magnesium OS - networking stack (DHCP, static IP, UDP DNS, ICMP/TCP ping,
 * HTTP/1.1 client), hardware driver discovery, and manual config loader.
 * Original code by alxa, MIT licensed.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include "net.h"
#include "gfx.h"
#include "registry.h"

/* ================= Low-level file & sysfs helpers ================= */

static int read_one_line(const char *path, char *out, size_t len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { if (len) out[0] = 0; return -1; }
    ssize_t n = read(fd, out, len - 1);
    close(fd);
    if (n <= 0) { out[0] = 0; return -1; }
    out[n] = 0;
    char *nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    return 0;
}

static void read_driver_link(const char *sysdir, char *drv, size_t len) {
    char p[320], target[320];
    snprintf(p, sizeof p, "%s/driver", sysdir);
    ssize_t n = readlink(p, target, sizeof target - 1);
    if (n <= 0) {
        snprintf(p, sizeof p, "%s/device/driver", sysdir);
        n = readlink(p, target, sizeof target - 1);
    }
    if (n > 0) {
        target[n] = 0;
        const char *b = strrchr(target, '/');
        snprintf(drv, len, "%.*s", (int)(len - 1), b ? b + 1 : target);
    } else {
        drv[0] = 0;
    }
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

/* ================= DNS Config (/etc/resolv.conf + Registry) ================= */

void net_get_dns(char *dns_out, size_t len) {
    const char *rk = reg_eff("Network/DNS", "");
    if (rk && rk[0] && inet_addr(rk) != INADDR_NONE) {
        snprintf(dns_out, len, "%s", rk);
        return;
    }
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            char ns[64];
            if (sscanf(line, "nameserver %63s", ns) == 1 && inet_addr(ns) != INADDR_NONE) {
                fclose(f);
                snprintf(dns_out, len, "%s", ns);
                return;
            }
        }
        fclose(f);
    }
    snprintf(dns_out, len, "10.0.2.3");
}

void net_set_dns(const char *dns_ip) {
    if (!dns_ip || !dns_ip[0]) return;
    mkdir("/etc", 0755);
    FILE *f = fopen("/etc/resolv.conf", "w");
    if (f) {
        fprintf(f, "nameserver %s\nnameserver 1.1.1.1\n", dns_ip);
        fclose(f);
    }
}

static void read_default_gw(const char *ifname, char *gw_out, size_t len) {
    gw_out[0] = 0;
    FILE *f = fopen("/proc/net/route", "r");
    if (!f) return;
    char line[256];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) {
        char iface[32];
        unsigned long dest = 0, gw = 0, flags = 0;
        if (sscanf(line, "%31s %lx %lx %lx", iface, &dest, &gw, &flags) == 4) {
            if (dest == 0 && (flags & 0x2) && (!ifname || !ifname[0] || !strcmp(iface, ifname))) {
                struct in_addr in;
                in.s_addr = (uint32_t)gw;
                snprintf(gw_out, len, "%s", inet_ntoa(in));
                break;
            }
        }
    }
    fclose(f);
}

/* ================= Interface Discovery & Configuration ================= */

int net_list_ifaces(struct net_iface *out, int max_out) {
    if (!out || max_out <= 0) return 0;
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return 0;

    char line[320];
    /* Skip 2 header lines */
    if (!fgets(line, sizeof line, f) || !fgets(line, sizeof line, f)) { fclose(f); return 0; }

    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int n = 0;
    char dns_ip[24];
    net_get_dns(dns_ip, sizeof dns_ip);

    while (fgets(line, sizeof line, f) && n < max_out) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *nm = line;
        while (*nm == ' ' || *nm == '\t') nm++;
        if (!*nm) continue;

        struct net_iface *it = &out[n];
        memset(it, 0, sizeof *it);
        snprintf(it->name, sizeof it->name, "%.23s", nm);
        snprintf(it->dns, sizeof it->dns, "%.19s", dns_ip);

        unsigned long long rxb = 0, rxp = 0, rxe = 0, rxd = 0, rxf = 0, rxfr = 0, rxc = 0, rxm = 0, txb = 0;
        sscanf(colon + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
               &rxb, &rxp, &rxe, &rxd, &rxf, &rxfr, &rxc, &rxm, &txb);
        it->rx_bytes = rxb;
        it->tx_bytes = txb;

        char sysp[128];
        snprintf(sysp, sizeof sysp, "/sys/class/net/%.32s", nm);
        read_driver_link(sysp, it->driver, sizeof it->driver);

        if (sock >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof ifr);
            snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%.15s", nm);
            if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
                it->up = (ifr.ifr_flags & IFF_UP) ? 1 : 0;
                it->running = (ifr.ifr_flags & IFF_RUNNING) ? 1 : 0;
                it->loopback = (ifr.ifr_flags & IFF_LOOPBACK) ? 1 : 0;
            }
            if (it->loopback && !it->driver[0]) snprintf(it->driver, sizeof it->driver, "loopback");
            else if (!it->driver[0]) snprintf(it->driver, sizeof it->driver, "netdev");

            if (!it->loopback && ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                const uint8_t *m = (const uint8_t *)ifr.ifr_hwaddr.sa_data;
                snprintf(it->mac, sizeof it->mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                         m[0], m[1], m[2], m[3], m[4], m[5]);
            } else {
                snprintf(it->mac, sizeof it->mac, "00:00:00:00:00:00");
            }

            if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_addr;
                snprintf(it->ip, sizeof it->ip, "%s", inet_ntoa(sa->sin_addr));
            } else {
                snprintf(it->ip, sizeof it->ip, "0.0.0.0");
            }

            if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
                struct sockaddr_in *sa = (struct sockaddr_in *)&ifr.ifr_netmask;
                snprintf(it->netmask, sizeof it->netmask, "%s", inet_ntoa(sa->sin_addr));
            } else {
                snprintf(it->netmask, sizeof it->netmask, "0.0.0.0");
            }
        }
        read_default_gw(nm, it->gateway, sizeof it->gateway);
        n++;
    }
    if (sock >= 0) close(sock);
    fclose(f);
    return n;
}

int net_default_iface(char *name_out, size_t len) {
    const char *pref = reg_eff("Network/Interface", "");
    struct net_iface ifs[16];
    int n = net_list_ifaces(ifs, 16);
    if (pref && pref[0] && strcasecmp(pref, "auto") != 0) {
        for (int i = 0; i < n; i++) {
            if (!strcmp(ifs[i].name, pref)) {
                snprintf(name_out, len, "%s", ifs[i].name);
                return 0;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (!ifs[i].loopback) {
            snprintf(name_out, len, "%s", ifs[i].name);
            return 0;
        }
    }
    return -1;
}

int net_if_updown(const char *ifname, int up) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) { close(s); return -1; }
    if (up) ifr.ifr_flags |= (short)(IFF_UP | IFF_RUNNING);
    else ifr.ifr_flags &= (short)~IFF_UP;
    int rc = ioctl(s, SIOCSIFFLAGS, &ifr);
    close(s);
    return rc;
}

static void set_sockaddr4(struct sockaddr *sa, uint32_t ip_net) {
    struct sockaddr_in *sin = (struct sockaddr_in *)sa;
    memset(sin, 0, sizeof *sin);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = ip_net;
}

static int apply_ipv4_raw(const char *ifname, uint32_t ip_net, uint32_t mask_net,
                          uint32_t gw_net, char *err, size_t errlen) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        if (err) snprintf(err, errlen, "%s", strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= (short)(IFF_UP | IFF_RUNNING);
        ioctl(s, SIOCSIFFLAGS, &ifr);
    }

    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
    set_sockaddr4(&ifr.ifr_addr, ip_net);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) {
        if (err) snprintf(err, errlen, "SIOCSIFADDR: %s", strerror(errno));
        close(s);
        return -1;
    }

    if (mask_net != 0) {
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
        set_sockaddr4(&ifr.ifr_netmask, mask_net);
        ioctl(s, SIOCSIFNETMASK, &ifr);

        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifname);
        set_sockaddr4(&ifr.ifr_broadaddr, (ip_net & mask_net) | ~mask_net);
        ioctl(s, SIOCSIFBRDADDR, &ifr);
    }

    if (gw_net != 0) {
        struct rtentry rt;
        /* Remove any existing default route first */
        memset(&rt, 0, sizeof rt);
        set_sockaddr4(&rt.rt_dst, 0);
        set_sockaddr4(&rt.rt_genmask, 0);
        rt.rt_flags = RTF_UP;
        ioctl(s, SIOCDELRT, &rt);

        memset(&rt, 0, sizeof rt);
        set_sockaddr4(&rt.rt_dst, 0);
        set_sockaddr4(&rt.rt_genmask, 0);
        set_sockaddr4(&rt.rt_gateway, gw_net);
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        char devbuf[IFNAMSIZ];
        snprintf(devbuf, sizeof devbuf, "%s", ifname);
        rt.rt_dev = devbuf;
        if (ioctl(s, SIOCADDRT, &rt) < 0 && errno != EEXIST) {
            if (err) snprintf(err, errlen, "route: %s", strerror(errno));
        }
    }
    close(s);
    return 0;
}

int net_set_static(const char *ifname, const char *ip, const char *mask,
                   const char *gw, const char *dns, char *err, size_t errlen) {
    struct in_addr a_ip, a_mask, a_gw;
    if (!ifname || !ifname[0]) { if (err) snprintf(err, errlen, "missing interface"); return -1; }
    if (!ip || !inet_aton(ip, &a_ip)) { if (err) snprintf(err, errlen, "invalid IP address"); return -1; }
    if (!mask || !mask[0]) mask = "255.255.255.0";
    if (!inet_aton(mask, &a_mask)) { if (err) snprintf(err, errlen, "invalid netmask"); return -1; }
    a_gw.s_addr = 0;
    if (gw && gw[0] && !inet_aton(gw, &a_gw)) { if (err) snprintf(err, errlen, "invalid gateway"); return -1; }

    if (apply_ipv4_raw(ifname, a_ip.s_addr, a_mask.s_addr, a_gw.s_addr, err, errlen) < 0)
        return -1;
    if (dns && dns[0]) net_set_dns(dns);
    return 0;
}

/* ================= Native RFC 2131 DHCPv4 Client ================= */

struct dhcp_pkt {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t cookie;
    uint8_t options[312];
} __attribute__((packed));

struct udp_ip_dhcp {
    struct iphdr ip;
    uint16_t uh_sport, uh_dport, uh_ulen, uh_sum;
    struct dhcp_pkt dhcp;
} __attribute__((packed));

static uint16_t ip_checksum(const void *buf, size_t len) {
    const uint16_t *w = (const uint16_t *)buf;
    uint32_t sum = 0;
    while (len > 1) { sum += *w++; len -= 2; }
    if (len == 1) sum += *(const uint8_t *)w;
    sum = (sum >> 16) + (sum & 0xFFFFu);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

static int dhcp_send_pkt(int pfd, int ifindex, const uint8_t mac[6], uint32_t xid,
                         uint8_t msg_type, uint32_t req_ip, uint32_t srv_ip) {
    struct udp_ip_dhcp pkt;
    memset(&pkt, 0, sizeof pkt);

    pkt.dhcp.op = 1;      /* BOOTREQUEST */
    pkt.dhcp.htype = 1;   /* Ethernet */
    pkt.dhcp.hlen = 6;
    pkt.dhcp.xid = xid;
    pkt.dhcp.flags = htons(0x8000); /* Broadcast reply flag */
    memcpy(pkt.dhcp.chaddr, mac, 6);
    pkt.dhcp.cookie = htonl(0x63825363u);

    int o = 0;
    pkt.dhcp.options[o++] = 53; pkt.dhcp.options[o++] = 1; pkt.dhcp.options[o++] = msg_type;
    pkt.dhcp.options[o++] = 61; pkt.dhcp.options[o++] = 7; pkt.dhcp.options[o++] = 1;
    memcpy(&pkt.dhcp.options[o], mac, 6); o += 6;
    if (req_ip) {
        pkt.dhcp.options[o++] = 50; pkt.dhcp.options[o++] = 4;
        memcpy(&pkt.dhcp.options[o], &req_ip, 4); o += 4;
    }
    if (srv_ip) {
        pkt.dhcp.options[o++] = 54; pkt.dhcp.options[o++] = 4;
        memcpy(&pkt.dhcp.options[o], &srv_ip, 4); o += 4;
    }
    /* Hostname "magnesium" */
    pkt.dhcp.options[o++] = 12; pkt.dhcp.options[o++] = 9;
    memcpy(&pkt.dhcp.options[o], "magnesium", 9); o += 9;
    /* Parameter Request List: Subnet, Router, DNS, Domain */
    pkt.dhcp.options[o++] = 55; pkt.dhcp.options[o++] = 4;
    pkt.dhcp.options[o++] = 1; pkt.dhcp.options[o++] = 3;
    pkt.dhcp.options[o++] = 6; pkt.dhcp.options[o++] = 15;
    pkt.dhcp.options[o++] = 255;

    uint16_t udp_len = (uint16_t)(8 + sizeof(struct dhcp_pkt));
    uint16_t ip_len  = (uint16_t)(sizeof(struct iphdr) + udp_len);

    pkt.uh_sport = htons(68);
    pkt.uh_dport = htons(67);
    pkt.uh_ulen  = htons(udp_len);
    pkt.uh_sum   = 0; /* Optional in IPv4 UDP */

    pkt.ip.ihl = 5;
    pkt.ip.version = 4;
    pkt.ip.tot_len = htons(ip_len);
    pkt.ip.id = htons((uint16_t)(xid & 0xFFFFu));
    pkt.ip.ttl = 64;
    pkt.ip.protocol = IPPROTO_UDP;
    pkt.ip.saddr = 0;
    pkt.ip.daddr = 0xFFFFFFFFu;
    pkt.ip.check = ip_checksum(&pkt.ip, sizeof pkt.ip);

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_IP);
    sll.sll_ifindex = ifindex;
    sll.sll_halen = 6;
    memset(sll.sll_addr, 0xFF, 6);

    return sendto(pfd, &pkt, sizeof pkt, 0, (struct sockaddr *)&sll, sizeof sll) >= 0 ? 0 : -1;
}

static int dhcp_parse_opts(const uint8_t *opt, size_t len, uint8_t *msg_type,
                           uint32_t *mask, uint32_t *gw, uint32_t *dns, uint32_t *srv) {
    *msg_type = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t code = opt[i++];
        if (code == 0) continue;
        if (code == 255) break;
        if (i >= len) break;
        uint8_t olen = opt[i++];
        if (i + olen > len) break;
        if (code == 53 && olen >= 1) *msg_type = opt[i];
        else if (code == 1 && olen >= 4) memcpy(mask, &opt[i], 4);
        else if (code == 3 && olen >= 4) memcpy(gw, &opt[i], 4);
        else if (code == 6 && olen >= 4) memcpy(dns, &opt[i], 4);
        else if (code == 54 && olen >= 4) memcpy(srv, &opt[i], 4);
        i += olen;
    }
    return *msg_type != 0 ? 0 : -1;
}

int net_dhcp(const char *ifname, char *status_out, size_t status_len) {
    char def_if[24];
    if (!ifname || !ifname[0]) {
        if (net_default_iface(def_if, sizeof def_if) < 0) {
            if (status_out) snprintf(status_out, status_len, "No network adapter found");
            return -1;
        }
        ifname = def_if;
    }

    int ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctl < 0) {
        if (status_out) snprintf(status_out, status_len, "socket: %s", strerror(errno));
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%.15s", ifname);
    if (ioctl(ctl, SIOCGIFINDEX, &ifr) < 0) {
        if (status_out) snprintf(status_out, status_len, "%s: %s", ifname, strerror(errno));
        close(ctl);
        return -1;
    }
    int ifindex = ifr.ifr_ifindex;

    if (ioctl(ctl, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= (short)(IFF_UP | IFF_RUNNING);
        if (ioctl(ctl, SIOCSIFFLAGS, &ifr) < 0) {
            if (status_out) snprintf(status_out, status_len, "%s: %s (need root)", ifname, strerror(errno));
            close(ctl);
            return -1;
        }
    }

    uint8_t mac[6] = { 0 };
    if (ioctl(ctl, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(ctl);

    int pfd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_IP));
    if (pfd < 0) {
        if (status_out) snprintf(status_out, status_len, "raw socket: %s (need root)", strerror(errno));
        return -1;
    }

    struct sockaddr_ll bind_sll;
    memset(&bind_sll, 0, sizeof bind_sll);
    bind_sll.sll_family = AF_PACKET;
    bind_sll.sll_protocol = htons(ETH_P_IP);
    bind_sll.sll_ifindex = ifindex;
    bind(pfd, (struct sockaddr *)&bind_sll, sizeof bind_sll);

    uint32_t xid = (uint32_t)(time(NULL) ^ (getpid() << 16) ^ 0x4D473132u);
    uint32_t offer_ip = 0, srv_ip = 0, mask_ip = htonl(0xFFFFFF00u), gw_ip = 0, dns_ip = 0;

    /* Phase 1: DISCOVER -> OFFER (up to 2 tries, 1.5s each) */
    for (int attempt = 0; attempt < 2 && !offer_ip; attempt++) {
        dhcp_send_pkt(pfd, ifindex, mac, xid, 1 /* DHCPDISCOVER */, 0, 0);
        uint64_t deadline = mono_ms() + 1500;
        while (mono_ms() < deadline) {
            int rem = (int)(deadline - mono_ms());
            struct pollfd pf = { pfd, POLLIN, 0 };
            if (poll(&pf, 1, rem > 0 ? rem : 1) <= 0) break;
            uint8_t rbuf[1600];
            ssize_t n = recv(pfd, rbuf, sizeof rbuf, 0);
            if (n < (ssize_t)(sizeof(struct iphdr) + 8 + 240)) continue;
            const struct iphdr *ip = (const struct iphdr *)rbuf;
            if (ip->version != 4 || ip->protocol != IPPROTO_UDP) continue;
            size_t ihl = (size_t)ip->ihl * 4;
            if (n < (ssize_t)(ihl + 8 + 240)) continue;
            uint16_t dport = 0;
            memcpy(&dport, rbuf + ihl + 2, 2);
            if (ntohs(dport) != 68) continue;
            const struct dhcp_pkt *dp = (const struct dhcp_pkt *)(rbuf + ihl + 8);
            if (dp->op != 2 || dp->xid != xid || memcmp(dp->chaddr, mac, 6) != 0) continue;
            if (ntohl(dp->cookie) != 0x63825363u) continue;
            size_t opt_len = (size_t)n - (ihl + 8 + offsetof(struct dhcp_pkt, options));
            uint8_t mtype = 0;
            if (dhcp_parse_opts(dp->options, opt_len, &mtype, &mask_ip, &gw_ip, &dns_ip, &srv_ip) == 0 &&
                (mtype == 2 || mtype == 5) && dp->yiaddr != 0) {
                offer_ip = dp->yiaddr;
                if (!srv_ip) srv_ip = ip->saddr;
                break;
            }
        }
    }

    if (!offer_ip) {
        close(pfd);
        if (status_out) snprintf(status_out, status_len, "No DHCP offer on %s", ifname);
        return -1;
    }

    /* Phase 2: REQUEST -> ACK */
    int acked = 0;
    dhcp_send_pkt(pfd, ifindex, mac, xid, 3 /* DHCPREQUEST */, offer_ip, srv_ip);
    uint64_t deadline = mono_ms() + 1500;
    while (mono_ms() < deadline) {
        int rem = (int)(deadline - mono_ms());
        struct pollfd pf = { pfd, POLLIN, 0 };
        if (poll(&pf, 1, rem > 0 ? rem : 1) <= 0) break;
        uint8_t rbuf[1600];
        ssize_t n = recv(pfd, rbuf, sizeof rbuf, 0);
        if (n < (ssize_t)(sizeof(struct iphdr) + 8 + 240)) continue;
        const struct iphdr *ip = (const struct iphdr *)rbuf;
        if (ip->version != 4 || ip->protocol != IPPROTO_UDP) continue;
        size_t ihl = (size_t)ip->ihl * 4;
        if (n < (ssize_t)(ihl + 8 + 240)) continue;
        uint16_t dport = 0;
        memcpy(&dport, rbuf + ihl + 2, 2);
        if (ntohs(dport) != 68) continue;
        const struct dhcp_pkt *dp = (const struct dhcp_pkt *)(rbuf + ihl + 8);
        if (dp->op != 2 || dp->xid != xid || memcmp(dp->chaddr, mac, 6) != 0) continue;
        size_t opt_len = (size_t)n - (ihl + 8 + offsetof(struct dhcp_pkt, options));
        uint8_t mtype = 0;
        if (dhcp_parse_opts(dp->options, opt_len, &mtype, &mask_ip, &gw_ip, &dns_ip, &srv_ip) == 0 && mtype == 5) {
            if (dp->yiaddr) offer_ip = dp->yiaddr;
            acked = 1;
            break;
        }
    }
    close(pfd);
    (void)acked;

    char err[96] = "";
    if (apply_ipv4_raw(ifname, offer_ip, mask_ip, gw_ip, err, sizeof err) < 0) {
        if (status_out) snprintf(status_out, status_len, "DHCP apply failed: %s", err);
        return -1;
    }

    char ip_s[20], gw_s[20], dns_s[20];
    struct in_addr in;
    in.s_addr = offer_ip; snprintf(ip_s, sizeof ip_s, "%s", inet_ntoa(in));
    in.s_addr = gw_ip;    snprintf(gw_s, sizeof gw_s, "%s", gw_ip ? inet_ntoa(in) : "none");
    if (dns_ip) {
        in.s_addr = dns_ip;
        snprintf(dns_s, sizeof dns_s, "%s", inet_ntoa(in));
        net_set_dns(dns_s);
    } else {
        net_get_dns(dns_s, sizeof dns_s);
    }

    if (status_out)
        snprintf(status_out, status_len, "%s: %s (gw %s, dns %s)", ifname, ip_s, gw_s, dns_s);
    return 0;
}

int net_apply_registry(char *status_out, size_t status_len) {
    char ifname[24];
    if (net_default_iface(ifname, sizeof ifname) < 0) {
        if (status_out) snprintf(status_out, status_len, "No network adapter detected");
        return -1;
    }
    const char *mode = reg_eff("Network/Mode", "DHCP");
    if (!strcasecmp(mode, "Manual") || !strcasecmp(mode, "Static")) {
        const char *ip   = reg_eff("Network/IP", "10.0.2.15");
        const char *mask = reg_eff("Network/Netmask", "255.255.255.0");
        const char *gw   = reg_eff("Network/Gateway", "10.0.2.2");
        const char *dns  = reg_eff("Network/DNS", "10.0.2.3");
        char err[96] = "";
        if (net_set_static(ifname, ip, mask, gw, dns, err, sizeof err) < 0) {
            if (status_out) snprintf(status_out, status_len, "Manual IP error: %s", err);
            return -1;
        }
        if (status_out) snprintf(status_out, status_len, "%s: manual %s (gw %s)", ifname, ip, gw);
        return 0;
    }
    return net_dhcp(ifname, status_out, status_len);
}

void net_boot_init(void) {
    /* Allow non-root ICMP datagram sockets (ping without setuid) */
    int pfd = open("/proc/sys/net/ipv4/ping_group_range", O_WRONLY | O_CLOEXEC);
    if (pfd >= 0) {
        ssize_t w = write(pfd, "0 2147483647\n", 13);
        (void)w;
        close(pfd);
    }

    /* Bring loopback 127.0.0.1/8 up immediately */
    apply_ipv4_raw("lo", htonl(0x7F000001u), htonl(0xFF000000u), 0, NULL, 0);

    /* Ensure /etc/hosts exists */
    mkdir("/etc", 0755);
    if (access("/etc/hosts", F_OK) != 0) {
        FILE *hf = fopen("/etc/hosts", "w");
        if (hf) { fprintf(hf, "127.0.0.1\tlocalhost magnesium\n"); fclose(hf); }
    }

    /* Configure primary Ethernet NIC in background so boot is never delayed */
    const char *auto_net = reg_get(HIVE_SYSTEM, "Network/AutoStart", "1");
    if (!auto_net || auto_net[0] != '0') {
        pid_t pid = fork();
        if (pid == 0) {
            char st[128];
            net_apply_registry(st, sizeof st);
            _exit(0);
        }
    }
}

/* ================= Native RFC 1035 UDP DNS Resolver ================= */

static int dns_skip_name(const uint8_t *pkt, size_t len, size_t off) {
    int jumps = 0;
    while (off < len && jumps < 16) {
        uint8_t c = pkt[off];
        if (c == 0) return (int)(off + 1);
        if ((c & 0xC0) == 0xC0) return off + 2 <= len ? (int)(off + 2) : -1;
        off += 1 + c;
        jumps++;
    }
    return -1;
}

static int dns_query_one(const char *server_ip, const char *host, char *ip_out, size_t ip_len) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    if (!inet_aton(server_ip, &sa.sin_addr)) return -1;

    uint8_t q[512];
    memset(q, 0, sizeof q);
    uint16_t id = (uint16_t)(mono_ms() ^ 0xA12Fu);
    q[0] = (uint8_t)(id >> 8); q[1] = (uint8_t)(id & 0xFF);
    q[2] = 0x01; q[3] = 0x00; /* RD = 1 */
    q[4] = 0x00; q[5] = 0x01; /* QDCOUNT = 1 */

    size_t o = 12;
    const char *p = host;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63 || o + l + 6 >= sizeof q) return -1;
        q[o++] = (uint8_t)l;
        memcpy(&q[o], p, l);
        o += l;
        if (!dot) break;
        p = dot + 1;
    }
    q[o++] = 0;
    q[o++] = 0; q[o++] = 1; /* QTYPE = A */
    q[o++] = 0; q[o++] = 1; /* QCLASS = IN */

    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (sendto(s, q, o, 0, (struct sockaddr *)&sa, sizeof sa) < 0) {
        close(s);
        return -1;
    }

    struct pollfd pf = { s, POLLIN, 0 };
    if (poll(&pf, 1, 1800) <= 0) { close(s); return -1; }

    uint8_t r[512];
    ssize_t n = recv(s, r, sizeof r, 0);
    close(s);
    if (n < 12 || r[0] != q[0] || r[1] != q[1]) return -1;
    if ((r[3] & 0x0F) != 0) return -1;

    int qd = (r[4] << 8) | r[5];
    int an = (r[6] << 8) | r[7];
    int pos = 12;
    for (int i = 0; i < qd; i++) {
        pos = dns_skip_name(r, (size_t)n, (size_t)pos);
        if (pos < 0 || pos + 4 > n) return -1;
        pos += 4;
    }
    for (int i = 0; i < an; i++) {
        pos = dns_skip_name(r, (size_t)n, (size_t)pos);
        if (pos < 0 || pos + 10 > n) return -1;
        uint16_t rtype = (uint16_t)((r[pos] << 8) | r[pos + 1]);
        uint16_t rdlen = (uint16_t)((r[pos + 8] << 8) | r[pos + 9]);
        pos += 10;
        if (pos + rdlen > n) return -1;
        if (rtype == 1 && rdlen == 4) {
            snprintf(ip_out, ip_len, "%u.%u.%u.%u", r[pos], r[pos + 1], r[pos + 2], r[pos + 3]);
            return 0;
        }
        pos += rdlen;
    }
    return -1;
}

int net_resolve(const char *host, char *ip_out, size_t ip_len) {
    if (!host || !host[0]) return -1;
    struct in_addr in;
    if (inet_aton(host, &in)) {
        snprintf(ip_out, ip_len, "%s", inet_ntoa(in));
        return 0;
    }
    if (!strcasecmp(host, "localhost") || !strcasecmp(host, "magnesium")) {
        snprintf(ip_out, ip_len, "127.0.0.1");
        return 0;
    }
    FILE *hf = fopen("/etc/hosts", "r");
    if (hf) {
        char line[200];
        while (fgets(line, sizeof line, hf)) {
            char ip[48], h1[64], h2[64] = "";
            if (line[0] == '#' || sscanf(line, "%47s %63s %63s", ip, h1, h2) < 2) continue;
            if (!strcasecmp(host, h1) || (h2[0] && !strcasecmp(host, h2))) {
                fclose(hf);
                snprintf(ip_out, ip_len, "%s", ip);
                return 0;
            }
        }
        fclose(hf);
    }

    char primary[24];
    net_get_dns(primary, sizeof primary);
    const char *servers[4] = { primary, "10.0.2.3", "1.1.1.1", "8.8.8.8" };
    for (int i = 0; i < 4; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) if (!strcmp(servers[i], servers[j])) dup = 1;
        if (!dup && dns_query_one(servers[i], host, ip_out, ip_len) == 0) return 0;
    }
    return -1;
}

/* ================= Ping (ICMP Echo + TCP RTT Fallback) ================= */

static int tcp_connect_timeout(const char *ip, int port, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_aton(ip, &sa.sin_addr);

    if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0) return s;
    if (errno != EINPROGRESS) { close(s); return -1; }

    struct pollfd pf = { s, POLLOUT, 0 };
    if (poll(&pf, 1, timeout_ms) <= 0) { close(s); return -1; }
    int so_err = 0;
    socklen_t sl = sizeof so_err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &sl) < 0 || so_err != 0) {
        close(s);
        return -1;
    }
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
    return s;
}

int net_ping_once(const char *host, int seq, char *ip_out, size_t ip_len,
                  int *ms_out, int *tcp_fallback, char *err, size_t errlen) {
    if (tcp_fallback) *tcp_fallback = 0;
    char ip[24];
    if (net_resolve(host, ip, sizeof ip) < 0) {
        if (err) snprintf(err, errlen, "cannot resolve %s", host);
        return -1;
    }
    if (ip_out) snprintf(ip_out, ip_len, "%s", ip);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    inet_aton(ip, &dst.sin_addr);

    int raw = 0;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (s < 0) { s = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP); raw = 1; }

    if (s >= 0) {
        uint8_t pkt[64];
        memset(pkt, 0x4D, sizeof pkt);
        struct icmphdr *ic = (struct icmphdr *)pkt;
        memset(ic, 0, sizeof *ic);
        ic->type = ICMP_ECHO;
        ic->un.echo.id = htons((uint16_t)(getpid() & 0xFFFF));
        ic->un.echo.sequence = htons((uint16_t)seq);
        ic->checksum = ip_checksum(pkt, sizeof pkt);

        uint64_t t0 = mono_ms();
        if (sendto(s, pkt, sizeof pkt, 0, (struct sockaddr *)&dst, sizeof dst) >= 0) {
            struct pollfd pf = { s, POLLIN, 0 };
            if (poll(&pf, 1, 1100) > 0) {
                uint8_t rbuf[256];
                ssize_t n = recv(s, rbuf, sizeof rbuf, 0);
                if (n >= (ssize_t)sizeof(struct icmphdr)) {
                    size_t off = raw ? ((rbuf[0] & 0x0F) * 4u) : 0;
                    if (n >= (ssize_t)(off + sizeof(struct icmphdr))) {
                        const struct icmphdr *ric = (const struct icmphdr *)(rbuf + off);
                        if (ric->type == ICMP_ECHOREPLY) {
                            close(s);
                            if (ms_out) *ms_out = (int)(mono_ms() - t0);
                            return 0;
                        }
                    }
                }
            }
        }
        close(s);
    }

    /* Fallback: TCP handshake RTT probe on port 80 (works through QEMU SLIRP NAT) */
    uint64_t t0 = mono_ms();
    int ts = tcp_connect_timeout(ip, 80, 1800);
    if (ts >= 0) {
        if (ms_out) *ms_out = (int)(mono_ms() - t0);
        if (tcp_fallback) *tcp_fallback = 1;
        close(ts);
        return 0;
    }

    if (err) snprintf(err, errlen, "request timed out to %s", ip);
    return -1;
}

/* ================= Native HTTP/1.1 Client & HTML Reader ================= */

static void decode_chunked_inplace(char *s) {
    char *r = s, *w = s;
    for (;;) {
        char *eol = strstr(r, "\r\n");
        if (!eol) break;
        unsigned long chunk = strtoul(r, NULL, 16);
        r = eol + 2;
        if (chunk == 0) break;
        size_t avail = strlen(r);
        if (chunk > avail) chunk = avail;
        memmove(w, r, chunk);
        w += chunk;
        r += chunk;
        if (r[0] == '\r' && r[1] == '\n') r += 2;
    }
    *w = 0;
}

int net_http_get(const char *url, char *out, size_t max_out,
                 int *http_status, char *err, size_t errlen) {
    if (!url || !out || max_out < 16) return -1;
    out[0] = 0;
    if (http_status) *http_status = 0;

    char cur_url[384];
    snprintf(cur_url, sizeof cur_url, "%s", url);
    if (strncasecmp(cur_url, "http://", 7) != 0 && strncasecmp(cur_url, "https://", 8) != 0)
        snprintf(cur_url, sizeof cur_url, "http://%s", url);

    for (int redir = 0; redir < 4; redir++) {
        if (!strncasecmp(cur_url, "https://", 8)) {
            if (err) snprintf(err, errlen, "HTTPS TLS requires a TLS library; try http:// URL");
            return -1;
        }
        const char *p = cur_url + (strncasecmp(cur_url, "http://", 7) == 0 ? 7 : 0);
        const char *slash = strchr(p, '/');
        char hostport[160], path[256];
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen == 0 || hlen >= sizeof hostport) {
            if (err) snprintf(err, errlen, "invalid URL host");
            return -1;
        }
        memcpy(hostport, p, hlen);
        hostport[hlen] = 0;
        snprintf(path, sizeof path, "%s", slash ? slash : "/");

        char host[128];
        int port = 80;
        char *col = strchr(hostport, ':');
        if (col) {
            *col = 0;
            port = atoi(col + 1);
            if (port <= 0 || port > 65535) port = 80;
        }
        snprintf(host, sizeof host, "%.127s", hostport);

        char ip[24];
        if (net_resolve(host, ip, sizeof ip) < 0) {
            if (err) snprintf(err, errlen, "DNS lookup failed for %s", host);
            return -1;
        }

        int s = tcp_connect_timeout(ip, port, 4000);
        if (s < 0) {
            if (err) snprintf(err, errlen, "cannot connect to %s:%d (%s)", host, port, ip);
            return -1;
        }

        struct timeval tv = { 4, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        char req[640];
        int rlen = snprintf(req, sizeof req,
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s\r\n"
                            "User-Agent: MagnesiumOS/v.A1\r\n"
                            "Accept: text/html,text/plain,*/*\r\n"
                            "Connection: close\r\n\r\n",
                            path, host);
        if (send(s, req, (size_t)rlen, 0) < 0) {
            close(s);
            if (err) snprintf(err, errlen, "send failed");
            return -1;
        }

        size_t cap = max_out + 4096;
        char *raw = malloc(cap);
        if (!raw) { close(s); return -1; }
        size_t total = 0;
        while (total + 1 < cap) {
            ssize_t n = recv(s, raw + total, cap - 1 - total, 0);
            if (n <= 0) break;
            total += (size_t)n;
        }
        close(s);
        raw[total] = 0;

        char *hdr_end = strstr(raw, "\r\n\r\n");
        if (!hdr_end) {
            snprintf(out, max_out, "%s", raw);
            free(raw);
            return (int)strlen(out);
        }
        *hdr_end = 0;
        char *body = hdr_end + 4;
        int code = 0;
        sscanf(raw, "HTTP/%*s %d", &code);
        if (http_status) *http_status = code;

        if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308)) {
            char *loc = strcasestr(raw, "\nLocation:");
            if (loc) {
                loc += 10;
                while (*loc == ' ' || *loc == '\t') loc++;
                char *le = strpbrk(loc, "\r\n");
                if (le) *le = 0;
                if (loc[0] == '/') snprintf(cur_url, sizeof cur_url, "http://%s:%d%s", host, port, loc);
                else snprintf(cur_url, sizeof cur_url, "%s", loc);
                free(raw);
                continue;
            }
        }

        int chunked = strcasestr(raw, "Transfer-Encoding: chunked") != NULL;
        size_t blen = total - (size_t)(body - raw);
        if (blen >= max_out) blen = max_out - 1;
        memcpy(out, body, blen);
        out[blen] = 0;
        if (chunked) decode_chunked_inplace(out);
        free(raw);
        return (int)strlen(out);
    }

    if (err) snprintf(err, errlen, "too many redirects");
    return -1;
}

void net_html_to_text(const char *html, char *out, size_t max_out) {
    if (!html || !out || max_out == 0) return;
    size_t w = 0;
    int in_tag = 0, in_skip = 0, last_sp = 1, last_nl = 1;
    for (size_t i = 0; html[i] && w + 2 < max_out; i++) {
        if (!in_tag && html[i] == '<') {
            if (!strncasecmp(&html[i], "<script", 7) || !strncasecmp(&html[i], "<style", 6)) in_skip = 1;
            else if (!strncasecmp(&html[i], "</script", 8) || !strncasecmp(&html[i], "</style", 7)) in_skip = 0;
            else if (!in_skip && (!strncasecmp(&html[i], "<br", 3) || !strncasecmp(&html[i], "<p", 2) ||
                     !strncasecmp(&html[i], "</p", 3) || !strncasecmp(&html[i], "<div", 4) ||
                     !strncasecmp(&html[i], "<li", 3) || !strncasecmp(&html[i], "<h1", 3) ||
                     !strncasecmp(&html[i], "<h2", 3) || !strncasecmp(&html[i], "<h3", 3) ||
                     !strncasecmp(&html[i], "</h", 3) || !strncasecmp(&html[i], "<tr", 3))) {
                if (!last_nl && w + 1 < max_out) { out[w++] = '\n'; last_nl = 1; last_sp = 1; }
                if (!strncasecmp(&html[i], "<li", 3) && w + 3 < max_out) {
                    out[w++] = ' '; out[w++] = '-'; out[w++] = ' '; last_sp = 1; last_nl = 0;
                }
            }
            in_tag = 1;
            continue;
        }
        if (in_tag) { if (html[i] == '>') in_tag = 0; continue; }
        if (in_skip) continue;

        char c = html[i];
        if (c == '&') {
            if (!strncasecmp(&html[i], "&amp;", 5)) { c = '&'; i += 4; }
            else if (!strncasecmp(&html[i], "&lt;", 4)) { c = '<'; i += 3; }
            else if (!strncasecmp(&html[i], "&gt;", 4)) { c = '>'; i += 3; }
            else if (!strncasecmp(&html[i], "&quot;", 6)) { c = '"'; i += 5; }
            else if (!strncasecmp(&html[i], "&nbsp;", 6)) { c = ' '; i += 5; }
            else if (!strncasecmp(&html[i], "&#39;", 5)) { c = '\''; i += 4; }
        }
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            if (!last_sp) { out[w++] = ' '; last_sp = 1; }
        } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
            out[w++] = c;
            last_sp = 0;
            last_nl = 0;
        }
    }
    out[w] = 0;
}

/* ================= Hardware & Driver Discovery ================= */

static const char *pci_vendor_name(unsigned int v) {
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1022: case 0x1002: return "AMD";
    case 0x10de: return "NVIDIA";
    case 0x1af4: return "VirtIO";
    case 0x1234: return "QEMU/Bochs";
    case 0x1b36: return "QEMU RedHat";
    case 0x10ec: return "Realtek";
    case 0x15ad: return "VMware";
    case 0x80ee: return "VirtualBox";
    case 0x1414: return "Hyper-V";
    case 0x1013: return "Cirrus Logic";
    default: return NULL;
    }
}

static void pci_class_info(unsigned int cls, const char **cat_out, const char **kind_out) {
    unsigned int hi = (cls >> 8) & 0xFFFFu;
    switch (hi) {
    case 0x0300: case 0x0302: case 0x0380: *cat_out = "GPU"; *kind_out = "Display Controller"; return;
    case 0x0200: *cat_out = "Network"; *kind_out = "Ethernet Controller"; return;
    case 0x0280: *cat_out = "Network"; *kind_out = "Network Controller"; return;
    case 0x0108: *cat_out = "Storage"; *kind_out = "NVMe SSD Controller"; return;
    case 0x0106: *cat_out = "Storage"; *kind_out = "SATA AHCI Controller"; return;
    case 0x0101: *cat_out = "Storage"; *kind_out = "IDE Disk Controller"; return;
    case 0x0100: *cat_out = "Storage"; *kind_out = "SCSI / VirtIO Storage"; return;
    case 0x0c03: *cat_out = "USB";     *kind_out = "USB Host Controller"; return;
    case 0x0403: case 0x0401: *cat_out = "Audio"; *kind_out = "Audio Controller"; return;
    case 0x0600: *cat_out = "System";  *kind_out = "Host Bridge"; return;
    case 0x0601: *cat_out = "System";  *kind_out = "ISA Bridge"; return;
    default:     *cat_out = "System";  *kind_out = "PCI Device"; return;
    }
}

int drv_scan(struct hw_driver_item *out, int max_out) {
    if (!out || max_out <= 0) return 0;
    int n = 0;

    /* 1. Active Display Adapter (shows Basic Display fallback even when PCI GPU has no KMS driver) */
    struct gfx_info gi;
    if (gfx_probe(&gi) != GFX_NONE && n < max_out) {
        struct hw_driver_item *it = &out[n++];
        snprintf(it->category, sizeof it->category, "GPU");
        snprintf(it->slot, sizeof it->slot, "%s", gi.devnode);
        snprintf(it->desc, sizeof it->desc, "%s (%dx%d)", gi.desc, gi.width, gi.height);
        snprintf(it->driver, sizeof it->driver, "%s", gi.driver);
        it->active = 1;
    }

    /* 2. PCI Bus Devices */
    DIR *dp = opendir("/sys/bus/pci/devices");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) != NULL && n < max_out) {
            if (e->d_name[0] == '.') continue;
            char base[320], vs[32], ds[32], cs[32], drv[32];
            snprintf(base, sizeof base, "/sys/bus/pci/devices/%s", e->d_name);
            char p[360];
            snprintf(p, sizeof p, "%s/vendor", base); read_one_line(p, vs, sizeof vs);
            snprintf(p, sizeof p, "%s/device", base); read_one_line(p, ds, sizeof ds);
            snprintf(p, sizeof p, "%s/class",  base); read_one_line(p, cs, sizeof cs);
            read_driver_link(base, drv, sizeof drv);

            unsigned int vid = (unsigned int)strtoul(vs, NULL, 0);
            unsigned int did = (unsigned int)strtoul(ds, NULL, 0);
            unsigned int cls = (unsigned int)strtoul(cs, NULL, 0);
            const char *cat = "System", *kind = "PCI Device";
            pci_class_info(cls, &cat, &kind);
            /* Skip noisy ISA/Host bridge lines unless space permits; keep all real peripherals */
            if (!strcmp(cat, "System") && ((cls >> 8) == 0x0680 || (cls >> 8) == 0x0601)) continue;

            struct hw_driver_item *it = &out[n++];
            snprintf(it->category, sizeof it->category, "%s", cat);
            snprintf(it->slot, sizeof it->slot, "%.31s", e->d_name + (strncmp(e->d_name, "0000:", 5) == 0 ? 5 : 0));
            const char *vn = pci_vendor_name(vid);
            if (vn) snprintf(it->desc, sizeof it->desc, "%s %s [%04x:%04x]", vn, kind, vid, did);
            else    snprintf(it->desc, sizeof it->desc, "%s [%04x:%04x]", kind, vid, did);

            if (drv[0]) {
                snprintf(it->driver, sizeof it->driver, "%s", drv);
                it->active = 1;
            } else if (!strcmp(cat, "GPU") && gi.backend == GFX_BASIC_DISPLAY) {
                snprintf(it->driver, sizeof it->driver, "%s (fallback)", gi.driver);
                it->active = 1;
            } else {
                snprintf(it->driver, sizeof it->driver, "unbound");
                it->active = 0;
            }
        }
        closedir(dp);
    }

    /* 3. Input Devices (Keyboards, Mice, USB Tablets) */
    dp = opendir("/sys/class/input");
    if (dp) {
        struct dirent *e;
        while ((e = readdir(dp)) != NULL && n < max_out) {
            if (strncmp(e->d_name, "input", 5) != 0) continue;
            char p[320], nm[72];
            snprintf(p, sizeof p, "/sys/class/input/%s/name", e->d_name);
            if (read_one_line(p, nm, sizeof nm) < 0 || !nm[0]) continue;
            if (strstr(nm, "Power Button")) continue;
            struct hw_driver_item *it = &out[n++];
            snprintf(it->category, sizeof it->category, "Input");
            snprintf(it->slot, sizeof it->slot, "%.31s", e->d_name);
            snprintf(it->desc, sizeof it->desc, "%s", nm);
            snprintf(it->driver, sizeof it->driver, "evdev");
            it->active = 1;
        }
        closedir(dp);
    }
    return n;
}

/* ================= Manual Configuration File (/etc/mg.conf & ~/.mg.conf) ================= */

const char *mg_conf_sys_path(char *buf, size_t len) {
    if (access("/data/etc", F_OK) == 0) snprintf(buf, len, "/data/etc/mg.conf");
    else snprintf(buf, len, "/etc/mg.conf");
    return buf;
}

const char *mg_conf_user_path(char *buf, size_t len) {
    const char *h = getenv("HOME");
    if (h && h[0]) snprintf(buf, len, "%s/.mg.conf", h);
    else snprintf(buf, len, "/root/.mg.conf");
    return buf;
}

void mg_conf_ensure_template(void) {
    char p[200];
    mg_conf_sys_path(p, sizeof p);
    if (access(p, F_OK) == 0) return;
    FILE *f = fopen(p, "w");
    if (!f) return;
    fputs("# Magnesium OS - Manual Configuration File (mg.conf)\n"
          "# Uncomment and edit any line below (or create ~/.mg.conf for your user).\n"
          "# Changes apply immediately when saved in Notes or reloaded in Settings.\n\n"
          "# --- Themes, VaporGlass & Smooth Fonts ---\n"
          "# Display/Theme = Magnesium          # Magnesium | Frost | Aurora | Obsidian | Ember | Cobalt | Sakura | Classic\n"
          "# Display/Accent = Sky               # Sky | Orange | Cyan | Emerald | Violet | Gold | Crimson | Mint\n"
          "# Display/Glass = VaporGlass         # VaporGlass | Frosted | Clear | Solid\n"
          "# Display/GlassOpacity = 78          # 40 to 100 (%)\n"
          "# Display/GlassBlur = Medium         # Off | Low | Medium | High\n"
          "# Display/CornerRadius = 10          # 0 to 16 (px)\n"
          "# Display/FontSmoothing = Smooth     # Smooth | Subpixel | Crisp\n"
          "# Desktop/Background = Aurora        # Aurora | Gradient | Nebula | Prism | Grid | Solid\n"
          "# Desktop/Icons = 1                  # 1 = show desktop shortcut icons, 0 = hide\n"
          "# Taskbar/Style = Floating           # Docked | Floating\n\n"
          "# --- Manual Hex Color Overrides (#RRGGBB, leave empty for theme default) ---\n"
          "# Manual/AccentHex =\n"
          "# Manual/DeskTopHex =\n"
          "# Manual/DeskBotHex =\n"
          "# Manual/WindowHex =\n\n"
          "# --- Network Configuration ---\n"
          "# Network/Mode = DHCP                # DHCP | Manual\n"
          "# Network/IP = 10.0.2.15\n"
          "# Network/Netmask = 255.255.255.0\n"
          "# Network/Gateway = 10.0.2.2\n"
          "# Network/DNS = 10.0.2.3\n", f);
    fclose(f);
}

static int parse_conf_into_hive(const char *path, int hive) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int applied = 0;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == '\r' || *s == '\n' || !*s) continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = s, *v = eq + 1;
        char *ke = eq - 1;
        while (ke >= k && (*ke == ' ' || *ke == '\t')) *ke-- = 0;
        while (*v == ' ' || *v == '\t') v++;
        /* Strip inline comment unless value starts with '#' (hex color!) */
        for (char *c = v; *c; c++) {
            if (*c == '\r' || *c == '\n') { *c = 0; break; }
            if (*c == '#' && c > v && (c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
        }
        size_t vl = strlen(v);
        while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
        if (k[0]) {
            const char *cur = reg_get(hive, k, "\x01");
            if (strcmp(cur, v) != 0 && reg_set(hive, k, v) == 0) applied++;
        }
    }
    fclose(f);
    return applied;
}

int mg_conf_load_all(void) {
    char p[200];
    int n = 0;
    if (getuid() == 0) n += parse_conf_into_hive(mg_conf_sys_path(p, sizeof p), HIVE_SYSTEM);
    n += parse_conf_into_hive(mg_conf_user_path(p, sizeof p), HIVE_USER);
    return n;
}

/* ================= Shell Builtin Commands ================= */

int cmd_ip(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "up") || !strcmp(argv[1], "down")) && argc >= 3) {
        if (net_if_updown(argv[2], !strcmp(argv[1], "up")) < 0) {
            fprintf(stderr, "ip: %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "set")) {
        const char *ifn = argv[2];
        const char *ip  = argv[3];
        const char *msk = argc >= 5 ? argv[4] : "255.255.255.0";
        const char *gw  = argc >= 6 ? argv[5] : "";
        const char *dns = argc >= 7 ? argv[6] : "";
        char err[96] = "";
        if (net_set_static(ifn, ip, msk, gw, dns, err, sizeof err) < 0) {
            fprintf(stderr, "ip set: %s\n", err);
            return 1;
        }
        if (getuid() == 0) {
            reg_set(HIVE_SYSTEM, "Network/Mode", "Manual");
            reg_set(HIVE_SYSTEM, "Network/Interface", ifn);
            reg_set(HIVE_SYSTEM, "Network/IP", ip);
            reg_set(HIVE_SYSTEM, "Network/Netmask", msk);
            if (gw[0]) reg_set(HIVE_SYSTEM, "Network/Gateway", gw);
            if (dns[0]) reg_set(HIVE_SYSTEM, "Network/DNS", dns);
        }
        printf("Configured %s -> %s / %s\n", ifn, ip, msk);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "help")) {
        puts("usage:");
        puts("  ip                                  show network interfaces & drivers");
        puts("  ip set IFACE IP [MASK] [GW] [DNS]   set manual static IPv4");
        puts("  ip up|down IFACE                    enable or disable interface");
        return 0;
    }

    struct net_iface ifs[16];
    int n = net_list_ifaces(ifs, 16);
    if (n == 0) {
        puts("No network interfaces found (check CONFIG_NET / NIC drivers).");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        printf("%-8s  [%s%s]  driver=%s  mac=%s\n",
               ifs[i].name,
               ifs[i].up ? "UP" : "DOWN",
               ifs[i].running ? ",LOWER_UP" : "",
               ifs[i].driver, ifs[i].mac);
        printf("          inet %-15s  netmask %-15s", ifs[i].ip, ifs[i].netmask);
        if (ifs[i].gateway[0]) printf("  gw %s", ifs[i].gateway);
        if (!ifs[i].loopback) printf("  dns %s", ifs[i].dns);
        printf("\n          RX %llu bytes  TX %llu bytes\n", ifs[i].rx_bytes, ifs[i].tx_bytes);
    }
    return 0;
}

int cmd_dhcp(int argc, char **argv) {
    const char *ifn = argc >= 2 ? argv[1] : NULL;
    char st[160];
    if (net_dhcp(ifn, st, sizeof st) < 0) {
        fprintf(stderr, "dhcp: %s\n", st);
        return 1;
    }
    if (getuid() == 0) reg_set(HIVE_SYSTEM, "Network/Mode", "DHCP");
    printf("DHCP lease acquired: %s\n", st);
    return 0;
}

int cmd_dns(int argc, char **argv) {
    if (argc < 2) {
        char d[32];
        net_get_dns(d, sizeof d);
        printf("Active DNS server: %s\nusage: dns HOSTNAME | dns set NAMESERVER_IP\n", d);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        net_set_dns(argv[2]);
        if (getuid() == 0) reg_set(HIVE_SYSTEM, "Network/DNS", argv[2]);
        printf("DNS server set to %s\n", argv[2]);
        return 0;
    }
    char ip[32];
    if (net_resolve(argv[1], ip, sizeof ip) < 0) {
        fprintf(stderr, "dns: could not resolve %s\n", argv[1]);
        return 1;
    }
    printf("%s -> %s\n", argv[1], ip);
    return 0;
}

int cmd_ping(int argc, char **argv) {
    if (argc < 2) { fputs("usage: ping HOST [COUNT]\n", stderr); return 1; }
    int count = argc >= 3 ? atoi(argv[2]) : 4;
    if (count < 1) count = 1;
    if (count > 20) count = 20;

    char ip[32] = "";
    if (net_resolve(argv[1], ip, sizeof ip) < 0) {
        fprintf(stderr, "ping: cannot resolve %s\n", argv[1]);
        return 1;
    }
    printf("PING %s (%s):\n", argv[1], ip);
    int ok = 0;
    for (int i = 1; i <= count; i++) {
        int ms = 0, tcp_fb = 0;
        char err[96] = "";
        if (net_ping_once(ip, i, NULL, 0, &ms, &tcp_fb, err, sizeof err) == 0) {
            printf("  reply from %s: seq=%d time=%d ms%s\n",
                   ip, i, ms, tcp_fb ? " (TCP-80 probe)" : "");
            ok++;
        } else {
            printf("  seq=%d: %s\n", i, err);
        }
        if (i < count) usleep(350000);
    }
    printf("--- %s: %d/%d received ---\n", argv[1], ok, count);
    return ok > 0 ? 0 : 1;
}

int cmd_fetch(int argc, char **argv) {
    if (argc < 2) {
        fputs("usage: fetch [-t] URL [OUTFILE]\n"
              "  -t   convert HTML tags into clean readable text\n", stderr);
        return 1;
    }
    int as_text = 0, ai = 1;
    if (ai < argc && !strcmp(argv[ai], "-t")) { as_text = 1; ai++; }
    if (ai >= argc) { fputs("usage: fetch [-t] URL [OUTFILE]\n", stderr); return 1; }
    const char *url = argv[ai];
    const char *outfile = ai + 1 < argc ? argv[ai + 1] : NULL;

    size_t cap = 131072;
    char *body = malloc(cap);
    if (!body) return 1;
    int code = 0;
    char err[128] = "";
    int n = net_http_get(url, body, cap, &code, err, sizeof err);
    if (n < 0) {
        fprintf(stderr, "fetch: %s\n", err);
        free(body);
        return 1;
    }
    if (outfile) {
        FILE *f = fopen(outfile, "wb");
        if (!f) { fprintf(stderr, "fetch: %s: %s\n", outfile, strerror(errno)); free(body); return 1; }
        fwrite(body, 1, (size_t)n, f);
        fclose(f);
        printf("HTTP %d: saved %d bytes to %s\n", code, n, outfile);
        free(body);
        return 0;
    }
    if (as_text) {
        char *txt = malloc(cap);
        if (txt) {
            net_html_to_text(body, txt, cap);
            puts(txt);
            free(txt);
        }
    } else {
        fwrite(body, 1, (size_t)n, stdout);
        if (n > 0 && body[n - 1] != '\n') putchar('\n');
    }
    free(body);
    return 0;
}

static void print_proc_net_table(const char *proto, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    if (!fgets(line, sizeof line, f)) { fclose(f); return; }
    while (fgets(line, sizeof line, f)) {
        unsigned int sl = 0, la = 0, lp = 0, ra = 0, rp = 0, st = 0;
        if (sscanf(line, " %u: %x:%x %x:%x %x", &sl, &la, &lp, &ra, &rp, &st) == 6) {
            struct in_addr in_l = { la }, in_r = { ra };
            char ls[32], rs[32];
            snprintf(ls, sizeof ls, "%s:%u", inet_ntoa(in_l), lp);
            snprintf(rs, sizeof rs, "%s:%u", inet_ntoa(in_r), rp);
            printf("%-5s %-22s %-22s state=0x%02x\n", proto, ls, rs, st);
        }
    }
    fclose(f);
}

int cmd_netstat(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("PROTO LOCAL                  REMOTE                 STATE");
    print_proc_net_table("tcp", "/proc/net/tcp");
    print_proc_net_table("udp", "/proc/net/udp");
    return 0;
}

int cmd_drivers(int argc, char **argv) {
    (void)argc; (void)argv;
    struct hw_driver_item list[64];
    int n = drv_scan(list, 64);
    if (n == 0) {
        puts("No hardware devices enumerated (check /sys mount).");
        return 0;
    }
    printf("%-8s  %-14s  %-20s  %s\n", "CLASS", "SLOT/DEV", "DRIVER", "HARDWARE");
    puts("--------------------------------------------------------------------------------");
    for (int i = 0; i < n; i++) {
        printf("%-8s  %-14s  %-20s  %s\n",
               list[i].category, list[i].slot, list[i].driver, list[i].desc);
    }
    return 0;
}

int cmd_lspci(int argc, char **argv) {
    (void)argc; (void)argv;
    DIR *dp = opendir("/sys/bus/pci/devices");
    if (!dp) { puts("lspci: /sys/bus/pci/devices not available"); return 1; }
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char base[320], p[360], vs[32], ds[32], cs[32], drv[32];
        snprintf(base, sizeof base, "/sys/bus/pci/devices/%s", e->d_name);
        snprintf(p, sizeof p, "%s/vendor", base); read_one_line(p, vs, sizeof vs);
        snprintf(p, sizeof p, "%s/device", base); read_one_line(p, ds, sizeof ds);
        snprintf(p, sizeof p, "%s/class",  base); read_one_line(p, cs, sizeof cs);
        read_driver_link(base, drv, sizeof drv);
        unsigned int vid = (unsigned int)strtoul(vs, NULL, 0);
        unsigned int did = (unsigned int)strtoul(ds, NULL, 0);
        unsigned int cls = (unsigned int)strtoul(cs, NULL, 0);
        const char *cat = "", *kind = "";
        pci_class_info(cls, &cat, &kind);
        const char *vn = pci_vendor_name(vid);
        printf("%s  %-24s  [%04x:%04x]  %s%s  driver=%s\n",
               e->d_name, kind, vid, did, vn ? vn : "", vn ? " " : "",
               drv[0] ? drv : "(none)");
    }
    closedir(dp);
    return 0;
}

int cmd_theme(int argc, char **argv) {
    static const char *themes = "Magnesium | Frost | Aurora | Obsidian | Ember | Cobalt | Sakura | Classic";
    if (argc < 2 || !strcmp(argv[1], "list")) {
        printf("Active theme:     %s\n", reg_eff("Display/Theme", "Magnesium"));
        printf("VaporGlass mode:  %s (opacity %s%%, blur %s)\n",
               reg_eff("Display/Glass", "VaporGlass"),
               reg_eff("Display/GlassOpacity", "78"),
               reg_eff("Display/GlassBlur", "Medium"));
        printf("Font rendering:   %s\n", reg_eff("Display/FontSmoothing", "Smooth"));
        printf("Available themes: %s\n\n", themes);
        puts("Usage:");
        puts("  theme set NAME                      choose a theme preset");
        puts("  theme glass VaporGlass|Frosted|Clear|Solid");
        puts("  theme font Smooth|Subpixel|Crisp");
        puts("  theme manual KEY=VALUE [...]        e.g. theme manual accent=#00E5FF opacity=75 radius=12");
        puts("  theme reload                        reload ~/.mg.conf & /etc/mg.conf");
        return 0;
    }
    if (!strcmp(argv[1], "set") && argc >= 3) {
        if (reg_set(HIVE_USER, "Display/Theme", argv[2]) < 0) { perror("theme set"); return 1; }
        /* Clear manual color overrides when switching to a named theme preset */
        reg_del(HIVE_USER, "Manual/DeskTopHex");
        reg_del(HIVE_USER, "Manual/DeskBotHex");
        reg_del(HIVE_USER, "Manual/WindowHex");
        printf("Theme set to '%s'\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "glass") && argc >= 3) {
        reg_set(HIVE_USER, "Display/Glass", argv[2]);
        printf("Glass mode set to '%s'\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "font") && argc >= 3) {
        reg_set(HIVE_USER, "Display/FontSmoothing", argv[2]);
        printf("Font rendering set to '%s'\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "reload")) {
        int n = mg_conf_load_all();
        printf("Reloaded manual configuration (%d setting%s updated).\n", n, n == 1 ? "" : "s");
        return 0;
    }
    if (!strcmp(argv[1], "manual")) {
        for (int i = 2; i < argc; i++) {
            char kv[160];
            snprintf(kv, sizeof kv, "%s", argv[i]);
            char *eq = strchr(kv, '=');
            if (!eq) continue;
            *eq = 0;
            const char *k = kv, *v = eq + 1;
            if (!strcasecmp(k, "accent")) reg_set(HIVE_USER, "Manual/AccentHex", v);
            else if (!strcasecmp(k, "desktop")) reg_set(HIVE_USER, "Manual/DeskTopHex", v);
            else if (!strcasecmp(k, "deskbot")) reg_set(HIVE_USER, "Manual/DeskBotHex", v);
            else if (!strcasecmp(k, "window")) reg_set(HIVE_USER, "Manual/WindowHex", v);
            else if (!strcasecmp(k, "opacity")) reg_set(HIVE_USER, "Display/GlassOpacity", v);
            else if (!strcasecmp(k, "blur")) reg_set(HIVE_USER, "Display/GlassBlur", v);
            else if (!strcasecmp(k, "radius")) reg_set(HIVE_USER, "Display/CornerRadius", v);
            else if (!strcasecmp(k, "glass")) reg_set(HIVE_USER, "Display/Glass", v);
            else if (!strcasecmp(k, "font")) reg_set(HIVE_USER, "Display/FontSmoothing", v);
            else reg_set(HIVE_USER, k, v);
        }
        puts("Manual theme overrides saved.");
        return 0;
    }
    fprintf(stderr, "theme: unknown subcommand '%s' (try 'theme list')\n", argv[1]);
    return 1;
}
