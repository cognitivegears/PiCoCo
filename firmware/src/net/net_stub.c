#include "net.h"
#include "ring.h"
#include <string.h>
/* No radio here (host build, Pico 2). Config setters still store, so the
 * console/save/replay path is testable on the host; the rings and state let
 * the host tests drive the mode pump through the PICOCO_HOST seam. */
net_stats_t net_stats;
static char ssid[NET_SSID_MAX + 1], psk[NET_PSK_MAX + 1], host[NET_HOST_MAX + 1];
static uint16_t port = NET_DEFAULT_PORT;
static net_state_t st = NET_OFF;
static uint8_t up_buf[1024], down_buf[1024];
static ring_t up, down; /* up: to server; down: from server */
static bool rings_ready;
static void rings(void) { if (!rings_ready) { ring_init(&up, up_buf, 1024); ring_init(&down, down_buf, 1024); rings_ready = true; } }
bool net_available(void) { return false; }
void net_init(void) {}
int net_set_ssid(const char *s) { if (!s[0] || strlen(s) > NET_SSID_MAX) return -1; strcpy(ssid, s); return 0; }
int net_set_psk(const char *s) { if (strlen(s) > NET_PSK_MAX) return -1; strcpy(psk, s); return 0; }
int net_set_server(const char *h, uint16_t p) { if (!h[0] || strlen(h) > NET_HOST_MAX) return -1; strcpy(host, h); port = p; return 0; }
void net_forget(void) { net_stop(); ssid[0] = psk[0] = host[0] = 0; port = NET_DEFAULT_PORT; }
bool net_configured(void) { return ssid[0] && host[0]; }
int net_start(void) { return -1; }
void net_stop(void) { rings_ready = false; rings(); st = NET_OFF; }
void net_poll(uint32_t now_ms) { (void)now_ms; }
net_state_t net_state(void) { return st; }
const char *net_state_name(net_state_t s) {
    switch (s) { case NET_JOINING: return "joining"; case NET_CONNECTING: return "connecting";
                 case NET_UP: return "up"; case NET_FAILED: return "failed"; default: return "off"; }
}
const char *net_last_error(void) { return ""; }
const char *net_ssid(void) { return ssid; }
const char *net_host(void) { return host; }
uint16_t net_port(void) { return port; }
bool net_psk_set(void) { return psk[0] != 0; }
const char *net_psk_plain(void) { return psk; }
const char *net_ip(void) { return "0.0.0.0"; }
size_t net_read(uint8_t *buf, size_t n) { rings(); size_t i = 0; while (i < n && ring_pop(&down, &buf[i])) i++; return i; }
size_t net_write(const uint8_t *buf, size_t n) {
    rings();
    if (st != NET_UP) return n;   /* bridge rule: never let the mode pump spin */
    size_t i = 0; while (i < n && ring_push(&up, buf[i])) i++; return i;
}
int net_scan(void (*cb)(const char *, int, int, void *), void *ctx) { (void)cb; (void)ctx; return -1; }
#ifdef PICOCO_HOST
void net_stub_set_state(net_state_t s) { st = s; }
void net_stub_push_from_server(const uint8_t *b, size_t n) { rings(); for (size_t i = 0; i < n; i++) ring_push(&down, b[i]); }
size_t net_stub_take_to_server(uint8_t *b, size_t n) { rings(); size_t i = 0; while (i < n && ring_pop(&up, &b[i])) i++; return i; }
#endif
