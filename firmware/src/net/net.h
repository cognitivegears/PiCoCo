#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* WiFi + one TCP client to a DriveWire server (spec 2026-09-29). Plus-W only:
 * net.c; the host build and the Pico 2 link net_stub.c, where everything
 * reports "no radio". All of it runs on core0. */
typedef enum { NET_OFF, NET_JOINING, NET_CONNECTING, NET_UP, NET_FAILED } net_state_t;

#define NET_SSID_MAX 32
#define NET_PSK_MAX  63
#define NET_HOST_MAX 64
#define NET_DEFAULT_PORT 65504
#define NET_BOOT_HOLD_MS 10000
#define NET_RETRY_MS 2000
#define NET_JOIN_TIMEOUT_MS 15000

typedef struct {
    uint32_t bytes_up, bytes_down, overrun, retries;
} net_stats_t;
extern net_stats_t net_stats;

bool   net_available(void);                    /* false on the host and the Pico 2 */
void   net_init(void);                         /* cyw43_arch_init; radio idle. No-op without a radio. */
int    net_set_ssid(const char *s);            /* 0 ok, -1 too long/empty */
int    net_set_psk(const char *s);             /* "" clears (open network) */
int    net_set_server(const char *host, uint16_t port);
void   net_forget(void);                       /* clear config, net_stop */
bool   net_configured(void);                   /* ssid and host stored */
int    net_start(void);                        /* -1 not configured / no radio; else begins joining */
void   net_stop(void);                         /* close socket, leave, NET_OFF, clear rings */
void   net_poll(uint32_t now_ms);              /* drive cyw43 + state machine + retries */
net_state_t net_state(void);
const char *net_state_name(net_state_t s);     /* "off","joining","connecting","up","failed" */
const char *net_last_error(void);              /* "" or one of the spec's reason strings */
const char *net_ssid(void);
const char *net_host(void);
uint16_t net_port(void);
bool   net_psk_set(void);
const char *net_ip(void);                      /* dotted quad while up, else "0.0.0.0" */
size_t net_read(uint8_t *buf, size_t n);       /* from-server ring */
size_t net_write(const uint8_t *buf, size_t n);/* to-server ring; returns n when not up */
int    net_scan(void (*cb)(const char *ssid, int rssi, int chan, void *ctx), void *ctx); /* blocking, <= 5 s; -1 no radio */
