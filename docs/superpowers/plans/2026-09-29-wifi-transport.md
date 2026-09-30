# WiFi DriveWire Transport (Plus-W) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Plus-W board reaches a DriveWire server (DW4, picoco-host or FujiNet) over WiFi through a new `becker net` mode, configured from the USB console and from the CoCo manager, with a boot hold and native fallback.

**Architecture:** Polled lwIP (`pico_cyw43_arch_lwip_poll`) on core0 only. A new `net.c` owns the WiFi/TCP state machine and two rings, exposing `net_read`/`net_write` shaped like the bridge hooks; `MODE_NET` in `mode.c` is the bridge case with those swapped in; `main.c` adds `net_poll` to its loop and a bounded wait before the /HALT release. core1 is untouched. Everything is behind `PICOCO_HAVE_NET` (Plus-W); the host build and the Pico 2 link a stub.

**Tech Stack:** C11, pico-sdk 2.3.1 (`pico_cyw43_arch_lwip_poll`, `pico_lwip_sntp`), lwIP raw TCP API, CMOC for the CoCo side, host tests via the repo's `test.h` + ctest.

**Spec:** `docs/superpowers/specs/2026-09-29-wifi-transport-design.md`

## Global Constraints

- Host build first: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host && ctest --test-dir build-host --output-on-failure` must stay green after every task.
- Both Pico targets must build after every firmware task: `ninja -C build-pico` (Pico 2) and `ninja -C build-pico-plusw` (Plus-W, configured with `-DPICOCO_BOARD=plusw -DPICO_SDK_PATH=/Users/cognitivegears/projects/pico-sdk`). Both run `check_core1_flash_free.py` as a POST_BUILD step; it must print `ok`.
- core1 stays flash-free and untouched. Nothing in this plan adds `BUS_HOT` code.
- All net code is behind `#ifdef PICOCO_HAVE_NET`, defined only in `firmware/boards/plusw.h`. The Pico 2 build must not reference cyw43 or lwIP.
- Radio pins (verified): WL_REG_ON 36, WL_DATA_OUT/IN/HOST_WAKE 37, WL_CS 38, WL_CLOCK 39, LED on WL_GPIO0, VBUS on WL_GPIO2, VSYS pin 46, `CYW43_USES_VSYS_PIN 0`.
- WiFi power save must be off after join: `cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 200, 1, 1, 10))`.
- Server default port 65504. Boot hold cap 10000 ms. Retry period 2000 ms. Join+DHCP timeout 15000 ms. Watchdog is 8 s: any wait loop calls `watchdog_update()`.
- Rings 1 KB each way. `net_write` returns `n` when not up (bridge rule, so the mode pump never spins).
- Config lines written by `save`, in this order, before the `becker` line: `net join <ssid>`, `net psk <psk>` (only if set), `net server <host> <port>`.
- `net status`/`status`/logs never print the PSK.
- Console verbs on the remote allowlist: `net` (all sub-commands). `becker` is already allowed? No: `becker` is NOT on the allowlist today and stays off; the manager toggles mode through the new `net mode net|native` sub-command instead (allowed).
- Manager keys are plain letters, case-folded with `upcase()` (see `coco/ui.c`), never SHIFT chords.
- Commits go on `pcb-v2.3` in a worktree branch `wifi` (executor creates it with the using-git-worktrees skill). Commit after each task with the message given.

## Review Focus

1. `picoco.cfg` written by an older firmware or edited by hand may put `becker net` before the `net server` line, or omit the server. Expected: replay logs `config: becker net` as an error and the board runs native; nothing hangs. Test in Task 3 (`becker_net_refused_without_config`).
2. An SSID or passphrase containing spaces, or a trailing space typed on the CoCo. Expected: stored exactly as typed minus the trailing space (raw tail semantics match `dw disk insert`). Test in Task 3 (`net_join_keeps_spaces`).
3. Server host given as a name that never resolves, or an IP with the server down. Expected: `net status` shows `dns failed` or `refused`, retries every 2 s, the CoCo boots native after the 10 s hold. Device check in Task 5 step 8; reason strings pinned by the host test in Task 4 (`net_stub_reason_strings`).
4. `net forget` or `net join` while the socket is up with bytes queued. Expected: mode drops to native first, rings are cleared, no stale server bytes reach the CoCo later. Test in Task 4 (`mode_net_drops_to_native_on_reconfigure`).
5. Manager network screen on a Pico 2. Expected: `NO RADIO ON THIS BOARD`, BREAK only, no crash on the refusal text. Test in Task 7 (`t_net_status_refused` in `test_parse.c`) and the on-device check in Task 7 step 9.

---

### Task 1: Board header, CMake wiring and the net stub (no behaviour yet)

**Files:**
- Modify: `firmware/boards/picoco_plusw.h` (append before the final `#endif`-free tail; the file has no include guard body to close)
- Modify: `firmware/boards/plusw.h`
- Modify: `firmware/CMakeLists.txt`
- Create: `firmware/src/net/net.h`, `firmware/src/net/net_stub.c`, `firmware/src/net/net.c`, `firmware/src/net/lwipopts.h`

**Interfaces:**
- Produces: `net.h` below. Every later task codes against it. The stub is what the host build and the Pico 2 link; `net.c` is Plus-W only and, in this task, only initialises the radio.

- [ ] **Step 1: Add the CYW43 block to the Plus-W SDK board header**

Append to `firmware/boards/picoco_plusw.h`:

```c
// --- CYW43 (Raspberry Pi RM2 on the Waveshare module; pins from the
// Waveshare schematic, verified with a live scan 2026-09-29) ---
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)
#ifndef CYW43_DEFAULT_PIN_WL_REG_ON
#define CYW43_DEFAULT_PIN_WL_REG_ON 36
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_OUT
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 37
#endif
#ifndef CYW43_DEFAULT_PIN_WL_DATA_IN
#define CYW43_DEFAULT_PIN_WL_DATA_IN 37
#endif
#ifndef CYW43_DEFAULT_PIN_WL_HOST_WAKE
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 37
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CLOCK
#define CYW43_DEFAULT_PIN_WL_CLOCK 39
#endif
#ifndef CYW43_DEFAULT_PIN_WL_CS
#define CYW43_DEFAULT_PIN_WL_CS 38
#endif
#ifndef CYW43_WL_GPIO_COUNT
#define CYW43_WL_GPIO_COUNT 3
#endif
#ifndef CYW43_WL_GPIO_LED_PIN
#define CYW43_WL_GPIO_LED_PIN 0
#endif
#ifndef CYW43_WL_GPIO_VBUS_PIN
#define CYW43_WL_GPIO_VBUS_PIN 2
#endif
#ifndef CYW43_USES_VSYS_PIN
#define CYW43_USES_VSYS_PIN 0
#endif
#ifndef PICO_VSYS_PIN
#define PICO_VSYS_PIN 46
#endif
```

- [ ] **Step 2: Mark the Plus-W pin header as having a radio**

In `firmware/boards/plusw.h`, after `#define PICOCO_BOARD_PLUSW 1` add:

```c
#define PICOCO_HAVE_NET 1   /* RM2 radio: firmware/src/net/net.c is compiled in */
```

- [ ] **Step 3: Write `net.h`**

Create `firmware/src/net/net.h`:

```c
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
```

- [ ] **Step 4: Write the stub**

Create `firmware/src/net/net_stub.c`:

```c
#include "net.h"
#include <string.h>
/* No radio here (host build, Pico 2). Config setters still store, so the
 * console/save/replay path is testable on the host. */
net_stats_t net_stats;
static char ssid[NET_SSID_MAX + 1], psk[NET_PSK_MAX + 1], host[NET_HOST_MAX + 1];
static uint16_t port = NET_DEFAULT_PORT;
bool net_available(void) { return false; }
void net_init(void) {}
int net_set_ssid(const char *s) { if (!s[0] || strlen(s) > NET_SSID_MAX) return -1; strcpy(ssid, s); return 0; }
int net_set_psk(const char *s) { if (strlen(s) > NET_PSK_MAX) return -1; strcpy(psk, s); return 0; }
int net_set_server(const char *h, uint16_t p) { if (!h[0] || strlen(h) > NET_HOST_MAX) return -1; strcpy(host, h); port = p; return 0; }
void net_forget(void) { ssid[0] = psk[0] = host[0] = 0; port = NET_DEFAULT_PORT; }
bool net_configured(void) { return ssid[0] && host[0]; }
int net_start(void) { return -1; }
void net_stop(void) {}
void net_poll(uint32_t now_ms) { (void)now_ms; }
net_state_t net_state(void) { return NET_OFF; }
const char *net_state_name(net_state_t s) {
    switch (s) { case NET_JOINING: return "joining"; case NET_CONNECTING: return "connecting";
                 case NET_UP: return "up"; case NET_FAILED: return "failed"; default: return "off"; }
}
const char *net_last_error(void) { return ""; }
const char *net_ssid(void) { return ssid; }
const char *net_host(void) { return host; }
uint16_t net_port(void) { return port; }
bool net_psk_set(void) { return psk[0] != 0; }
const char *net_ip(void) { return "0.0.0.0"; }
size_t net_read(uint8_t *buf, size_t n) { (void)buf; (void)n; return 0; }
size_t net_write(const uint8_t *buf, size_t n) { (void)buf; return n; }
int net_scan(void (*cb)(const char *, int, int, void *), void *ctx) { (void)cb; (void)ctx; return -1; }
```

- [ ] **Step 5: Write the first `net.c` (radio init only) and `lwipopts.h`**

Create `firmware/src/net/lwipopts.h` (the SDK examples' common options, trimmed to raw TCP, DHCP, DNS, SNTP, IPv4):

```c
#pragma once
#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    4000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_MSS                     1460
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_STATS                  0
#define LWIP_DEBUG                  0
/* SNTP: one server by name, hands the time to net.c (Task 6). */
#define SNTP_SERVER_DNS             1
#define SNTP_SERVER_ADDRESS         "pool.ntp.org"
void net_sntp_set(uint32_t sec);
#define SNTP_SET_SYSTEM_TIME(sec)   net_sntp_set(sec)
```

Create `firmware/src/net/net.c` with only the config store and radio init (the state machine comes in Task 4):

```c
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
```

`LOG_M_MAIN` exists in `log.h`; add a `LOG_M_NET` module in Task 4 when logging gets real.

- [ ] **Step 6: CMake**

In `firmware/CMakeLists.txt`:

1. `CORE_SRC` stays as is. Add after the `HOST_ONLY_SRC` line:
   ```cmake
   set(NET_STUB_SRC src/net/net_stub.c)
   ```
2. In the Pico branch, after the `add_executable(picoco ...)` call, add:
   ```cmake
   if(PICOCO_BOARD STREQUAL "plusw")
     target_sources(picoco PRIVATE src/net/net.c)
     target_include_directories(picoco PRIVATE src/net)   # lwipopts.h
     target_link_libraries(picoco pico_cyw43_arch_lwip_poll pico_lwip_sntp)
   else()
     target_sources(picoco PRIVATE ${NET_STUB_SRC})
   endif()
   ```
   and add `src/net` to the existing `target_include_directories(picoco PRIVATE ...)` list.
3. In the host branch: `add_library(picoco_core STATIC ${CORE_SRC} ${HOST_ONLY_SRC} ${NET_STUB_SRC} host/plat_host.c)` and add `src/net` to its PUBLIC include list.
4. `PICOCO_VERSION` becomes `"1.3"`.

- [ ] **Step 7: Build all three**

Run:
```bash
cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host && ctest --test-dir build-host --output-on-failure
ninja -C build-pico
ninja -C build-pico-plusw
```
Expected: host green except `version_is_1_2` (fix it to `"version 1.3\n"` in `firmware/tests/test_console.c` and rename the test `version_is_1_3`); both Pico builds link, `check_core1_flash_free: ok` on both. The Plus-W link pulls in lwIP and the cyw43 firmware blob; expect the `.uf2` to grow by roughly 250 KB.

- [ ] **Step 8: Commit**

```bash
git add firmware/boards/picoco_plusw.h firmware/boards/plusw.h firmware/CMakeLists.txt firmware/src/net firmware/tests/test_console.c
git commit -m "firmware: net scaffold: Plus-W radio pins, lwIP link, net.h and stub; version 1.3"
```

---

### Task 2: `bus selftest` primitives exposed for reuse

**Files:**
- Modify: `firmware/src/bus/fake6809.h`, `firmware/src/bus/fake6809.c`

**Interfaces:**
- Produces: `int fake6809_begin(void)` (guards + pin takeover + `bus drive on`; returns -2 if the bus is live, -1 no SM), `uint8_t fake6809_cycle(uint16_t addr, bool rd, uint8_t data)` (one selected cycle with `SAMPLE_LATE`), `void fake6809_end(void)` (release pins, restore drive). Task 6 builds the net self-test on these.

- [ ] **Step 1: Declare them**

Append to `firmware/src/bus/fake6809.h`:

```c
#include <stdbool.h>
/* Building blocks for other on-device self-tests (net_selftest.c): the same
 * guards and pin takeover fake6809_selftest uses, one selected cycle at a
 * time. Never with a CoCo attached. */
int     fake6809_begin(void);                                   /* 0 ok, -1 no PIO SM, -2 refused: bus live */
uint8_t fake6809_cycle(uint16_t addr, bool rd, uint8_t data);   /* rd: byte core1 drove; write: data goes to core1 */
void    fake6809_end(void);
```

- [ ] **Step 2: Implement by factoring the existing selftest's prologue and epilogue**

In `firmware/src/bus/fake6809.c`, add after `pins_idle_10ms`:

```c
static bool s_drive_was;
int fake6809_begin(void) {
    uint32_t c0 = bus_stats.cycles;
    sleep_ms(100);
    if (bus_stats.cycles != c0) return -2;
    if (!pins_idle_10ms()) return -2;
    if (start() < 0) return -1;
    s_drive_was = bus_drive_get();
    bus_drive_set(true);
    { uint16_t di; uint8_t dd; while (bus_pop_write(&di, &dd)) { } }   /* pin-takeover blip, see fake6809_selftest */
    return 0;
}
uint8_t fake6809_cycle(uint16_t addr, bool rd, uint8_t data) { return cycle(addr, rd, true, data, SAMPLE_LATE); }
void fake6809_end(void) { bus_drive_set(s_drive_was); stop(); }
```

`SAMPLE_LATE` is defined above these lines already (`#define SAMPLE_LATE 40`); move the `#define` above `pins_idle_10ms` if it is not.

- [ ] **Step 3: Build both Pico targets and run the existing self-test on the bare Pico 2 if it is on USB**

Run: `ninja -C build-pico && ninja -C build-pico-plusw`. If a bare Pico 2 is attached: `picotool load -x build-pico/picoco.uf2` after `bootsel` on the console, then `python3 firmware/tools/bench.py --port /dev/cu.usbmodem3103`. Expected: `selftest pass`, 13/13. (No board on USB: builds passing is the gate.)

- [ ] **Step 4: Commit**

```bash
git add firmware/src/bus/fake6809.h firmware/src/bus/fake6809.c
git commit -m "firmware: expose fake6809 begin/cycle/end for other self-tests"
```

---

### Task 3: Console `net` verb, `becker net`, save and replay (host-testable)

**Files:**
- Modify: `firmware/src/console/console.c`, `firmware/src/console/mode.h`, `firmware/src/console/mode.c`
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Consumes: `net.h` (Task 1).
- Produces: `MODE_NET` in `picoco_mode`, `mode_name` returns `"net"`; console verb `net` with sub-commands `join|psk|server|forget|scan|status|mode`; `becker net`; `save` line order.

- [ ] **Step 1: Write the failing tests**

Append to `firmware/tests/test_console.c` before `int main`:

```c
TEST(net_join_keeps_spaces) {
    setup();
    ASSERT_EQ(console_exec("net join My Home Net  "), 0);
    ASSERT(strcmp(net_ssid(), "My Home Net") == 0);
    ASSERT_EQ(console_exec("net psk pass word 1"), 0);
    ASSERT(net_psk_set());
    ASSERT_EQ(console_exec("net server dw.local 65505"), 0);
    ASSERT(strcmp(net_host(), "dw.local") == 0);
    ASSERT_EQ(net_port(), 65505);
    ASSERT_EQ(console_exec("net server 10.0.0.5"), 0);
    ASSERT_EQ(net_port(), 65504);
    ASSERT_EQ(console_exec("net join"), -1);          /* usage */
    ASSERT_EQ(console_exec("net server"), -1);
    ASSERT_EQ(console_exec("net server x 70000"), -1);  /* bad port */
}

TEST(becker_net_refused_without_config) {
    setup();
    outn = 0;
    ASSERT_EQ(console_exec("becker net"), -1);
    ASSERT(strstr(out, "set ssid and server first") || strstr(out, "needs Plus-W"));
    ASSERT_EQ(mode_get(), MODE_DIAG);
    console_exec("net join a"); console_exec("net server b");
    outn = 0;
    ASSERT_EQ(console_exec("becker net"), -1);          /* host stub: no radio */
    ASSERT(strstr(out, "needs Plus-W"));
    ASSERT_EQ(mode_get(), MODE_DIAG);
    ASSERT_EQ(console_exec("net mode native"), 0);
    ASSERT_EQ(mode_get(), MODE_NATIVE);
}

TEST(net_status_hides_psk_and_save_order) {
    setup();
    console_exec("net join Lab"); console_exec("net psk s3cret"); console_exec("net server 10.0.0.9 65504");
    outn = 0;
    ASSERT_EQ(console_exec("net status"), 0);
    ASSERT(strstr(out, "net state off\n"));
    ASSERT(strstr(out, "net ssid Lab\n"));
    ASSERT(strstr(out, "net psk set\n"));
    ASSERT(!strstr(out, "s3cret"));
    ASSERT(strstr(out, "net server 10.0.0.9 65504\n"));
    outn = 0;
    ASSERT_EQ(console_exec("net"), 0);                  /* alias of status */
    ASSERT(strstr(out, "net state off\n"));
    ASSERT_EQ(console_exec("save"), 0);
    char cfg[1024]; int n = plat_cfg_read(cfg, sizeof cfg - 1); ASSERT(n > 0); cfg[n] = 0;
    const char *j = strstr(cfg, "net join Lab\n"), *p = strstr(cfg, "net psk s3cret\n"),
               *s = strstr(cfg, "net server 10.0.0.9 65504\n"), *b = strstr(cfg, "becker ");
    ASSERT(j && p && s && b);
    ASSERT(j < p && p < s && s < b);
    console_exec("net forget");
    ASSERT(!net_configured());
    console_exec("save");
    n = plat_cfg_read(cfg, sizeof cfg - 1); cfg[n] = 0;
    ASSERT(!strstr(cfg, "net join"));
}

TEST(net_remote_allowed) {
    setup();
    ASSERT_EQ(remote("net status"), 0);
    ASSERT(strstr(rbuf, "net state off\n"));
    ASSERT_EQ(remote("net join Lab"), 0);
    ASSERT_EQ(remote("net psk x"), 0);
    ASSERT_EQ(remote("net server 1.2.3.4"), 0);
    ASSERT_EQ(remote("net mode native"), 0);
    ASSERT_EQ(remote("becker net"), 255);               /* becker stays console-only */
    ASSERT_EQ(remote("net scan"), 255);                 /* stub: refused as "no radio" -> err */
    ASSERT(strcmp(rbuf, "net: needs Plus-W") == 0);
}
```

Add `#include "net.h"` at the top of the test file, and `RUN(net_join_keeps_spaces); RUN(becker_net_refused_without_config); RUN(net_status_hides_psk_and_save_order); RUN(net_remote_allowed);` before the closing of `main`. Also update the existing `remote_refuses_console_only` deny list: it must still contain `"becker off"`; add `"becker net"` to it.

- [ ] **Step 2: Run to verify they fail**

Run: `ninja -C build-host && ./build-host/test_console 2>&1 | tail -5`
Expected: compile error (`net.h` symbols fine, but `MODE_NET`/`net` verb missing → the tests fail at `console_exec("net ...") == -1 "unknown command"`).

- [ ] **Step 3: `MODE_NET`**

`firmware/src/console/mode.h`: `typedef enum { MODE_DIAG, MODE_LOOP, MODE_BRIDGE, MODE_NATIVE, MODE_NET } picoco_mode;`
`firmware/src/console/mode.c`, `mode_name`: add `case MODE_NET: return "net";`. Add `#include "net.h"` and, in `mode_pump`, the case (the bridge case with the calls swapped):

```c
        case MODE_NET: {
            uint8_t buf[64];
            size_t n = becker_read(buf, sizeof(buf));
            if (n) net_write(buf, n);
            size_t want = becker_tx_free();
            if (want > sizeof(buf)) want = sizeof(buf);
            if (want) {
                size_t got = net_read(buf, want);
                if (got) becker_write(buf, got);
            }
            break;
        }
```

- [ ] **Step 4: Console**

In `firmware/src/console/console.c`:

1. `#include "net.h"`.
2. `cmd_becker`: usage string becomes `"usage: becker off|loop|bridge|native|net"`, add the branch:
   ```c
   else if (strcasecmp(argv[1], "net") == 0) {
       if (!net_available()) return cerr("net: needs Plus-W");
       if (!net_configured()) return cerr("net: set ssid and server first");
       if (net_start() != 0) return cerr("net: start failed");
       m = MODE_NET;
   }
   ```
   and before `mode_set(m)`: `if (mode_get() == MODE_NET && m != MODE_NET) net_stop();`
3. New command, placed after `cmd_becker`:
   ```c
   static void scan_line(const char *ssid, int rssi, int chan, void *ctx) {
       (void)ctx;
       outf("ssid %s rssi %d chan %d\n", ssid, rssi, chan);
   }
   /* Drop out of net mode before touching its config so a live socket is
    * never reconfigured underneath the mode pump. */
   static void net_leave_if_active(void) {
       if (mode_get() == MODE_NET) { net_stop(); mode_set(MODE_NATIVE); }
   }
   static int cmd_net(int argc, char **argv) {
       const char *usage = "usage: net [status]|join <ssid>|psk [<psk>]|server <host> [port]|forget|scan|mode net|native";
       if (argc < 2 || strcasecmp(argv[1], "status") == 0) {
           outf("net state %s\n", net_state_name(net_state()));
           if (net_last_error()[0]) outf("net error %s\n", net_last_error());
           outf("net ssid %s\n", net_ssid());
           outf("net psk %s\n", net_psk_set() ? "set" : "unset");
           outf("net server %s %u\n", net_host(), net_port());
           outf("net ip %s\n", net_ip());
           outf("net bytes up %u down %u overrun %u retries %u\n",
                net_stats.bytes_up, net_stats.bytes_down, net_stats.overrun, net_stats.retries);
           outf("net radio %s\n", net_available() ? "yes" : "no");
           return 0;
       }
       if (strcasecmp(argv[1], "join") == 0) {
           if (argc < 3) return cerr(usage);
           net_leave_if_active();
           return net_set_ssid(raw_tail(2)) == 0 ? 0 : cerr("net: ssid too long");
       }
       if (strcasecmp(argv[1], "psk") == 0) {
           net_leave_if_active();
           return net_set_psk(argc < 3 ? "" : raw_tail(2)) == 0 ? 0 : cerr("net: psk too long");
       }
       if (strcasecmp(argv[1], "server") == 0) {
           if (argc < 3) return cerr(usage);
           long port = argc >= 4 ? atol(argv[3]) : NET_DEFAULT_PORT;
           if (port < 1 || port > 65535) return cerr("net: bad port");
           net_leave_if_active();
           return net_set_server(argv[2], (uint16_t)port) == 0 ? 0 : cerr("net: host too long");
       }
       if (strcasecmp(argv[1], "forget") == 0) { net_leave_if_active(); net_forget(); return 0; }
       if (strcasecmp(argv[1], "scan") == 0) {
           if (!net_available()) return cerr("net: needs Plus-W");
           if (net_scan(scan_line, NULL) != 0) return cerr("net: scan failed");
           return 0;
       }
       if (strcasecmp(argv[1], "mode") == 0) {
           if (argc < 3) return cerr(usage);
           if (strcasecmp(argv[2], "net") == 0) { char *bv[] = { "becker", "net" }; return cmd_becker(2, bv); }
           if (strcasecmp(argv[2], "native") == 0) { char *bv[] = { "becker", "native" }; return cmd_becker(2, bv); }
           return cerr(usage);
       }
       return cerr(usage);
   }
   ```
   `raw_tail` trims leading spaces only; trailing spaces come from the tokenizer's view of the line. Strip them: in `raw_tail`, after the `snprintf`, add `size_t l = strlen(tail); while (l && tail[l-1] == ' ') tail[--l] = 0;`. (`dw disk insert` already tolerates a trailing space via the same helper; the test `disk_insert_name_with_spaces` covers that and must stay green.)
4. Dispatch: `if (strcasecmp(v, "net") == 0) return cmd_net(argc, argv);` next to `becker`. Help line: add `net` after `becker`.
5. `cmd_save`: before the `becker` line:
   ```c
   if (net_ssid()[0] && !cfg_append(cfg, sizeof(cfg), &len, "net join %s\n", net_ssid())) return cerr("config too large");
   if (net_psk_set() && !cfg_append(cfg, sizeof(cfg), &len, "net psk %s\n", net_psk_plain())) return cerr("config too large");
   if (net_host()[0] && !cfg_append(cfg, sizeof(cfg), &len, "net server %s %u\n", net_host(), net_port())) return cerr("config too large");
   ```
   This needs one more accessor that only `save` uses: add `const char *net_psk_plain(void);` to `net.h`, returning the stored string, implemented in both `net.c` and `net_stub.c` (`return psk;`). Nothing else may call it; a comment on the declaration says so.
6. `cmd_status`: after the `becker reads` line add `if (net_available()) outf("net %s %s\n", net_state_name(net_state()), net_ip());`.
7. `remote_allow`: add `"net"` (the bare verb, so every sub-command passes). `becker` is not added.
8. In `cmd_time`'s `time set` branch nothing changes.

- [ ] **Step 5: Run the tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: all green, including the four new tests and the pre-existing `disk_insert_name_with_spaces`, `remote_refuses_console_only`.

- [ ] **Step 6: Build both Pico targets**

Run: `ninja -C build-pico && ninja -C build-pico-plusw`. Expected: both link, flash-free ok.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/console firmware/src/net/net.h firmware/src/net/net.c firmware/src/net/net_stub.c firmware/tests/test_console.c
git commit -m "firmware: net console verb, becker net, MODE_NET pump, save/replay order"
```

---

### Task 4: The real `net.c`: join, DHCP, DNS, connect, rings, retries

**Files:**
- Modify: `firmware/src/net/net.c`, `firmware/src/log.h` (add `LOG_M_NET`), `firmware/src/log.c` (its name in `log_module_names`)
- Test: on-device (bare Plus-W over USB); `firmware/tests/test_net_mode.c` for the ring/mode contract via the stub seam

**Interfaces:**
- Consumes: `net.h`; `ring.h`; `cyw43_arch` and lwIP raw API.
- Produces: the full state machine. Also a test seam in the stub: `void net_stub_push_from_server(const uint8_t *b, size_t n)` and `size_t net_stub_take_to_server(uint8_t *b, size_t n)`, plus `void net_stub_set_state(net_state_t s)`, declared in `net.h` under `#ifdef PICOCO_HOST`.

- [ ] **Step 1: Host test of the mode pump through the stub seam**

Create `firmware/tests/test_net_mode.c`:

```c
#include "test.h"
#include "mode.h"
#include "becker.h"
#include "bus.h"
#include "device.h"
#include "net.h"
#include <string.h>

static dw_server dw;
static void setup(void) {
    bus_init(); device_reset(); becker_init(); device_init_all();
    mode_reset();
    net_forget();
    net_stub_set_state(NET_UP);
    mode_set(MODE_NET);
}

/* Bytes the CoCo writes to $FF42 come out of the to-server side. */
TEST(coco_writes_reach_server) {
    setup();
    for (int i = 0; i < 5; i++) bus_on_write(0x3F42, (uint8_t)(0x50 + i), 0);
    mode_pump(&dw, 0);
    uint8_t got[16];
    size_t n = net_stub_take_to_server(got, sizeof got);
    ASSERT_EQ(n, 5);
    ASSERT(got[0] == 0x50 && got[4] == 0x54);
}

/* Bytes from the server land in the Becker to-coco queue. */
TEST(server_bytes_reach_coco) {
    setup();
    const uint8_t reply[3] = { 1, 2, 3 };
    net_stub_push_from_server(reply, 3);
    mode_pump(&dw, 0);
    ASSERT_EQ(becker_tx_free(), 255 - 3);
}

/* Not up: the pump must not spin or lose the CoCo's bytes forever; net_write
 * reports them consumed (bridge rule) and the ring stays empty. */
TEST(not_up_consumes_quietly) {
    setup();
    net_stub_set_state(NET_CONNECTING);
    bus_on_write(0x3F42, 0x23, 0);
    mode_pump(&dw, 0);
    uint8_t got[4];
    ASSERT_EQ(net_stub_take_to_server(got, sizeof got), 0);
}

TEST(net_stub_reason_strings) {
    ASSERT(strcmp(net_state_name(NET_OFF), "off") == 0);
    ASSERT(strcmp(net_state_name(NET_JOINING), "joining") == 0);
    ASSERT(strcmp(net_state_name(NET_CONNECTING), "connecting") == 0);
    ASSERT(strcmp(net_state_name(NET_UP), "up") == 0);
    ASSERT(strcmp(net_state_name(NET_FAILED), "failed") == 0);
}

int main(void) {
    RUN(coco_writes_reach_server);
    RUN(server_bytes_reach_coco);
    RUN(not_up_consumes_quietly);
    RUN(net_stub_reason_strings);
    return test_report();
}
```

Check `firmware/tests/test.h` for the exact names of `RUN`/`test_report`/`ASSERT_EQ` and match them (other tests use them; copy from `test_becker.c`). `bus_on_write` and `becker_tx_free` are declared in `bus.h`/`becker.h`; `device_dispatch_writes` inside `mode_pump` hands the write to the Becker device.

- [ ] **Step 2: Add the seam to the stub**

`net.h`, at the end:
```c
#ifdef PICOCO_HOST
void   net_stub_set_state(net_state_t s);
void   net_stub_push_from_server(const uint8_t *b, size_t n);
size_t net_stub_take_to_server(uint8_t *b, size_t n);
#endif
```
`net_stub.c`: replace `net_state`, `net_read`, `net_write` with ring-backed versions:
```c
#include "ring.h"
static net_state_t st = NET_OFF;
static uint8_t up_buf[1024], down_buf[1024];
static ring_t up, down;   /* up: to server; down: from server */
static bool rings_ready;
static void rings(void) { if (!rings_ready) { ring_init(&up, up_buf, 1024); ring_init(&down, down_buf, 1024); rings_ready = true; } }
net_state_t net_state(void) { return st; }
size_t net_read(uint8_t *buf, size_t n) { rings(); size_t i = 0; while (i < n && ring_pop(&down, &buf[i])) i++; return i; }
size_t net_write(const uint8_t *buf, size_t n) {
    rings();
    if (st != NET_UP) return n;
    size_t i = 0; while (i < n && ring_push(&up, buf[i])) i++; return i;
}
void net_stub_set_state(net_state_t s) { st = s; }
void net_stub_push_from_server(const uint8_t *b, size_t n) { rings(); for (size_t i = 0; i < n; i++) ring_push(&down, b[i]); }
size_t net_stub_take_to_server(uint8_t *b, size_t n) { rings(); size_t i = 0; while (i < n && ring_pop(&up, &b[i])) i++; return i; }
```
(`ring.h` lives in `firmware/src`; the include path already has it.) `net_stub_set_state` is compiled only for the host (`PICOCO_HOST` is defined by the host CMake branch; check with `grep -n PICOCO_HOST firmware/CMakeLists.txt`, and if it is only a CMake option, add `target_compile_definitions(picoco_core PUBLIC PICOCO_HOST=1)` there).

- [ ] **Step 3: Run the host tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`. Expected: `test_net_mode` green (4/4), everything else unchanged.

- [ ] **Step 4: Log module**

`firmware/src/log.h`: add `LOG_M_NET` before `LOG_M_COUNT`. `firmware/src/log.c`: add `"net"` at the matching index of `log_module_names`. (Check `test_console.c` has no assertion on the module count; `cart_command_and_save` and the `log` tests do not.)

- [ ] **Step 5: The state machine in `net.c`**

Replace the body of `firmware/src/net/net.c` (keep the config accessors from Task 1) with:

```c
#include "net.h"
#include "ring.h"
#include "log.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/apps/sntp.h"
#include "lwip/netif.h"
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
static uint8_t s_err_kind;                /* first-failure-of-a-kind logging: bit per reason index */

static const char *const reasons[] = { "no such network", "bad password", "dhcp timeout", "dns failed", "refused", "link lost", "not configured" };
static void fail(int reason_idx, uint32_t now_ms) {
    s_err = reasons[reason_idx];
    if (!(s_err_kind & (1u << reason_idx))) { s_err_kind |= (1u << reason_idx); LOG_I(LOG_M_NET, "net: %s", s_err); }
    else LOG_D(LOG_M_NET, "net: %s (retry %u)", s_err, net_stats.retries);
    net_stats.retries++;
    s_state = NET_FAILED;
    s_retry_at_ms = now_ms + NET_RETRY_MS;
}

static void pcb_drop(void) {
    if (s_pcb) { tcp_arg(s_pcb, NULL); tcp_recv(s_pcb, NULL); tcp_err(s_pcb, NULL); if (tcp_close(s_pcb) != ERR_OK) tcp_abort(s_pcb); s_pcb = NULL; }
}

static void on_err(void *arg, err_t err) {
    (void)arg; (void)err;
    s_pcb = NULL;                         /* lwIP has already freed it */
    if (s_state == NET_UP) fail(5, 0); else fail(4, 0);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg; (void)err;
    if (!p) { pcb_drop(); fail(5, 0); return ERR_OK; }   /* server closed */
    for (struct pbuf *q = p; q; q = q->next)
        for (uint16_t i = 0; i < q->len; i++)
            if (ring_push(&down, ((uint8_t *)q->payload)[i])) net_stats.bytes_down++; else net_stats.overrun++;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg; (void)pcb;
    if (err != ERR_OK) { fail(4, 0); return ERR_OK; }
    s_state = NET_UP;
    s_err = "";
    LOG_I(LOG_M_NET, "net up %s -> %s:%u", s_ip, s_host, s_port);
    return ERR_OK;
}

static void on_dns(const char *name, const ip_addr_t *addr, void *arg) {
    (void)name; (void)arg;
    if (addr) { s_addr = *addr; s_addr_ok = true; } else fail(3, 0);
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
    if (cyw43_arch_wifi_connect_async(s_ssid, s_psk[0] ? s_psk : NULL, auth) != 0) fail(0, now_ms);
}

bool net_available(void) { return true; }
void net_init(void) {
    ring_init(&up, up_buf, 1024); ring_init(&down, down_buf, 1024);
    s_radio = cyw43_arch_init() == 0;
    if (!s_radio) { LOG_E(LOG_M_NET, "net: cyw43 init failed"); return; }
    cyw43_arch_enable_sta_mode();
}
int net_start(void) {
    if (!s_radio) return -1;
    if (!net_configured()) { s_err = reasons[6]; return -1; }
    s_wanted = true;
    s_err_kind = 0;
    join_start(0);
    return 0;
}
void net_stop(void) {
    s_wanted = false;
    pcb_drop();
    if (s_radio) cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    ring_init(&up, up_buf, 1024); ring_init(&down, down_buf, 1024);
    s_state = NET_OFF;
    strcpy(s_ip, "0.0.0.0");
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
                snprintf(s_ip, sizeof s_ip, "%s", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
                s_state = NET_CONNECTING;
                s_since_ms = now_ms;
                err_t r = dns_gethostbyname(s_host, &s_addr, on_dns, NULL);
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
            uint8_t chunk[256]; size_t n = 0; uint8_t b;
            while (n < sizeof chunk && ring_peek(&up, &b)) { chunk[n++] = b; ring_pop(&up, &b); }
            if (n) {
                if (tcp_write(s_pcb, chunk, n, TCP_WRITE_FLAG_COPY) == ERR_OK) { tcp_output(s_pcb); net_stats.bytes_up += n; }
                else { pcb_drop(); fail(5, now_ms); }
            }
            break;
        }
        case NET_FAILED:
            if ((int32_t)(now_ms - s_retry_at_ms) >= 0) {
                int ls = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
                if (ls == CYW43_LINK_UP) { s_state = NET_CONNECTING; s_since_ms = now_ms; s_addr_ok = false; s_pcb = NULL;
                    err_t r = dns_gethostbyname(s_host, &s_addr, on_dns, NULL);
                    if (r == ERR_OK) s_addr_ok = true; else if (r != ERR_INPROGRESS) fail(3, now_ms); }
                else join_start(now_ms);
            }
            break;
        default: break;
    }
}

net_state_t net_state(void) { return s_state; }
const char *net_last_error(void) { return s_err; }
const char *net_ip(void) { return s_ip; }
size_t net_read(uint8_t *buf, size_t n) { size_t i = 0; while (i < n && ring_pop(&down, &buf[i])) i++; return i; }
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
void net_sntp_set(uint32_t sec) { (void)sec; }   /* Task 6 */
```

Keep the accessor functions (`net_set_ssid`, `net_set_psk`, `net_set_server`, `net_forget`, `net_configured`, `net_state_name`, `net_ssid`, `net_host`, `net_port`, `net_psk_set`, `net_psk_plain`) from Tasks 1 and 3. `net_forget` must call `net_stop()` first (it does). If `pico/cyw43_arch.h` does not pull in `lwip/netif.h` for `netif_ip4_addr`, include `lwip/netif.h`. `cyw43_ev_scan_result_t` fields are `ssid`, `ssid_len`, `rssi`, `channel` (see `lib/cyw43-driver/src/cyw43_ll.h`). `cyw43_wifi_leave` is in `cyw43.h`.

One subtlety: `fail()` is also called from lwIP callbacks with `now_ms = 0`; `s_retry_at_ms` then becomes 2000, which `net_poll` treats as "retry at once if now > 2000 ms since boot". Acceptable: the retry period matters for the steady state, and the first retry after a callback failure being immediate is fine.

- [ ] **Step 6: Build, flash the bare Plus-W, exercise on the console**

Run: `ninja -C build-pico-plusw`, then with the module in BOOTSEL: `picotool load -x build-pico-plusw/picoco.uf2`. Start the server on the Mac: `./build-host/picoco-host --dir /tmp/pc --mount 0=DINORUN.dsk --hdbdos on --port 65504` (any directory with a disk image; this runs sandbox-off). Find the Mac's LAN address with `ipconfig getifaddr en0`. Then on the console (`python3 firmware/tools/pconsole.py /dev/cu.usbmodemXXXX3 ...`):

```
net join <ssid>
net psk <psk>
net server <mac-ip> 65504
becker net
```
wait 5 s, then `net status`. Expected: `net state up`, `net ip 192.168.x.y`, `net server <mac-ip> 65504`, and picoco-host prints `client connected`. `net scan` lists networks. `becker native` then `net status`: `net state off`, picoco-host prints `client disconnected`.

- [ ] **Step 7: Failure paths on the device**

`net server 10.255.255.1 65504`, `becker net`, wait 20 s, `net status`: expected `net state failed`, `net error refused` (or `link lost`), `retries` climbing by one every 2 s. `net join NoSuchNet`, `becker net`, wait: `net error no such network`. `net server nosuch.example.invalid`, `becker net`: `net error dns failed`. Restore the good config after.

- [ ] **Step 8: Build the Pico 2 target too, then commit**

Run: `ninja -C build-pico` (must not reference cyw43; if it does, a header leaked outside `#ifdef PICOCO_HAVE_NET`).

```bash
git add firmware/src/net firmware/src/log.h firmware/src/log.c firmware/tests/test_net_mode.c firmware/CMakeLists.txt
git commit -m "firmware: net.c WiFi join, DHCP, DNS, TCP client with retries; host mode-pump tests via the stub"
```

---

### Task 5: Boot hold and native fallback in `main.c`

**Files:**
- Modify: `firmware/src/main.c`

**Interfaces:**
- Consumes: `net_init`, `net_poll`, `net_state`, `net_last_error`, `net_available`, `mode_get/mode_set`.

- [ ] **Step 1: Wire `net_init` and the hold**

In `main()` after `console_init(...)` add `net_init();`. After the config replay block and before `multicore_launch_core1`, add:

```c
    /* becker net in the config: hold /HALT (already asserted by the boot
     * pull-up) until the server socket is up, at most NET_BOOT_HOLD_MS, so a
     * CoCo never sees a half-connected board. Fall back to native otherwise:
     * the saved config still says net and the next boot tries again. */
    if (mode_get() == MODE_NET) {
        uint32_t t0 = plat_now_ms();
        while (net_state() != NET_UP && plat_now_ms() - t0 < NET_BOOT_HOLD_MS) {
            tud_task();
            net_poll(plat_now_ms());
            watchdog_update();
        }
        if (net_state() == NET_UP) {
            LOG_I(LOG_M_MAIN, "net up in %u ms", plat_now_ms() - t0);
        } else {
            LOG_I(LOG_M_MAIN, "net failed (%s), native fallback", net_last_error()[0] ? net_last_error() : "timeout");
            mode_set(MODE_NATIVE);
        }
    }
```

`net_stop()` is deliberately not called on fallback: the link keeps retrying in the background so `becker net` on the console later succeeds at once, and `net status` shows the state. Add `#include "net.h"`. In the main `for (;;)` loop add `net_poll(now);` right after `mode_pump(&g_dw, now);`.

- [ ] **Step 2: Build both targets**

Run: `ninja -C build-pico && ninja -C build-pico-plusw`. Expected: clean; flash-free ok.

- [ ] **Step 3: Device check: hold and fallback**

Flash the Plus-W. With the good config: `becker net`, `save`, `reboot`. Then `log dump`: expected lines `fs ok, config lines N`, `net up in <ms>` (typically 2000-6000), `core1 up, halt released`, and `status` shows `mode net`. Stop picoco-host, `reboot`, `log dump`: expected `net failed (refused), native fallback` roughly 10 s after `boot`, then `core1 up`; `status` shows `mode native`, `net status` shows `failed` with retries counting. Start picoco-host again; `becker net`; `net status` reaches `up` within a few seconds.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/main.c
git commit -m "firmware: boot hold until the DriveWire socket is up, native fallback after 10 s"
```

---

### Task 6: SNTP clock and the `bus selftest net` end-to-end check

**Files:**
- Modify: `firmware/src/net/net.c` (SNTP), `firmware/CMakeLists.txt` (net_selftest source), `firmware/src/console/console.c` (`bus selftest net`)
- Create: `firmware/src/net/net_selftest.c`, `firmware/src/net/net_selftest.h`

**Interfaces:**
- Consumes: `fake6809_begin/cycle/end` (Task 2), `mode_pump`, `net_poll`, `net_state`.
- Produces: `int net_selftest(void (*line)(const char *s))` — 0 pass, -1 fail, -2 refused (bus live or mode not net/up).

- [ ] **Step 1: SNTP**

In `net.c`: in `on_connected` after `s_state = NET_UP;` add a one-shot start:
```c
    static bool sntp_started;
    if (!sntp_started) { sntp_started = true; sntp_setoperatingmode(SNTP_OPMODE_POLL); sntp_setservername(0, SNTP_SERVER_ADDRESS); sntp_init(); }
```
and replace `net_sntp_set`:
```c
#include "plat.h"
#include "dw.h"
extern dw_server *net_dw;   /* set by main.c: the server whose clock SNTP updates */
dw_server *net_dw;
void net_sntp_set(uint32_t sec) {
    plat_rtc_set((int64_t)sec);
    if (net_dw) dw_time_set(net_dw, (int64_t)sec, plat_now_ms());
    LOG_I(LOG_M_NET, "net: sntp set %u", sec);
}
```
Declare `extern dw_server *net_dw;` in `net.h` under `#ifdef PICOCO_HAVE_NET` and set `net_dw = &g_dw;` in `main.c` right after `net_init()`. The stub does not define it.

- [ ] **Step 2: The net self-test**

Create `firmware/src/net/net_selftest.h`:
```c
#pragma once
int net_selftest(void (*line)(const char *s));   /* 0 pass, -1 fail, -2 refused */
```
Create `firmware/src/net/net_selftest.c`:
```c
#include "net_selftest.h"
#include "net.h"
#include "mode.h"
#include "fake6809.h"
#include "plat.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>

/* Push one DriveWire request through the real path (fake 6809 -> core1 ->
 * Becker ring -> mode pump -> net -> server -> back) and collect the reply.
 * The console runs inside main's loop, so the pumps are driven here. */
static dw_server *dw_of_mode;   /* mode_pump needs the server only in NATIVE; NULL is fine in NET */

static int becker_wait_byte(uint8_t *out, uint32_t timeout_ms) {
    uint32_t t0 = plat_now_ms();
    for (;;) {
        mode_pump(dw_of_mode, plat_now_ms());
        net_poll(plat_now_ms());
        watchdog_update();
        if (fake6809_cycle(0xFF41, true, 0) == 0x02) { *out = fake6809_cycle(0xFF42, true, 0); return 0; }
        if (plat_now_ms() - t0 > timeout_ms) return -1;
    }
}

static int transact(const uint8_t *req, size_t rn, uint8_t *rep, size_t want, uint32_t *rtt_ms) {
    uint32_t t0 = plat_now_ms();
    for (size_t i = 0; i < rn; i++) fake6809_cycle(0xFF42, false, req[i]);
    for (size_t i = 0; i < want; i++) if (becker_wait_byte(&rep[i], 2000) != 0) return -1;
    *rtt_ms = plat_now_ms() - t0;
    return 0;
}

int net_selftest(void (*line)(const char *s)) {
    char buf[96];
    if (mode_get() != MODE_NET || net_state() != NET_UP) return -2;
    int b = fake6809_begin();
    if (b != 0) return b;
    int fails = 0;
    uint8_t rep[8]; uint32_t rtt;
    const uint8_t dwinit[1] = { 0x5A };
    if (transact(dwinit, 1, rep, 1, &rtt) == 0) { snprintf(buf, sizeof buf, "selftest net dwinit -> %02x in %u ms", rep[0], rtt); line(buf); }
    else { line("selftest net dwinit FAIL (no reply)"); fails++; }
    const uint8_t optime[1] = { 0x23 };
    uint32_t worst = 0;
    for (int i = 0; i < 20; i++) {
        if (transact(optime, 1, rep, 6, &rtt) != 0) { line("selftest net time FAIL (no reply)"); fails++; break; }
        if (rtt > worst) worst = rtt;
        if (i == 0) { snprintf(buf, sizeof buf, "selftest net time %02u-%02u-%02u %02u:%02u:%02u in %u ms", rep[0], rep[1], rep[2], rep[3], rep[4], rep[5], rtt); line(buf); }
    }
    snprintf(buf, sizeof buf, "selftest net time worst %u ms over 20", worst); line(buf);
    if (worst > 200) { line("selftest net FAIL: worst round trip over 200 ms (power save?)"); fails++; }
    if (rep[0] < 26 || rep[0] > 99) { line("selftest net FAIL: year out of range"); fails++; }
    fake6809_end();
    return fails ? -1 : 0;
}
```
`OP_TIME` replies six bytes: year-1900, month, day, hour, minute, second (`dw_server.c` `do_time`); DW4 and picoco-host agree on that format.

- [ ] **Step 3: Console and CMake**

`console.c`, in `cmd_bus`'s `selftest` branch, before the existing fake6809 call:
```c
        if (argc >= 3 && strcasecmp(argv[2], "net") == 0) {
#ifdef PICOCO_HAVE_NET
            int rc = net_selftest(selftest_line);
            if (rc == -2) return cerr("bus selftest net: needs becker net with the link up, and no CoCo attached");
            if (rc != 0) return cerr("selftest FAIL");
            outf("selftest net pass\n");
            return 0;
#else
            return cerr("net: needs Plus-W");
#endif
        }
```
with `#ifdef PICOCO_HAVE_NET #include "net_selftest.h" #endif` at the top. In CMake's Plus-W branch add `src/net/net_selftest.c` to the `target_sources`.

- [ ] **Step 4: Build and run on the bare Plus-W**

Flash. With picoco-host running and `becker net` up: `bus selftest net`. Expected:
```
selftest net dwinit -> 04 in N ms
selftest net time 26-09-29 HH:MM:SS in N ms
selftest net time worst N ms over 20
selftest net pass
```
with N in single digits to low tens. Then `time`: expected the SNTP-set value and `clock kept`.

- [ ] **Step 5: Host build unchanged, Pico 2 build unchanged**

Run: `ninja -C build-host && ctest --test-dir build-host && ninja -C build-pico`. Expected green.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/net firmware/src/console/console.c firmware/src/main.c firmware/CMakeLists.txt
git commit -m "firmware: SNTP clock on link up; bus selftest net drives a DriveWire op end to end over WiFi"
```

---

### Task 7: CoCo manager network screen

**Files:**
- Modify: `coco/ui.c`, `coco/parse.c`, `coco/parse.h`, `coco/test_parse.c`, `coco/README.md`

**Interfaces:**
- Consumes: remote `net status`, `net scan`, `net join`, `net psk`, `net server`, `net mode`.
- Produces: `int parse_scan(char *text, file_ent *out, int max)` in `parse.c` (SSID into `name`, RSSI as a positive number into `kb`); the `W` key on the settings screen.

- [ ] **Step 1: Failing parser tests**

Append to `coco/test_parse.c` before `main`:
```c
static void t_scan(void) {
    char text[] = "ssid Oar5 rssi -36 chan 1\nssid My Net rssi -70 chan 9\nbad line\n";
    file_ent f[4];
    int n = parse_scan(text, f, 4);
    CHECK(n == 2);
    CHECK(strcmp(f[0].name, "Oar5") == 0 && f[0].kb == 36);
    CHECK(strcmp(f[1].name, "My Net") == 0 && f[1].kb == 70);
}
static void t_net_status_refused(void) {
    char fail[] = "FAIL 255 net: needs Plus-W\n\r";
    char *b;
    CHECK(parse_reply(fail, &b) == 255 && strcmp(b, "net: needs Plus-W") == 0);
}
```
and call `t_scan(); t_net_status_refused();` from `main`. `parse.h`: `int parse_scan(char *text, file_ent *out, int max);`.

- [ ] **Step 2: Run to see them fail**

Run: `make -C coco test`. Expected: link error on `parse_scan`.

- [ ] **Step 3: Implement `parse_scan`**

In `coco/parse.c`:
```c
/* "ssid <name> rssi <-n> chan <c>" lines from `net scan`; name may hold
 * spaces, so cut at the last " rssi ". kb carries -rssi (a small positive
 * number the list shows next to the name). */
int parse_scan(char *text, file_ent *out, int max)
{
    int n = 0;
    char *p = text;
    while (*p && n < max) {
        char *eol = p;
        while (*eol && *eol != '\n') eol++;
        char save = *eol; *eol = '\0';
        if (strncmp(p, "ssid ", 5) == 0) {
            char *r = NULL, *q = p;
            while ((q = strstr(q, " rssi ")) != NULL) { r = q; q++; }
            if (r) {
                *r = '\0';
                out[n].name = p + 5;
                out[n].kb = (u16)(-(int)dec_to_u32(r + 7));   /* skip " rssi -" */
                n++;
            }
        }
        *eol = save;
        p = *eol ? eol + 1 : eol;
    }
    return n;
}
```
`dec_to_u32` parses the digits after the sign; `r + 7` skips `" rssi -"`. If a positive RSSI ever appears, `r + 6` would be the digits; treat that as `kb = 0` by checking `r[6] == '-'` first (one `if`).

- [ ] **Step 4: Run the parser tests**

Run: `make -C coco test`. Expected: `all passed`.

- [ ] **Step 5: The screen**

In `coco/ui.c`, add before `settings_run`:

```c
static void net_screen(void)
{
    char st[16], ss[33], pk[8], sv[64], ip[16], er[24], mode[8], *body;
    u8 k;
    int n, i;
    for (;;) {
        st[0] = ss[0] = pk[0] = sv[0] = ip[0] = er[0] = mode[0] = '\0';
        if (ui_cmd("net status", &body) != 0) { msg("NO RADIO ON THIS BOARD"); return; }
        line_value(body, "net state ", st, sizeof st);
        line_value(body, "net ssid ", ss, sizeof ss);
        line_value(body, "net psk ", pk, sizeof pk);
        line_value(body, "net server ", sv, sizeof sv);
        line_value(body, "net ip ", ip, sizeof ip);
        line_value(body, "net error ", er, sizeof er);
        if (ui_cmd("status", &body) != 0) return;
        line_value(body, "mode ", mode, sizeof mode);
        clear_screen();
        put_at(0, 0, "WIFI", 0);
        put_at(2, 0, "S:SSID   ", 0); put_at(2, 9, ss[0] ? ss : "(NONE)", 0);
        put_at(3, 0, "P:PSK    ", 0); put_at(3, 9, strcmp(pk, "set") == 0 ? "********" : "(NONE)", 0);
        put_at(4, 0, "H:SERVER ", 0); put_at(4, 9, sv[0] ? sv : "(NONE)", 0);
        put_at(5, 0, "M:MODE   ", 0); put_at(5, 9, strcmp(mode, "net") == 0 ? "NET" : "NATIVE", 0);
        put_at(7, 0, "STATE ", 0);    put_at(7, 6, st, 0);
        put_at(8, 0, "IP    ", 0);    put_at(8, 6, ip, 0);
        if (er[0]) put_at(9, 0, er, 0);
        put_at(11, 0, "SHIFT+0 TOGGLES LOWERCASE", 0);
        put_at(12, 0, "V:SAVE   BREAK:BACK", 0);
        k = upcase(key());
        if (k == 3) return;
        if (k == 'S') {
            if (picoco_cmd("net scan", LSBUF, LSBUF_SIZE, &body) != 0) { msg("SCAN FAILED"); continue; }
            n = parse_scan(body, files, MAX_FILES);
            if (!n) { msg("NO NETWORKS FOUND"); continue; }
            i = pick_list("NETWORK", files, n);
            if (i < 0) continue;
            strcpy(line, "net join "); strcat(line, files[i].name);
            if (ui_cmd(line, &body) != 0) continue;
            if (input_line("PSK: ", line + 8, 63) >= 0) { memcpy(line, "net psk ", 8); ui_cmd(line, &body); }
            ui_dirty = 1;
        } else if (k == 'P') {
            if (input_line("PSK: ", line + 8, 63) >= 0) { memcpy(line, "net psk ", 8); if (ui_cmd(line, &body) == 0) ui_dirty = 1; }
        } else if (k == 'H') {
            char port[6];
            strcpy(line, "net server ");
            if (input_line("HOST: ", line + 11, 63) <= 0) continue;
            if (input_line("PORT (65504): ", port, 5) > 0) { strcat(line, " "); strcat(line, port); }
            if (ui_cmd(line, &body) == 0) ui_dirty = 1;
        } else if (k == 'M') {
            if (ui_cmd(strcmp(mode, "net") == 0 ? "net mode native" : "net mode net", &body) == 0) ui_dirty = 1;
        } else if (k == 'V') do_save();
    }
}
```
`upcase` is the static helper added on 2026-09-29 above `msg`; move it above `net_screen` if the compiler complains about order (it is defined near the top of the file, before `msg`, so it is already visible). `files`, `MAX_FILES`, `LSBUF`, `LSBUF_SIZE`, `pick_list`, `input_line` all exist in `ui.c`. `input_line` returns the length, `<= 0` for empty/BREAK; the PSK prompt accepts empty (open network), hence `>= 0` there.

In `settings_run`: add the row `put_at(9, 0, "W:WIFI", 0);` (row 9 is free unless the clock-lost note is on row 8; keep it at 9) and the key `else if (k == 'W') net_screen();`.

- [ ] **Step 6: Build the manager and check the size budget**

Run: `make -C coco`. Expected: `PICOCO.BIN` and `PICOCO.DSK` rebuilt; the build prints the program end address, which must stay below `$7800` (Task 13 of the manager plan put it at `$73D5`; this screen adds roughly 1 KB).

- [ ] **Step 7: README**

In `coco/README.md`, settings section, add `W` to the key list and a short "WiFi screen" subsection: rows, keys (`S` scan and pick, `P` passphrase, `H` host and port, `M` toggle mode, `V` save), the `SHIFT+0` note, and `NO RADIO ON THIS BOARD` on a Pico 2. Mention that saving is needed for the settings to survive a reset.

- [ ] **Step 8: Commit**

```bash
git add coco/ui.c coco/parse.c coco/parse.h coco/test_parse.c coco/README.md
git commit -m "coco: WiFi screen in the manager (scan, psk, server, mode, save)"
```

- [ ] **Step 9: Device check (when a CoCo and the PCB are available; otherwise XRoar for the Pico 2 path)**

On a Pico 2 board (or XRoar against `picoco-host`, which uses the stub): settings, `W`: expect `NO RADIO ON THIS BOARD`. On the Plus-W PCB in a CoCo: `W`, `S` lists the networks, pick one, type the PSK, `H` host, `M` to NET, `V`; reset the CoCo: HDB-DOS boots after the hold, `DIR` lists the server's disk.

---

### Task 8: Docs and test plan

**Files:**
- Modify: `firmware/README.md`, `docs/firmware-architecture.md`, `docs/FUJINET_SETUP.md`, `firmware/TEST_PLAN.md`, `CLAUDE.md` (the project one, untracked: leave a note for the user to fold in, do not `git add` it)

- [ ] **Step 1: `firmware/README.md`**

Console command list: add the `net` verb table from the spec §3.5 and `becker net`; a "WiFi (Plus-W)" section: pins verified, `picoco.cfg` lines, the boot hold and fallback, the 200 ms round-trip rule with the reason (server op timeout), the link-drop limitation, `bus selftest net` and what it prints, `net scan`. State that DNS names work and `.local` names do not.

- [ ] **Step 2: `docs/firmware-architecture.md`**

One subsection "WiFi transport" under the DriveWire integration chapter: the polled-lwIP choice, `net.c`'s state machine, `MODE_NET`, where `net_poll` runs, why core1 is untouched, SNTP.

- [ ] **Step 3: `docs/FUJINET_SETUP.md`**

Option B stops saying "not written yet": `net server <fujinet-host> 65504` with BoIP enabled on the FujiNet side, `becker net`, `save`. Keep the DTR note for option A.

- [ ] **Step 4: `firmware/TEST_PLAN.md`**

Add section G "WiFi (Plus-W)": the bare-module checks (Task 4 step 6-7, Task 5 step 3, Task 6 step 4) and the PCB checks (boot hold with a CoCo, DIR/LOADM from DW4 and FujiNet-PC over WiFi, fallback with the server stopped, manager screen, VSYS scope trace during LOADM with the 300 mA cart budget in mind). Leave the results table blank for the PCB rows; fill the bare-module rows with today's numbers from the executor's runs.

- [ ] **Step 5: Run the whole verification set**

```bash
ninja -C build-host && ctest --test-dir build-host --output-on-failure
make -C coco test && make -C coco
ninja -C build-pico && ninja -C build-pico-plusw
```
Expected: all green, both flash-free checks ok.

- [ ] **Step 6: Commit**

```bash
git add firmware/README.md docs/firmware-architecture.md docs/FUJINET_SETUP.md firmware/TEST_PLAN.md
git commit -m "docs: WiFi transport: console verbs, boot hold, latency rule, FujiNet over BoIP, test plan section G"
```

Note for the user (not committed by the executor): `CLAUDE.md` should gain three lines under Firmware: `net.c` is Plus-W and core0 only; `becker net` holds boot up to 10 s then falls back to native; `bus selftest net` needs `becker net` up and no CoCo.
