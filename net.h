/*
 * Magnesium OS - networking stack (DHCP, static IP, UDP DNS, ICMP/TCP ping,
 * HTTP/1.1 client), hardware driver discovery, and manual config loader.
 * Original code by alxa, MIT licensed.
 */
#ifndef MG_NET_H
#define MG_NET_H

#include <stddef.h>
#include <stdint.h>

struct net_iface {
    char name[24];           /* "eth0", "ens3", "lo" */
    char driver[32];         /* "virtio_net", "e1000", "r8169", "loopback" */
    char mac[20];            /* "52:54:00:12:34:56" */
    char ip[20];             /* "10.0.2.15" or "0.0.0.0" */
    char netmask[20];        /* "255.255.255.0" */
    char gateway[20];        /* "10.0.2.2" */
    char dns[20];            /* "10.0.2.3" */
    int up;                  /* IFF_UP */
    int running;             /* IFF_RUNNING / carrier */
    int loopback;            /* IFF_LOOPBACK */
    unsigned long long rx_bytes, tx_bytes;
};

struct hw_driver_item {
    char category[16];       /* "GPU", "Network", "Storage", "USB", "Input", "System" */
    char slot[32];           /* "0000:00:02.0", "usb1", "input1" */
    char desc[128];          /* Human-readable hardware model/class */
    char driver[48];         /* Bound kernel driver ("virtio_gpu", "e1000", "simpledrm", "unbound") */
    int active;              /* 1 if driver is bound/active */
};

/* Boot initialization: loopback up, ping_group_range, background DHCP/manual setup */
void net_boot_init(void);

/* Enumerate network interfaces (returns count, up to max_out) */
int net_list_ifaces(struct net_iface *out, int max_out);

/* Pick primary non-loopback interface name (returns 0 on success) */
int net_default_iface(char *name_out, size_t len);

/* Bring interface up/down */
int net_if_updown(const char *ifname, int up);

/* Configure manual static IPv4 + netmask + optional gateway & DNS */
int net_set_static(const char *ifname, const char *ip, const char *mask,
                   const char *gw, const char *dns, char *err, size_t errlen);

/* Run RFC 2131 DHCPv4 client on ifname (returns 0 on lease acquired) */
int net_dhcp(const char *ifname, char *status_out, size_t status_len);

/* Apply Network registry settings (DHCP or Manual) */
int net_apply_registry(char *status_out, size_t status_len);

/* Pure-C UDP DNS A-record resolver (works in static builds without libnss) */
int net_resolve(const char *host, char *ip_out, size_t ip_len);
void net_get_dns(char *dns_out, size_t len);
void net_set_dns(const char *dns_ip);

/* Single ping probe (ICMP echo with TCP-80 RTT fallback behind user-mode NAT) */
int net_ping_once(const char *host, int seq, char *ip_out, size_t ip_len,
                  int *ms_out, int *tcp_fallback, char *err, size_t errlen);

/* HTTP/1.1 GET client (returns body byte length >= 0 on success, -1 on error) */
int net_http_get(const char *url, char *out, size_t max_out,
                 int *http_status, char *err, size_t errlen);

/* Convert raw HTML into clean readable plain text */
void net_html_to_text(const char *html, char *out, size_t max_out);

/* Enumerate PCI, USB, GPU, Network, Storage, and Input hardware + bound drivers */
int drv_scan(struct hw_driver_item *out, int max_out);

/* Manual configuration file helpers (/etc/mg.conf, /data/etc/mg.conf, ~/.mg.conf) */
void mg_conf_ensure_template(void);
int  mg_conf_load_all(void);
const char *mg_conf_user_path(char *buf, size_t len);
const char *mg_conf_sys_path(char *buf, size_t len);

/* Shell commands */
int cmd_ip(int argc, char **argv);
int cmd_dhcp(int argc, char **argv);
int cmd_ping(int argc, char **argv);
int cmd_dns(int argc, char **argv);
int cmd_fetch(int argc, char **argv);
int cmd_netstat(int argc, char **argv);
int cmd_drivers(int argc, char **argv);
int cmd_lspci(int argc, char **argv);
int cmd_theme(int argc, char **argv);

#endif
