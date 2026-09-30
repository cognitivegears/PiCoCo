#include "net.h"
#include "log.h"
#include "pico/cyw43_arch.h"
#include <string.h>

net_stats_t net_stats;
static char s_ssid[NET_SSID_MAX + 1], s_psk[NET_PSK_MAX + 1], s_host[NET_HOST_MAX + 1];
static uint16_t s_port = NET_DEFAULT_PORT;
static net_state_t s_state = NET_OFF;
static const char *s_err = "";
static bool s_radio;

bool net_available(void) { return true; }
void net_init(void) {
    s_radio = cyw43_arch_init() == 0;
    if (!s_radio) LOG_E(LOG_M_MAIN, "net: cyw43 init failed");
    else cyw43_arch_enable_sta_mode();
}
int net_set_ssid(const char *s) { if (!s[0] || strlen(s) > NET_SSID_MAX) return -1; strcpy(s_ssid, s); return 0; }
int net_set_psk(const char *s) { if (strlen(s) > NET_PSK_MAX) return -1; strcpy(s_psk, s); return 0; }
int net_set_server(const char *h, uint16_t p) { if (!h[0] || strlen(h) > NET_HOST_MAX) return -1; strcpy(s_host, h); s_port = p; return 0; }
void net_forget(void) { net_stop(); s_ssid[0] = s_psk[0] = s_host[0] = 0; s_port = NET_DEFAULT_PORT; }
bool net_configured(void) { return s_ssid[0] && s_host[0]; }
int net_start(void) { if (!s_radio || !net_configured()) return -1; s_state = NET_JOINING; return 0; }  /* Task 4 fills this in */
void net_stop(void) { s_state = NET_OFF; }
void net_poll(uint32_t now_ms) { (void)now_ms; if (s_radio) cyw43_arch_poll(); }
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
const char *net_ip(void) { return "0.0.0.0"; }
size_t net_read(uint8_t *buf, size_t n) { (void)buf; (void)n; return 0; }
size_t net_write(const uint8_t *buf, size_t n) { (void)buf; return n; }
int net_scan(void (*cb)(const char *, int, int, void *), void *ctx) { (void)cb; (void)ctx; return -1; }
void net_sntp_set(uint32_t sec) { (void)sec; }
