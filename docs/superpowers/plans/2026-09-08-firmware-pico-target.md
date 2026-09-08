# PiCoCo Firmware Plan B: Pico Target Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the host-tested core into a running RP2350 firmware: USB console, flash filesystem with USB drive export, the core1 bus loop, crash record and watchdog, ending with a runbook for the breadboard milestones.

**Architecture:** Same `picoco_core` sources as Plan A, now linked with `src/plat_pico.c` (platform functions), `src/usb/*` (TinyUSB composite: 2 CDC + MSC), `src/fs_flash.c` + `third_party/fatfs` (FAT on a 2.5 MB flash partition, also served over MSC in export mode), `src/bus/bus_core1.c` (the flash-free bus loop on core1), and `src/crash.c`. Core0 runs everything else in one main loop.

**Tech Stack:** Pico SDK 2.3.1 (`PICO_BOARD=pico2`), TinyUSB (bundled with the SDK), FatFS R0.15 (vendored), CMake + Ninja, arm-none-eabi-gcc, UF2 flashing over BOOTSEL.

**Spec:** `docs/superpowers/specs/2026-09-07-firmware-design.md` sections 4.1, 4.3, 4.4, 7, 8, 9 (crash record, isolation modes), 11 steps 4-5. Plan A's result is on `main` at 5381aee.

## Global Constraints

- Host build must keep passing after every task: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host && ctest --test-dir build-host --output-on-failure` (9+ suites, ASan/UBSan, no warnings). Any change under `firmware/src/` that the host compiles is covered by this.
- Pico build: `cmake -B build-pico -G Ninja -DPICO_SDK_PATH=/Users/cognitivegears/projects/pico-sdk firmware && ninja -C build-pico` produces `build-pico/picoco.uf2`. Zero warnings in `firmware/` sources (SDK/TinyUSB/FatFS warnings are not ours).
- **core1 is flash-free.** Everything core1 executes is `BUS_HOT` or `static inline`; core1 never calls libc, logging, `plat_*`, or anything that might touch flash. The build defines `PICO_FLASH_ASSUME_CORE1_SAFE=1`. The Pico build uses `-O2` (SDK Release) so `ring.h` inlines are never emitted out of line.
- Only core1's read hooks write `bus_table[0x3F41]`/`[0x3F42]` (single-writer rule, spec section 5).
- Nothing in the core0 main loop blocks: USB writes drop when the host is not reading; flash writes are the one long operation and are bounded to one 4 KB erase per call.
- Flash partition: firmware in the first 1.5 MB (`0x000000..0x17FFFF`), FAT volume at `PICOCO_FS_OFFSET 0x180000`, size `PICOCO_FS_SIZE 0x280000` (2.5 MB), 512-byte logical sectors, 4 KB erase blocks.
- On-target verification steps need a Pico 2 on USB (no CoCo). If none is on hand, complete the build step, mark the on-target step "not run" in the report, and continue; do not fake output.
- Commit once per task, only files under `firmware/` (plus `docs/` when a task says so). Message `firmware: <task summary>`, ending with:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01RmFQ1HMPQxdKfGPZdaihVx`. Per-task commits were approved by the user for Plan A on 2026-09-07 and Plan B continues that.
- Keep it lazy: smallest code that meets the step. Mark deliberate shortcuts with `// ponytail:` naming the ceiling. No abstractions beyond those named here.

## File map

| Path | Responsibility | Task |
|---|---|---|
| `firmware/CMakeLists.txt` (Pico branch) | SDK, sources, defines, libraries, UF2 | 1, 2, 3, 5 |
| `firmware/README.md` | SDK install, flash, on-target checks | 1, 6 |
| `firmware/boards/pico2_breadboard.h` | GPIO map (spec 4.1) | 2 |
| `firmware/src/plat_pico.c` | `plat.h` for RP2350: time, reboot, bootsel, halt, smoke, bridge (CDC0), fs/cfg (FatFS), export, crash test | 2, 3, 4, 5 |
| `firmware/src/usb/tusb_config.h`, `usb_descriptors.c` | composite device: CDC0 (DriveWire bridge), CDC1 (console), MSC | 2, 4 |
| `firmware/src/main.c` | core0: init order, main loop (tud_task, mode_pump, log drain, console, LED, watchdog), core1 launch | 2, 5 |
| `firmware/third_party/fatfs/` | FatFS R0.15 (`ff.c ff.h ffconf.h diskio.h ffunicode.c`) | 3 |
| `firmware/src/fs_flash.c/.h` | flash-backed `diskio`, 4 KB read-modify-write, mount/format/unmount, MSC block access | 3, 4 |
| `firmware/src/dw/dw_store_fatfs.c` | `dw_store_ops` on FatFS | 3 |
| `firmware/src/console/console.c` | `dw selftest`, `bus drive on|off`, `crash` | 3, 5 |
| `firmware/src/bus/bus.h/.c` | `bus_drive_set/get` flag (host too) | 5 |
| `firmware/src/bus/bus_core1.c` | the core1 loop | 5 |
| `firmware/src/crash.c/.h` | HardFault/panic record in uninitialised RAM, `crash_report()` | 5 |
| `firmware/tests/test_console.c`, `test_bus.c` | host tests for `bus drive`, `dw selftest` | 3, 5 |
| `docs/breadboard-plan.md` | milestone table gains console commands and tags | 6 |

---

### Task 1: Pico SDK and the first UF2

**Files:**
- Modify: `firmware/CMakeLists.txt` (Pico branch), `firmware/README.md`
- Create: `firmware/tools/flash.sh`

**Interfaces:**
- Produces: a working `build-pico/` configuration; `PICO_SDK_PATH` convention; `tools/flash.sh` that copies a UF2 to the mounted `RP2350` BOOTSEL volume.

- [ ] **Step 1: Install the SDK**

```bash
cd /Users/cognitivegears/projects
git clone --branch 2.3.1 --depth 1 --recurse-submodules --shallow-submodules https://github.com/raspberrypi/pico-sdk.git
ls pico-sdk/lib/tinyusb/src/tusb.h   # must exist: TinyUSB submodule present
brew list picotool >/dev/null 2>&1 || brew install picotool
```
If the sandbox blocks the clone, rerun that one command with the sandbox disabled. picotool is optional: the SDK builds its own copy for UF2 generation when it is missing, at the cost of a longer first configure.

- [ ] **Step 2: Fix the Pico branch of CMakeLists.txt**

Replace the block under `if(NOT PICOCO_HOST)` with:
```cmake
if(NOT PICOCO_HOST)
  set(PICO_BOARD pico2 CACHE STRING "")
  set(PICO_PLATFORM rp2350-arm-s CACHE STRING "")
  include(pico_sdk_import.cmake)
  project(picoco C CXX ASM)
  set(CMAKE_C_STANDARD 11)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)     # -O2: keeps ring.h inlines inline
  endif()
  pico_sdk_init()
  add_executable(picoco src/main.c)
  target_compile_options(picoco PRIVATE -Wall -Wextra)
  target_link_libraries(picoco pico_stdlib)
  pico_add_extra_outputs(picoco)
  return()
endif()
```

- [ ] **Step 3: Build**

```bash
cmake -B build-pico -G Ninja -DPICO_SDK_PATH=/Users/cognitivegears/projects/pico-sdk firmware && ninja -C build-pico
ls -la build-pico/picoco.uf2
```
Expected: the UF2 exists; no warnings from `src/main.c`.

- [ ] **Step 4: flash.sh and README**

`firmware/tools/flash.sh`:
```bash
#!/bin/sh
# Copies the UF2 to a Pico 2 in BOOTSEL mode (hold BOOTSEL while plugging in).
set -e
UF2=${1:-build-pico/picoco.uf2}
VOL=/Volumes/RP2350
[ -d "$VOL" ] || { echo "no $VOL mounted: hold BOOTSEL while plugging the Pico in"; exit 1; }
cp "$UF2" "$VOL/" && echo "flashed $UF2"
```
README: add a "Pico build" section with the clone command, the configure line, `tools/flash.sh`, and "the console appears as the second of two `/dev/tty.usbmodem*` ports; use `screen /dev/tty.usbmodemXXXX2` or `picocom`".

- [ ] **Step 5: On target (if a Pico 2 is on hand)**

Flash; the onboard LED blinks at 1 Hz. This is milestone `fw-0.1-blink`; tag it: `git tag fw-0.1-blink`.

- [ ] **Step 6: Commit** (`firmware/CMakeLists.txt`, `firmware/README.md`, `firmware/tools/flash.sh`).

---

### Task 2: USB composite device, console over CDC1, platform basics, watchdog

**Files:**
- Create: `firmware/boards/pico2_breadboard.h`, `firmware/src/usb/tusb_config.h`, `firmware/src/usb/usb_descriptors.c`, `firmware/src/plat_pico.c`
- Modify: `firmware/src/main.c`, `firmware/CMakeLists.txt`

**Interfaces:**
- Consumes: `console_init/console_feed/console_run_config` (`src/console/console.h`), `mode_pump/mode_bind/mode_dw_send/mode_get` (`src/console/mode.h`), `log_init/log_drain/log_dropped` (`src/log.h`), `bus_init` (`src/bus/bus.h`), `device_reset/rom_init/becker_init/device_init_all` (`src/dev/*.h`), `dw_init` (`src/dw/dw.h`), `plat.h`.
- Produces: `boards/pico2_breadboard.h` defines `PIN_D0 0, PIN_A0 8, PIN_RW 22, PIN_OE_BUS 26, PIN_HALT 27, PIN_E 28, PIN_LED PICO_DEFAULT_LED_PIN`; `usb_cdc_console_write(const char*)` (drops when host not connected); all `plat_*` in `plat_pico.c` (fs/cfg/export return -1 until Task 3/4; `plat_fs_dir`/`plat_host_set_dir` return `""`/no-op).

- [ ] **Step 1: TinyUSB config and descriptors**

`firmware/src/usb/tusb_config.h`:
```c
#pragma once
#define CFG_TUSB_MCU          OPT_MCU_RP2040   /* RP2350 uses the same USB IP; SDK sets the right value via board header */
#define CFG_TUSB_OS           OPT_OS_PICO
#define CFG_TUD_ENABLED       1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_CDC           2
#define CFG_TUD_MSC           1
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_MSC_EP_BUFSIZE 512
```
If the SDK's `pico2` board header already defines `CFG_TUSB_MCU`, drop that line (the SDK sets it for you; a redefinition warning tells you).

`firmware/src/usb/usb_descriptors.c`: device descriptor (VID 0x2E8A Raspberry Pi, PID 0x000A "Pico SDK CDC" is taken; use `0x2E8A:0x1042` and note it is a placeholder), one configuration with interfaces in this order: CDC0 (2 interfaces, EP 0x81 notify, 0x02/0x82 data), CDC1 (EP 0x83, 0x04/0x84), MSC (EP 0x05/0x85). String descriptors: manufacturer "PiCoCo", product "PiCoCo Cartridge", serial from `pico_get_unique_board_id_string`, interface strings "PiCoCo DriveWire", "PiCoCo Console", "PiCoCo Storage". Use the `TUD_CDC_DESCRIPTOR` and `TUD_MSC_DESCRIPTOR` macros; total length `TUD_CONFIG_DESC_LEN + 2*TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN`. Provide the three callbacks `tud_descriptor_device_cb`, `tud_descriptor_configuration_cb`, `tud_descriptor_string_cb`. MSC callbacks are stubbed in this task: `tud_msc_test_unit_ready_cb` returns `false` (no medium), `inquiry` fills "PiCoCo", "Flash FS", "1.0"; `capacity` returns 0 blocks; `read10`/`write10` return -1; `scsi_cb` returns -1 for anything else. Task 4 fills them in.

- [ ] **Step 2: plat_pico.c**

```c
#include "plat.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include PICOCO_BOARD_H
uint32_t plat_now_us(void) { return time_us_32(); }
uint32_t plat_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
void plat_reboot(bool bootsel) { if (bootsel) reset_usb_boot(0, 0); watchdog_reboot(0, 0, 0); for (;;) tight_loop_contents(); }
void plat_halt(bool assert_halt) { gpio_put(PIN_HALT, assert_halt); }   /* HIGH = Q2 on = /HALT low */
void plat_smoke(void) {
    /* Toggle every header GPIO at 10 Hz for 5 s; core1 is not running yet in Task 2.
       ponytail: after Task 5 this must only run with bus drive off; the console enforces it. */
    for (int t = 0; t < 50; t++) {
        for (int g = 0; g <= 28; g++) if (g < 23 || g > 25) { gpio_set_dir(g, GPIO_OUT); gpio_put(g, t & 1); }
        sleep_ms(50); tud_task();
    }
    for (int g = 0; g <= 28; g++) if (g < 23 || g > 25) gpio_set_dir(g, GPIO_IN);
}
size_t plat_bridge_read(uint8_t *buf, size_t n)  { return tud_cdc_n_connected(0) ? tud_cdc_n_read(0, buf, n) : 0; }
size_t plat_bridge_write(const uint8_t *buf, size_t n) {
    if (!tud_cdc_n_connected(0)) return n;            /* drop: no host */
    size_t w = tud_cdc_n_write(0, buf, n); tud_cdc_n_write_flush(0); return w;
}
/* Filesystem and config: Task 3. Export: Task 4. */
int plat_fs_list(void (*cb)(const char *, uint32_t, void *), void *ctx) { (void)cb; (void)ctx; return -1; }
int plat_fs_remove(const char *n) { (void)n; return -1; }
int plat_fs_format(void) { return -1; }
int plat_fs_export(bool on) { (void)on; return -1; }
int plat_cfg_read(char *b, size_t m) { (void)b; (void)m; return -1; }
int plat_cfg_write(const char *b, size_t n) { (void)b; (void)n; return -1; }
const char *plat_fs_dir(void) { return ""; }
void plat_host_set_dir(const char *d) { (void)d; }
```
`plat_bridge_write` returns `n` when disconnected so `mode_pump` does not spin on a full-but-absent host; add a `// ponytail:` note.

- [ ] **Step 3: main.c**

```c
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include PICOCO_BOARD_H
#include "bus.h"
#include "device.h"
#include "rom.h"
#include "becker.h"
#include "dw.h"
#include "dw_store.h"
#include "log.h"
#include "console.h"
#include "mode.h"
#include "plat.h"

static dw_server g_dw;
static dw_store  g_store;   /* Task 3 initialises it; until then ops == NULL */

static void console_out(void *ctx, const char *s) {
    (void)ctx;
    if (!tud_cdc_n_connected(1)) return;              /* drop when nobody is listening */
    tud_cdc_n_write_str(1, s); tud_cdc_n_write_flush(1);
}
static void gpio_setup(void) {
    for (int g = 0; g <= 28; g++) {
        if (g >= 23 && g <= 25) continue;             /* internal on the module */
        gpio_init(g); gpio_set_dir(g, GPIO_IN); gpio_pull_up(g);
    }
    gpio_init(PIN_HALT); gpio_set_dir(PIN_HALT, GPIO_OUT); gpio_put(PIN_HALT, 1);   /* keep /HALT asserted until Task 5 releases it */
    gpio_init(PIN_LED);  gpio_set_dir(PIN_LED, GPIO_OUT);
}
int main(void) {
    gpio_setup();
    tusb_init();
    log_init();
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
    dw_init(&g_dw, &g_store, mode_dw_send, NULL);
    console_init(console_out, NULL, &g_dw, &g_store);
    watchdog_enable(8000, true);
    LOG_I(LOG_M_MAIN, "boot");
    /* Task 3: mount fs, console_run_config().  Task 5: crash_report(), core1 launch, halt release. */
    uint32_t last_blink = 0; bool led = false;
    for (;;) {
        tud_task();
        uint32_t now = plat_now_ms();
        mode_pump(&g_dw, now);
        if (tud_cdc_n_available(1)) { uint8_t b[64]; uint32_t n = tud_cdc_n_read(1, b, sizeof b); console_feed(b, n); }
        char lb[256]; size_t ln = log_drain(lb, sizeof lb - 1);
        if (ln) { lb[ln] = 0; console_out(NULL, lb); }
        uint32_t period = mode_get() == MODE_NATIVE ? 250 : 500;      /* 2 Hz native, 1 Hz otherwise */
        if (now - last_blink >= period) { last_blink = now; led = !led; gpio_put(PIN_LED, led); }
        watchdog_update();
    }
}
```
`dw_init` with a store whose `ops` is NULL: check `dw_mount` and the console's `dw capture`/`rom load` paths return an error rather than dereferencing NULL. If they dereference, add `if (!s->store->ops) return -1;` guards in `dw_mount` and in the two console paths (host tests still pass; add one host test `mount_without_store_fails` in `test_dw_server.c`).

- [ ] **Step 4: CMake**

In the Pico branch: `set(PICOCO_BOARD pico2_breadboard CACHE STRING "")`; add all `CORE_SRC` files plus `src/plat_pico.c src/usb/usb_descriptors.c src/main.c` to the `picoco` executable; `target_include_directories(picoco PRIVATE src src/usb src/dw src/bus src/dev src/console boards)`; `target_compile_definitions(picoco PRIVATE PICOCO_BOARD_H="${PICOCO_BOARD}.h" PICOCO_LOG_LEVEL=3 PICOCO_VERSION="${PICOCO_VERSION}" PICO_FLASH_ASSUME_CORE1_SAFE=1 CFG_TUSB_CONFIG_FILE="tusb_config.h")`; libraries `pico_stdlib pico_unique_id pico_multicore hardware_flash hardware_watchdog tinyusb_device tinyusb_board`. Add `set(PICOCO_VERSION "0.2-usb" CACHE STRING "")` near the top and pass it to the host build too so `version` prints it there. Move the `CORE_SRC` list above the Pico branch so both branches share it (the `EXISTS` filtering loop can go: every file exists now).

- [ ] **Step 5: Build both targets**

Host: full ctest green (the `version` test, if any, must accept the new string). Pico: UF2 builds, no warnings in our sources.

- [ ] **Step 6: On target**

Flash. `ls /dev/tty.usbmodem*` shows two ports. On the second: `version` → `version 0.2-usb` / `ok`; `status` → mode off, counters zero; `log main debug` / `log dump` shows the boot line; `smoke` → LED and header pins toggle (multimeter or LED on GP0), then `ok`; `halt on` / `halt off`; `reboot` → port disappears and returns within 2 s; `bootsel` → `RP2350` volume mounts. Paste the console transcript in the report. Tag `fw-0.2-gpio-smoke`.

- [ ] **Step 7: Commit.**

---

### Task 3: FatFS on flash, storage backend, config, `dw selftest`

**Files:**
- Create: `firmware/third_party/fatfs/{ff.c,ff.h,ffconf.h,diskio.h,ffunicode.c,LICENSE.txt}`, `firmware/src/fs_flash.h`, `firmware/src/fs_flash.c`, `firmware/src/dw/dw_store_fatfs.c`
- Modify: `firmware/src/plat_pico.c` (fs/cfg), `firmware/src/main.c` (mount + config), `firmware/src/console/console.c` (`dw selftest`), `firmware/src/dw/dw.h` (declare `dw_store_fatfs_init`), `firmware/CMakeLists.txt`
- Test: `firmware/tests/test_console.c` (`dw selftest` on host against POSIX store)

**Interfaces:**
- Produces:
```c
/* fs_flash.h */
#define PICOCO_FS_OFFSET 0x180000u
#define PICOCO_FS_SIZE   0x280000u
#define FS_SECTOR 512u
#define FS_SECTORS (PICOCO_FS_SIZE / FS_SECTOR)
int  fs_flash_mount(void);        /* 0 ok; formats first if no valid volume; -1 on failure */
void fs_flash_unmount(void);
int  fs_flash_format(void);       /* f_mkfs FAT16 with 4 KB clusters, then mount; 0 ok */
bool fs_flash_mounted(void);
/* raw block access shared by diskio and MSC (Task 4): */
int  fs_flash_read_blocks(uint32_t lba, uint8_t *buf, uint32_t n);    /* memcpy from XIP */
int  fs_flash_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t n); /* 4 KB read-modify-write; interrupts off per erase */
/* dw.h */
void dw_store_fatfs_init(dw_store *s);   /* names are FAT paths at the root; rejects '/' */
```

- [ ] **Step 1: Vendor FatFS**

```bash
mkdir -p firmware/third_party/fatfs && cd firmware/third_party/fatfs
B=https://raw.githubusercontent.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/main/src/ff15/source
for f in ff.c ff.h diskio.h ffunicode.c ffconf.h; do curl -fsSL -o $f $B/$f; done
curl -fsSL -o LICENSE.txt https://raw.githubusercontent.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/main/src/ff15/LICENSE.txt
grep -m1 "FF_DEFINED" ff.h   # R0.15: 80286
```
If that path 404s, fetch `http://elm-chan.org/fsw/ff/arc/ff15a.zip` with `-L` and unzip `source/`. Edit `ffconf.h`: `FF_FS_READONLY 0`, `FF_FS_MINIMIZE 0`, `FF_USE_MKFS 1`, `FF_USE_LFN 1`, `FF_MAX_LFN 64`, `FF_LFN_UNICODE 0`, `FF_CODE_PAGE 437`, `FF_FS_RPATH 0`, `FF_VOLUMES 1`, `FF_MIN_SS 512`, `FF_MAX_SS 512`, `FF_FS_TINY 0`, `FF_FS_EXFAT 0`, `FF_FS_NORTC 1` (fixed timestamp 2026-01-01), `FF_FS_LOCK 4`, `FF_FS_REENTRANT 0`. Do not edit `ff.c`. Add the FatFS license to `firmware/README.md`'s licensing line.

- [ ] **Step 2: fs_flash.c**

`diskio` functions (`disk_status`, `disk_initialize`, `disk_read`, `disk_write`, `disk_ioctl` for `CTRL_SYNC`, `GET_SECTOR_COUNT` = `FS_SECTORS`, `GET_SECTOR_SIZE` = 512, `GET_BLOCK_SIZE` = 8) forwarding to the two block functions. `fs_flash_read_blocks`: `memcpy(buf, (const uint8_t *)(XIP_BASE + PICOCO_FS_OFFSET + lba * 512), n * 512)` with bounds check. `fs_flash_write_blocks`: for each 4 KB block touched: copy the block from XIP into a static `uint8_t blk[4096]`, patch the sectors, `uint32_t irq = save_and_disable_interrupts(); flash_range_erase(off, 4096); flash_range_program(off, blk, 4096); restore_interrupts(irq);`. `// ponytail: RMW per 4 KB erase, ~50 ms; capture-to-flash and heavy DW writes feel it. Buffering or a log-structured layer is the upgrade.` `fs_flash_mount`: `f_mount(&fs, "", 1)`; on `FR_NO_FILESYSTEM` call `fs_flash_format()` (static 4 KB work buffer for `f_mkfs`, `MKFS_PARM{FM_FAT, 1, 0, 0, 4096}`), then mount again. Track `mounted`.

- [ ] **Step 3: dw_store_fatfs.c**

A static pool of 6 `FIL` objects (4 drives + capture + rom load) handed out by `open`/`create` (`f_open` with `FA_READ|FA_WRITE`, fall back to `FA_READ` → return 1; `FA_CREATE_ALWAYS` for create); `read`/`write` = `f_lseek` + `f_read`/`f_write`; `size` = `f_size`; `sync` = `f_sync`; `close` = `f_close` and release the slot. Reject names containing `/`. `dw_store_fatfs_init` sets `ops`. Return -1 when the pool is exhausted.

- [ ] **Step 4: plat_pico.c fs/cfg**

`plat_fs_list`: `f_opendir("/")` + `f_readdir`, skipping directories and names starting with `.`; `plat_fs_remove`: `f_unlink`; `plat_fs_format`: `fs_flash_format()`; `plat_cfg_read/write`: `picoco.cfg` at the root via `f_open`/`f_read`/`f_write`/`f_close`, `FA_CREATE_ALWAYS` on write.

- [ ] **Step 5: `dw selftest` in console.c (host and Pico)**

`dw selftest`: creates `selftest.dsk` through `g_store->ops->create` (10 sectors, sector n has byte 0 = n), mounts it on drive 3 read-write, saves and swaps the server's `send` for a local capturing callback, feeds READ drive 3 LSN 5 (expects rc 0, data[0]==5), WRITE drive 3 LSN 7 with 0x5A fill (rc 0), READEX drive 3 LSN 7 + client checksum (data all 0x5A, rc 0), restores `send`, ejects, removes the file, prints `selftest ok` or `selftest FAIL <step>` and returns accordingly. Uses `dw_feed` directly, so it works with no CoCo and no USB host. Host test `selftest_passes` in `test_console.c`: `console_exec("dw selftest") == 0` and output contains `selftest ok`.

- [ ] **Step 6: main.c**

After `dw_init`: `if (fs_flash_mount() == 0) { dw_store_fatfs_init(&g_store); int n = console_run_config(); LOG_I(LOG_M_MAIN, "fs ok, config lines %d", n); } else LOG_E(LOG_M_FS, "fs mount failed");` `g_store` must be initialised before `dw_init`/`console_init` since they keep the pointer; call `dw_store_fatfs_init(&g_store)` unconditionally before `dw_init` and only `console_run_config()` when the mount succeeded.

- [ ] **Step 7: Build both, ctest green, then on target**

`fs format` → `ok`; `fs ls` → empty; `dw selftest` → `selftest ok`; `dw hdbdos off`, `becker native`, `save` → `ok`; `reboot`; `status` shows `mode native` and `hdbdos off` restored from config; `fs ls` shows `picoco.cfg`. Paste the transcript.

- [ ] **Step 8: Commit.**

---

### Task 4: USB mass storage export

**Files:**
- Modify: `firmware/src/usb/usb_descriptors.c` (MSC callbacks), `firmware/src/plat_pico.c` (`plat_fs_export`), `firmware/src/fs_flash.c/.h` (`fs_flash_export_state`), `firmware/README.md`

**Interfaces:**
- Produces: `fs export` unmounts FatFS and presents the partition as a removable drive; `fs import` (or reboot) re-mounts. `bool fs_flash_exporting(void)`.

- [ ] **Step 1: MSC callbacks**

`tud_msc_test_unit_ready_cb`: return `fs_flash_exporting()`; when false also set sense `SCSI_SENSE_NOT_READY, 0x3A, 0x00` (medium not present). `tud_msc_capacity_cb`: `*block_count = FS_SECTORS; *block_size = 512`. `tud_msc_read10_cb`: `fs_flash_read_blocks(lba, buffer, bufsize/512)`, return bytes (respect `offset` within the block: TinyUSB may call with `offset != 0` for partial blocks; handle by reading the whole block into a scratch and copying). `tud_msc_write10_cb`: only when exporting; assemble full 512-byte blocks (TinyUSB delivers `bufsize` multiples of 512 when `CFG_TUD_MSC_EP_BUFSIZE` is 512) and `fs_flash_write_blocks`. `tud_msc_start_stop_cb(lun, power, start, load_eject)`: on eject set `ejected = true`. `tud_msc_is_writable_cb`: true while exporting. `tud_msc_scsi_cb`: return -1 (unsupported) with `SCSI_SENSE_ILLEGAL_REQUEST`.

- [ ] **Step 2: Export state**

In `fs_flash.c`: `static bool exporting;` `int fs_flash_export(bool on)`: on → `f_mount(NULL, "", 0)` (unmount, after `f_sync` on nothing pending: the console is the only writer and `dw` files are closed by `dw eject`; require no drives mounted: return -1 with the console printing `err eject all drives first` if any `drives[n].mounted`), then `exporting = true`; off → `exporting = false`, `fs_flash_mount()`. `plat_fs_export` calls it. The console's `fs export` already exists (Task 9 of Plan A) and prints `ok`; make it also print `usb drive exported; run fs import or reboot when done`. `tud_msc_start_stop_cb` eject does not auto-import (the Mac ejects on unmount; the user runs `fs import`).

- [ ] **Step 3: LED and watchdog**

In `main.c`: solid LED while exporting. Flash writes from `write10` can take 50 ms per 4 KB and run inside `tud_task()`; the watchdog is 8 s, fine.

- [ ] **Step 4: Build and on target**

`dw eject 0..3` as needed, `fs export` → the Mac mounts a volume named after the FAT label (set label `PICOCO` in `fs_flash_format` via `f_setlabel`). Copy a `.dsk` (any 161280-byte file) onto it, eject in Finder, `fs import` → `ok`, `fs ls` lists it with the right size, `dw mount 0 <name>` → `ok`, `dw selftest` still `ok`. Reboot and confirm `fs ls` persists. Record in README: exporting while drives are mounted is refused; macOS may write `.fseventsd`/`._*` files, which `fs ls` hides (names starting with `.`) and which are harmless.

- [ ] **Step 5: Commit.**

---

### Task 5: core1 bus loop, halt release, crash record, `bus drive`

**Files:**
- Create: `firmware/src/bus/bus_core1.c`, `firmware/src/crash.h`, `firmware/src/crash.c`
- Modify: `firmware/src/bus/bus.h/.c` (`bus_drive_set/get`), `firmware/src/console/console.c` (`bus drive on|off`, `crash`, `status` shows drive + last reset), `firmware/src/main.c` (core1 launch, halt release, crash report), `firmware/src/plat.h` + `host/plat_host.c` + `src/plat_pico.c` (`plat_crash_test`, `plat_last_reset`), `firmware/CMakeLists.txt`
- Test: `firmware/tests/test_bus.c` (`drive_flag`), `firmware/tests/test_console.c` (`bus_drive_cmd`, `save` includes `bus drive`)

**Interfaces:**
- Produces:
```c
/* bus.h */
void bus_drive_set(bool on);   /* false = never drive D0..D7 (capture-only, milestone 0.4); default false */
bool bus_drive_get(void);
extern volatile bool bus_drive;   /* read by core1 each cycle */
/* bus_core1.c */
void bus_core1_main(void);     /* never returns; BUS_HOT */
/* crash.h */
typedef struct { uint32_t magic, reason, pc, lr, cfsr, mode, uptime_ms; } crash_rec_t;
void crash_init(void);         /* installs nothing; validates the record left by a previous run */
const crash_rec_t *crash_last(void);   /* NULL if the last reset was clean */
void crash_clear(void);
/* plat.h additions */
void plat_crash_test(void);    /* deliberately fault (Pico) / no-op (host) */
const char *plat_last_reset(void);   /* "power-on" | "watchdog" | "hardfault pc=0x... lr=0x..." | "panic: <msg>" */
```

- [ ] **Step 1: Host-visible parts first (TDD on host)**

`bus_drive` flag in `bus.c` (`bus_init` sets false). Console: `bus drive on|off` and `bus` alone prints `bus drive on|off`; `status` prints `bus drive <on|off>` and `last reset <plat_last_reset()>`; `save` writes `bus drive on` when on; `crash` calls `plat_crash_test()` (host: prints `err crash test is Pico only`). Tests: `drive_flag` (default false, set/get), `bus_drive_cmd` (`bus drive on` → `bus_drive_get()`; `save` output contains `bus drive on`; `bus drive sideways` → err). ctest green. `plat_host.c`: `plat_crash_test` no-op, `plat_last_reset` returns `"host"`.

- [ ] **Step 2: bus_core1.c**

```c
#include "bus.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include PICOCO_BOARD_H
#define OE_MASK (1u << PIN_OE_BUS)
#define RW_MASK (1u << PIN_RW)
#define D_MASK  (0xFFu << PIN_D0)

BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();          /* never restored: core1 does nothing else */
    for (;;) {
        while (sio_hw->gpio_in & OE_MASK) { }     /* wait for a cart cycle (OE_BUS low) */
        uint32_t in  = sio_hw->gpio_in;
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        if (in & RW_MASK) {                       /* CoCo read */
            if (bus_drive) {
                sio_hw->gpio_clr = D_MASK;
                sio_hw->gpio_set = (uint32_t)bus_table[idx] << PIN_D0;
                sio_hw->gpio_oe_set = D_MASK;
                while (!(sio_hw->gpio_in & OE_MASK)) { }
                sio_hw->gpio_oe_clr = D_MASK;
            } else {
                while (!(sio_hw->gpio_in & OE_MASK)) { }
            }
            bus_on_read_done(idx, time_us_32());
        } else {                                  /* CoCo write: last sample before OE_BUS rises */
            uint32_t d;
            do { d = sio_hw->gpio_in; } while (!(d & OE_MASK));
            bus_on_write(idx, (uint8_t)((d >> PIN_D0) & 0xFF), time_us_32());
        }
    }
}
```
`time_us_32()` is a static inline register read; `bus_on_*` are `BUS_HOT`; `bus_drive` and `bus_table` are SRAM data. Confirm with `arm-none-eabi-nm build-pico/picoco.elf | grep -E "bus_core1_main|bus_on_read_done|bus_on_write|bus_set_read|becker_(status|data)_hook|becker_refresh|trace_record"`: every address must be in SRAM (`0x2000xxxx`), none in flash (`0x1xxxxxxx`). Paste that output in the report; it is the acceptance check for the flash-free rule.

- [ ] **Step 3: crash.c**

```c
#include "crash.h"
#include "pico/platform.h"
#include "hardware/watchdog.h"
#include "hardware/structs/scb.h"
#define CRASH_MAGIC 0xC0C0DEAD
static crash_rec_t __uninitialized_ram(rec);
static crash_rec_t last; static bool have_last;
void crash_init(void) { if (rec.magic == CRASH_MAGIC) { last = rec; have_last = true; } rec.magic = 0; }
const crash_rec_t *crash_last(void) { return have_last ? &last : NULL; }
void crash_clear(void) { have_last = false; }
static void __attribute__((noreturn)) record_and_reboot(uint32_t reason, uint32_t pc, uint32_t lr) {
    rec.magic = CRASH_MAGIC; rec.reason = reason; rec.pc = pc; rec.lr = lr;
    rec.cfsr = scb_hw->cfsr; rec.mode = crash_mode_hook ? crash_mode_hook() : 0; rec.uptime_ms = to_ms_since_boot(get_absolute_time());
    watchdog_reboot(0, 0, 0); for (;;) { }
}
void __attribute__((naked)) isr_hardfault(void) {
    __asm volatile("tst lr, #4\n ite eq\n mrseq r0, msp\n mrsne r0, psp\n b hardfault_c");
}
void hardfault_c(uint32_t *frame) { record_and_reboot(1 /*hardfault*/, frame[6], frame[5]); }
void picoco_panic(const char *fmt, ...) { (void)fmt; record_and_reboot(2 /*panic*/, (uint32_t)__builtin_return_address(0), 0); }
```
`crash_mode_hook` is a function pointer set by `main.c` to return `mode_get()`. Add `-DPICO_PANIC_FUNCTION=picoco_panic` to the Pico target so SDK `panic()` calls ours. Reason codes: 1 hardfault, 2 panic. `plat_last_reset` (Pico): if `crash_last()` → format `"hardfault pc=0x%08x lr=0x%08x cfsr=0x%08x mode=%u up=%ums"` or `"panic pc=0x%08x ..."`; else `watchdog_caused_reboot() ? "watchdog" : "power-on"`. `plat_crash_test`: `((void (*)(void))0xFFFFFFF1)();` after a `LOG_E` and a short `sleep_ms(50)` so the log drains? No: the log cannot drain before the fault. Print `crashing now` via the console out callback first, then fault.

- [ ] **Step 4: main.c wiring**

After `console_init`: `crash_init(); crash_mode_hook = mode_get_u32;` then after the config replay: `multicore_launch_core1(bus_core1_main);` then `gpio_put(PIN_HALT, 0);` (release /HALT: spec 8.1) and `LOG_I(LOG_M_MAIN, "core1 up, halt released")`. `smoke` must refuse while core1 runs unless `bus drive off` (it toggles the same pins): in `console.c`, `smoke` returns `err bus drive on` if `bus_drive_get()`; and `plat_smoke` on Pico skips GP26..28 (OE_BUS input, HALT, E) so it cannot fake a cart cycle. On the host `multicore_launch_core1` does not exist; guard the core1 launch in `main.c` only (host never compiles `main.c`).

- [ ] **Step 5: Build both; ctest green; on target**

1. `status` → `last reset power-on`, `bus drive off`, `bus cycles 0`.
2. Jumper GP26 to GND for a moment (OE_BUS low, R/W high from the pull-up): `status` → `bus cycles` > 0, `reads` > 0; `trace dump 4` shows entries with idx `3fff` (all address pins pulled high) and `R`. Remove the jumper.
3. `rom pattern`, `bus drive on`. The pattern fills only indices 0x0000..0x1FFF, so A13 (GP21) must be low to land inside it: jumper GP21 (pin 27) and GP8 (pin 11) to GND, then touch GP26 to GND for a fresh cycle and hold it. `trace dump 1` → `... 1ffe R fe`; while GP26 is held, a meter shows GP0 ≈ 0 V and GP1..GP7 ≈ 3.3 V. Remove jumpers. (Bench note 2026-09-08: with only GP8 low the index is 0x3FFE, outside the pattern, and the byte is 0xFF; a held GP26 logs no new cycle until it is re-touched.)
4. `halt on` → GP27 high (meter), `halt off` → low. Boot state: GP27 high until the release line, then low: confirm with `log main debug` + `log dump` after reboot showing `core1 up, halt released`.
5. `crash` → console prints `crashing now`, port drops, returns; `status` → `last reset hardfault pc=0x... mode=0 up=...ms`. Tag `fw-0.3-halt-ctrl` (the halt circuit is now controllable; the on-CoCo measurement is milestone step 8 in Task 6).

- [ ] **Step 6: Commit.**

---

### Task 6: Breadboard bring-up runbook

**Files:**
- Modify: `docs/breadboard-plan.md` section 6 (milestone table), `firmware/README.md` ("Bring-up" section)

This task produces the runbook; the runs themselves happen when the breakout boards arrive, with the user at the bench. Each row below replaces the corresponding row's firmware column in `docs/breadboard-plan.md` section 6.

- [ ] **Step 1: Write the runbook** (both files, same content, README has the commands and expected console output; breadboard-plan cross-references it):

| Step | Console | Pass check | Tag |
|---|---|---|---|
| 3 | (none) | Pico powered from the CoCo rail: LED blinks 1 Hz; `status` over USB (USB power and CoCo 5 V share only GND; `VBUS` is NC on the final board, on the breadboard do not back-power the CoCo from USB: use a USB cable with VBUS cut, or accept that the Pico is powered by USB during console sessions) | fw-0.1-blink |
| 4 | `bus drive off`, `trace run`, on the CoCo `PEEK(&HC123)`, then `trace dump 8` | dump shows an entry `0123 R ff` (idx = $C123 - $C000); `status` bus reads incremented by the number of PEEKs; `PEEK(&HFF41)` shows `3f41 R` | fw-0.4-bus-capture |
| 5 | (hardware) | LA: OE_BUS low only during E-high of cart cycles | |
| 6 | `rom pattern`, `bus drive on`, `save` | `PEEK(&HC000)` = 0, `PEEK(&HC001)` = 1, `FOR I=0 TO 255: PRINT PEEK(&HC000+I);: NEXT` counts up; slowest CoCo first; on CoCo 3 repeat after `POKE 65497,0` | fw-0.5-rom-static |
| 7 | `fs export`, copy `hdbdos_dw.rom` (8 KB), `fs import`, `rom load hdbdos_dw.rom`, `save` | power-cycle: CoCo autostarts HDB-DOS (or `DOS` enters it); `DIR` fails cleanly | fw-0.6-rom-hdbdos |
| 8 | `log main debug`; LA on /RESET and $C000 | measure Pico cold boot to `core1 up, halt released` vs CoCo reset to first $C000 read; decide Q2/R7/R8 per breadboard-plan 2.3 | fw-0.3-halt-ctrl |
| 9 | `becker loop`, `bus drive on` | `POKE &HFF42,65: PRINT PEEK(&HFF41), PEEK(&HFF42)` → `2 65` (first `PEEK(&HFF41)` after the POKE may read 0 once: single-writer rule) | fw-0.7-becker-loop |
| 10 | `becker bridge`; host: `pyDriveWire --port /dev/tty.usbmodemXXXX1 --speed 115200 <image>` (CDC0) | `DIR` in HDB-DOS lists the image; `LOADM` a program | fw-0.8-bridge |
| 11 | `fs export`, copy a DSK, `fs import`, `dw mount 0 <dsk>`, `becker native`, `save` | `DIR`, `LOADM`, `SAVE` a program, power-cycle, `DIR` still shows it; `dw stats` shows reads/writes, `crc_err 0`, `timeouts 0` | fw-1.0-native |

Also list the three debugging tools per row: `trace dump` + `tools/tracedump.py`, `dw capture on <file>` + `picoco-host --replay`, and `status` counters, with one sentence each on when to reach for it.

- [ ] **Step 2: Commit** (`docs/breadboard-plan.md`, `firmware/README.md`), message `docs: breadboard bring-up runbook with console commands`.

---

## Self-review notes

- Spec coverage: 4.1 pin map → Task 2 board header; 4.3 loop → Task 5; 4.4 flash safety → Tasks 2 (define), 3 (RMW with interrupts off), 5 (nm check); 7 storage + export → Tasks 3, 4; 8 console additions (`dw selftest`, `bus drive`, `crash`) → Tasks 3, 5; 9 crash record + watchdog + isolation modes → Tasks 2, 5, 6; 11 steps 4-5 → Tasks 2-5 and 6.
- Interfaces consistent: `fs_flash_read_blocks/write_blocks` shared by diskio (Task 3) and MSC (Task 4); `bus_drive` read by core1 (Task 5) and set by console; `plat_crash_test`/`plat_last_reset` declared in Task 5 for both platforms.
- Deviations from spec: `bus drive on|off` is new (spec 7 said `mode=diag` drives nothing; this makes it explicit and saveable); `dw selftest` was named in spec 11 and is defined here; `crash` console command is new (spec 9 only defined the record). Reflect these in spec section 8 when Task 5 lands.
- Deferred to a later plan: SD card (`dw_store_fatfs` already speaks FatFS; SD needs a second diskio and SPI pins the Pico 2 breadboard does not have), PIO/DMA engine, bridge-mode host stub, capture buffering in RAM.
