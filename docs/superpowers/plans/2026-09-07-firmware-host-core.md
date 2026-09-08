# PiCoCo Firmware Plan A: Host-Testable Core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and test every firmware module that does not touch a GPIO (DriveWire server, disk images, bus tables, Becker, ROM, log, console) on the Mac, ending with a host binary that boots HDB-DOS in XRoar.

**Architecture:** One C11 static library `picoco_core` compiled for both host and RP2350. Devices talk to the bus engine only through `bus_table[]`, a write-event ring and read hooks; the DriveWire server is a push-style state machine fed bytes and calling a `send` callback. Platform functions (`plat_*`) are resolved at link time: `host/plat_host.c` here, `src/plat_pico.c` in Plan B.

**Tech Stack:** C11, CMake + Ninja, clang on macOS with `-fsanitize=address,undefined`, ctest, Python 3 for tools. Pico SDK 2.1+ only in Plan B.

**Spec:** `docs/superpowers/specs/2026-09-07-firmware-design.md` (read it first; section numbers below refer to it).

## Global Constraints

- Language C11, `-Wall -Wextra`, no warnings in new code. No dynamic allocation except in `host/` and tests.
- Host build: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host && ctest --test-dir build-host --output-on-failure`. Install once: `brew install cmake ninja`.
- Tests are plain C with `firmware/tests/test.h`, one executable per module, registered with `add_test`. Sanitizers on in the host build.
- No `printf`/logging inside `bus_on_*`, `becker_*` or anything the core1 loop will call. Use the stats structs.
- Protocol constants are exactly those in spec 6.3 (from pyDriveWire `dwconstants.py`). Checksum = 16-bit sum of bytes, big-endian.
- Sector size is 256. LSN is 3 bytes big-endian on the wire.
- Commit once per task after review passes, staging only the files the task touched under `firmware/` (never `PiCoCo/*.v1-backup` or KiCad files). Message: `firmware: <task summary>` and end with the two attribution lines:
  `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01RmFQ1HMPQxdKfGPZdaihVx`. The user approved per-task commits for this plan on 2026-09-07.
- Keep it lazy: smallest code that passes the tests in the task. No abstractions beyond those named here. Mark deliberate shortcuts with a `// ponytail:` comment naming the ceiling.

## File map

| Path | Responsibility | Task |
|---|---|---|
| `firmware/CMakeLists.txt` | host/pico switch, `picoco_core` lib, tests, `picoco-host` | 1 |
| `firmware/tests/test.h` | TEST/ASSERT/RUN macros | 1 |
| `firmware/src/plat.h` | platform functions, link-time resolved | 1 |
| `firmware/host/plat_host.c` | POSIX implementation of `plat.h` | 1, 9 |
| `firmware/src/ring.h` | SPSC byte ring | 1 |
| `firmware/src/dw/dw_util.h/.c` | checksum, LSN pack/unpack | 2 |
| `firmware/src/dw/dw_store.h`, `dw_store_posix.c` | storage ops + POSIX impl | 2 |
| `firmware/src/dw/dw_disk.h/.c` | image format detection, LSN mapping | 2 |
| `firmware/src/dw/dw.h`, `dw_server.c` | protocol state machine | 3, 4 |
| `firmware/host/picoco_host.c` | TCP server + stdin console | 5, 9 |
| `firmware/tools/dwtest.py` | DriveWire client exerciser | 5 |
| `firmware/src/bus/bus.h/.c` | table, write ring, hooks, trace, stats | 6 |
| `firmware/src/dev/device.h/.c`, `rom.h/.c`, `becker.h/.c` | devices | 7 |
| `firmware/src/log.h/.c` | ring-buffered leveled log | 8 |
| `firmware/src/console/console.h/.c`, `mode.h/.c` | command parser, modes, config = saved commands | 9 |
| `firmware/host/sim_bus.h/.c` | virtual CoCo | 10 |
| `firmware/tools/tracedump.py` | trace decoder | 10 |
| `firmware/tests/test_*.c`, `tests/fixtures/` | tests | each |

Spec deviation recorded here: `picoco.cfg` is a list of console commands replayed at boot (`save` writes them), not `key=value`. Same information, zero extra parser.

---

### Task 1: Skeleton, test harness, platform header, ring

**Files:**
- Create: `firmware/CMakeLists.txt`, `firmware/tests/test.h`, `firmware/tests/test_ring.c`, `firmware/src/ring.h`, `firmware/src/plat.h`, `firmware/host/plat_host.c`, `firmware/.gitignore`, `firmware/README.md`

**Interfaces:**
- Produces: `test.h` macros `TEST(name)`, `ASSERT(c)`, `ASSERT_EQ(a,b)`, `RUN(name)`, `TEST_MAIN_END`; `ring.h` type `ring_t` with `ring_init(r, buf, size_pow2)`, `ring_push(r, byte)->bool`, `ring_pop(r, &byte)->bool`, `ring_count(r)`, `ring_free(r)`, `ring_peek(r,&byte)->bool`; `plat.h` functions listed in step 4.

- [ ] **Step 1: Write the test harness and a failing ring test**

`firmware/tests/test.h`:
```c
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static int t_fail = 0, t_run = 0;
#define TEST(name) static void name(void)
#define ASSERT(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); t_fail++; return; } } while (0)
#define ASSERT_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); t_fail++; return; } } while (0)
#define ASSERT_MEMEQ(a, b, n) do { if (memcmp((a), (b), (n)) != 0) { printf("  FAIL %s:%d: memcmp %s %s\n", __FILE__, __LINE__, #a, #b); t_fail++; return; } } while (0)
#define RUN(name) do { t_run++; printf("%s\n", #name); name(); } while (0)
#define TEST_MAIN_END printf("%d tests, %d failed\n", t_run, t_fail); return t_fail ? 1 : 0;
```

`firmware/tests/test_ring.c`:
```c
#include "test.h"
#include "ring.h"
TEST(push_pop_order) {
    uint8_t buf[8]; ring_t r; ring_init(&r, buf, 8);
    ASSERT_EQ(ring_count(&r), 0); ASSERT_EQ(ring_free(&r), 7);
    for (int i = 0; i < 7; i++) ASSERT(ring_push(&r, (uint8_t)i));
    ASSERT(!ring_push(&r, 99));              /* full: 7 usable in a size-8 ring */
    uint8_t b; ASSERT(ring_peek(&r, &b)); ASSERT_EQ(b, 0);
    for (int i = 0; i < 7; i++) { ASSERT(ring_pop(&r, &b)); ASSERT_EQ(b, i); }
    ASSERT(!ring_pop(&r, &b));
}
TEST(wraps) {
    uint8_t buf[4]; ring_t r; ring_init(&r, buf, 4); uint8_t b;
    for (int i = 0; i < 100; i++) { ASSERT(ring_push(&r, (uint8_t)i)); ASSERT(ring_pop(&r, &b)); ASSERT_EQ(b, (uint8_t)i); }
}
int main(void) { RUN(push_pop_order); RUN(wraps); TEST_MAIN_END }
```

- [ ] **Step 2: Write CMakeLists.txt with the host branch**

`firmware/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.20)
option(PICOCO_HOST "Build host library, tests and picoco-host instead of Pico firmware" OFF)

if(NOT PICOCO_HOST)
  # Pico branch is completed and verified in Plan B. Kept minimal here.
  set(PICO_BOARD pico2 CACHE STRING "")
  include(pico_sdk_import.cmake)
  project(picoco C CXX ASM)
  pico_sdk_init()
  add_executable(picoco src/main.c)
  target_link_libraries(picoco pico_stdlib)
  pico_add_extra_outputs(picoco)
  return()
endif()

project(picoco_host C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -g -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)
add_compile_definitions(PICOCO_HOST=1 PICOCO_LOG_LEVEL=3)

set(CORE_SRC
  src/dw/dw_util.c src/dw/dw_disk.c src/dw/dw_store_posix.c src/dw/dw_server.c
  src/bus/bus.c src/dev/device.c src/dev/rom.c src/dev/becker.c
  src/log.c src/console/console.c src/console/mode.c)
# Only files that exist are compiled, so early tasks build.
set(CORE_EXISTING)
foreach(f ${CORE_SRC})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f})
    list(APPEND CORE_EXISTING ${f})
  endif()
endforeach()
add_library(picoco_core STATIC ${CORE_EXISTING} host/plat_host.c)
target_include_directories(picoco_core PUBLIC src src/dw src/bus src/dev src/console host)

enable_testing()
file(GLOB TEST_SRCS tests/test_*.c)
foreach(t ${TEST_SRCS})
  get_filename_component(name ${t} NAME_WE)
  add_executable(${name} ${t} $<$<TARGET_EXISTS:sim_bus>:>)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/host/sim_bus.c)
    target_sources(${name} PRIVATE host/sim_bus.c)
  endif()
  target_link_libraries(${name} picoco_core)
  target_include_directories(${name} PRIVATE tests)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/host/picoco_host.c)
  add_executable(picoco-host host/picoco_host.c)
  target_link_libraries(picoco-host picoco_core)
endif()
```
Remove the stray `$<$<TARGET_EXISTS:sim_bus>:>` generator expression if CMake complains; the `if(EXISTS ...)` line is what matters.

- [ ] **Step 3: Run to see the ring test fail to build**

Run: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host`
Expected: error, `ring.h` not found.

- [ ] **Step 4: Write ring.h and plat.h**

`firmware/src/ring.h` (SPSC, power-of-two size, one slot wasted):
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { uint8_t *buf; uint32_t mask; volatile uint32_t head, tail; } ring_t;
static inline void ring_init(ring_t *r, uint8_t *buf, uint32_t size_pow2) { r->buf = buf; r->mask = size_pow2 - 1; r->head = r->tail = 0; }
static inline uint32_t ring_count(const ring_t *r) { return (r->head - r->tail) & r->mask; }
static inline uint32_t ring_free(const ring_t *r) { return r->mask - ring_count(r); }
static inline bool ring_push(ring_t *r, uint8_t b) {
    uint32_t h = r->head, n = (h + 1) & r->mask;
    if (n == r->tail) return false;
    r->buf[h] = b; __atomic_store_n(&r->head, n, __ATOMIC_RELEASE); return true;
}
static inline bool ring_peek(const ring_t *r, uint8_t *b) { if (r->head == r->tail) return false; *b = r->buf[r->tail]; return true; }
static inline bool ring_pop(ring_t *r, uint8_t *b) {
    uint32_t t = r->tail; if (__atomic_load_n(&r->head, __ATOMIC_ACQUIRE) == t) return false;
    *b = r->buf[t]; __atomic_store_n(&r->tail, (t + 1) & r->mask, __ATOMIC_RELEASE); return true;
}
```

`firmware/src/plat.h`:
```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Platform functions. host/plat_host.c on the Mac, src/plat_pico.c on the RP2350 (Plan B). */
uint32_t plat_now_us(void);
uint32_t plat_now_ms(void);
int  plat_fs_list(void (*cb)(const char *name, uint32_t size, void *ctx), void *ctx); /* 0 ok */
int  plat_fs_remove(const char *name);
int  plat_fs_format(void);
int  plat_fs_export(bool on);                 /* USB MSC export; host: no-op returning -1 */
int  plat_cfg_read(char *buf, size_t max);    /* bytes read, <0 none */
int  plat_cfg_write(const char *buf, size_t n);
void plat_reboot(bool bootsel);
void plat_halt(bool assert_halt);
void plat_smoke(void);                        /* GPIO toggle test; host: no-op */
size_t plat_bridge_read(uint8_t *buf, size_t n);   /* CDC0 in bridge mode; host: 0 */
size_t plat_bridge_write(const uint8_t *buf, size_t n);
const char *plat_fs_dir(void);                /* host only: directory backing the "filesystem" */
void plat_host_set_dir(const char *dir);      /* host only */
```

`firmware/host/plat_host.c`: implement with `clock_gettime(CLOCK_MONOTONIC)` for time, a static `dir` (default `"."`) for `plat_fs_dir`, `opendir/readdir/stat` for list, `unlink` for remove, format = remove every regular file, cfg read/write on `<dir>/picoco.cfg`, `plat_reboot` = `exit(0)`, halt/smoke = `fprintf(stderr, ...)`, export returns -1, bridge returns 0.

- [ ] **Step 5: Build and run**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: `test_ring` passes, `2 tests, 0 failed`.

- [ ] **Step 6: Add .gitignore and README**

`firmware/.gitignore`: `build*/`, `roms/*` except `roms/.gitkeep`, `*.cfg` under host runs.
`firmware/README.md`: the two build commands from Global Constraints, the `brew install` line, and a pointer to the spec. Also create an empty `firmware/src/main.c` containing a blink loop for the Pico branch (`#include "pico/stdlib.h"`; toggle `PICO_DEFAULT_LED_PIN` every 500 ms) and an untouched copy of `pico_sdk_import.cmake` from the SDK's `external/` directory (download from https://raw.githubusercontent.com/raspberrypi/pico-sdk/master/external/pico_sdk_import.cmake). These are compiled only in Plan B.

---

### Task 2: Checksum, storage ops, disk image formats

**Files:**
- Create: `firmware/src/dw/dw_util.h`, `dw_util.c`, `dw_store.h`, `dw_store_posix.c`, `dw_disk.h`, `dw_disk.c`
- Test: `firmware/tests/test_dw_disk.c`

**Interfaces:**
- Produces:
```c
/* dw_util.h */
uint16_t dw_checksum(const uint8_t *p, size_t n);          /* 16-bit sum */
uint32_t dw_lsn_unpack(const uint8_t p[3]);                 /* big-endian */
/* dw_store.h */
typedef struct { void *h; } dw_file;
typedef struct dw_store_ops {
    int  (*open)(void *ctx, const char *name, bool write, dw_file *f); /* 0 rw, 1 opened read-only, <0 error */
    int  (*read)(dw_file *f, uint32_t off, void *buf, uint32_t n);      /* bytes read (0 at EOF), <0 error */
    int  (*write)(dw_file *f, uint32_t off, const void *buf, uint32_t n);/* bytes written, <0 error */
    int  (*size)(dw_file *f, uint32_t *out);
    int  (*sync)(dw_file *f);
    void (*close)(dw_file *f);
} dw_store_ops;
typedef struct { const dw_store_ops *ops; void *ctx; } dw_store;
void dw_store_posix_init(dw_store *s, const char *dir);     /* names resolve to dir/name */
/* dw_disk.h */
typedef enum { DW_FMT_RAW, DW_FMT_JVC, DW_FMT_VDK, DW_FMT_OS9 } dw_fmt;
typedef struct {
    dw_store *store; dw_file f; bool mounted, read_only;
    dw_fmt fmt; uint32_t byte_offset; uint32_t sectors;    /* sectors in the image now */
    char name[32];
} dw_disk;
int  dw_disk_open(dw_store *store, const char *name, bool read_only, dw_disk *d); /* 0 ok, -1 not found, -2 unsupported (JVC sector size != 256) */
void dw_disk_close(dw_disk *d);
int  dw_disk_read(dw_disk *d, uint32_t lsn, uint8_t buf[256]);   /* 0 ok, DW_E_EOF, DW_E_READ; short read past end returns zeros with 0 */
int  dw_disk_write(dw_disk *d, uint32_t lsn, const uint8_t buf[256]); /* 0, DW_E_WRPROT, DW_E_EOF (OS9 only), DW_E_WRITE; extends non-OS9 images and updates d->sectors */
```
Error constants live in `dw_util.h`: `DW_E_OK 0, DW_E_EOF 0xD3, DW_E_WRPROT 0xF2, DW_E_CRC 0xF3, DW_E_READ 0xF4, DW_E_WRITE 0xF5, DW_E_NOTRDY 0xF6`.

Format rules (pyDriveWire `dwfile.py`): VDK if bytes 0..1 are `"dk"`, `byte_offset` = u16 LE at offset 2, sectors = (size - offset)/256. JVC if size % 256 != 0: header = size % 256 bytes (1..5): spt, sides, sector-size code (`128 << code`, must equal 256 else -2), first sector, flags; `byte_offset` = header length. OS9 if bytes 0..2 (DD_TOT, big-endian 24-bit), byte 3 (DD_TKS), byte 0x10 (DD_FMT, sides = (fmt&1)+1), u16 BE at 0x11 (DD_SPT) satisfy `DD_TKS == DD_SPT` and `DD_TOT == (DD_TOT / DD_SPT / sides) * DD_SPT * sides` with all nonzero. Otherwise RAW. Reads: `off = byte_offset + lsn*256`; EOF if `lsn >= sectors` for reads; short read fills zeros. Writes: read-only → WRPROT; OS9 and `lsn >= sectors` → EOF; else write, extend `sectors = max(sectors, lsn+1)`.

- [ ] **Step 1: Write failing tests**

`firmware/tests/test_dw_disk.c` creates a temp dir with `mkdtemp`, writes synthetic images with a helper `mk(name, hdr, hdrlen, nsect)` (header bytes then `nsect` sectors where byte 0 of sector n is `n & 0xFF` and byte 1 is `n >> 8`), then:
```c
TEST(raw_detect) { mk("raw.dsk", NULL, 0, 630); dw_disk d; ASSERT_EQ(dw_disk_open(&st, "raw.dsk", false, &d), 0);
    ASSERT_EQ(d.fmt, DW_FMT_RAW); ASSERT_EQ(d.byte_offset, 0); ASSERT_EQ(d.sectors, 630); dw_disk_close(&d); }
TEST(jvc_detect) { uint8_t h[1] = {18}; mk("j.dsk", h, 1, 630); ... ASSERT_EQ(d.fmt, DW_FMT_JVC); ASSERT_EQ(d.byte_offset, 1);
    uint8_t s[256]; ASSERT_EQ(dw_disk_read(&d, 5, s), 0); ASSERT_EQ(s[0], 5); }
TEST(jvc_bad_sector_size) { uint8_t h[3] = {18, 1, 2}; mk("j512.dsk", h, 3, 10); ASSERT_EQ(dw_disk_open(&st, "j512.dsk", false, &d), -2); }
TEST(vdk_detect) { uint8_t h[12] = {'d','k',12,0, 1,1,0,0, 35,1, 0,0}; mk("v.vdk", h, 12, 630); ... fmt VDK, byte_offset 12, read lsn 7 gives s[0]==7 }
TEST(os9_detect) { /* raw image whose sector 0 has DD_TOT=630 (00 02 76), DD_TKS=18, DD_FMT=0 at 0x10, DD_SPT=0x0012 at 0x11 */ ... fmt OS9;
    ASSERT_EQ(dw_disk_write(&d, 630, s), DW_E_EOF); }
TEST(read_eof_and_zero_fill) { raw 10 sectors: read lsn 10 -> DW_E_EOF; truncate file to 9.5 sectors via truncate(); read lsn 9 -> 0 and tail bytes zero }
TEST(write_extends_and_wrprot) { raw 10: write lsn 20 -> 0, d.sectors == 21, read back matches; open with read_only=true, write -> DW_E_WRPROT }
TEST(checksum) { uint8_t z[256] = {0}; ASSERT_EQ(dw_checksum(z, 256), 0); uint8_t o[3] = {0xFF, 0xFF, 0x02}; ASSERT_EQ(dw_checksum(o, 3), 0x200);
    uint8_t l[3] = {0x01, 0x02, 0x03}; ASSERT_EQ(dw_lsn_unpack(l), 0x010203); }
TEST(missing_file) { ASSERT_EQ(dw_disk_open(&st, "nope.dsk", false, &d), -1); }
```
Write every test in full (the `...` above must be expanded in the real file).

- [ ] **Step 2: Build, confirm failure** (missing headers).

- [ ] **Step 3: Implement** `dw_util.c`, `dw_store_posix.c` (`open` with `O_RDWR`, fall back to `O_RDONLY` returning 1; `pread`/`pwrite`; `fstat` for size; `fsync`; store the fd in `f->h` via `(void*)(intptr_t)`), `dw_disk.c` per the rules above. Keep `dw_disk_open` under 80 lines: read the first 512 bytes once, then run the four checks in order.

- [ ] **Step 4: Build and run** `ctest` — all pass, ASan clean.

---

### Task 3: DriveWire server core: parser, control ops, READ, WRITE

**Files:**
- Create: `firmware/src/dw/dw.h`, `firmware/src/dw/dw_server.c`
- Test: `firmware/tests/test_dw_server.c`

**Interfaces:**
- Consumes: Task 2 (`dw_store`, `dw_disk_*`, `dw_checksum`, `DW_E_*`).
- Produces:
```c
/* dw.h */
#define DW_MAX_DRIVES 4
#define DW_PAYLOAD_TIMEOUT_MS 250
#define DW_HDBDOS_DISK_SECTORS 630
typedef void (*dw_send_fn)(void *ctx, const uint8_t *buf, size_t n);
typedef void (*dw_capture_fn)(void *ctx, int dir /*0 rx,1 tx*/, const uint8_t *buf, size_t n);
typedef struct { uint32_t ops[256], reads, writes, read_err, write_err, crc_err, timeouts, unknown_op, notrdy; } dw_stats;
typedef enum { DW_IDLE, DW_PAYLOAD, DW_READEX_CKSUM } dw_state;
typedef struct dw_server {
    dw_store *store; dw_send_fn send; void *send_ctx;
    dw_capture_fn capture; void *capture_ctx;
    dw_disk drives[DW_MAX_DRIVES];
    bool hdbdos;
    dw_state state; uint8_t op; uint8_t buf[264]; uint16_t have, need;
    uint32_t last_rx_ms;
    uint8_t sector[256]; uint16_t sector_sum; uint8_t pending_rc;
    int64_t time_base;      /* unix seconds at time_base_ms */
    uint32_t time_base_ms;
    dw_stats stats;
} dw_server;
void dw_init(dw_server *s, dw_store *store, dw_send_fn send, void *ctx);
void dw_feed(dw_server *s, const uint8_t *buf, size_t n, uint32_t now_ms);
void dw_tick(dw_server *s, uint32_t now_ms);
int  dw_mount(dw_server *s, int drive, const char *name, bool read_only);   /* dw_disk_open result */
void dw_eject(dw_server *s, int drive);
void dw_time_set(dw_server *s, int64_t unix_secs, uint32_t now_ms);
void dw_set_capture(dw_server *s, dw_capture_fn fn, void *ctx);
```

Opcodes (dw.h `#define`s): `DW_OP_NOP 0x00, DW_OP_NAMEOBJ_MOUNT 0x01, DW_OP_NAMEOBJ_CREATE 0x02, DW_OP_TIME 0x23, DW_OP_SERREAD 0x43, DW_OP_SERGETSTAT 0x44, DW_OP_SERINIT 0x45, DW_OP_PRINTFLUSH 0x46, DW_OP_GETSTAT 0x47, DW_OP_INIT 0x49, DW_OP_PRINT 0x50, DW_OP_READ 0x52, DW_OP_SETSTAT 0x53, DW_OP_TERM 0x54, DW_OP_WRITE 0x57, DW_OP_DWINIT 0x5A, DW_OP_SERREADM 0x63, DW_OP_SERWRITEM 0x64, DW_OP_REREAD 0x72, DW_OP_REWRITE 0x77, DW_OP_FASTWRITE_BASE 0x80 (..0x8F), DW_OP_SERWRITE 0xC3, DW_OP_SERSETSTAT 0xC4, DW_OP_SERTERM 0xC5, DW_OP_READEX 0xD2, DW_OP_REREADEX 0xF2, DW_OP_RESET3 0xF8, DW_OP_RESET2 0xFE, DW_OP_RESET1 0xFF`.

Behaviour (pyDriveWire `dwserver.py` cmdRead/cmdWrite/cmdTime):
- READ/REREAD payload 4 (drive, LSN3). Reply always 259 bytes: rc, checksum(2, BE) of the data actually sent, data(256). rc E_NOTRDY if drive unmounted (data zeros); E_EOF if `lsn >= sectors` and not hdbdos; else `dw_disk_read` result.
- HDB-DOS mode (`s->hdbdos`, default true): `drive = lsn / 630; lsn %= 630` for READ/READEX/WRITE; drive byte ignored; if the resulting drive >= DW_MAX_DRIVES → E_NOTRDY. EOF check skipped (zeros past end).
- WRITE/REWRITE payload 262 (drive, LSN3, data256, cksum2). rc only. Order: checksum mismatch → E_CRC (nothing written); unmounted → E_NOTRDY; else `dw_disk_write` result.
- TIME reply 6 bytes from `gmtime` of `time_base + (now_ms - time_base_ms)/1000`: year-1900, month 1..12, day, hour, min, sec. Default base 2026-01-01 00:00:00 UTC = 1767225600.
- NOP/TERM/RESETx: nothing. INIT: nothing sent, `state = DW_IDLE`. DWINIT payload 1 → reply `0xFF`.
- Unknown opcode: `stats.unknown_op++`, stay IDLE.
- `stats.ops[op]++` on every opcode accepted.
- Timeout: in `dw_feed` set `last_rx_ms = now_ms` per byte; `dw_tick`: if `state != DW_IDLE && now_ms - last_rx_ms > 250` → `state = DW_IDLE; stats.timeouts++`.
- Every `send` goes through a static `tx(s, buf, n)` that also calls `capture(ctx, 1, ...)`; `dw_feed` calls `capture(ctx, 0, ...)` on entry.

Parser: `static uint16_t payload_len(uint8_t op)` returns 0 for no-payload ops, 4 READ family, 262 WRITE family, 1 DWINIT/PRINT/SERINIT/SERTERM/FASTWRITE/NAMEOBJ(len byte), 2 GETSTAT/SETSTAT/SERGETSTAT/SERSETSTAT/SERWRITE/SERWRITEM/SERREADM. `dw_feed` loop: IDLE → read op, `need = payload_len`, if 0 dispatch now else `state = DW_PAYLOAD, have = 0`. PAYLOAD → copy bytes until `have == need`, then `dispatch()`; dispatch may set `need` larger and return to keep collecting (variable-length ops, Task 4) or set state.

- [ ] **Step 1: Write failing tests**

Helper in the test: a `send` callback appending to a global `out[4096]`, `outn`; a `feed(bytes...)` macro; `setup()` that makes a temp dir with `mk()` images as in Task 2 and mounts `raw.dsk` on drive 0 with hdbdos off.
```c
TEST(nop_and_reset_send_nothing) { feed 0x00, 0xFF, 0xFE, 0xF8; ASSERT_EQ(outn, 0); ASSERT_EQ(s.stats.ops[0xFF], 1); }
TEST(dwinit_replies_ff) { feed 0x5A, 'A'; ASSERT_EQ(outn, 1); ASSERT_EQ(out[0], 0xFF); }
TEST(read_ok) { feed 0x52, 0, 0,0,5; ASSERT_EQ(outn, 259); ASSERT_EQ(out[0], 0); ASSERT_EQ((out[1]<<8)|out[2], dw_checksum(out+3, 256)); ASSERT_EQ(out[3], 5); }
TEST(read_unmounted_is_notrdy_with_full_reply) { feed 0x52, 2, 0,0,0; ASSERT_EQ(outn, 259); ASSERT_EQ(out[0], DW_E_NOTRDY); ASSERT_EQ(out[1]|out[2], 0); }
TEST(read_eof) { feed 0x52, 0, 0,2,0x76 /*630*/; ASSERT_EQ(out[0], DW_E_EOF); ASSERT_EQ(outn, 259); }
TEST(hdbdos_splits_drive_by_lsn) { s.hdbdos = true; mount "raw2.dsk" on 1; feed 0x52, 0, 0,2,0x77 /*631 -> drive1 lsn1*/; ASSERT_EQ(out[0], 0); ASSERT_EQ(out[3], 1); /* drive 1's sector 1 marker */ }
TEST(hdbdos_past_end_reads_zeros) { s.hdbdos = true; feed 0x52, 0, 0,2,0x00 /*512 < 630, beyond a 10-sector image*/; ASSERT_EQ(out[0], 0); }
TEST(write_ok_then_read_back) { uint8_t d[256]; fill 0xA5; sum = dw_checksum(d,256); feed 0x57, 0, 0,0,3, d[256], sum>>8, sum&0xFF; ASSERT_EQ(outn,1); ASSERT_EQ(out[0],0); outn=0; feed 0x52,0,0,0,3; ASSERT_EQ(out[3],0xA5); }
TEST(write_bad_checksum_is_crc_and_untouched) { ... sum ^ 1 ...; ASSERT_EQ(out[0], DW_E_CRC); read back sector 3 still original; ASSERT_EQ(s.stats.crc_err, 1); }
TEST(write_readonly_is_wrprot) { mount "raw.dsk" on 3 read_only; write drive 3 -> DW_E_WRPROT }
TEST(payload_split_across_feeds) { feed 0x52, 0; feed 0,0; feed 5; ASSERT_EQ(outn, 259); }
TEST(payload_timeout_resets) { feed(0x52, 0, 0) at t=0; dw_tick(&s, 300); ASSERT_EQ(s.state, DW_IDLE); ASSERT_EQ(s.stats.timeouts, 1); feed 0x00 at 300; ASSERT_EQ(outn, 0); }
TEST(unknown_op_ignored) { feed 0x99; ASSERT_EQ(outn, 0); ASSERT_EQ(s.stats.unknown_op, 1); feed 0x5A, 0; ASSERT_EQ(outn, 1); }
TEST(time_default_and_set) { feed 0x23; ASSERT_EQ(outn, 6); ASSERT_EQ(out[0], 126); ASSERT_EQ(out[1], 1); ASSERT_EQ(out[2], 1); outn = 0;
    dw_time_set(&s, 1767225600 + 3661, 1000); feed_at(0x23, 2000); ASSERT_EQ(out[3], 1); ASSERT_EQ(out[4], 1); ASSERT_EQ(out[5], 2); }
TEST(capture_sees_both_directions) { set capture appending (dir,byte) pairs; feed 0x5A, 'A'; expect rx 2 bytes then tx 1 byte }
```
Expand every test fully.

- [ ] **Step 2: Build, confirm failure.**
- [ ] **Step 3: Implement `dw_server.c`** as described. Keep handlers as one `static void dispatch(dw_server *s, uint32_t now_ms)` switch plus `do_read`, `do_write`, `do_time` helpers. Use `gmtime_r`.
- [ ] **Step 4: Build and run** `ctest` — all pass.

---

### Task 4: READEX, serial/print/named-object stubs

**Files:**
- Modify: `firmware/src/dw/dw_server.c`, `firmware/src/dw/dw.h`
- Test: `firmware/tests/test_dw_server.c` (append)

**Interfaces:** same as Task 3; adds `DW_READEX_CKSUM` state handling.

Behaviour (pyDriveWire cmdReadEx):
- READEX/REREADEX payload 4. Compute rc and data exactly as READ. Send data(256) only. Store `pending_rc`, `sector_sum = checksum(data)`, `state = DW_READEX_CKSUM`, `need = 2, have = 0`.
- In `DW_READEX_CKSUM` collect 2 bytes. If `pending_rc == E_OK` and client sum != `sector_sum` → rc = E_CRC, `stats.crc_err++`. Send 1 byte rc. State IDLE.
- If the 2 bytes never arrive, the 250 ms timeout returns to IDLE and sends nothing (the CoCo retries with REREADEX).
- SERREAD: reply `00 00`. SERREADM payload 2 (chan, count): reply one byte `0x00`... pyDriveWire replies with the bytes available; with no channels reply nothing beyond what the CoCo expects: send `count` zero bytes? No: reply exactly 0 bytes is unsafe. Spec says `$00` for count; implement: reply 1 byte `0x00`. `// ponytail: no vserial channels; real reply when networking lands`.
- SERGETSTAT (2), SERINIT (1), SERTERM (1), SERWRITE (2), FASTWRITE 0x80..0x8F (1), PRINT (1), PRINTFLUSH (0), GETSTAT/SETSTAT (2): consume, no reply.
- SERSETSTAT payload 2 (chan, code); if code == 0x28 extend `need` by 26 and keep collecting; no reply.
- SERWRITEM payload 2 (chan, count) then extend `need` by count; no reply.
- NAMEOBJ_MOUNT/CREATE payload 1 (len) then extend by len; reply 1 byte `0x00`.

- [ ] **Step 1: Append failing tests**
```c
TEST(readex_ok) { feed 0xD2, 0, 0,0,5; ASSERT_EQ(outn, 256); ASSERT_EQ(out[0], 5); uint16_t sum = dw_checksum(out, 256); outn = 0;
    feed sum>>8, sum&0xFF; ASSERT_EQ(outn, 1); ASSERT_EQ(out[0], 0); ASSERT_EQ(s.state, DW_IDLE); }
TEST(readex_bad_client_checksum) { ... feed 0xFF, 0xFF; ASSERT_EQ(out[0], DW_E_CRC); }
TEST(readex_unmounted) { feed 0xD2, 2, 0,0,0; ASSERT_EQ(outn, 256); all zero; feed 0,0; ASSERT_EQ(out[0], DW_E_NOTRDY); }
TEST(readex_checksum_timeout_sends_nothing) { feed 0xD2, 0, 0,0,5 at 0; outn = 0; dw_tick(&s, 300); ASSERT_EQ(outn, 0); ASSERT_EQ(s.state, DW_IDLE); }
TEST(rereadex_same_as_readex) { 0xF2 path identical to readex_ok }
TEST(serread_no_data) { feed 0x43; ASSERT_EQ(outn, 2); ASSERT_EQ(out[0] | out[1], 0); }
TEST(sersetstat_comst_consumes_26_more) { feed 0xC4, 1, 0x28; then 26 bytes of 0x55; then 0x5A, 'A'; ASSERT_EQ(outn, 1); ASSERT_EQ(out[0], 0xFF); }
TEST(sersetstat_other_code) { feed 0xC4, 1, 0x29; feed 0x5A, 'A'; ASSERT_EQ(outn, 1); }
TEST(serwritem_consumes_count) { feed 0x64, 1, 3, 'a','b','c'; feed 0x5A,'A'; ASSERT_EQ(outn, 1); }
TEST(nameobj_replies_zero) { feed 0x01, 3, 'a','b','c'; ASSERT_EQ(outn, 1); ASSERT_EQ(out[0], 0); }
TEST(fastwrite_and_print_consumed) { feed 0x81, 'x', 0x50, 'y', 0x46, 0x47, 0, 0, 0x53, 0, 0; ASSERT_EQ(outn, 0); }
```
- [ ] **Step 2: Build, confirm failure.**
- [ ] **Step 3: Implement.** `dispatch()` handles the extension by returning after `s->need += n` without changing state.
- [ ] **Step 4: Build and run** `ctest` — all pass.

---

### Task 5: picoco-host TCP server and dwtest.py

**Files:**
- Create: `firmware/host/picoco_host.c`, `firmware/tools/dwtest.py`
- Modify: `firmware/README.md` (usage + XRoar check)

**Interfaces:**
- Consumes: Task 3/4 `dw_*`, Task 2 `dw_store_posix_init`.
- Produces: `picoco-host [--dir DIR] [--port 65504] [--mount N=FILE[,ro]]... [--hdbdos on|off] [--replay FILE]`. Stdin console is added in Task 9; leave a `console_feed` hook commented with `// Task 9`.

- [ ] **Step 1: Write dwtest.py first (it is the test)**

Python 3, stdlib only. `argparse`: `--host 127.0.0.1 --port 65504 --image PATH --drive N --scratch PATH --hdbdos`. Class `DW` with `sock`, methods:
```python
def dwinit(self): self.s.sendall(b'\x5aA'); assert self.recv(1) == b'\xff'
def read(self, drive, lsn): self.s.sendall(bytes([0x52, drive]) + lsn.to_bytes(3,'big')); r = self.recv(259); return r[0], r[1:3], r[3:]
def readex(self, drive, lsn, corrupt=False): send 0xD2...; data = self.recv(256); cs = sum(data) & 0xFFFF; if corrupt: cs ^= 1; send cs BE; rc = self.recv(1)[0]; return rc, data
def write(self, drive, lsn, data, corrupt=False): send 0x57, drive, lsn3, data, cs; return self.recv(1)[0]
def time(self): send 0x23; return self.recv(6)
```
`recv(n)` loops until n bytes or 2 s timeout (raise). Checks, each printing `ok`/`FAIL` and counting: (1) dwinit; (2) every sector of `--image` via readex matches the file (skip header bytes for JVC by size%256; VDK offset from header); (3) `read` of sector 0 checksum matches data; (4) `readex(corrupt=True)` → rc 0xF3; (5) `read(3, 0)` on unmounted drive → rc 0xF6 and 259 bytes; (6) if `--scratch`: write 0xA5*256 to lsn 7 → rc 0, readex back equal, `write(corrupt=True)` → 0xF3 and sector unchanged; (7) `time()` year byte ≥ 126. When `--hdbdos`, addresses use `lsn + drive*630` with drive byte 0. Exit 1 on any FAIL.

- [ ] **Step 2: Write picoco_host.c**

`getopt_long` for the options. `dw_store_posix_init(&store, dir)`; `dw_init(&srv, &store, send_sock, &fd)`; mounts. `--replay FILE`: read file, `dw_feed` in 64-byte chunks with `dw_tick` between, print stats, exit. Else: `socket/bind/listen` on port (SO_REUSEADDR), accept one client at a time; loop `poll(fd, 50 ms)`: on data `recv` into 512-byte buffer → `dw_feed(now_ms)`; always `dw_tick(now_ms)`; on disconnect log and reset to `DW_IDLE`, accept again. `send_sock` writes with `send(fd, ..., MSG_NOSIGNAL)` looping on partial writes. Print `dw stats` summary on SIGINT and exit.

- [ ] **Step 3: Run the integration test**

```
ninja -C build-host
python3 -c "open('/tmp/pc/t.dsk','wb').write(bytes([(i>>8)&255 for i in range(161280)]))"  # after mkdir -p /tmp/pc
cp /tmp/pc/t.dsk /tmp/pc/s.dsk
./build-host/picoco-host --dir /tmp/pc --mount 0=t.dsk --mount 1=s.dsk --hdbdos off &
python3 firmware/tools/dwtest.py --image /tmp/pc/t.dsk --drive 0 --scratch s.dsk
```
Expected: all checks `ok`, exit 0. Then rerun with `--hdbdos on` on both sides.

- [ ] **Step 4: Document the XRoar check in README**

`xroar -machine coco3 -cart becker -becker-ip 127.0.0.1 -becker-port 65504 -cart-rom hdbdos_dw.rom` (flags per XRoar manual; user supplies the HDB-DOS DriveWire ROM in `firmware/roms/`). Expected: `DIR` lists the image mounted on drive 0. Record this as a manual check, not automated.

---

### Task 6: Bus engine data structures

**Files:**
- Create: `firmware/src/bus/bus.h`, `firmware/src/bus/bus.c`
- Test: `firmware/tests/test_bus.c`

**Interfaces:**
- Consumes: `ring.h`, `plat_now_us`.
- Produces:
```c
/* bus.h */
#define BUS_TABLE_SIZE 16384
#define BUS_IDX_BECKER_STATUS 0x3F41
#define BUS_IDX_BECKER_DATA   0x3F42
#define BUS_TRACE_SIZE 4096
#define BUS_MAX_HOOKS 4
#ifdef PICOCO_HOST
#define BUS_HOT
#else
#define BUS_HOT __attribute__((section(".time_critical.bus")))   /* SRAM, see Plan B */
#endif
typedef struct { uint32_t t_us; uint16_t idx; uint8_t rw; uint8_t data; } bus_trace_entry;
typedef struct { uint32_t cycles, reads, writes, write_overrun; } bus_stats_t;
extern uint8_t bus_table[BUS_TABLE_SIZE];
extern volatile bus_stats_t bus_stats;
void bus_init(void);                                   /* table = 0xFF, rings empty, trace running */
void bus_set_read(uint16_t idx, uint8_t v);
void bus_set_read_range(uint16_t idx, const uint8_t *p, size_t n);   /* clipped at table end */
int  bus_add_read_hook(uint16_t idx, void (*fn)(void));             /* 0 ok, -1 full */
bool bus_pop_write(uint16_t *idx, uint8_t *data);                   /* core0 consumer */
void bus_trace_freeze(bool freeze);
size_t bus_trace_copy(bus_trace_entry *out, size_t max);            /* oldest first, newest last */
/* producer side (core1 loop and sim_bus): */
void bus_on_read_done(uint16_t idx, uint32_t t_us);   /* runs hook for idx, traces (rw=1, data=bus_table[idx]), stats */
void bus_on_write(uint16_t idx, uint8_t data, uint32_t t_us);   /* pushes write event, traces, stats */
```
Write ring: 256 entries of `{uint16_t idx; uint8_t data;}` stored in a `ring_t` of 1024 bytes as 3 bytes per entry? No: use a dedicated array `bus_write_ev[256]` with head/tail indices, same discipline as `ring.h` (SPSC, atomics). Overrun drops the new event and increments `write_overrun`. Trace ring: fixed array with a `uint32_t pos` counter and `bool frozen`; `bus_trace_copy` returns `min(max, entries_recorded)` in chronological order.

- [ ] **Step 1: Write failing tests**
```c
TEST(table_defaults_ff) { bus_init(); ASSERT_EQ(bus_table[0], 0xFF); ASSERT_EQ(bus_table[BUS_TABLE_SIZE-1], 0xFF); }
TEST(set_read_and_range_clipped) { uint8_t p[4]={1,2,3,4}; bus_set_read_range(BUS_TABLE_SIZE-2, p, 4); ASSERT_EQ(bus_table[BUS_TABLE_SIZE-1], 2); bus_set_read(7, 0x42); ASSERT_EQ(bus_table[7], 0x42); }
TEST(write_events_fifo_and_overrun) { for i<256: bus_on_write(i, i, i); ASSERT_EQ(bus_stats.write_overrun, 1) /* 255 usable */; pop all, check order; ASSERT(!bus_pop_write(...)); }
TEST(read_hook_runs_only_for_its_index) { static int n; hook increments n; bus_add_read_hook(0x3F42, hook); bus_on_read_done(0x3F41, 0); ASSERT_EQ(n,0); bus_on_read_done(0x3F42, 0); ASSERT_EQ(n,1); }
TEST(hook_table_full) { add 4 hooks ok, 5th returns -1 }
TEST(trace_records_and_wraps) { bus_init(); for i<BUS_TRACE_SIZE+10: bus_on_write(i&0x3FFF, 0, i); bus_trace_entry e[BUS_TRACE_SIZE]; size_t n = bus_trace_copy(e, BUS_TRACE_SIZE); ASSERT_EQ(n, BUS_TRACE_SIZE); ASSERT_EQ(e[0].t_us, 10); ASSERT_EQ(e[n-1].t_us, BUS_TRACE_SIZE+9); ASSERT_EQ(e[0].rw, 0); }
TEST(trace_freeze) { bus_init(); bus_on_read_done(1, 5); bus_trace_freeze(true); bus_on_read_done(2, 6); n = bus_trace_copy(e, 10); ASSERT_EQ(n, 1); ASSERT_EQ(e[0].rw, 1); ASSERT_EQ(e[0].data, 0xFF); }
TEST(stats) { bus_init(); bus_on_read_done(1,0); bus_on_write(1,0,0); ASSERT_EQ(bus_stats.cycles, 2); ASSERT_EQ(bus_stats.reads, 1); ASSERT_EQ(bus_stats.writes, 1); }
```
- [ ] **Step 2: Build, confirm failure.** **Step 3: Implement `bus.c`.** All arrays `static` except `bus_table` and `bus_stats`; mark `bus_on_read_done`, `bus_on_write` and the arrays with `BUS_HOT`. **Step 4: `ctest` passes.**

---

### Task 7: Devices: registry, ROM, Becker

**Files:**
- Create: `firmware/src/dev/device.h`, `device.c`, `rom.h`, `rom.c`, `becker.h`, `becker.c`
- Test: `firmware/tests/test_becker.c` (covers device and rom too)

**Interfaces:**
- Consumes: Task 6 `bus_*`, `ring.h`, Task 2 `dw_store`.
- Produces:
```c
/* device.h */
typedef struct { const char *name; uint16_t idx_lo, idx_hi; void (*init)(void); void (*on_write)(uint16_t idx, uint8_t data); } device_t;
#define DEVICE_MAX 8
int  device_register(const device_t *d);      /* 0 ok, -1 full */
void device_init_all(void);
void device_reset(void);                     /* clears registry (tests) */
size_t device_dispatch_writes(void);         /* drains bus write ring to on_write by range; returns events handled */
/* rom.h */
void rom_init(void);                         /* registers device "rom" 0x0000..0x3EFF, no on_write */
void rom_pattern(void);                      /* bus_table[i] = i & 0xFF for i < 0x2000 */
void rom_off(void);                          /* 0xFF for 0x0000..0x3EFF */
int  rom_load_mem(const uint8_t *p, size_t n);   /* 8192 or 16384 only; 16 K fills 0..0x3FFF then Becker entries are re-asserted by becker_refresh() */
int  rom_load_file(dw_store *st, const char *name);   /* size must be 8192 or 16384; -1 not found, -2 bad size */
/* becker.h */
typedef struct { uint32_t reads, writes, underrun, overrun; } becker_stats_t;
extern becker_stats_t becker_stats;
void   becker_init(void);                    /* registers device 0x3F40..0x3F5F, read hook on 0x3F42, refreshes table */
void   becker_refresh(void);                 /* rewrite table[0x3F41], table[0x3F42] from to_coco */
size_t becker_read(uint8_t *buf, size_t n);  /* core0: bytes the CoCo wrote */
size_t becker_write(const uint8_t *buf, size_t n);  /* core0: queue for CoCo; returns accepted count */
size_t becker_rx_avail(void);
size_t becker_tx_free(void);
void   becker_loopback_pump(void);           /* copy from_coco -> to_coco */
```
Table semantics: `table[0x3F41] = to_coco empty ? 0x00 : 0x02`; `table[0x3F42] = to_coco empty ? 0xFF : head`. Read hook on 0x3F42: if a byte was queued pop it and `reads++`, else `underrun++`; then `becker_refresh()`. `on_write`: idx 0x3F42 → push to `from_coco` (1024) or `overrun++`, `writes++`; other idx ignored. Queues: `to_coco` 256 bytes.

- [ ] **Step 1: Write failing tests**
```c
static void setup(void) { bus_init(); device_reset(); rom_init(); becker_init(); device_init_all(); }
TEST(status_follows_queue) { setup(); ASSERT_EQ(bus_table[0x3F41], 0); ASSERT_EQ(bus_table[0x3F42], 0xFF); uint8_t b = 0x41; ASSERT_EQ(becker_write(&b,1), 1); ASSERT_EQ(bus_table[0x3F41], 2); ASSERT_EQ(bus_table[0x3F42], 0x41); }
TEST(read_hook_pops_one) { setup(); becker_write("AB",2); bus_on_read_done(0x3F42, 0); ASSERT_EQ(bus_table[0x3F42], 'B'); bus_on_read_done(0x3F42, 0); ASSERT_EQ(bus_table[0x3F41], 0); ASSERT_EQ(becker_stats.reads, 2); bus_on_read_done(0x3F42, 0); ASSERT_EQ(becker_stats.underrun, 1); }
TEST(status_read_does_not_pop) { setup(); becker_write("A",1); bus_on_read_done(0x3F41, 0); ASSERT_EQ(bus_table[0x3F42], 'A'); }
TEST(coco_write_reaches_stream) { setup(); bus_on_write(0x3F42, 0x52, 0); bus_on_write(0x3F41, 0x99, 0); ASSERT_EQ(device_dispatch_writes(), 2); uint8_t buf[4]; ASSERT_EQ(becker_read(buf, 4), 1); ASSERT_EQ(buf[0], 0x52); ASSERT_EQ(becker_stats.writes, 1); }
TEST(tx_backpressure) { setup(); uint8_t big[300]; ASSERT_EQ(becker_write(big, 300), 255); ASSERT_EQ(becker_tx_free(), 0); }
TEST(rx_overrun_counted) { setup(); for i<1100: bus_on_write(0x3F42, 1, 0), device_dispatch_writes(); ASSERT(becker_stats.overrun > 0); ASSERT_EQ(becker_rx_avail(), 1023); }
TEST(loopback) { setup(); bus_on_write(0x3F42, 65, 0); device_dispatch_writes(); becker_loopback_pump(); ASSERT_EQ(bus_table[0x3F42], 65); }
TEST(rom_pattern_and_off) { setup(); rom_pattern(); ASSERT_EQ(bus_table[0x1234], 0x34); ASSERT_EQ(bus_table[0x2000], 0xFF); rom_off(); ASSERT_EQ(bus_table[0x1234], 0xFF); }
TEST(rom_16k_keeps_becker) { setup(); static uint8_t img[16384]; memset(img, 0x11, sizeof img); becker_write("Z",1); ASSERT_EQ(rom_load_mem(img, 16384), 0); ASSERT_EQ(bus_table[0x3EFF], 0x11); ASSERT_EQ(bus_table[0x3F42], 'Z'); ASSERT_EQ(rom_load_mem(img, 100), -2); }
TEST(rom_load_file) { temp dir with 8192-byte file of 0x22 via dw_store_posix; ASSERT_EQ(rom_load_file(&st, "r.rom"), 0); ASSERT_EQ(bus_table[0], 0x22); ASSERT_EQ(rom_load_file(&st, "x"), -1); }
TEST(dispatch_ignores_unowned) { setup(); bus_on_write(0x1000, 1, 0); ASSERT_EQ(device_dispatch_writes(), 1); ASSERT_EQ(becker_stats.writes, 0); }
```
- [ ] **Step 2: Build, confirm failure.** **Step 3: Implement.** `rom_load_mem` for 16 K writes the range then calls `becker_refresh()` (declare `becker_refresh` in becker.h; rom.c includes it). **Step 4: `ctest` passes.**

---

### Task 8: Logging

**Files:**
- Create: `firmware/src/log.h`, `firmware/src/log.c`
- Test: `firmware/tests/test_log.c`

**Interfaces:**
```c
enum { LOG_OFF = 0, LOG_ERROR = 1, LOG_INFO = 2, LOG_DEBUG = 3 };
enum { LOG_M_MAIN, LOG_M_BUS, LOG_M_DW, LOG_M_BECKER, LOG_M_CONSOLE, LOG_M_FS, LOG_M_COUNT };
extern const char *const log_module_names[LOG_M_COUNT];   /* "main","bus","dw","becker","console","fs" */
extern uint32_t log_dropped;
void log_init(void);                          /* all modules LOG_INFO */
void log_set_level(int module, int level);
int  log_level(int module);
int  log_module_by_name(const char *name);   /* -1 unknown */
void log_write(int module, int level, const char *fmt, ...);   /* formats "<t_us> <mod> <E|I|D> msg\n" into a 4096-byte ring; drops whole line if it won't fit */
size_t log_drain(char *out, size_t max);      /* copies out bytes, returns count */
#ifndef PICOCO_LOG_LEVEL
#define PICOCO_LOG_LEVEL LOG_INFO
#endif
#define LOG_AT(m, lvl, ...) do { if ((lvl) <= PICOCO_LOG_LEVEL && (lvl) <= log_level(m)) log_write((m), (lvl), __VA_ARGS__); } while (0)
#define LOG_E(m, ...) LOG_AT(m, LOG_ERROR, __VA_ARGS__)
#define LOG_I(m, ...) LOG_AT(m, LOG_INFO, __VA_ARGS__)
#define LOG_D(m, ...) LOG_AT(m, LOG_DEBUG, __VA_ARGS__)
```
Timestamp from `plat_now_us()`. Line formatted into a 160-byte stack buffer with `vsnprintf`, truncated with `...\n` if longer, then pushed byte by byte into a `ring_t` of 4096 only if `ring_free >= len`.

- [ ] **Step 1: Failing tests**: `level_filter` (DEBUG suppressed at INFO, then enabled), `format_has_module_and_level` (drain contains `" dw I hello 42\n"`), `drop_when_full` (write 200 lines of 100 chars, `log_dropped > 0`, drained bytes ≤ 4095, every drained line is complete, i.e. ends with `\n` and the last char drained is `\n`), `module_by_name` (`"dw"` → LOG_M_DW, `"x"` → -1), `truncates_long_line`.
- [ ] **Step 2-4:** fail, implement, pass.

---

### Task 9: Console, modes, config-as-commands, host stdin console

**Files:**
- Create: `firmware/src/console/console.h`, `console.c`, `mode.h`, `mode.c`
- Modify: `firmware/host/picoco_host.c` (stdin console), `firmware/host/plat_host.c` (cfg on `<dir>/picoco.cfg`)
- Test: `firmware/tests/test_console.c`

**Interfaces:**
- Consumes: Tasks 2-8.
- Produces:
```c
/* mode.h */
typedef enum { MODE_DIAG, MODE_LOOP, MODE_BRIDGE, MODE_NATIVE } picoco_mode;
void mode_set(picoco_mode m);               /* also resets dw parser to IDLE on entering NATIVE */
picoco_mode mode_get(void);
const char *mode_name(picoco_mode m);       /* "off","loop","bridge","native" (console words) */
void mode_pump(dw_server *dw, uint32_t now_ms);   /* one iteration: dispatch writes; LOOP: becker_loopback_pump; BRIDGE: becker<->plat_bridge_*; NATIVE: becker_read -> dw_feed, dw_tick */
/* console.h */
typedef void (*console_out_fn)(void *ctx, const char *s);
void console_init(console_out_fn out, void *ctx, dw_server *dw, dw_store *store);
void console_feed(const uint8_t *buf, size_t n);   /* accumulates up to 127 chars; executes on \r or \n */
int  console_exec(const char *line);               /* 0 ok (printed "ok"), -1 err (printed "err <msg>") */
int  console_run_config(void);                     /* plat_cfg_read, exec each line; returns lines executed or <0 */
```
Commands (`console_exec`, tokens split on spaces, case-insensitive verb):
| Line | Effect |
|---|---|
| `help` | list commands |
| `status` | mode, uptime, `bus_stats`, `becker_stats`, `log_dropped`, mounted drives |
| `version` | `PICOCO_VERSION` string (CMake define, default `"dev"`) |
| `smoke` | `plat_smoke()` |
| `halt on\|off` | `plat_halt` |
| `trace dump [n]` | `bus_trace_freeze(true)`, print last n (default 64) as `%u %04x %c %02x\n` (t_us, idx, R/W, data), then unfreeze |
| `trace freeze\|run` | freeze flag |
| `rom pattern\|off\|load <file>` | rom_* ; remembers `rom_cmd` string for `save` |
| `becker off\|loop\|bridge\|native` | `mode_set` |
| `dw mount <n> <file> [ro]`, `dw eject <n>`, `dw hdbdos on\|off`, `dw stats` | dw_*; `dw stats` prints nonzero `ops[]`, reads, writes, errors, timeouts |
| `dw capture on <file>\|off` | opens file via `dw_store` (write, create) and `dw_set_capture` with a fn that appends one chunk per call: `dir`, `len_lo`, `len_hi`, then the bytes; off closes |
| `fs ls`, `fs rm <f>`, `fs format`, `fs export`, `fs import` | `plat_fs_*` (`import` = `plat_fs_export(false)`) |
| `time set <unix>`, `time` | `dw_time_set`; print current unix time |
| `log <module> off\|error\|info\|debug`, `log dump` | log_*; `log dump` drains the ring to out |
| `stats reset` | zero bus/becker/dw stats |
| `save` | write config: `becker <mode>`, `rom <cmd>` if set, `dw hdbdos on|off`, `dw mount n name [ro]` per mounted drive, `log <m> <level>` for non-default |
| `reboot`, `bootsel` | `plat_reboot(false/true)` |
Unknown verb or bad args → `err usage: ...`. Every successful command prints `ok`.

Config: `console_run_config` reads `plat_cfg_read` into a 1024-byte buffer and calls `console_exec` per line, skipping blanks and lines starting with `#`, logging errors with `LOG_E(LOG_M_CONSOLE, ...)` but continuing. No config → mode stays `MODE_DIAG`.

- [ ] **Step 1: Failing tests** with an `out` callback appending to a buffer and a `dw_server` mounted against a temp dir (`plat_host_set_dir`):
```c
TEST(unknown_is_err) { ASSERT_EQ(console_exec("frob"), -1); ASSERT(strstr(out, "err")); }
TEST(becker_mode_switch) { ASSERT_EQ(console_exec("becker native"), 0); ASSERT_EQ(mode_get(), MODE_NATIVE); console_exec("becker off"); ASSERT_EQ(mode_get(), MODE_DIAG); ASSERT_EQ(console_exec("becker sideways"), -1); }
TEST(rom_commands) { console_exec("rom pattern"); ASSERT_EQ(bus_table[0x10], 0x10); console_exec("rom off"); ASSERT_EQ(bus_table[0x10], 0xFF); ASSERT_EQ(console_exec("rom load nope.rom"), -1); }
TEST(dw_mount_and_status) { ASSERT_EQ(console_exec("dw mount 0 raw.dsk"), 0); ASSERT(dw.drives[0].mounted); console_exec("status"); ASSERT(strstr(out, "raw.dsk")); ASSERT_EQ(console_exec("dw mount 9 raw.dsk"), -1); console_exec("dw eject 0"); ASSERT(!dw.drives[0].mounted); }
TEST(feed_splits_lines) { console_feed("becker lo", 9); ASSERT_EQ(mode_get(), MODE_DIAG); console_feed("op\r\n", 4); ASSERT_EQ(mode_get(), MODE_LOOP); }
TEST(trace_dump_format) { bus_on_write(0x3F42, 0x41, 123); console_exec("trace dump 1"); ASSERT(strstr(out, "123 3f42 W 41")); }
TEST(log_level_cmd) { console_exec("log dw debug"); ASSERT_EQ(log_level(LOG_M_DW), LOG_DEBUG); ASSERT_EQ(console_exec("log nosuch info"), -1); }
TEST(time_set) { console_exec("time set 1767225600"); ... `time` prints 1767225600 }
TEST(save_and_run_config_round_trip) { console_exec("becker native"); console_exec("dw mount 1 raw.dsk ro"); console_exec("dw hdbdos off"); console_exec("log dw debug"); ASSERT_EQ(console_exec("save"), 0);
    /* reset state */ mode_set(MODE_DIAG); dw_eject(&dw, 1); dw.hdbdos = true; log_set_level(LOG_M_DW, LOG_INFO);
    ASSERT(console_run_config() > 0); ASSERT_EQ(mode_get(), MODE_NATIVE); ASSERT(dw.drives[1].mounted && dw.drives[1].read_only); ASSERT(!dw.hdbdos); ASSERT_EQ(log_level(LOG_M_DW), LOG_DEBUG); }
TEST(config_bad_line_continues) { plat_cfg_write("frob\nbecker loop\n", ...); console_run_config(); ASSERT_EQ(mode_get(), MODE_LOOP); }
TEST(capture_writes_file) { console_exec("dw capture on cap.bin"); mode_set(MODE_NATIVE); bus_on_write(0x3F42, 0x5A, 0); bus_on_write(0x3F42, 0x41, 0); mode_pump(&dw, 0); console_exec("dw capture off"); /* file = chunks of dir, len_lo, len_hi, bytes: rx chunk {0,2,0,0x5A,0x41} then tx chunk {1,1,0,0xFF}; assert 9 bytes exactly */ }
TEST(native_pump_end_to_end) { console_exec("dw mount 0 raw.dsk"); mode_set(MODE_NATIVE); bus_on_write(0x3F42, 0x52, 0); for b in {0,0,0,5}: bus_on_write(0x3F42, b, 0); mode_pump(&dw, 0); bus_on_read_done(0x3F41, 0) /* status poll publishes (single-writer rule) */; ASSERT_EQ(bus_table[0x3F41], 2); ASSERT_EQ(bus_table[0x3F42], 0) /* rc */; bus_on_read_done(0x3F42,0); bus_on_read_done(0x3F42,0); bus_on_read_done(0x3F42,0); ASSERT_EQ(bus_table[0x3F42], 5) /* first data byte */; }
```
Note for `native_pump_end_to_end`: the 259-byte reply exceeds the 255-byte `to_coco` queue, so `mode_pump` must hold unsent reply bytes. Implement that in `mode.c` with a 512-byte pending buffer: `dw` sends into `pending`, `mode_pump` moves as much as `becker_tx_free()` allows each call. Test asserts that after draining 259 bytes through read hooks (loop `bus_on_read_done` and `mode_pump`) the queue is empty and the bytes were rc, sum hi, sum lo, then data with `data[0]==5`.
- [ ] **Step 2: Build, confirm failure.** **Step 3: Implement** `mode.c`, `console.c`. Tokenizer: `strtok_r` on a copy, max 6 tokens. `// ponytail: linear if-chain dispatch; table when >30 commands`. **Step 4: `ctest` passes.**
- [ ] **Step 5: Host stdin console.** In `picoco_host.c`: `console_init(print_stdout, NULL, &srv, &store)`, run `console_run_config()` at start, add stdin to the `poll` set and pass bytes to `console_feed`; drain `log_drain` to stderr each loop. Start in `becker native`-equivalent for TCP: the TCP client feeds `dw_feed` directly as before (the socket path bypasses Becker on the host). Verify manually: `echo "dw stats" | ./build-host/picoco-host --dir /tmp/pc` prints stats and `ok`.

---

### Task 10: Virtual CoCo, end-to-end stack test, replay fixtures, trace decoder

**Files:**
- Create: `firmware/host/sim_bus.h`, `sim_bus.c`, `firmware/tests/test_stack.c`, `firmware/tests/test_replay.c`, `firmware/tests/fixtures/README.md`, `firmware/tools/tracedump.py`
- Modify: `firmware/host/picoco_host.c` (`--replay` uses fixtures format from Task 9 capture: leading dir byte per chunk; feed only dir-0 chunks)

**Interfaces:**
```c
/* sim_bus.h: the CoCo side. Addresses are full 16-bit CoCo addresses ($C000..$FFFF). */
uint8_t sim_read(uint16_t addr);             /* returns bus_table[addr & 0x3FFF], then bus_on_read_done(idx, plat_now_us()) */
void    sim_write(uint16_t addr, uint8_t d); /* bus_on_write(idx, d, plat_now_us()) */
/* HDB-DOS style helpers: */
int     sim_becker_getc(dw_server *dw, int max_polls);   /* polls $FF41 bit 1, pumping mode_pump between polls; returns byte or -1 */
void    sim_becker_putc(dw_server *dw, uint8_t b);       /* sim_write($FF42, b); mode_pump */
```

- [ ] **Step 1: Failing test_stack.c.** Setup: temp dir, `raw.dsk` 630 sectors + `s.dsk`, `bus_init`, devices, `log_init`, `dw_init`, `console_init`, `console_exec("dw mount 0 raw.dsk")`, `console_exec("dw mount 1 s.dsk")`, `console_exec("dw hdbdos off")`, `mode_set(MODE_NATIVE)`.
```c
TEST(rom_pattern_visible_to_coco) { console_exec("rom pattern"); ASSERT_EQ(sim_read(0xC123), 0x23); ASSERT_EQ(sim_read(0xE000), 0xFF); }
TEST(coco_readex_sector) { putc 0xD2, 0, 0,0,9; uint8_t d[256]; for i<256: d[i] = getc; ASSERT_EQ(d[0], 9); uint16_t sum = dw_checksum(d,256); putc sum>>8; putc sum&0xFF; ASSERT_EQ(getc, 0); ASSERT_EQ(sim_read(0xFF41), 0); }
TEST(coco_read_259) { putc 0x52,0,0,0,9; ASSERT_EQ(getc, 0); hi=getc; lo=getc; read 256 -> checksum matches; }
TEST(coco_write_then_read_back) { 0x57 to drive 1 lsn 4 with 0x3C fill; ASSERT_EQ(getc, 0); readex drive 1 lsn 4 -> all 0x3C }
TEST(coco_bad_checksum_gets_crc) { readex with corrupted sum -> getc == 0xF3 }
TEST(unmounted_drive) { readex drive 3 -> 256 zeros then 0xF6 }
TEST(loopback_mode) { mode_set(MODE_LOOP); sim_write(0xFF42, 0x77); mode_pump(&dw, 0); sim_read(0xFF41) /* first poll returns stale 0 and publishes */; ASSERT_EQ(sim_read(0xFF41), 2); ASSERT_EQ(sim_read(0xFF42), 0x77); }
TEST(status_when_idle_is_zero_and_data_ff) { ASSERT_EQ(sim_read(0xFF41), 0); ASSERT_EQ(sim_read(0xFF42), 0xFF); ASSERT(becker_stats.underrun >= 1); }
TEST(trace_shows_transaction) { bus_trace_entry e[8]; size_t n = bus_trace_copy(e, 8); ASSERT(n > 0); ASSERT_EQ(e[n-1].idx, 0x3F42); }
```
`sim_becker_getc`: loop up to `max_polls` (use 1000): `mode_pump(dw, plat_now_ms())`; `if (sim_read(0xFF41) & 2) return sim_read(0xFF42);` return -1. (Under the single-writer rule the first poll after a push returns stale 0x00 and publishes; the loop absorbs that.)

- [ ] **Step 2: Build, confirm failure. Step 3: Implement sim_bus.c. Step 4: pass.**
- [ ] **Step 5: Replay fixtures.** Generate `tests/fixtures/readex_boot.cap` by running `test_stack` with `console_exec("dw capture on readex_boot.cap")` in `coco_readex_sector` once (then copy the file into fixtures and remove the capture call), and write `test_replay.c`: for each `*.cap` in `fixtures/` (path from `FIXTURE_DIR` compile definition set in CMake to `${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures`), mount `raw.dsk`, feed all dir-0 chunks through `dw_feed`, assert `stats.timeouts == 0 && stats.unknown_op == 0` and that the bytes sent equal the dir-1 chunks in the file. `fixtures/README.md` documents the chunk format from Task 9: `dir (0 rx / 1 tx), len_lo, len_hi, bytes`.
- [ ] **Step 6: tracedump.py.** Reads `trace dump` lines (`t_us idx R|W data`) from stdin or a file. Prints `t_us  $ADDR  R/W  data  note`, where `$ADDR = 0xC000 + idx`; note: `ROM` for idx < 0x2000, `BECKER_STATUS`/`BECKER_DATA` for 0x3F41/0x3F42, `data avail`/`no data` decoded on status reads, and a running DriveWire decoder: a `W $FF42` byte while idle names the opcode (`READ drive=.. lsn=..` once the 4 payload bytes are seen; `READEX`, `WRITE`, `TIME`, `DWINIT`, ...). Test it manually with the output of `console_exec("trace dump 64")` from `test_stack` saved to `tests/fixtures/trace_readex.txt`; expected output includes a line containing `READEX drive=0 lsn=9`.

---

## Plan B (separate document, after Plan A)

Pico target: `src/plat_pico.c`, `bus_core1.c` loop with `PICOCO_BOARD` header, TinyUSB with 2 CDC + MSC, FatFS on flash + `dw_store_fatfs.c`, `crash.c`, watchdog, `main.c` wiring `mode_pump`, `dw selftest`, then hardware milestones 0.1 to native from `docs/breadboard-plan.md` section 6. Its first task installs the Pico toolchain and verifies the blink build left in Task 1 here.

## Self-review notes

- Spec coverage: 4.2 → Task 6; 5 → Task 7; 6 → Tasks 3-4; 6.5-6.6 → Task 2; 8 → Task 9; 9 (host first, flight recorder, counters, capture/replay, logging) → Tasks 6, 8, 9, 10; 9 crash record and 4.3/4.4/7 → Plan B. Spec 10 test table → one task each.
- Interface names are consistent across tasks: `bus_on_read_done`, `bus_on_write`, `becker_refresh`, `mode_pump(dw, now_ms)`, `dw_feed(s, buf, n, now_ms)`.
- Deviation from spec: config file is a list of console commands (Task 9); capture files use length-prefixed chunks (Task 9).
- Ruling during execution (Task 7): single-writer Becker table entries, hooks on both $FF41 and $FF42, core0 never writes them. Spec section 5 updated. Update the spec's section 7 and 9 wording when Plan A is done.
