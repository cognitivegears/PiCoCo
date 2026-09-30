#include "net.h"
#include "ring.h"
#include "log.h"
#include "plat.h"
#include <stdint.h>
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/netif.h"
#include "lwip/apps/sntp.h"
#include "dw.h"
#include "hardware/watchdog.h"
#include <string.h>
#include <stdio.h>

net_stats_t net_stats;
static char s_ssid[NET_SSID_MAX + 1], s_psk[NET_PSK_MAX + 1], s_host[NET_HOST_MAX + 1];
static uint16_t s_port = NET_DEFAULT_PORT;
static net_state_t s_state = NET_OFF;
static const char *s_err = "";
static bool s_radio, s_wanted;            /* s_wanted: net_start called, not net_stop */
static uint32_t s_since_ms, s_retry_at_ms;
static struct tcp_pcb *s_pcb;
static ip_addr_t s_addr;
static bool s_addr_ok;
static uint8_t up_buf[1024], down_buf[1024];
static ring_t up, down;                   /* up: CoCo -> server, down: server -> CoCo */
static char s_ip[16] = "0.0.0.0";
static uint32_t s_dns_gen;                /* bumped on teardown: lwIP cannot cancel a lookup */
static uint8_t s_err_kind;                /* first-failure-of-a-kind logging: bit per reason index */

static const char *const reasons[] = { "no such network", "bad password", "dhcp timeout", "dns failed", "refused", "link lost", "not configured", "join rejected" };
static void fail(int reason_idx, uint32_t now_ms) {
    s_err = reasons[reason_idx];
    if (!(s_err_kind & (1u << reason_idx))) { s_err_kind |= (1u << reason_idx); LOG_I(LOG_M_NET, "net: %s", s_err); }
    else LOG_D(LOG_M_NET, "net: %s (retry %u)", s_err, (unsigned)net_stats.retries);
    net_stats.retries++;
    s_state = NET_FAILED;
    s_retry_at_ms = now_ms + NET_RETRY_MS;
}

/* true when it had to tcp_abort (pcb freed; a callback must then return ERR_ABRT) */
static bool pcb_drop(void) {
    bool aborted = false;
    if (s_pcb) { tcp_arg(s_pcb, NULL); tcp_recv(s_pcb, NULL); tcp_err(s_pcb, NULL); if (tcp_close(s_pcb) != ERR_OK) { tcp_abort(s_pcb); aborted = true; } s_pcb = NULL; }
    return aborted;
}

static void on_err(void *arg, err_t err) {
    (void)arg; (void)err;
    s_pcb = NULL;                         /* lwIP has already freed it */
    if (s_state == NET_UP) fail(5, plat_now_ms()); else fail(4, plat_now_ms());
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (!p) { bool ab = pcb_drop(); fail(5, plat_now_ms()); return ab ? ERR_ABRT : ERR_OK; }   /* server closed */
    for (struct pbuf *q = p; q; q = q->next)
        for (uint16_t i = 0; i < q->len; i++)
            if (ring_push(&down, ((uint8_t *)q->payload)[i])) net_stats.bytes_down++; else net_stats.overrun++;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)pcb;
    if (err != ERR_OK) { fail(4, plat_now_ms()); return ERR_OK; }
    s_state = NET_UP;
    s_err = "";
    LOG_I(LOG_M_NET, "net up %s -> %s:%u", s_ip, s_host, s_port);
    return ERR_OK;
}

static void on_dns(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name;
    if ((uintptr_t)arg != s_dns_gen || s_state != NET_CONNECTING) return;   /* stale lookup */
    if (addr) { s_addr = *addr; s_addr_ok = true; } else fail(3, plat_now_ms());
}

/* Every entry into CONNECTING: link is up. Fresh rings so bytes queued before a drop
 * never open the next connection; SNTP starts once per boot, on the link, not the socket. */
static void ip_refresh(void) {
    snprintf(s_ip, sizeof s_ip, "%s", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
    ring_init(&up, up_buf, 1024); ring_init(&down, down_buf, 1024);
    static bool sntp_started;
    if (!sntp_started) { sntp_started = true; sntp_setoperatingmode(SNTP_OPMODE_POLL); sntp_setservername(0, SNTP_SERVER_ADDRESS); sntp_init(); }
}

static void connect_start(uint32_t now_ms) {
    s_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!s_pcb) { fail(4, now_ms); return; }
    tcp_arg(s_pcb, NULL);
    tcp_recv(s_pcb, on_recv);
    tcp_err(s_pcb, on_err);
    tcp_nagle_disable(s_pcb);
    if (tcp_connect(s_pcb, &s_addr, s_port, on_connected) != ERR_OK) { pcb_drop(); fail(4, now_ms); }
}

static void join_start(uint32_t now_ms) {
    s_state = NET_JOINING;
    s_since_ms = now_ms;
    s_addr_ok = false;
    uint32_t auth = s_psk[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    if (cyw43_arch_wifi_connect_async(s_ssid, s_psk[0] ? s_psk : NULL, auth) != 0) fail(7, now_ms);   /* SDK argument error */
}

/* Socket, association and rings; the state is left to the caller. */
static void teardown(void) {
    s_dns_gen++;
    pcb_drop();
    if (s_radio) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    ring_init(&up, up_buf, 1024); ring_init(&down, down_buf, 1024);
    strcpy(s_ip, "0.0.0.0");
}

bool net_available(void) { return true; }
void net_init(void) {
    ring_init(&up, up_buf, 1024); ring_init(&down, down_buf, 1024);
    s_radio = cyw43_arch_init() == 0;
    if (!s_radio) { LOG_E(LOG_M_NET, "net: cyw43 init failed"); return; }
    cyw43_arch_enable_sta_mode();
}
int net_set_ssid(const char *s) { if (!s[0] || strlen(s) > NET_SSID_MAX) return -1; strcpy(s_ssid, s); return 0; }
int net_set_psk(const char *s) { if (strlen(s) > NET_PSK_MAX) return -1; strcpy(s_psk, s); return 0; }
int net_set_server(const char *h, uint16_t p) { if (!h[0] || strlen(h) > NET_HOST_MAX) return -1; strcpy(s_host, h); s_port = p; return 0; }
void net_forget(void) { net_stop(); s_ssid[0] = s_psk[0] = s_host[0] = 0; s_port = NET_DEFAULT_PORT; }
bool net_configured(void) { return s_ssid[0] && s_host[0]; }

int net_start(void) {
    if (!s_radio) return -1;
    if (!net_configured()) { s_err = reasons[6]; return -1; }
    teardown();                           /* safe when already started (becker net twice) */
    s_wanted = true;
    s_err_kind = 0;
    s_err = "";
    join_start(plat_now_ms());
    return 0;
}
void net_stop(void) {
    s_wanted = false;
    teardown();
    s_state = NET_OFF;
}

void net_poll(uint32_t now_ms) {
    if (!s_radio) return;
    cyw43_arch_poll();
    if (!s_wanted) return;
    switch (s_state) {
        case NET_JOINING: {
            int ls = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (ls == CYW43_LINK_UP) {
                cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 200, 1, 1, 10));
                ip_refresh();
                s_state = NET_CONNECTING;
                s_since_ms = now_ms;
                s_addr_ok = false;
                err_t r = dns_gethostbyname(s_host, &s_addr, on_dns, (void *)(uintptr_t)s_dns_gen);
                if (r == ERR_OK) { s_addr_ok = true; }
                else if (r != ERR_INPROGRESS) fail(3, now_ms);
            } else if (ls == CYW43_LINK_NONET) fail(0, now_ms);
            else if (ls == CYW43_LINK_BADAUTH) fail(1, now_ms);
            else if (ls == CYW43_LINK_FAIL) fail(0, now_ms);
            else if (now_ms - s_since_ms > NET_JOIN_TIMEOUT_MS) fail(2, now_ms);
            break;
        }
        case NET_CONNECTING:
            if (s_addr_ok && !s_pcb) connect_start(now_ms);
            else if (now_ms - s_since_ms > NET_JOIN_TIMEOUT_MS) { pcb_drop(); fail(4, now_ms); }
            break;
        case NET_UP: {
            if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) { pcb_drop(); fail(5, now_ms); break; }
            if (!s_pcb) break;            /* on_err/on_recv already dropped it and failed */
            /* Bytes leave the ring only once tcp_write accepted them; ERR_MEM just retries next poll. */
            size_t room = tcp_sndbuf(s_pcb);
            if (tcp_sndqueuelen(s_pcb) >= TCP_SND_QUEUELEN - 2) room = 0;
            size_t n = ring_count(&up);
            if (n > room) n = room;
            if (n > 256) n = 256;
            if (n) {
                uint8_t chunk[256], b;
                for (size_t i = 0; i < n; i++) chunk[i] = up.buf[(up.tail + i) & up.mask];
                err_t r = tcp_write(s_pcb, chunk, n, TCP_WRITE_FLAG_COPY);
                if (r == ERR_OK) { for (size_t i = 0; i < n; i++) ring_pop(&up, &b); net_stats.bytes_up += n; tcp_output(s_pcb); }
                else if (r == ERR_MEM) tcp_output(s_pcb);
                else { pcb_drop(); fail(5, now_ms); }
            } else tcp_output(s_pcb);
            break;
        }
        case NET_FAILED:
            if ((int32_t)(now_ms - s_retry_at_ms) >= 0) {
                int ls = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                if (ls == CYW43_LINK_UP) { ip_refresh(); s_state = NET_CONNECTING; s_since_ms = now_ms; s_addr_ok = false; pcb_drop();
                    err_t r = dns_gethostbyname(s_host, &s_addr, on_dns, (void *)(uintptr_t)s_dns_gen);
                    if (r == ERR_OK) s_addr_ok = true; else if (r != ERR_INPROGRESS) fail(3, now_ms); }
                else join_start(now_ms);
            }
            break;
        default: break;
    }
}

net_state_t net_state(void) { return s_state; }
const char *net_state_name(net_state_t s) {
    switch (s) { case NET_JOINING: return "joining"; case NET_CONNECTING: return "connecting";
                 case NET_UP: return "up"; case NET_FAILED: return "failed"; default: return "off"; }
}
const char *net_last_error(void) { return s_err; }
const char *net_ssid(void) { return s_ssid; }
const char *net_host(void) { return s_host; }
uint16_t net_port(void) { return s_port; }
bool net_psk_set(void) { return s_psk[0] != 0; }
const char *net_psk_plain(void) { return s_psk; }
const char *net_ip(void) { return s_ip; }
size_t net_read(uint8_t *buf, size_t n) { size_t i = 0; while (i < n && ring_pop(&down, &buf[i])) i++; return i; }
size_t net_write_free(void) { return s_state == NET_UP ? ring_free(&up) : 1024; }   /* net_write eats bytes when not up */
size_t net_write(const uint8_t *buf, size_t n) {
    if (s_state != NET_UP) return n;      /* bridge rule: never let the mode pump spin on a dead link */
    size_t i = 0; while (i < n && ring_push(&up, buf[i])) i++; return i;
}

typedef struct { void (*cb)(const char *, int, int, void *); void *ctx; } scan_ctx_t;
static int scan_cb(void *env, const cyw43_ev_scan_result_t *r) {
    scan_ctx_t *c = env;
    if (r) { char name[33]; snprintf(name, sizeof name, "%.*s", (int)r->ssid_len, (const char *)r->ssid); if (name[0]) c->cb(name, r->rssi, r->channel, c->ctx); }
    return 0;
}
int net_scan(void (*cb)(const char *, int, int, void *), void *ctx) {
    if (!s_radio) return -1;
    scan_ctx_t c = { cb, ctx };
    cyw43_wifi_scan_options_t opt = { 0 };
    if (cyw43_wifi_scan(&cyw43_state, &opt, &c, scan_cb) != 0) return -1;
    absolute_time_t t = make_timeout_time_ms(5000);
    while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(t)) { cyw43_arch_poll(); watchdog_update(); sleep_ms(20); }
    return 0;
}

extern dw_server *net_dw;   /* set by main.c: the server whose clock SNTP updates */
dw_server *net_dw;
static bool s_sntp_applied;
/* SNTP only seeds a stopped clock, once per boot: it is UTC, and must never undo a local-time `time set`. */
void net_sntp_set(uint32_t sec) {
    int64_t cur;
    if (s_sntp_applied || plat_rtc_get(&cur)) return;
    plat_rtc_set((int64_t)sec);
    if (net_dw) dw_time_set(net_dw, (int64_t)sec, plat_now_ms());
    s_sntp_applied = true;
    LOG_I(LOG_M_NET, "net: sntp seeded %u", (unsigned)sec);
}
