# WiFi DriveWire transport (Plus-W) — design

Date: 2026-09-29. Status: approved in conversation, awaiting written review.
Builds on: `2026-09-27-plusw-bus-engine-design.md` (Plus-W board support,
core1 hooks), `2026-09-23-coco-manager-design.md` (manager, remote
allowlist), `firmware/TEST_PLAN.md` 2026-09-29 results (bridge mode and the
server latency limit), `docs/FUJINET_SETUP.md` option B.

## 1. Goal

A Waveshare RP2350B-Plus-W board in the CoCo reaches a DriveWire server over
WiFi with no USB cable. Day one the server is any of DriveWire 4, `picoco-host`
or FujiNet (FujiNet-PC or an ESP32 FujiNet with BoIP): the same TCP client
pointed at a different host and port. Later work (an HTTP server on the board
for uploading ROMs, disks and config; DriveWire's TCP features) reuses the
same stack, so this design chooses a full lwIP, not a minimal socket shim.

Success on day one: on a bare Plus-W over USB, `bus selftest net` pushes a
DWINIT and an OP_TIME through the fake 6809, core1, the Becker ring, the
socket and back against `picoco-host` on the Mac, and passes. On the PCB
later: HDB-DOS boots with the hold, DIR and LOADM run from DW4 over WiFi,
the fallback engages with the server stopped.

## 2. Facts this design rests on

- Radio pin map, verified 2026-09-29 from the Waveshare schematic and a live
  scan on a module: WL_ON = GP36, WL_D = GP37 (HOST_WAKE shares it, as on the
  Pico W), WL_CS = GP38, WL_CLK = GP39, LED1 = RM2 WL_GPIO0, LED2 = GP23,
  VBUS detect = RM2 GPIO2, VSYS sense = GP46 (ADC6, not shared with the
  radio), PSRAM_CS = GP47. pico-sdk 2.3.1's `pico_cyw43_arch` drives a radio
  on the high GPIO bank with these as `CYW43_DEFAULT_PIN_WL_*` defines plus
  `PICO_CYW43_SUPPORTED=1`; `boards/pimoroni_pico_plus2_w_rp2350.h` is the
  template.
- A DriveWire server abandons a half-finished op after 200-250 ms without
  bytes (`DW_PAYLOAD_TIMEOUT_MS` 250 in `picoco-host`, `ReadByteWait` 200 in
  DW4). The CoCo's Becker read loop never times out. So the link must keep
  its round trip well under 200 ms, and a dead link hangs the CoCo until
  reset. WiFi power save on the CYW43 adds latency spikes of hundreds of
  ms and must be off.
- The Pico 2 has no radio. Everything here compiles out on that board.
- core1 stays flash-free and untouched. All networking runs on core0.

## 3. Components

### 3.1 Board header

`firmware/boards/picoco_plusw.h` gains the CYW43 block:

```c
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)
#define CYW43_DEFAULT_PIN_WL_REG_ON 36
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 37
#define CYW43_DEFAULT_PIN_WL_DATA_IN 37
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 37
#define CYW43_DEFAULT_PIN_WL_CLOCK 39
#define CYW43_DEFAULT_PIN_WL_CS 38
#define CYW43_WL_GPIO_COUNT 3
#define CYW43_WL_GPIO_LED_PIN 0
#define CYW43_WL_GPIO_VBUS_PIN 2
#define CYW43_USES_VSYS_PIN 0
#define PICO_VSYS_PIN 46
```

`firmware/boards/plusw.h` defines `PICOCO_HAVE_NET 1`. `pico2_breadboard.h`
does not, and every net symbol is behind `#ifdef PICOCO_HAVE_NET`.

### 3.2 `firmware/src/net/net.c` (Plus-W only)

Built on `pico_cyw43_arch_lwip_poll`. Owns:

- one state machine: `NET_OFF`, `NET_JOINING`, `NET_CONNECTING`, `NET_UP`,
  `NET_FAILED`;
- stored config: ssid (32), psk (63), host (64), port (u16, default 65504);
- one lwIP raw TCP PCB;
- two 1 KB rings, to-server and from-server (same `ring.h` as Becker);
- counters and the last error reason for `net status`.

Interface, shaped like the bridge platform hooks:

```c
void   net_init(void);                       /* cyw43_arch_init, radio off */
int    net_set_ssid(const char *s);          /* -1 too long */
int    net_set_psk(const char *s);
int    net_set_server(const char *host, uint16_t port);
void   net_forget(void);                     /* clear all, stop */
int    net_start(void);                      /* -1 if ssid or host missing; begins joining */
void   net_stop(void);                       /* close socket, leave the network, NET_OFF */
void   net_poll(uint32_t now_ms);            /* cyw43_arch_poll + state machine + retries */
net_state_t net_state(void);
size_t net_read(uint8_t *buf, size_t n);     /* from-server ring */
size_t net_write(const uint8_t *buf, size_t n); /* to-server ring; returns n when not up (bridge rule) */
int    net_scan(void (*cb)(const char *ssid, int rssi, int chan, void *), void *ctx);
void   net_status_print(void (*out)(const char *));
bool   net_configured(void);                 /* ssid and host stored */
```

Behaviour rules:

- After a successful join: `cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE)`
  (the SDK's no-power-save constant), then DHCP, then DNS if the host is a
  name, then `tcp_connect`. `TCP_NODELAY` on the PCB. One connection at a time.
- `net_poll` drains the to-server ring into `tcp_write` + `tcp_output` and
  copies `recv` callbacks into the from-server ring. A full from-server ring
  counts `overrun` and drops, like `becker overrun`; it cannot happen with
  DriveWire's one-reply-at-a-time traffic.
- Any failure (join, DHCP, DNS, refused, socket closed, association lost)
  records a reason, moves to `NET_JOINING` or `NET_CONNECTING` as appropriate
  and retries every 2 s forever. The first failure of a kind logs at info,
  later ones at debug.
- SNTP: once per boot, when the link first comes up, `sntp` sets the clock via
  `plat_rtc_set` and `dw_time_set` (pool.ntp.org). Failure is silent.
- The lwIP config (`firmware/src/net/lwipopts.h`) is the SDK example's with
  only raw TCP, DHCP, DNS, SNTP and IPv4. No threads, no sockets API.
- DNS names are allowed for the server; `.local` names are not (no mDNS
  resolver in lwIP) and the docs say so.

Host build: `firmware/src/net/net_stub.c` returns `NET_OFF`, `-1` from
`net_start`, `0` from reads, `n` from writes, so tests and `picoco-host` link
unchanged.

### 3.3 `mode.c`: `MODE_NET`

The bridge case with `plat_bridge_read`/`plat_bridge_write` swapped for
`net_read`/`net_write`. `mode_name` returns `net`. On the Pico 2 the enum
value exists but `becker net` is refused, so the case is unreachable.

### 3.4 `main.c`

- `net_init()` after `console_init`, before config replay.
- After config replay, before core1 launch and the /HALT release: if the mode
  is `MODE_NET`, wait until `net_state() == NET_UP` or 10 s, pumping
  `net_poll`, `tud_task` and `watchdog_update` (the watchdog is 8 s). Then:
  - up: log `net up <ip> -> <host>:<port>`;
  - not up: `mode_set(MODE_NATIVE)`, log `net failed (<reason>), native
    fallback`. The saved config still says net; the next boot tries again.
- The main loop adds `net_poll(now)` beside `tud_task` and `mode_pump`.

### 3.5 Console (`console.c`)

New `net` verb. R marks commands also on the remote allowlist.

| Command | Effect |
|---|---|
| `net join <ssid>` (R) | Store the SSID (raw line tail, spaces allowed). Joining starts only with `becker net`. |
| `net psk <psk>` (R) | Store the passphrase (raw tail). Never echoed. Empty clears it (open network). |
| `net server <host> [port]` (R) | Server, port default 65504. |
| `net forget` (R) | Clear all, stop the radio. Falls back to native first if in net mode. |
| `net scan` (R) | One line per SSID: `ssid <name> rssi <n> chan <n>`. |
| `net status` (R) | State, SSID, IP, server, last error, retries, bytes each way, overrun. PSK shown as `set`/`unset`. |
| `net` (R) | Same as `net status`. |
| `becker net` (console only) | Select the mode. Refused on a Pico 2 (`net: needs Plus-W`) and when `net_configured()` is false (`net: set ssid and server first`). Calls `net_start`. |
| `net mode net\|native` (R) | The manager's way to switch: the same two `becker` cases, so the CoCo can never pick loop or bridge. |

`save` writes `net join`, `net psk`, `net server` before the `becker` line so
replay has them first. `status` prints one `net` summary line on the Plus-W.
`net join`, `net psk` and `net server` while in net mode fall back to native first (`net_stop`), so a change never races a live socket; `becker net` re-arms.

### 3.6 CoCo manager (`coco/`)

Settings screen gets `W:WIFI`. The network screen, on the ROM-picker pattern:

- Rows: `SSID`, `PSK` (`********` or `(NONE)`), `SERVER`, `PORT`,
  `MODE` (`NET`/`NATIVE`), `STATE` (from `net status`, refreshed on each key).
- `S`: `net scan`, pick from the list, then prompt for the PSK. `H`: prompt
  for host, then port (default 65504). `M`: toggle `net mode net` /
  `net mode native` and show the firmware's refusal text if any. `V`: save.
  `BREAK`: back.
- The PSK prompt honours the CoCo's SHIFT+0 lowercase mode; the screen says
  `SHIFT+0 TOGGLES LOWERCASE`.
- On a Pico 2 (`net status` refused): `NO RADIO ON THIS BOARD`, BREAK only.
- `parse.c` gains a `net status` line parser (key-value lines, same shape as
  `time`).

## 4. Error handling and limits

- Reasons kept for `net status`: `no such network`, `bad password`,
  `dhcp timeout`, `dns failed`, `refused`, `link lost`, `not configured`.
- Link drop mid-transfer: the reply for the op in flight is gone and the CoCo
  waits until reset; on the PCB the CoCo's reset reboots the Pico, which
  reconnects at boot. Accepted for this design (approach 1 of three); the
  op-aware proxy that could synthesize `E_NOTRDY` is deferred, and `net.c`'s
  read/write seam is where it would go.
- Credentials sit in plain text in `picoco.cfg`, the same trust boundary as
  every other line there. `net status` and `log` never print the PSK.
- Power: no firmware change, but the PCB bring-up must put a scope on VSYS
  during a LOADM over WiFi. Radio transmit bursts and the cart's 300 mA
  budget have not met yet; bulk capacitance may need a bump.
- Not in scope: access-point setup mode, HTTP server, mDNS, IPv6, WPA3,
  multiple server profiles, Pico 2 W.

## 5. Testing

- Host (`ctest`): `mode.c`'s net case through a fake `net_read`/`net_write`
  pair; `save` line order and replay of `net join`/`net psk`/`net server`
  with spaces; `becker net` refusals on host (stub reports no radio) and
  without config; `net status` output format; manager `parse.c` cases.
- Bare Plus-W over USB, no CoCo: `bus selftest net` (Plus-W only) sends
  DWINIT and OP_TIME through the fake 6809 → core1 → Becker ring → socket →
  `picoco-host` on the Mac → back, checks the replies, and prints the
  OP_TIME round trip in ms. Expect single-digit ms on the LAN. This is the
  day-one pass criterion. The fake 6809 and `picoco-host` already exist.
- Power-save check: the OP_TIME round trip stays under 20 ms across 100
  repeats; any spike over 200 ms fails the check (would trip the server).
- PCB (later, `firmware/TEST_PLAN.md`): boot hold and release, DIR and LOADM
  from DW4 and from FujiNet-PC over WiFi, fallback with the server stopped,
  manager network screen end to end, VSYS scope trace during LOADM.

## 6. Files

- Modify: `firmware/boards/picoco_plusw.h`, `firmware/boards/plusw.h`,
  `firmware/CMakeLists.txt`, `firmware/src/main.c`, `firmware/src/console/
  mode.c`, `mode.h`, `console.c`, `firmware/src/bus/fake6809.c` (net case),
  `coco/ui.c`, `coco/parse.c`, `coco/test_parse.c`, `coco/README.md`,
  `firmware/README.md`, `docs/firmware-architecture.md`, `docs/FUJINET_SETUP.md`
  (option B becomes real), `firmware/TEST_PLAN.md`.
- Add: `firmware/src/net/net.c`, `net.h`, `net_stub.c`, `lwipopts.h`,
  `firmware/tests/test_net_mode.c`.
