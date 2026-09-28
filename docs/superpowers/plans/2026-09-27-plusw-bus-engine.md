# Plus-W Bus Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the firmware decode cart addresses itself on a Plus-W, run core1 write hooks, serve banked ROMs, pulse `/CART` for autostart paks, and test all of it on a bare Pico with an on-chip fake 6809.

**Architecture:** core1 stays a flash-free polling loop; the Plus-W gets a second, board-conditional loop that samples the address on Q and drives U10's `/OE` from GP31. Write hooks dispatch inside `bus_on_write` so the host simulator exercises them. Banked ROM is a pointer swap (`bus_rom_base`) done by a `$FF40` write hook. A PIO1 program on the same chip drives the bus pins with 6809 timing so the real core1 loop can be tested without a CoCo.

**Tech Stack:** C11, pico-sdk (RP2350, `hardware_pio`), CMake/Ninja, host build with ASan/UBSan and the repo's `tests/test.h` macros, Python 3 stdlib for the bench script.

**Spec:** `docs/superpowers/specs/2026-09-27-plusw-bus-engine-design.md`

## Global Constraints

- core1 executes only SRAM code: everything reachable from `bus_core1_main` or a hook is `BUS_HOT` or `static inline`; `firmware/tools/check_core1_flash_free.py` runs POST_BUILD and must stay green.
- Single-writer rule: no `bus_table` entry or shared variable gets two writers. `bus_rom_base` is written by core0 (loader) and by the `$FF40` hook on core1 only while the loader has finished (loader sets banks first, then the base, then `rom_nbanks`).
- The Pico 2 core1 loop keeps today's behaviour byte for byte apart from `bus_peek` and the write-hook call.
- Firmware-decoded addresses are limited to `$FF60-$FF7F`; `bus_fw_enable` rejects anything else.
- Console lines are at most 6 tokens / 135 chars (`console_exec`); errors go through `cerr("...")`, success prints `ok`.
- Host build: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host && ctest --test-dir build-host --output-on-failure`. Pico builds: `cmake -B build-pico2 -G Ninja firmware && ninja -C build-pico2`, and `-DPICOCO_BOARD=plusw` into `build-plusw`.
- Commit messages start with `firmware:` and end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Never drive the bus pins from core0/PIO while a powered CoCo is attached: the self-test refuses when live cycles are seen.

## Review Focus

1. A bank-register write larger than the bank count must wrap (mask), never index past `rom_banks` — test in Task 2.
2. A ROM file whose size is 24 KB, 48 KB or 96 KB (not a power-of-two bank count) must be refused with a message and leave the previous ROM intact — test in Task 2.
3. `bus_fw_enable` with an address outside `$FF60-$FF7F` must be a no-op that returns -1, never set a bit — test in Task 4.
4. `bus selftest` on a live bus must refuse before touching any pin — on-device check in Task 3 (host: the command reports pico-only).
5. `cart auto` with an `HDB-DOS`-style ROM (`DK` signature) must not pulse `/CART`; with a plain pak it must; with no ROM it must not — test in Task 5.

---

### Task 1: Core1 write hooks

**Files:**
- Modify: `firmware/src/bus/bus.h`
- Modify: `firmware/src/bus/bus.c`
- Modify: `firmware/src/console/console.c` (`cmd_status`)
- Modify: `firmware/tools/check_core1_flash_free.py:47`
- Test: `firmware/tests/test_bus.c`

**Interfaces:**
- Consumes: existing `bus_on_write(uint16_t idx, uint8_t data, uint32_t t_us)`, `bus_stats_t`.
- Produces: `int bus_add_write_hook(uint16_t idx, void (*fn)(uint8_t data));` (0 ok, -1 full), new `bus_stats` fields `whooks_run`, `hw_selected`, `fw_selected` (all `uint32_t`, zeroed in `bus_init`).

- [ ] **Step 1: Write the failing tests**

Append to `firmware/tests/test_bus.c` before `int main`:

```c
static int wh_order;            /* 1 = hook ran before the event was queued */
static uint8_t wh_data;
static void whook_capture(uint8_t d) {
    uint16_t idx; uint8_t data;
    wh_data = d;
    wh_order = bus_pop_write(&idx, &data) ? 2 : 1;   /* ring must still be empty */
}
static void whook_noop(uint8_t d) { (void)d; }

TEST(write_hook_runs_before_queue_with_data) {
    bus_init();
    wh_order = 0; wh_data = 0;
    ASSERT_EQ(bus_add_write_hook(0x3F40, whook_capture), 0);
    bus_on_write(0x3F41, 0x11, 0);            /* other index: no hook */
    ASSERT_EQ(wh_order, 0);
    bus_on_write(0x3F40, 0x5A, 0);
    ASSERT_EQ(wh_order, 1);
    ASSERT_EQ(wh_data, 0x5A);
    ASSERT_EQ(bus_stats.whooks_run, 1);
    uint16_t idx; uint8_t data;
    ASSERT(bus_pop_write(&idx, &data));        /* 0x3F41 event */
    ASSERT(bus_pop_write(&idx, &data));        /* 0x3F40 event still queued after the hook */
    ASSERT_EQ(idx, 0x3F40);
    ASSERT_EQ(data, 0x5A);
}

TEST(write_hook_table_full) {
    bus_init();
    for (int i = 0; i < BUS_MAX_HOOKS; i++) ASSERT_EQ(bus_add_write_hook((uint16_t)i, whook_noop), 0);
    ASSERT_EQ(bus_add_write_hook(99, whook_noop), -1);
}

TEST(stats_new_fields_zeroed) {
    bus_stats.whooks_run = 5; bus_stats.hw_selected = 5; bus_stats.fw_selected = 5;
    bus_init();
    ASSERT_EQ(bus_stats.whooks_run, 0);
    ASSERT_EQ(bus_stats.hw_selected, 0);
    ASSERT_EQ(bus_stats.fw_selected, 0);
}
```

Add `RUN(write_hook_runs_before_queue_with_data); RUN(write_hook_table_full); RUN(stats_new_fields_zeroed);` in `main`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host 2>&1 | tail -5`
Expected: compile error, `bus_add_write_hook` undeclared / no member `whooks_run`.

- [ ] **Step 3: Implement**

`firmware/src/bus/bus.h`: change the stats typedef and add the API:

```c
typedef struct {
    uint32_t cycles, reads, writes, write_overrun, addr_resample, addr_resample_bits;
    uint32_t whooks_run;     /* write hooks executed on core1 */
    uint32_t hw_selected;    /* Plus-W loop: cycles selected by /CTS or /SCS */
    uint32_t fw_selected;    /* Plus-W loop: cycles selected by bus_fw_mask ($FF60-$FF7F) */
} bus_stats_t;

int  bus_add_write_hook(uint16_t idx, void (*fn)(uint8_t data));   /* 0 ok, -1 full; fn is BUS_HOT, runs on core1 before the event is queued */
```

`firmware/src/bus/bus.c`:

```c
typedef struct { uint16_t idx; void (*fn)(uint8_t); } bus_whook_t;
static bus_whook_t whooks[BUS_MAX_HOOKS];
static int whook_count;
```

In `bus_init` add `whook_count = 0; bus_stats.whooks_run = 0; bus_stats.hw_selected = 0; bus_stats.fw_selected = 0;` (keep the existing field resets; `addr_resample*` were never reset, leave that as is).

```c
int bus_add_write_hook(uint16_t idx, void (*fn)(uint8_t)) {
    if (whook_count >= BUS_MAX_HOOKS) return -1;
    whooks[whook_count].idx = idx;
    whooks[whook_count].fn = fn;
    whook_count++;
    return 0;
}
```

At the top of `bus_on_write`, before the ring push:

```c
    for (int i = 0; i < whook_count; i++) {
        if (whooks[i].idx == idx) { whooks[i].fn(data); bus_stats.whooks_run++; }
    }
```

`firmware/src/console/console.c` `cmd_status`, after the `bus addr_resample` line:

```c
    outf("bus whooks %u hw_sel %u fw_sel %u\n", bus_stats.whooks_run, bus_stats.hw_selected, bus_stats.fw_selected);
```

`firmware/tools/check_core1_flash_free.py` line 47:

```python
HOOK_RE = re.compile(r"bus_add_(?:read|write)_hook\([^,]+,\s*([A-Za-z_]\w*)\s*\)")
```

and update the docstring sentence "plus the Becker read hooks it calls indirectly through bus_add_read_hook" to "plus every hook registered through bus_add_read_hook or bus_add_write_hook".

- [ ] **Step 4: Run tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: all suites pass, `test_bus` shows 12 tests, 0 failed.

- [ ] **Step 5: Pico build still flash-free**

Run: `cmake -B build-pico2 -G Ninja firmware && ninja -C build-pico2 2>&1 | grep -E 'check_core1|error' `
Expected: `check_core1_flash_free: ok (...)`.

- [ ] **Step 6: Commit**

```bash
git add firmware/src/bus/bus.h firmware/src/bus/bus.c firmware/src/console/console.c firmware/tools/check_core1_flash_free.py firmware/tests/test_bus.c
git commit -m "firmware: core1 write hooks, dispatched inside bus_on_write

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Banked ROM via `bus_rom_base`

**Files:**
- Modify: `firmware/src/bus/bus.h`, `firmware/src/bus/bus.c`, `firmware/src/bus/bus_core1.c:33`
- Modify: `firmware/host/sim_bus.c:8`
- Modify: `firmware/src/dev/rom.h`, `firmware/src/dev/rom.c`
- Modify: `firmware/src/console/console.c` (`cmd_rom`)
- Create: `firmware/tests/test_rom.c`

**Interfaces:**
- Consumes: `bus_add_write_hook` (Task 1), `dw_store` ops, `dw_store_posix_init`.
- Produces:
  - `extern const uint8_t *volatile bus_rom_base;` (bus.h), reset to `bus_table` by `bus_init`.
  - `static inline uint8_t bus_peek(uint16_t idx)` (bus.h): `idx < 0x3F00 ? bus_rom_base[idx] : bus_table[idx]`.
  - `#define ROM_BANK_SIZE 16384`, `#define ROM_MAX_BANKS 8` (rom.h).
  - `int rom_load_mem(const uint8_t *p, size_t n);` accepts 8192, 16384, or 32768/65536/131072; -2 otherwise.
  - `int rom_load_file(dw_store *st, const char *name);` same sizes; -1 not found/read error, -2 bad size.
  - `int rom_bank_count(void);` 0 when unbanked. `bool rom_loaded(void);` true after any successful load, false after `rom_off`/`rom_pattern`. `bool rom_is_dos(void);` true when loaded and bytes 0,1 are `'D','K'`.

- [ ] **Step 1: Write the failing tests**

Create `firmware/tests/test_rom.c`:

```c
#include "test.h"
#include "bus.h"
#include "device.h"
#include "rom.h"
#include "becker.h"
#include "dw_store.h"
#include "sim_bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_dir[256];
static dw_store store;
static uint8_t img[ROM_MAX_BANKS * ROM_BANK_SIZE];

static void setup(void) {
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
}

/* bank b is filled with b, except byte 0 of bank 0 which is 'X' (not 'D'). */
static void fill_banks(int nbanks) {
    for (int b = 0; b < nbanks; b++)
        for (int i = 0; i < ROM_BANK_SIZE; i++) img[b * ROM_BANK_SIZE + i] = (uint8_t)b;
    img[0] = 'X';
}

static void write_file(const char *name, size_t n) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *fp = fopen(path, "wb");
    fwrite(img, 1, n, fp);
    fclose(fp);
}

TEST(small_loads_unchanged) {
    setup();
    fill_banks(1);
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT_EQ(rom_bank_count(), 0);
    ASSERT_EQ(sim_read(0xC000), 'X');
    ASSERT_EQ(sim_read(0xC001), 0);
    ASSERT(rom_loaded());
    ASSERT(!rom_is_dos());
    ASSERT_EQ(rom_load_mem(img, 16384), 0);
    ASSERT_EQ(rom_bank_count(), 0);
    ASSERT_EQ(bus_table[BUS_IDX_BECKER_STATUS], 0x00);   /* becker's entry untouched */
}

TEST(banked_switches_on_ff40_write) {
    setup();
    fill_banks(4);
    ASSERT_EQ(rom_load_mem(img, 4 * ROM_BANK_SIZE), 0);
    ASSERT_EQ(rom_bank_count(), 4);
    ASSERT_EQ(sim_read(0xC001), 0);
    sim_write(0xFF40, 2);
    ASSERT_EQ(sim_read(0xC001), 2);          /* visible on the very next read, no core0 drain */
    ASSERT_EQ(sim_read(0xFEFF), 2);
    sim_write(0xFF40, 9);                     /* 9 & 3 == 1: masked, never out of range */
    ASSERT_EQ(sim_read(0xC001), 1);
    ASSERT_EQ(bus_table[BUS_IDX_BECKER_STATUS], 0x00);   /* I/O page still comes from bus_table */
    ASSERT_EQ(bus_stats.whooks_run, 2);
}

TEST(rom_off_disables_banking) {
    setup();
    fill_banks(2);
    ASSERT_EQ(rom_load_mem(img, 2 * ROM_BANK_SIZE), 0);
    rom_off();
    ASSERT_EQ(rom_bank_count(), 0);
    ASSERT(!rom_loaded());
    ASSERT_EQ(sim_read(0xC001), 0xFF);
    sim_write(0xFF40, 1);
    ASSERT_EQ(sim_read(0xC001), 0xFF);       /* hook is inert when unbanked */
}

TEST(bad_sizes_refused_previous_kept) {
    setup();
    fill_banks(2);
    ASSERT_EQ(rom_load_mem(img, 2 * ROM_BANK_SIZE), 0);
    ASSERT_EQ(rom_load_mem(img, 24576), -2);
    ASSERT_EQ(rom_load_mem(img, 3 * ROM_BANK_SIZE), -2);
    ASSERT_EQ(rom_load_mem(img, 6 * ROM_BANK_SIZE), -2);
    ASSERT_EQ(rom_load_mem(img, 9 * ROM_BANK_SIZE), -2);
    ASSERT_EQ(rom_bank_count(), 2);
    sim_write(0xFF40, 1);
    ASSERT_EQ(sim_read(0xC001), 1);
}

TEST(dos_signature) {
    setup();
    fill_banks(1);
    img[0] = 'D'; img[1] = 'K';
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(rom_is_dos());
    rom_off();
    ASSERT(!rom_is_dos());
}

TEST(file_load_banked) {
    setup();
    fill_banks(8);
    write_file("big.rom", 8 * ROM_BANK_SIZE);
    write_file("odd.rom", 24576);
    ASSERT_EQ(rom_load_file(&store, "big.rom"), 0);
    ASSERT_EQ(rom_bank_count(), 8);
    sim_write(0xFF40, 7);
    ASSERT_EQ(sim_read(0xC001), 7);
    ASSERT_EQ(rom_load_file(&store, "odd.rom"), -2);
    ASSERT_EQ(rom_bank_count(), 8);
    ASSERT_EQ(rom_load_file(&store, "missing.rom"), -1);
}

int main(void) {
    snprintf(g_dir, sizeof(g_dir), "/tmp/picoco_test_rom_%d", (int)getpid());
    char cmd[300]; snprintf(cmd, sizeof(cmd), "mkdir -p %s", g_dir); system(cmd);
    dw_store_posix_init(&store, g_dir);
    RUN(small_loads_unchanged);
    RUN(banked_switches_on_ff40_write);
    RUN(rom_off_disables_banking);
    RUN(bad_sizes_refused_previous_kept);
    RUN(dos_signature);
    RUN(file_load_banked);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir); system(cmd);
    TEST_MAIN_END
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware && ninja -C build-host 2>&1 | tail -5`
Expected: compile errors for `ROM_BANK_SIZE`, `rom_bank_count`, `rom_loaded`.

- [ ] **Step 3: Implement bus side**

`firmware/src/bus/bus.h`, after `extern uint8_t bus_table[...]`:

```c
/* ROM window source: bus_table by default, a 16 KB bank when a banked image
 * is loaded (rom.c). Swapped by the $FF40 write hook on core1; a pointer
 * store is atomic so a read in flight sees the old or the new bank whole. */
extern const uint8_t *volatile bus_rom_base;

/* What a CoCo read of idx returns: ROM window from bus_rom_base, I/O page
 * ($FF00-$FFFF, idx >= 0x3F00) from bus_table. always_inline: used by core1. */
static inline __attribute__((always_inline)) uint8_t bus_peek(uint16_t idx) {
    return idx < 0x3F00 ? bus_rom_base[idx] : bus_table[idx];
}
```

`firmware/src/bus/bus.c`: `const uint8_t *volatile bus_rom_base = bus_table;` next to the table; in `bus_init` add `bus_rom_base = bus_table;`; in `bus_on_read_done` change `uint8_t data = bus_table[idx];` to `uint8_t data = bus_peek(idx);`.

`firmware/src/bus/bus_core1.c:33`: `sio_hw->gpio_set = (uint32_t)bus_peek(idx) << PIN_D0;`

`firmware/host/sim_bus.c:8`: `uint8_t v = bus_peek(idx);`

- [ ] **Step 4: Implement rom.c**

Replace `firmware/src/dev/rom.h`:

```c
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dw_store.h"

#define ROM_BANK_SIZE 16384
#define ROM_MAX_BANKS 8          /* 128 KB: the largest Games Master Cartridge image (MAME coco_gmc) */

void rom_init(void);      /* registers device "rom" 0x0000..0x3EFF and the $FF40 bank-select write hook */
void rom_pattern(void);   /* bus_table[i] = i & 0xFF for i < 0x2000; unbanked */
void rom_off(void);       /* 0xFF for 0x0000..0x3EFF; unbanked; rom_loaded() false */
/* 8192 or 16384: copied into bus_table as before. 32768/65536/131072: 16 KB
 * banks selected by a write to $FF40 (value & (banks-1)). Returns -2 for any
 * other size and leaves the current ROM in place. */
int  rom_load_mem(const uint8_t *p, size_t n);
int  rom_load_file(dw_store *st, const char *name);   /* -1 not found/read error, -2 bad size */
int  rom_bank_count(void);   /* 0 = unbanked */
bool rom_loaded(void);       /* true after a successful load until rom_off/rom_pattern */
bool rom_is_dos(void);       /* loaded and bytes 0,1 == "DK" (HDB-DOS / RS-DOS style ROM) */
```

Replace `firmware/src/dev/rom.c`:

```c
#include "rom.h"
#include "device.h"
#include "bus.h"
#include <string.h>

#define ROM_LO 0x0000
#define ROM_HI 0x3EFF

static const device_t rom_device = { "rom", ROM_LO, ROM_HI, NULL, NULL };

/* 128 KB of SRAM, .bss. Only the loaded banks are meaningful. */
static uint8_t rom_banks[ROM_MAX_BANKS][ROM_BANK_SIZE];
static volatile uint8_t rom_nbanks;     /* 0 = unbanked; written by core0 (loader) only */
static volatile uint8_t rom_bank_mask;
static bool rom_have;
static bool rom_dos;

/* core1: swap the ROM window on a $FF40 write. Inert while unbanked, so it
 * stays registered for the life of the firmware (no remove API needed). */
static BUS_HOT void rom_bank_hook(uint8_t data) {
    if (rom_nbanks) bus_rom_base = rom_banks[data & rom_bank_mask];
}

void rom_init(void) {
    device_register(&rom_device);
    bus_add_write_hook(0x3F40, rom_bank_hook);
}

static void unbank(void) {
    rom_nbanks = 0;
    rom_bank_mask = 0;
    bus_rom_base = bus_table;
}

void rom_pattern(void) {
    unbank();
    for (uint32_t i = 0; i < 0x2000; i++) bus_table[i] = (uint8_t)(i & 0xFF);
    rom_have = false; rom_dos = false;
}

void rom_off(void) {
    unbank();
    memset(&bus_table[ROM_LO], 0xFF, ROM_HI - ROM_LO + 1);
    rom_have = false; rom_dos = false;
}

static int bank_count_for(size_t n) {
    if (n == 2 * ROM_BANK_SIZE || n == 4 * ROM_BANK_SIZE || n == 8 * ROM_BANK_SIZE) return (int)(n / ROM_BANK_SIZE);
    return 0;
}

int rom_load_mem(const uint8_t *p, size_t n) {
    int nb = bank_count_for(n);
    if (n != 8192 && n != 16384 && nb == 0) return -2;
    if (nb) {
        /* Order matters for core1: fill banks, point the base at bank 0, then
         * publish the count so the hook can start switching. */
        rom_nbanks = 0;
        for (int b = 0; b < nb; b++) memcpy(rom_banks[b], p + (size_t)b * ROM_BANK_SIZE, ROM_BANK_SIZE);
        bus_rom_base = rom_banks[0];
        rom_bank_mask = (uint8_t)(nb - 1);
        rom_nbanks = (uint8_t)nb;
    } else {
        unbank();
        if (n == 8192) {
            memcpy(&bus_table[0], p, n);
        } else {
            /* 16 K load: bus_table[0x3F41]/[0x3F42] are becker's, not ROM's, and
             * only core1's read hooks may write them (single-writer rule). */
            memcpy(&bus_table[0], p, 0x3F41);
            memcpy(&bus_table[0x3F43], p + 0x3F43, n - 0x3F43);
        }
    }
    rom_have = true;
    rom_dos = (p[0] == 'D' && p[1] == 'K');
    return 0;
}

static uint8_t rom_file_buf[ROM_MAX_BANKS * ROM_BANK_SIZE];   /* ponytail: 128 KB staging; could read bank by bank into rom_banks */

int rom_load_file(dw_store *st, const char *name) {
    dw_file f;
    if (st->ops->open(st->ctx, name, false, &f) < 0) return -1;
    uint32_t size;
    if (st->ops->size(&f, &size) < 0) { st->ops->close(&f); return -1; }
    if (size != 8192 && size != 16384 && bank_count_for(size) == 0) { st->ops->close(&f); return -2; }
    uint32_t got = 0;
    while (got < size) {
        int r = st->ops->read(&f, got, rom_file_buf + got, size - got);
        if (r <= 0) { st->ops->close(&f); return -1; }
        got += (uint32_t)r;
    }
    st->ops->close(&f);
    return rom_load_mem(rom_file_buf, size);
}

int  rom_bank_count(void) { return rom_nbanks; }
bool rom_loaded(void)     { return rom_have; }
bool rom_is_dos(void)     { return rom_have && rom_dos; }
```

Note on RAM: `rom_banks` + `rom_file_buf` is 256 KB of `.bss`. The RP2350 has 520 KB and the firmware's static use before this task is well under 100 KB; if the Pico link fails on RAM, drop `rom_file_buf` and read bank by bank into `rom_banks` (the ponytail note names this). Check the map: `arm-none-eabi-size build-pico2/picoco.elf` bss must stay under 400 KB.

`firmware/src/console/console.c` `cmd_rom`, replace the `load` branch's error handling:

```c
        int r = rom_load_file(g_store, argv[2]);
        if (r == -2) return cerr("rom load: size must be 8K, 16K, or banked 32K/64K/128K");
        if (r != 0) return cerr("rom load failed");
```

- [ ] **Step 5: Run tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure`
Expected: `test_rom` 6 tests 0 failed; every other suite still green (`test_console`, `test_replay`, `test_stack` load ROMs through the same path).

- [ ] **Step 6: Pico build, flash-free check, RAM check**

Run: `ninja -C build-pico2 2>&1 | grep -E 'check_core1|error|region' ; arm-none-eabi-size build-pico2/picoco.elf`
Expected: `check_core1_flash_free: ok`, `bss` under 400000.

- [ ] **Step 7: Commit**

```bash
git add firmware/src/bus firmware/host/sim_bus.c firmware/src/dev/rom.h firmware/src/dev/rom.c firmware/src/console/console.c firmware/tests/test_rom.c
git commit -m "firmware: banked ROM (16 KB banks, \$FF40 select) via bus_rom_base swap on core1

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: On-chip fake 6809 (Pico 2), `bus selftest`, `bench.py`

**Files:**
- Create: `firmware/src/bus/fake6809.pio`, `firmware/src/bus/fake6809.c`, `firmware/src/bus/fake6809.h`
- Modify: `firmware/CMakeLists.txt` (Pico target: sources, `hardware_pio`, `pico_generate_pio_header`)
- Modify: `firmware/src/console/console.c` (`cmd_bus`, help line)
- Create: `firmware/tools/bench.py`
- Modify: `firmware/CMakeLists.txt` (host: `add_test(NAME bench ...)` with `SKIP_RETURN_CODE 77`)
- Test: `firmware/tests/test_console.c` (host: `bus selftest` reports pico-only), on-device via `bench.py`

**Interfaces:**
- Consumes: `bus_peek`, `rom_load_mem`, `rom_off`, `bus_drive_set/get`, `bus_stats`, board `PIN_A0`, `PIN_RW`, `PIN_OE_BUS`, `PIN_D0`.
- Produces (`fake6809.h`, Pico only):

```c
#pragma once
#include <stdint.h>
#include <stddef.h>
/* On-chip fake 6809: PIO1 drives A0-A13, R/W and OE_BUS (Pico 2) with 6809
 * timing so core1's real loop can be tested with no CoCo attached. */
typedef struct {
    uint32_t cycles, mismatches, ring_overrun;
    int first_ok_delay;          /* smallest sample delay (PIO cycles after OE fell) with correct read data, -1 none */
    uint32_t delay_ns;           /* first_ok_delay in ns at the PIO clock */
} fake_result_t;
int  fake6809_selftest(fake_result_t *r, void (*line)(const char *s));   /* 0 pass, -1 fail, -2 refused: bus live */
```

- [ ] **Step 1: Host test for the refusal path**

Append to `firmware/tests/test_console.c` before `main`:

```c
TEST(bus_selftest_is_pico_only_on_host) {
    setup();
    outn = 0;
    ASSERT_EQ(console_exec("bus selftest"), -1);
    ASSERT(strstr(out, "pico only"));
}
```
and `RUN(bus_selftest_is_pico_only_on_host);` in `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `ninja -C build-host && ./build-host/test_console | tail -3`
Expected: `FAIL ... strstr(out, "pico only")` (today the command prints the usage error).

- [ ] **Step 3: PIO program**

Create `firmware/src/bus/fake6809.pio`:

```
; Fake 6809 bus master for the Pico 2 build. Two words per cycle from the TX FIFO:
;   word 0: bits 0-13 = A0..A13, bit 14 = R/W (1 = read)         -> GP8..GP22 (out pins, 15)
;   word 1: bit 0 = selected (pull OE_BUS low while "E is high"), bits 1-8 = data sample delay
; Side-set pin = OE_BUS (GP26). One RX word per cycle: D0..D7 sampled during OE low (or 0 if unselected).
; At clkdiv 1.1 on a 150 MHz system clock: ~1.1 us per cycle, OE low ~ (68 + delay) PIO cycles.
.program fake6809_p2
.side_set 1
.wrap_target
    pull block          side 1
    out pins, 15        side 1        ; address + R/W valid (E low, Q about to rise)
    pull block          side 1 [31]   ; address setup time
    out x, 1            side 1
    out y, 8            side 1
    jmp !x nosel        side 1
    nop                 side 0        ; OE_BUS low: E high and cart selected
dly:
    jmp y-- dly         side 0        ; delay+1 cycles
    in pins, 8          side 0        ; sample D0..D7 (what core1 drives on a read)
    push block          side 0 [31]
    nop                 side 0 [31]
    jmp done            side 1        ; OE_BUS high: E falling
nosel:
    nop                 side 1 [31]
    nop                 side 1 [31]
    in null, 8          side 1
    push block          side 1
done:
    nop                 side 1 [31]   ; E low
.wrap
```

- [ ] **Step 4: fake6809.c**

Create `firmware/src/bus/fake6809.c`:

```c
#include "fake6809.h"
#include "bus.h"
#include "rom.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"
#include "fake6809.pio.h"
#include PICOCO_BOARD_H
#include <stdio.h>
#include <string.h>

#define CLKDIV 1.1f
#define D_MASK 0xFFu

static PIO pio = pio1;
static int sm = -1;
static uint offset;

static void pins_to_pio(void) {
    /* OE_BUS must read high before the SM takes the pin, or core1 sees a fake cycle. */
    pio_sm_set_pins_with_mask(pio, sm, 1u << PIN_OE_BUS, 1u << PIN_OE_BUS);
    for (int g = PIN_A0; g <= PIN_RW; g++) pio_gpio_init(pio, g);
    pio_gpio_init(pio, PIN_OE_BUS);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 15, true);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_OE_BUS, 1, true);
}

static void pins_to_sio(void) {
    for (int g = PIN_A0; g <= PIN_RW; g++) { gpio_set_function(g, GPIO_FUNC_SIO); gpio_set_dir(g, GPIO_IN); gpio_pull_up(g); }
    gpio_set_function(PIN_OE_BUS, GPIO_FUNC_SIO); gpio_set_dir(PIN_OE_BUS, GPIO_IN); gpio_pull_up(PIN_OE_BUS);
}

static int start(void) {
    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) return -1;
    offset = pio_add_program(pio, &fake6809_p2_program);
    pio_sm_config c = fake6809_p2_program_get_default_config(offset);
    sm_config_set_out_pins(&c, PIN_A0, 15);
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_sideset_pins(&c, PIN_OE_BUS);
    sm_config_set_out_shift(&c, true, false, 32);    /* shift right: bit 0 first */
    sm_config_set_in_shift(&c, false, false, 32);    /* shift left: one in pins,8 leaves the byte in bits 0-7 */
    sm_config_set_clkdiv(&c, CLKDIV);
    pins_to_pio();
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
    return 0;
}

static void stop(void) {
    pio_sm_set_enabled(pio, sm, false);
    pins_to_sio();
    pio_remove_program(pio, &fake6809_p2_program, offset);
    pio_sm_unclaim(pio, sm);
    sm = -1;
}

/* One bus cycle. For writes, D0..D7 are driven from core0's SIO for the
 * duration (core1 only samples them on a write). Returns the byte the PIO
 * sampled during OE low (a read's data), or 0 for an unselected cycle. */
static uint8_t cycle(uint16_t addr, bool rd, bool sel, uint8_t data, uint8_t delay) {
    if (!rd) { sio_hw->gpio_clr = D_MASK; sio_hw->gpio_set = data; sio_hw->gpio_oe_set = D_MASK; }
    uint32_t w0 = (addr & 0x3FFF) | (rd ? 0x4000u : 0);
    uint32_t w1 = (sel ? 1u : 0) | ((uint32_t)delay << 1);
    pio_sm_put_blocking(pio, sm, w0);
    pio_sm_put_blocking(pio, sm, w1);
    uint32_t got = pio_sm_get_blocking(pio, sm);
    if (!rd) sio_hw->gpio_oe_clr = D_MASK;
    return (uint8_t)(got & 0xFF);
}

#define SAMPLE_LATE 40   /* delay used for functional checks: well after core1 has driven data */

static uint8_t img[2 * ROM_BANK_SIZE];

int fake6809_selftest(fake_result_t *r, void (*line)(const char *s)) {
    char buf[96];
    memset(r, 0, sizeof *r);
    r->first_ok_delay = -1;

    uint32_t c0 = bus_stats.cycles;
    sleep_ms(100);
    if (bus_stats.cycles != c0) return -2;                 /* a CoCo is driving the bus: refuse */
    if (start() < 0) return -1;

    bool drive_was = bus_drive_get();
    bus_drive_set(true);
    /* Synthetic 32 KB banked image: bank b is filled with b, byte 0 marked. */
    for (int b = 0; b < 2; b++) memset(img + b * ROM_BANK_SIZE, b, ROM_BANK_SIZE);
    img[0] = 0xA5;
    rom_load_mem(img, sizeof img);

    uint32_t cyc0 = bus_stats.cycles, wr0 = bus_stats.writes, ov0 = bus_stats.write_overrun;
    int fails = 0;
    #define CHECK(name, cond) do { bool ok_ = (cond); r->cycles++; if (!ok_) { fails++; r->mismatches++; } \
        snprintf(buf, sizeof buf, "selftest %s %s", name, ok_ ? "ok" : "FAIL"); line(buf); } while (0)

    CHECK("read_bank0_marker", cycle(0xC000, true, true, 0, SAMPLE_LATE) == 0xA5);
    CHECK("read_bank0_fill",   cycle(0xC001, true, true, 0, SAMPLE_LATE) == 0x00);
    CHECK("read_top_of_window", cycle(0xFEFF, true, true, 0, SAMPLE_LATE) == 0x00);
    cycle(0xFF40, false, true, 1, SAMPLE_LATE);            /* bank select 1 */
    CHECK("bank_switch_next_read", cycle(0xC001, true, true, 0, SAMPLE_LATE) == 0x01);
    cycle(0xFF40, false, true, 0, SAMPLE_LATE);
    CHECK("bank_switch_back", cycle(0xC001, true, true, 0, SAMPLE_LATE) == 0x00);
    CHECK("becker_status_read", cycle(0xFF41, true, true, 0, SAMPLE_LATE) == 0x00);
    for (int i = 0; i < 10; i++) cycle(0xFF42, false, true, (uint8_t)i, SAMPLE_LATE);
    CHECK("writes_counted", bus_stats.writes - wr0 == 10 + 2);
    uint32_t before = bus_stats.cycles;
    cycle(0xC001, true, false, 0, SAMPLE_LATE);            /* unselected: core1 must not see it */
    CHECK("unselected_ignored", bus_stats.cycles == before);
    CHECK("no_ring_overrun", bus_stats.write_overrun == ov0);
    CHECK("cycles_counted", bus_stats.cycles - cyc0 == 8 + 10);   /* 6 reads + 2 bank writes + 10 $FF42 writes; the unselected cycle is not seen */

    /* Timing sweep: smallest sample delay at which core1's read data is already valid. */
    for (int d = 0; d <= 60; d++) {
        if (cycle(0xC001, true, true, 0, (uint8_t)d) == 0x00 && cycle(0xC000, true, true, 0, (uint8_t)d) == 0xA5) {
            r->first_ok_delay = d;
            break;
        }
    }
    float ns_per = 1e9f * CLKDIV / (float)clock_get_hz(clk_sys);
    r->delay_ns = r->first_ok_delay < 0 ? 0 : (uint32_t)((r->first_ok_delay + 2) * ns_per);   /* +2: nop + first jmp before the sample */
    snprintf(buf, sizeof buf, "selftest response first_ok_delay %d (~%u ns after OE_BUS fell)", r->first_ok_delay, r->delay_ns);
    line(buf);
    CHECK("response_measured", r->first_ok_delay >= 0);
    r->ring_overrun = bus_stats.write_overrun - ov0;
    #undef CHECK

    rom_off();
    bus_drive_set(drive_was);
    stop();
    line("selftest rom cleared; reload with rom load");
    return fails ? -1 : 0;
}
```

- [ ] **Step 5: Console command**

In `firmware/src/console/console.c`, near the top add:

```c
#ifndef PICOCO_HOST
#include "fake6809.h"
#endif
static void selftest_line(const char *s) { outf("%s\n", s); }
```

Replace `cmd_bus`:

```c
static int cmd_bus(int argc, char **argv) {
    if (argc < 2) { outf("bus drive %s\n", bus_drive_get() ? "on" : "off"); return 0; }
    if (strcasecmp(argv[1], "drive") == 0) {
        if (argc < 3) return cerr("usage: bus drive on|off");
        if (strcasecmp(argv[2], "on") == 0) { bus_drive_set(true); return 0; }
        if (strcasecmp(argv[2], "off") == 0) { bus_drive_set(false); return 0; }
        return cerr("usage: bus drive on|off");
    }
    if (strcasecmp(argv[1], "selftest") == 0) {
#ifdef PICOCO_HOST
        return cerr("bus selftest: pico only");
#else
        fake_result_t r;
        int rc = fake6809_selftest(&r, selftest_line);
        if (rc == -2) return cerr("bus selftest: bus is live (CoCo attached), refused");
        outf("selftest cycles %u mismatches %u ring_overrun %u\n", r.cycles, r.mismatches, r.ring_overrun);
        if (rc != 0) return cerr("selftest FAIL");
        outf("selftest pass\n");
        return 0;
#endif
    }
    return cerr("usage: bus drive on|off | bus selftest");
}
```

Help line: add `bus selftest` is covered by `bus`; leave the help string as is.

- [ ] **Step 6: CMake**

In the Pico branch of `firmware/CMakeLists.txt`: add `src/bus/fake6809.c` to `add_executable(picoco ...)`, add `hardware_pio` to `target_link_libraries(picoco ...)`, and after `add_executable`:

```cmake
  pico_generate_pio_header(picoco ${CMAKE_CURRENT_LIST_DIR}/src/bus/fake6809.pio)
```

In the host branch, after the test loop:

```cmake
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME bench COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/bench.py --skip-if-absent)
  set_tests_properties(bench PROPERTIES SKIP_RETURN_CODE 77)
endif()
```

- [ ] **Step 7: bench.py**

Create `firmware/tools/bench.py`:

```python
#!/usr/bin/env python3
"""Run the on-device self-tests over the USB console. Stdlib only.

Usage: bench.py [--port /dev/cu.usbmodemXXXX3] [--skip-if-absent]
Exit 0 pass, 1 fail, 77 skipped (no Pico console found and --skip-if-absent).
The console is CDC1: the *second* of the two usbmodem nodes a PiCoCo creates.
"""
import argparse
import glob
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pconsole import open_port  # noqa: E402

TIMEOUT_S = 20.0


def run(fd, cmd):
    os.write(fd, (cmd + "\r\n").encode())
    lines, buf = [], b""
    deadline = time.monotonic() + TIMEOUT_S
    while time.monotonic() < deadline:
        try:
            chunk = os.read(fd, 256)
        except BlockingIOError:
            chunk = b""
        if not chunk:
            time.sleep(0.02)
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.rstrip(b"\r").decode(errors="replace")
            lines.append(line)
            if line == "ok" or line.startswith("err"):
                return lines
    lines.append("<timeout>")
    return lines


def find_port():
    nodes = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))
    return nodes[1] if len(nodes) >= 2 else (nodes[0] if nodes else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--skip-if-absent", action="store_true")
    a = ap.parse_args()
    port = a.port or find_port()
    if not port:
        print("no Pico console found")
        return 77 if a.skip_if_absent else 1
    fd = open_port(port)
    ok = True
    for line in run(fd, "version"):
        print(line)
    out = run(fd, "bus selftest")
    for line in out:
        print(line)
        if "FAIL" in line or line.startswith("err") or line == "<timeout>":
            ok = False
    if not any(l == "selftest pass" for l in out):
        ok = False
    # A saved config may have loaded a ROM the self-test cleared; replay it.
    for line in run(fd, "reboot"):
        print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 8: Build and run**

Run: `ninja -C build-host && ./build-host/test_console | tail -2 && ninja -C build-pico2 2>&1 | grep -E 'check_core1|error'`
Expected: `test_console` 0 failed; `check_core1_flash_free: ok` (the fake 6809 is core0 code and is not reachable from core1).

Flash the Pico 2 (breadboard rig or an unplugged PCB, USB only): `firmware/tools/flash.sh build-pico2/picoco.uf2` then `python3 firmware/tools/bench.py`.
Expected: every `selftest <name> ok`, a `selftest response first_ok_delay N (~M ns ...)` line, `selftest pass`, exit 0. Record M in the commit message. If `bank_switch_next_read` fails, the hook or `bus_peek` inlining is wrong; if `unselected_ignored` fails, OE_BUS glitched low during `pins_to_pio` (check the `pio_sm_set_pins_with_mask` call runs before the pindir change).

- [ ] **Step 9: Commit**

```bash
git add firmware/src/bus/fake6809.pio firmware/src/bus/fake6809.c firmware/src/bus/fake6809.h firmware/CMakeLists.txt firmware/src/console/console.c firmware/tools/bench.py firmware/tests/test_console.c
git commit -m "firmware: on-chip fake 6809 (PIO1) and bus selftest; tools/bench.py

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Plus-W core1 loop with firmware address decode

**Files:**
- Modify: `firmware/boards/plusw.h`
- Modify: `firmware/src/bus/bus.h`, `firmware/src/bus/bus.c`
- Modify: `firmware/src/bus/bus_core1.c`
- Modify: `firmware/src/main.c` (`gpio_setup`)
- Modify: `firmware/src/bus/fake6809.pio`, `firmware/src/bus/fake6809.c` (Plus-W variant)
- Test: `firmware/tests/test_bus.c`

**Interfaces:**
- Consumes: `bus_peek`, write hooks, `bus_stats.hw_selected/fw_selected`.
- Produces:
  - `plusw.h`: `#define PICOCO_BOARD_PLUSW 1`, `PIN_CTS 24`, `PIN_SCS 25`, `PIN_E 26`, `PIN_Q 27`, `PIN_SLENB 28`, `PIN_A14 29`, `PIN_A15 30`, `PIN_OE_FW 31`.
  - `bus.h`: `extern volatile uint32_t bus_fw_mask;` `int bus_fw_enable(uint16_t addr);` (0 ok, -1 outside `$FF60-$FF7F`), `void bus_fw_disable(uint16_t addr);`, `static inline bool bus_fw_selected(uint16_t addr, uint32_t mask)`.

- [ ] **Step 1: Write the failing tests**

Append to `firmware/tests/test_bus.c`:

```c
TEST(fw_decode_mask) {
    bus_init();
    ASSERT_EQ(bus_fw_mask, 0);
    ASSERT_EQ(bus_fw_enable(0xFF7E), 0);
    ASSERT_EQ(bus_fw_enable(0xFF6E), 0);
    ASSERT_EQ(bus_fw_enable(0xFF5F), -1);      /* /SCS territory: hardware decodes it */
    ASSERT_EQ(bus_fw_enable(0xFF80), -1);
    ASSERT_EQ(bus_fw_enable(0xC000), -1);
    ASSERT_EQ(bus_fw_mask, (1u << 0x1E) | (1u << 0x0E));
    ASSERT(bus_fw_selected(0xFF7E, bus_fw_mask));
    ASSERT(bus_fw_selected(0xFF6E, bus_fw_mask));
    ASSERT(!bus_fw_selected(0xFF7D, bus_fw_mask));
    ASSERT(!bus_fw_selected(0xBF7E, bus_fw_mask));   /* A14/A15 low: a different page */
    ASSERT(!bus_fw_selected(0xFF7E, 0));
    bus_fw_disable(0xFF7E);
    ASSERT(!bus_fw_selected(0xFF7E, bus_fw_mask));
    ASSERT(bus_fw_selected(0xFF6E, bus_fw_mask));
}
```
and `RUN(fw_decode_mask);`.

- [ ] **Step 2: Run to verify it fails**

Run: `ninja -C build-host 2>&1 | tail -3`
Expected: `bus_fw_mask` undeclared.

- [ ] **Step 3: bus.h / bus.c**

`bus.h`:

```c
/* Firmware address decode (Plus-W with JP2 2-3). One bit per address in
 * $FF60-$FF7F; written by core0 only (32-bit store, atomic), read by core1
 * every cycle. Everything else is hardware-selected by /CTS and /SCS. */
extern volatile uint32_t bus_fw_mask;
int  bus_fw_enable(uint16_t addr);    /* 0 ok, -1 if addr is outside $FF60-$FF7F */
void bus_fw_disable(uint16_t addr);
static inline __attribute__((always_inline)) bool bus_fw_selected(uint16_t addr, uint32_t mask) {
    return (addr & 0xFFE0) == 0xFF60 && ((mask >> (addr & 0x1F)) & 1u);
}
```

`bus.c`: `volatile uint32_t bus_fw_mask;` reset to 0 in `bus_init`;

```c
int bus_fw_enable(uint16_t addr) {
    if ((addr & 0xFFE0) != 0xFF60) return -1;
    bus_fw_mask |= 1u << (addr & 0x1F);
    return 0;
}
void bus_fw_disable(uint16_t addr) {
    if ((addr & 0xFFE0) == 0xFF60) bus_fw_mask &= ~(1u << (addr & 0x1F));
}
```

- [ ] **Step 4: Board header and init**

`firmware/boards/plusw.h`, replace the "Pad-grid inputs, capture only" comment block with:

```c
#define PICOCO_BOARD_PLUSW 1
/* Pad-grid signals (all in sio_hw->gpio_in, bits < 32). The Plus-W core1
 * loop (bus_core1.c) samples the address on Q and drives U10 /OE itself
 * from PIN_OE_FW when JP2 is in the 2-3 position. GP34/GP42 (audio) are
 * left untouched: not driven, not pulled. */
#define PIN_CTS     24
#define PIN_SCS     25
#define PIN_E       26
#define PIN_Q       27
#define PIN_SLENB   28
#define PIN_A14     29
#define PIN_A15     30
#define PIN_OE_FW   31   /* output, active low; high = U10 disabled */
```

`firmware/src/main.c` `gpio_setup`, after the `PIN_CART_DRV` block:

```c
#ifdef PIN_OE_FW
    gpio_init(PIN_OE_FW); gpio_put(PIN_OE_FW, 1); gpio_set_dir(PIN_OE_FW, GPIO_OUT);   /* buffer disabled until core1 selects a cycle */
#endif
```

- [ ] **Step 5: The Plus-W loop**

In `firmware/src/bus/bus_core1.c`, wrap the existing `bus_core1_main` in `#ifndef PICOCO_BOARD_PLUSW ... #else ... #endif` and add the Plus-W body:

```c
#else  /* PICOCO_BOARD_PLUSW */
#define E_MASK     (1u << PIN_E)
#define Q_MASK     (1u << PIN_Q)
#define CTS_MASK   (1u << PIN_CTS)
#define SCS_MASK   (1u << PIN_SCS)
#define A14_MASK   (1u << PIN_A14)
#define A15_MASK   (1u << PIN_A15)
#define OEFW_MASK  (1u << PIN_OE_FW)

/* Plus-W: decode during E low, act during E high. Works with JP2 in either
 * position (1-2: U15 also enables U10 for hardware-selected cycles, which is
 * consistent with what we do; 2-3: only PIN_OE_FW enables it, which is what
 * lets $FF60-$FF7F respond). */
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    for (;;) {
        while (sio_hw->gpio_in & E_MASK) { }            /* wait E low */
        while (!(sio_hw->gpio_in & Q_MASK)) { }         /* wait Q high: address valid */
        uint32_t in = sio_hw->gpio_in;
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        bool sel;
        if ((in & (CTS_MASK | SCS_MASK)) != (CTS_MASK | SCS_MASK)) {
            sel = true;
            bus_stats.hw_selected++;
        } else {
            uint16_t addr = idx | ((in & A14_MASK) ? 0x4000 : 0) | ((in & A15_MASK) ? 0x8000 : 0);
            sel = bus_fw_selected(addr, bus_fw_mask);
            if (sel) bus_stats.fw_selected++;
        }
        while (!(sio_hw->gpio_in & E_MASK)) { }         /* wait E high */
        if (!sel) continue;                             /* loop top waits for E low: the end of this cycle */
        if (in & RW_MASK) {                             /* CoCo read */
            if (bus_drive) {
                sio_hw->gpio_clr = D_MASK;
                sio_hw->gpio_set = (uint32_t)bus_peek(idx) << PIN_D0;
                sio_hw->gpio_oe_set = D_MASK;
                sio_hw->gpio_clr = OEFW_MASK;           /* U10 outward */
                while (sio_hw->gpio_in & E_MASK) { }
                sio_hw->gpio_set = OEFW_MASK;
                sio_hw->gpio_oe_clr = D_MASK;
            } else {
                while (sio_hw->gpio_in & E_MASK) { }
            }
            bus_on_read_done(idx, time_us_32());
        } else {                                        /* CoCo write: last sample while E was high */
            sio_hw->gpio_clr = OEFW_MASK;               /* U10 inward */
            uint32_t d, prev = sio_hw->gpio_in;
            for (;;) {
                d = sio_hw->gpio_in;
                if (!(d & E_MASK)) break;
                prev = d;
            }
            sio_hw->gpio_set = OEFW_MASK;
            bus_on_write(idx, (uint8_t)((prev >> PIN_D0) & 0xFF), time_us_32());
        }
    }
}
#endif
```

Note `OE_HIGH`, `BUS_OE_REG`/`BUS_OE_MASK` remain used only by the Pico 2 branch; keep the macros at the top of the file for both.

- [ ] **Step 6: Plus-W fake 6809**

Append to `fake6809.pio`:

```
; Plus-W variant: drives GP8..GP30 (A0-A13, R/W, GP23 LED, CTS, SCS, E, Q, SLENB, A14, A15)
; from five words per cycle: four phase words (pin levels) and one control word (bits 0-7 sample delay).
;   P0: address valid, E=0 Q=0    P1: Q=1    P2: E=1 (sample after delay)    P3: Q=0, E still 1
; The next cycle's P0 drops E. Bit k of a phase word is GP(8+k).
.program fake6809_pw
.wrap_target
    pull block
    out pins, 23    [31]     ; P0
    pull block
    out pins, 23    [31]     ; P1
    pull block
    out pins, 23             ; P2: E rises
    pull block
    out y, 8
dly:
    jmp y-- dly
    in pins, 8
    push block      [15]
    pull block
    out pins, 23    [31]     ; P3
.wrap
```

In `fake6809.c`, make the program, pin setup and `cycle()` board-conditional:

```c
#ifdef PICOCO_BOARD_PLUSW
#define PW_BIT(gp) (1u << ((gp) - PIN_A0))
static uint32_t phase(uint16_t addr, bool rd, bool sel, bool e, bool q) {
    uint32_t w = (addr & 0x3FFF) | (rd ? PW_BIT(PIN_RW) : 0) | PW_BIT(PIN_SLENB)
               | ((addr & 0x4000) ? PW_BIT(PIN_A14) : 0) | ((addr & 0x8000) ? PW_BIT(PIN_A15) : 0)
               | (e ? PW_BIT(PIN_E) : 0) | (q ? PW_BIT(PIN_Q) : 0);
    /* hardware select: /CTS for the ROM window, /SCS for $FF40-$FF5F; firmware-decoded addresses assert neither */
    bool cts = sel && addr < 0xFF00, scs = sel && (addr & 0xFFE0) == 0xFF40;
    if (!cts) w |= PW_BIT(PIN_CTS);
    if (!scs) w |= PW_BIT(PIN_SCS);
    return w;
}
static void pins_to_pio(void) {
    uint32_t idle = phase(0, true, false, false, false);
    pio_sm_set_pins_with_mask(pio, sm, idle << PIN_A0, 0x7FFFFFu << PIN_A0);
    for (int g = PIN_A0; g <= PIN_A15; g++) pio_gpio_init(pio, g);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 23, true);
}
static void pins_to_sio(void) {
    for (int g = PIN_A0; g <= PIN_A15; g++) { gpio_set_function(g, GPIO_FUNC_SIO); gpio_set_dir(g, GPIO_IN); gpio_pull_up(g); }
#ifdef PIN_LED
    gpio_set_dir(PIN_LED, GPIO_OUT);
#endif
}
static uint8_t cycle(uint16_t addr, bool rd, bool sel, uint8_t data, uint8_t delay) {
    if (!rd) { sio_hw->gpio_clr = D_MASK; sio_hw->gpio_set = data; sio_hw->gpio_oe_set = D_MASK; }
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, false));
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, true));
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, true, true));
    pio_sm_put_blocking(pio, sm, delay);
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, true, false));
    uint32_t got = pio_sm_get_blocking(pio, sm);
    if (!rd) sio_hw->gpio_oe_clr = D_MASK;
    return (uint8_t)(got & 0xFF);
}
#define PROGRAM fake6809_pw_program
#define CONFIG  fake6809_pw_program_get_default_config
#define OUT_COUNT 23
#else
/* Pico 2 versions of pins_to_pio / pins_to_sio / cycle from Task 3 */
#define PROGRAM fake6809_p2_program
#define CONFIG  fake6809_p2_program_get_default_config
#define OUT_COUNT 15
#endif
```

`start()` uses `PROGRAM`, `CONFIG`, `sm_config_set_out_pins(&c, PIN_A0, OUT_COUNT)`, and calls `sm_config_set_sideset_pins` only under `#ifndef PICOCO_BOARD_PLUSW`. `stop()` removes `PROGRAM`.

Extend the self-test list under `#ifdef PICOCO_BOARD_PLUSW`, after `unselected_ignored`:

```c
    uint32_t fw0 = bus_stats.fw_selected, cyc_fw = bus_stats.cycles;
    cycle(0xFF7E, true, false, 0, SAMPLE_LATE);            /* not enabled: ignored */
    CHECK("fw_unenabled_ignored", bus_stats.cycles == cyc_fw);
    bus_fw_enable(0xFF7E);
    bus_set_read(0x3F7E, 0x9F);
    CHECK("fw_read", cycle(0xFF7E, true, false, 0, SAMPLE_LATE) == 0x9F);
    uint32_t wr_fw = bus_stats.writes;
    cycle(0xFF7E, false, false, 0x42, SAMPLE_LATE);
    CHECK("fw_write_queued", bus_stats.writes == wr_fw + 1 && bus_stats.fw_selected == fw0 + 2);
    CHECK("fw_other_page_ignored", (cycle(0xBF7E, true, false, 0, SAMPLE_LATE), bus_stats.fw_selected == fw0 + 2));
    bus_fw_disable(0xFF7E);
    bus_set_read(0x3F7E, 0xFF);
```

and adjust `cycles_counted`'s expected delta to `8 + 10 + 2` on the Plus-W (use `#ifdef` for the constant; the two extra are `fw_read` and `fw_write_queued`).

- [ ] **Step 7: Build both boards, run host tests**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure && ninja -C build-pico2 2>&1 | grep check_core1 && cmake -B build-plusw -G Ninja -DPICOCO_BOARD=plusw firmware && ninja -C build-plusw 2>&1 | grep -E 'check_core1|error'`
Expected: host green; both Pico builds `check_core1_flash_free: ok`.

On a Plus-W board unplugged from the CoCo (USB only): flash `build-plusw/picoco.uf2`, run `python3 firmware/tools/bench.py`.
Expected: all checks `ok` including the `fw_*` ones, `selftest pass`. On the bench CoCo 3 with JP2 at 2-3: HDB-DOS still boots (hardware-selected cycles through the firmware `/OE`), and `status` after a `DIR` shows `hw_sel` climbing and `fw_sel` 0.

- [ ] **Step 8: Commit**

```bash
git add firmware/boards/plusw.h firmware/src/bus firmware/src/main.c firmware/tests/test_bus.c
git commit -m "firmware: Plus-W core1 loop with firmware /OE and \$FF60-\$FF7F decode; fake 6809 Plus-W variant

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: Firmware `/CART` pulse (Plus-W)

**Files:**
- Modify: `firmware/src/dev/rom.h`, `firmware/src/dev/rom.c`
- Modify: `firmware/src/console/console.c` (`cart` command, `cmd_save`, help)
- Modify: `firmware/src/main.c`
- Test: `firmware/tests/test_rom.c`, `firmware/tests/test_console.c`

**Interfaces:**
- Consumes: `rom_loaded`, `rom_is_dos` (Task 2), `PIN_CART_DRV` (plusw.h).
- Produces: `typedef enum { CART_AUTO = 0, CART_ON, CART_OFF } cart_mode_t;` `void rom_cart_set(cart_mode_t m); cart_mode_t rom_cart_get(void); bool rom_cart_wanted(void);` (rom.h). `rom_cart_wanted`: `ON` → true; `OFF` → false; `AUTO` → `rom_loaded() && !rom_is_dos()`.

- [ ] **Step 1: Write the failing tests**

Append to `firmware/tests/test_rom.c`:

```c
TEST(cart_autostart_decision) {
    setup();
    ASSERT_EQ(rom_cart_get(), CART_AUTO);
    ASSERT(!rom_cart_wanted());                 /* no ROM */
    fill_banks(1);
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(rom_cart_wanted());                  /* plain pak */
    img[0] = 'D'; img[1] = 'K';
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(!rom_cart_wanted());                 /* DOS ROM: never pulse, it would jump to $C000 as code */
    rom_cart_set(CART_ON);
    ASSERT(rom_cart_wanted());
    rom_cart_set(CART_OFF);
    img[0] = 'X';
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(!rom_cart_wanted());
    rom_cart_set(CART_AUTO);
}
```
and `RUN(cart_autostart_decision);`.

Append to `firmware/tests/test_console.c`:

```c
TEST(cart_command_and_save) {
    setup();
    ASSERT_EQ(console_exec("cart on"), 0);
    ASSERT_EQ(rom_cart_get(), CART_ON);
    ASSERT_EQ(console_exec("cart off"), 0);
    ASSERT_EQ(rom_cart_get(), CART_OFF);
    ASSERT_EQ(console_exec("cart auto"), 0);
    ASSERT_EQ(rom_cart_get(), CART_AUTO);
    ASSERT_EQ(console_exec("cart sideways"), -1);
    console_exec("cart on");
    outn = 0;
    ASSERT_EQ(console_exec("save"), 0);
    char cfg[1024]; int n = plat_cfg_read(cfg, sizeof cfg - 1); ASSERT(n > 0); cfg[n] = 0;
    ASSERT(strstr(cfg, "cart on\n"));
    console_exec("cart auto");
    ASSERT_EQ(console_exec("save"), 0);
    n = plat_cfg_read(cfg, sizeof cfg - 1); cfg[n] = 0;
    ASSERT(!strstr(cfg, "cart "));              /* default is omitted */
}
```
and `RUN(cart_command_and_save);`. (`test_console`'s `setup` already calls `plat_host_set_dir`, so `plat_cfg_read/write` work there.)

- [ ] **Step 2: Run to verify it fails**

Run: `ninja -C build-host 2>&1 | tail -3`
Expected: `rom_cart_get` undeclared.

- [ ] **Step 3: Implement**

`rom.h`:

```c
typedef enum { CART_AUTO = 0, CART_ON, CART_OFF } cart_mode_t;
void        rom_cart_set(cart_mode_t m);
cart_mode_t rom_cart_get(void);
bool        rom_cart_wanted(void);   /* pulse /CART after reset? ON: yes; OFF: no; AUTO: a non-DOS ROM is loaded */
```

`rom.c`:

```c
static cart_mode_t cart_mode;
void        rom_cart_set(cart_mode_t m) { cart_mode = m; }
cart_mode_t rom_cart_get(void)          { return cart_mode; }
bool rom_cart_wanted(void) {
    if (cart_mode == CART_ON)  return true;
    if (cart_mode == CART_OFF) return false;
    return rom_have && !rom_dos;
}
```

`console.c`:

```c
static int cmd_cart(int argc, char **argv) {
    if (argc < 2) { outf("cart %s\n", rom_cart_get() == CART_ON ? "on" : rom_cart_get() == CART_OFF ? "off" : "auto"); return 0; }
    if (strcasecmp(argv[1], "on") == 0)   { rom_cart_set(CART_ON);   return 0; }
    if (strcasecmp(argv[1], "off") == 0)  { rom_cart_set(CART_OFF);  return 0; }
    if (strcasecmp(argv[1], "auto") == 0) { rom_cart_set(CART_AUTO); return 0; }
    return cerr("usage: cart on|off|auto");
}
```

Dispatch: `if (strcasecmp(v, "cart") == 0) return cmd_cart(argc, argv);`. Help string: add `cart`. In `cmd_save`, after the `rom` line:

```c
    if (rom_cart_get() != CART_AUTO && !cfg_append(cfg, sizeof(cfg), &len, "cart %s\n", rom_cart_get() == CART_ON ? "on" : "off"))
        return cerr("config too large");
```

`main.c`: after `gpio_put(PIN_HALT, 0);`:

```c
#ifdef PIN_CART_DRV
    /* Autostart paks expect /CART pulsing after reset (a real pak ties it to
     * Q). Toggle Q4 for 500 ms after the /HALT release so Color BASIC's
     * cart check sees an edge after it has initialised the PIA; DOS ROMs
     * ("DK") never get this, they would jump to $C000 as code. Power-on and
     * Pico reboot only: a CoCo reset button press is not visible to us. */
    uint32_t cart_until = rom_cart_wanted() ? plat_now_ms() + 500 : 0;
#endif
```

and in the main loop, after `mode_pump`:

```c
#ifdef PIN_CART_DRV
        if (cart_until) {
            if (now < cart_until) gpio_put(PIN_CART_DRV, now & 1);
            else { gpio_put(PIN_CART_DRV, 0); cart_until = 0; }
        }
#endif
```

- [ ] **Step 4: Run tests and builds**

Run: `ninja -C build-host && ctest --test-dir build-host --output-on-failure && ninja -C build-plusw 2>&1 | grep -E 'check_core1|error|warning: unused'`
Expected: green; no unused-variable warning on the Pico 2 build (`cart_until` is inside `#ifdef PIN_CART_DRV`, which only the Plus-W defines).

Bench (Plus-W on the CoCo 3, a GMC image loaded, `cart auto`): power on, the game starts without `EXEC`. With HDB-DOS loaded, BASIC comes up normally.

- [ ] **Step 5: Commit**

```bash
git add firmware/src/dev/rom.h firmware/src/dev/rom.c firmware/src/console/console.c firmware/src/main.c firmware/tests/test_rom.c firmware/tests/test_console.c
git commit -m "firmware: firmware-pulsed /CART for autostart paks (Plus-W); cart on|off|auto

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: Docs and rules

**Files:**
- Modify: `docs/firmware-architecture.md`, `docs/hardware-design.md`, `docs/ADDITIONAL_ROADMAP.md`, `firmware/README.md`, `CLAUDE.md`
- Modify: `firmware/boards/pico2_breadboard.h` (comment only)

- [ ] **Step 1: firmware-architecture.md**

Add a section "Plus-W bus loop" after the existing core1 description covering: the E-low/Q-high sampling order, `bus_fw_mask` and `bus_fw_enable`, `PIN_OE_FW` and the two JP2 positions, the `bus_stats` counters `hw_selected`/`fw_selected`/`whooks_run`. Add "Write hooks" (API, BUS_HOT, dispatched inside `bus_on_write` before the ring push, checked by `check_core1_flash_free.py`). Add "Banked ROM" (`bus_rom_base`, `bus_peek`, the `$FF40` hook, sizes, load ordering). Add "Self-test: fake 6809" (what it drives, the refusal rule, why it is safe on an unplugged PCB: +3.3 V rail off, LVC Ioff; the measured response time from Task 3). Replace the pin-table note for GP31/GP33 "left untouched" with their new roles.

- [ ] **Step 2: hardware-design.md**

§4.4a (JP2): note that the Plus-W firmware drives `OE_FW` in both positions and only `$FF60-$FF7F` needs 2-3. §4.5 (/CART): reference the `cart` command and the 500 ms pulse. §3.2 pin table: GP31 `OE_FW` and GP33 `CART_DRV` marked "firmware-driven (bus-engine spec)".

- [ ] **Step 3: README.md and roadmap**

`firmware/README.md` "Console checks": add `bus selftest`, `cart`, banked `rom load`, and a "Self-test without a CoCo" subsection with the `bench.py` invocation and the rule "unplug from the CoCo first; the command refuses if it sees live cycles". `docs/ADDITIONAL_ROADMAP.md` §6 item 12: mark DONE with the date and `cart auto` semantics.

- [ ] **Step 4: CLAUDE.md hard rules**

Under "Firmware", add:

```
- **Write hooks run on core1.** Anything passed to `bus_add_write_hook` is
  `BUS_HOT`, touches only SRAM, and is followed by
  `check_core1_flash_free.py`. Hooks run inside `bus_on_write` before the
  event is queued, so core0 still sees every write.
- **`bus_rom_base` is swapped only by rom.c**: the loader (core0, after the
  banks are filled) and the `$FF40` hook (core1). Read the ROM window
  through `bus_peek()`, never `bus_table[]` directly.
- **`bus_fw_mask` is core0-written, `$FF60-$FF7F` only.** Register a
  firmware-decoded address with `bus_fw_enable`; it is Plus-W only and needs
  JP2 2-3.
- **`bus selftest` drives the bus pins from PIO1.** Never run it with a
  powered CoCo attached; the command refuses when it sees live cycles, but
  don't rely on that.
```

`firmware/boards/pico2_breadboard.h`: no code change; extend the GP28 comment with "GP26/GP8-22 are taken over by PIO1 during `bus selftest` only (fake6809.c)".

- [ ] **Step 5: Confirm the XRoar check still passes**

Run the existing `firmware/README.md` §"Manual check: XRoar" procedure once against `build-host/picoco-host` (HDB-DOS boots, `DIR` lists the mounted image). No code change expected; this is the spec's §4.7 and §6 XRoar item.

- [ ] **Step 6: Commit**

```bash
git add docs/firmware-architecture.md docs/hardware-design.md docs/ADDITIONAL_ROADMAP.md firmware/README.md CLAUDE.md firmware/boards/pico2_breadboard.h
git commit -m "docs: Plus-W bus engine (firmware /OE, write hooks, banked ROM, fake 6809 self-test, /CART)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review notes

- Spec §4.1-4.6 map to Tasks 4, 4, 1, 2, 5, 3 respectively; §4.7 needs no work (listener exists); §5 errors are in Tasks 2, 3, 4, 5; §6 host suites are `test_bus` (decode, hooks), `test_rom` (banked), `test_console` (selftest refusal, cart); on-device is `bench.py`; XRoar in Task 6.
- Names used across tasks: `bus_add_write_hook`, `bus_peek`, `bus_rom_base`, `bus_fw_mask`, `bus_fw_enable/disable/selected`, `rom_bank_count`, `rom_loaded`, `rom_is_dos`, `rom_cart_*`, `fake6809_selftest`, `fake_result_t`, `bus_stats.whooks_run/hw_selected/fw_selected` — consistent.
- Review Focus items 1, 2 → Task 2 tests; 3 → Task 4 test; 4 → Task 3 host test plus the on-device refusal; 5 → Task 5 test.
