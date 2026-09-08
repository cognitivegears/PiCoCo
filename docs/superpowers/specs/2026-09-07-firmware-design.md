# PiCoCo firmware design

Status: approved design, 2026-09-07. Supersedes the "proposed" sections of
`docs/firmware-architecture.md` where they differ (noted in section 12).

## 1. Goals

1. One firmware binary that serves both the breadboard bring-up
   (`docs/breadboard-plan.md` section 6) and the shipping cartridge.
   Behaviour is selected at runtime from a USB console, not by
   flashing different programs.
2. Native DriveWire server on the RP2350, speaking the Becker port at
   $FF41/$FF42, serving disk images from the Pico's flash and later an
   SD card. Bridge mode (Becker to USB serial, pyDriveWire on the host)
   is kept as a bring-up step and a known-good fallback.
3. Everything that does not touch a GPIO compiles and runs on the
   development Mac. The host build is the primary development and test
   target.
4. Debuggable at every stage: a permanent bus flight recorder, counters
   in every module, capture and replay of the CoCo byte stream, logging
   that never perturbs timing, and a crash record that survives reset.
5. A device layer that later features from `docs/RP2350B_IDEAS.md`
   (SDC, ACIA, RTC, sound, MPI) plug into without touching the bus
   engine or DriveWire.

Non-goals for this spec: virtual serial channels over TCP, SD card,
PIO/DMA bus engine, any RP2350B-only feature. Each gets its own spec.

## 2. Decisions

| Decision | Choice | Why |
|---|---|---|
| Language | C11, Pico SDK 2.1+ | Bus path must be C or PIO anyway; MicroPython GC pauses would trip HDB-DOS's DriveWire timeouts; the protocol is a few hundred lines |
| Targets | RP2350 only (Pico 2 module now, RP2350B carrier later) | One SDK platform. Pin maps differ per board, chip does not |
| Bus engine v1 | Core1 C loop from SRAM, interrupts off | Breadboard plan 7.2: fastest to debug; PIO+DMA only if the analyzer shows jitter |
| Device interface | Response table + write events + read hooks | Lets the loop become PIO+DMA later without touching devices |
| DriveWire server | Push-style state machine, transport agnostic | Same code on host (socket) and Pico (Becker stream); replayable |
| Storage | FatFS on a flash partition, exported over USB mass storage on demand; SD later on the same library | One filesystem library for flash and SD; drag-and-drop image upload for free. Deviation from the initial LittleFS preference, see 7 |
| USB | Two CDC interfaces + MSC (only in export mode) | CDC0 raw DriveWire bytes in bridge mode, CDC1 console. No in-band escapes |
| Tests | Plain C asserts under ctest with ASan/UBSan; Python 3 integration client | No framework dependency; sanitizers catch what asserts miss |
| Commits | Milestone git tags as in `firmware-architecture.md` section 9 | |

## 3. Layout

```
firmware/
  CMakeLists.txt              Pico target (PICO_BOARD=pico2). -DPICOCO_HOST=ON builds host lib + tests + picoco-host
  pico_sdk_import.cmake       SDK fetched from git if PICO_SDK_PATH unset
  boards/
    pico2_breadboard.h        GPIO map as #defines (see 4.1). Selected by -DPICOCO_BOARD=pico2_breadboard
  src/
    bus/bus.h bus.c           response table, write ring, trace ring, read hooks, stats (pure C, host-safe)
    bus/bus_core1.c           the core1 loop; the only file that touches SIO GPIO
    dev/device.h device.c     device registry and write-event dispatch
    dev/rom.c                 ROM device
    dev/becker.c becker.h     Becker port device + byte stream API
    dw/dw.h dw_server.c       DriveWire protocol state machine
    dw/dw_disk.c dw_disk.h    image format detection, LSN mapping
    dw/dw_store.h             storage ops struct
    dw/dw_store_posix.c       host implementation (also used by picoco-host)
    dw/dw_store_fatfs.c       Pico implementation
    console/console.c         line parser + commands
    console/config.c          picoco.cfg load/save
    log.h log.c               ring-buffered leveled logging
    crash.c                   hard fault + panic record in noinit RAM
    fs_flash.c                FatFS diskio over the flash partition; USB MSC glue
    usb_descriptors.c tusb_config.h
    main.c                    core0: init, mode switch, main loop
  host/
    picoco_host.c             TCP 65504 server around dw_server with POSIX storage
    sim_bus.c sim_bus.h       virtual CoCo: sim_read(addr)/sim_write(addr,data) against bus.c
  tests/
    test.h                    ~30-line TEST/ASSERT macros
    test_dw_disk.c test_dw_server.c test_becker.c test_bus.c test_console.c test_config.c test_stack.c
    fixtures/                 synthetic images, captured byte streams, trace dumps
  tools/
    dwtest.py                 DriveWire client exerciser (TCP)
    tracedump.py              decode `trace dump` output
  roms/                       user-supplied ROM images, gitignored
```

Toolchain on the Mac: `brew install cmake ninja picotool`; arm-none-eabi-gcc
is already installed. Build: `cmake -B build -G Ninja && ninja -C build`
for the Pico, `cmake -B build-host -DPICOCO_HOST=ON && ninja -C build-host
&& ctest --test-dir build-host` for the host.

## 4. Bus engine

### 4.1 Pin map (boards/pico2_breadboard.h)

| Define | Value | Signal |
|---|---|---|
| `PIN_D0` | 0 | D0..D7 on GP0..GP7 |
| `PIN_A0` | 8 | A0..A13 on GP8..GP21 |
| `PIN_RW` | 22 | R/W (1 = CoCo read) |
| `PIN_OE_BUS` | 26 | low = cart selected and E high |
| `PIN_HALT` | 27 | high = hold /HALT |
| `PIN_E` | 28 | E clock (unused by the v1 loop) |

If the breadboard side experiment moves E or removes buffers, only this
header changes.

### 4.2 Data structures (bus.h, all in SRAM)

- `uint8_t bus_table[16384]` indexed by A0..A13. The byte driven for any
  cart-selected read. Default 0xFF. ROM occupies 0x0000..0x1FFF (8 KB;
  a 16 KB image fills 0x0000..0x3FFF and is overridden at the Becker
  indices). Becker is index 0x3F41 (status) and 0x3F42 (data).
  Because /CTS also asserts for $E000..$FEFF with A13 = 1, those indices
  hold 0xFF unless a 16 KB image is loaded. No aliasing: only the exact
  indices respond.
- `write_ring`: single-producer (core1) single-consumer (core0) ring of
  `uint16_t addr14, uint8_t data`. 256 entries. Overrun increments
  `bus_stats.write_overrun` and drops the newest.
- `read_hooks[4]`: `{uint16_t idx; void (*fn)(void);}` run on core1
  after a read cycle at that index has completed. Becker registers one.
- `trace_ring`: 4096 entries of `{uint32_t t_us; uint16_t addr14;
  uint8_t rw; uint8_t data;}`, always on, overwrites oldest. Write
  cost is one store per cycle. `trace_freeze()` stops recording so a
  dump is consistent.
- `bus_stats`: cycles, reads, writes, write_overrun.

API used by devices: `bus_set_read(idx, byte)`, `bus_set_read_range(idx,
ptr, len)`, `bus_add_read_hook(idx, fn)`, `bus_pop_write(&addr, &data)`.
Nothing else. Same API on host, where `sim_write()` pushes into the
write ring and `sim_read()` returns `bus_table[idx]` and runs the hook.

### 4.3 Core1 loop (bus_core1.c)

Runs from SRAM (`__not_in_flash_func`), interrupts disabled, never
calls into flash-resident code. Sketch:

```
for (;;) {
    while (gpio_in & OE_MASK) ;                 // wait for cart cycle
    in  = gpio_in;
    idx = (in >> PIN_A0) & 0x3FFF;
    if (in & RW_MASK) {                         // CoCo read
        gpio_out_masked(bus_table[idx]);
        gpio_oe_set(D_MASK);
        while (!(gpio_in & OE_MASK)) ;          // hold until cycle ends
        gpio_oe_clr(D_MASK);
        run_read_hook(idx);
        trace(idx, 1, bus_table[idx]);
    } else {                                    // CoCo write
        do { d = gpio_in; } while (!(d & OE_MASK));   // last sample before OE_BUS rose
        write_ring_push(idx, d & 0xFF);
        trace(idx, 0, d & 0xFF);
    }
}
```

Budget: about 70 ns from OE_BUS low to data driven at 150 MHz, against a
280 ns window on a CoCo 3 at 1.79 MHz. Verified on hardware at
milestone 0.5 with the logic analyzer; if jitter shows, the PIO+DMA
engine replaces this file only.

Write data is sampled as the last value seen while OE_BUS was low,
which is the end of E high when the CoCo's write data is valid.

### 4.4 Flash safety

Core0 writes flash (saving disk sectors, config). Core1 never executes
from or reads flash, so the build sets `PICO_FLASH_ASSUME_CORE1_SAFE=1`
and core0 uses `flash_range_erase/program` directly. Optionally
`halt_assert()` around writes longer than a few ms; the CoCo is mid
transaction waiting for a reply byte, so halting it is harmless.

## 5. Devices

`struct device { const char *name; void (*init)(void); void (*on_write)(uint16_t idx, uint8_t data); uint16_t idx_lo, idx_hi; }`
in a fixed array in device.c. Core0's main loop pops write events and
calls `on_write` of the device whose range contains the index.

- **rom**: `rom_load_pattern()` (byte = low 8 bits of address, milestone
  0.5), `rom_load_file(name)` (8 or 16 KB from storage), `rom_load_mem(ptr,
  len)` (build-time embedded image via `-DPICOCO_EMBED_ROM=path`),
  `rom_off()` (0xFF). No on_write.
- **becker**: queues `to_coco` (256 bytes) and `from_coco` (1024 bytes).
  Table entries: 0x3F41 = `to_coco` empty ? 0x00 : 0x02; 0x3F42 = head
  of `to_coco` or 0xFF. The read hook on 0x3F42 pops one byte and
  rewrites both entries. `on_write` for idx 0x3F42 appends to
  `from_coco`; writes to 0x3F41 are ignored. Stream API for core0:
  `becker_read(buf, n)`, `becker_write(buf, n)`, `becker_rx_avail()`,
  `becker_tx_free()`. Stats: reads, writes, to_coco_underrun (read with
  nothing queued), from_coco_overrun. Loopback mode (milestone 0.7):
  core0 copies from_coco to to_coco.

The `to_coco` producer is core0 and the consumer is the core1 hook;
`from_coco` is filled only from core0's write-event dispatch. Both are
plain SPSC rings with no locks.

**Single writer rule (ruled 2026-09-07 during Plan A, Task 7):** only
core1 writes the two Becker table entries. Read hooks are registered on
both 0x3F41 and 0x3F42; the status hook refreshes the entries, the data
hook pops one byte then refreshes. Core0 (`becker_write`, the loopback
pump, `rom_load_mem`) only pushes and never touches those entries. A
byte pushed by core0 becomes visible on the CoCo's next $FF41 poll, one
poll of latency (about 10 µs). Two writers had a race where the status
could read "data ready" while the data entry still held 0xFF.

Precondition: the data hook pops a byte only when the status entry
already shows 0x02, so a $FF42 read that races a fresh push returns
0xFF without consuming the byte; DriveWire clients always poll $FF41
first, so this costs nothing in practice.

## 6. DriveWire server (dw/)

### 6.1 Interface

```
void dw_init(dw_server *s, const dw_store_ops *store, dw_send_fn send, void *ctx);
void dw_feed(dw_server *s, const uint8_t *buf, size_t n);   // bytes from the CoCo
void dw_tick(dw_server *s, uint32_t now_ms);                // timeouts
int  dw_mount(dw_server *s, int drive, const char *name);   // 0..3
void dw_eject(dw_server *s, int drive);
```

`send` is called with reply bytes. On the Pico, `send` is
`becker_write`; on the host it is `write(2)` to a socket.

### 6.2 Parser

States: `IDLE` (expect opcode), `PAYLOAD` (accumulate `need` bytes),
`READEX_WAIT_CKSUM`. Payload length is fixed per opcode except
NAMEOBJ (len byte), SERWRITEM (count byte), SERSETSTAT with code
$28 (+26). Any payload wait longer than 250 ms returns to `IDLE` and
increments `stats.timeouts`, matching pyDriveWire. Unknown opcode:
`stats.unknown_op++`, stay `IDLE`.

### 6.3 Opcodes

Byte layouts as extracted from pyDriveWire (`dwconstants.py`,
`dwserver.py`). LSN is 3 bytes big-endian. Checksum is the 16-bit sum of
the 256 data bytes, big-endian.

| Op | Request after opcode | Reply |
|---|---|---|
| $00 NOP, $54 TERM, $F8/$FE/$FF RESET | none | none; RESET also zeros `stats` and returns the parser to IDLE |
| $49 INIT | none | none; ejects nothing, resets the parser to IDLE |
| $5A DWINIT | client id | protocol version $04 (DW4 sends this; spec requires non-zero). If the client id is < $80 (NitrOS-9 ≤$3F, CoCoBoot $40-$4F, LWOS $60-$6F: real drive numbers), hdbdos mode is turned off, since the drive-by-LSN split is meaningless for those clients |
| $23 TIME | none | year-1900, month, day, hour, min, sec |
| $24 SETTIME (DW4) | year-1900, month, day, hour, min, sec | none; calibrates the clock (see §6.7) |
| $25 TIMER (DW4) | 1 byte (ignored) | now_ms, 4 bytes big-endian |
| $26 RESET_TIMER (DW4) | 1 byte (ignored) | none; no per-timer state is kept, so this is a no-op |
| $52 READ, $72 REREAD | drive, LSN3 | on success: rc 0, cksum2, data256. On error: rc only (1 byte) |
| $D2 READEX, $F2 REREADEX | drive, LSN3 | data256; then wait cksum2 from client; then rc. If cksum never arrives: no rc, back to IDLE |
| $57 WRITE, $77 REWRITE | drive, LSN3, data256, cksum2 | rc |
| $47 GETSTAT, $53 SETSTAT | drive, code | none |
| $43 SERREAD | none | $00 $00 |
| $44 SERGETSTAT, $C4 SERSETSTAT, $45 SERINIT, $C5 SERTERM, $C3 SERWRITE, $80..$8F FASTWRITE, $90..$9F FASTWRITE (DW4, window ports) | consumed per layout | none |
| $64 SERWRITEM | chan, count, data(count, 0 means 256) | none |
| $63 SERREADM | chan, count (0 means 256) | none; no vserial channels to read from, so nothing is sent back |
| $01/$02 NAMEOBJ, $03 NAMEOBJ_TYPE (DW4) | len, name | $00 (fail) |
| $50 PRINT, $46 PRINTFLUSH | consumed | none |
| $41 AARON, $E6 230K230K, $FD 230K115K (DW4) | none | none |
| $42 WIREBUG_MODE (DW4) | 23 bytes | none |

`$D6` OP_RFM is a deliberate stub: it opens a variable-length remote file
manager sub-protocol we don't implement, so it stays an unknown opcode.

Error codes: E_OK 0, E_CRC $F3, E_READ $F4, E_WRITE $F5, E_NOTRDY $F6,
E_WRPROT $F2, E_EOF $D3 (used internally by `dw_disk_read`/`dw_disk_write`,
never on the wire; see below). On READ of an unmounted drive: rc
E_NOTRDY and nothing else (the spec's Read Failure packet is byte 0 only).
On READEX of an unmounted drive: 256 zeros, then rc E_NOTRDY after the
client checksum. On WRITE: E_NOTRDY, E_CRC if the client checksum
mismatches (data not written), E_WRPROT for read-only mounts, E_WRITE on
storage failure.

### 6.4 HDB-DOS mode

Default on. Drive byte ignored; `drive = lsn / 630; lsn %= 630`. The
split is unconditional, matching pyDriveWire's `cmdRead`. DriveWire 4
style multi-disk images (one large file holding many 630-sector virtual
disks) are served with `dw hdbdos off`: the drive byte selects the
drive and the LSN addresses into the file; writes extend the file. `dw
hdbdos off` uses the drive byte directly for OS-9 use. `$5A DWINIT` can
also turn hdbdos off as a side effect (see the opcode table); the console
`dw hdbdos on|off` command always wins if issued afterward.

The spec defines no EOF-on-read: a READ/READEX/REREAD/REREADEX past the
end of the mounted image comes back as rc E_OK with 256 zero bytes, in
every mode (hdbdos on or off), matching the Swift and DW4 reference
servers. Only writes can extend an image; reads never see E_EOF on the
wire.

### 6.5 Disk images (dw_disk.c)

Detection order VDK, JVC, OS9, raw. `dw_disk_open(store, name, &disk)`
fills `{byte_offset, sector_size(256), max_lsn, read_only}`. `dw_disk_read
(disk, lsn, buf)` and `dw_disk_write` compute `byte_offset + lsn*256`
once, return E_EOF beyond `max_lsn` (except HDB-DOS mode grows on
write). VDK: magic "dk", header size u16 LE at offset 2; flags byte at offset 10,
bit 0 set makes the mount read-only regardless of the `--mount ,ro` flag
(DW4 honours this bit). JVC: header = size % 256 bytes; sector size code
honoured. OS9: LSN0 DD_TOT/DD_TKS/DD_SPT consistency check. Raw: size/256
sectors.

### 6.6 Storage ops (dw_store.h)

```
struct dw_store_ops {
    int (*open)(void *ctx, const char *name, bool write, dw_file *f);
    int (*read)(dw_file *f, uint32_t off, void *buf, uint32_t n);
    int (*write)(dw_file *f, uint32_t off, const void *buf, uint32_t n);
    int (*size)(dw_file *f, uint32_t *out);
    int (*sync)(dw_file *f);
    void (*close)(dw_file *f);
};
```

POSIX and FatFS implementations. Stats: read/write count and max
latency in microseconds, exposed by `dw stats`. Per-store latency
counters are deferred to Plan B, where flash latency is what matters.

### 6.7 Time

`dw_time_set(unix_seconds)` from the console; OP_TIME returns that plus
uptime delta. Boot default 2026-01-01 00:00:00 so HDB-DOS never sees a
zero date.

## 7. Storage on the Pico

Flash: 4 MB on Pico 2. Firmware in the first 1.5 MB, FAT12/16 volume in
the top 2.5 MB (`PICOCO_FS_OFFSET`, `PICOCO_FS_SIZE` in CMake). FatFS
`diskio` maps 512-byte sectors onto flash with 4 KB erase blocks
(read-modify-write). Format on first boot if no valid volume.

`fs export` unmounts FatFS, enables the USB MSC interface over the same
region, and blinks the LED. The Mac sees a removable drive. `fs import`
or a reboot ends export and remounts. FatFS and MSC are never active at
the same time.

Wear: no levelling. A DriveWire sector write costs one 4 KB erase.
Acceptable for saving programs; heavy write loads belong on SD. If this
proves wrong, `dw_store_ops` makes LittleFS a one-file swap plus a
console upload command.

`picoco.cfg` is a list of console commands (section 8) replayed at
boot; `save` writes it. Missing file means `mode=diag` (nothing driven
except 0xFF, console only).

## 8. Console (CDC1)

Line oriented, `\r` or `\n` terminated, `>` prompt, `ok`/`err <msg>`
replies so tools can script it. Commands and the bring-up step they
serve:

| Command | Step |
|---|---|
| `help`, `status`, `version` | |
| `smoke` (toggle every GPIO at 10 Hz until a key) | 0.2 |
| `halt on\|off` | 0.3 / step 8 |
| `trace dump [n]`, `trace freeze\|run` | 0.4 |
| `bus drive on\|off` (gate whether core1 ever drives D0-7; off is capture-only) | 0.4, 0.5 |
| `rom pattern\|load <file>\|off` | 0.5, 0.6 |
| `becker loop\|bridge\|native\|off` | 0.7, 0.8, native |
| `dw mount <n> <file> [ro]`, `dw eject <n>`, `dw hdbdos on\|off`, `dw stats`, `dw capture on\|off <file>` | native |
| `dw selftest` (mounts a flash image, runs READ/WRITE through `dw_feed` internally, checks the replies) | pre-0.1 bench test, no CoCo needed |
| `crash` (force a HardFault to exercise the crash record; `crash panic` forces a panic record instead) | pre-0.1 bench test, verified across reset |
| `fs ls`, `fs rm <file>`, `fs export`, `fs import`, `fs format` | |
| `time set <unix>`, `time` | |
| `log <module> off\|error\|info\|debug`, `log dump` | |
| `stats reset`, `save`, `reboot`, `bootsel` | |

Core0 main loop: `tud_task()`, drain write ring to devices, run the
active Becker consumer (loop / bridge pump to CDC0 / `dw_feed` +
`dw_tick`), drain the log ring to CDC1, console input, LED heartbeat
(1 Hz diag, 2 Hz native, solid in export, fast blink after a crash
record).

## 9. Debuggability

- **Host first.** All of `bus.c`, `dev/`, `dw/`, `console/`, `log.c`
  build on the Mac. Test build uses `-fsanitize=address,undefined -g`.
  `host/sim_bus.c` is the virtual CoCo: `sim_read(0x3F41)` returns the
  table byte and runs hooks exactly as core1 would.
- **Flight recorder.** Trace ring always on, `trace dump` prints the
  last n entries as `t_us addr rw data`; `tools/tracedump.py` renders
  symbolic addresses ($C000+idx or $FF40+low bits), and groups
  consecutive $FF41/$FF42 cycles into DriveWire transactions with the
  decoded opcode.
- **Counters.** Every module has a stats struct printed by `status` /
  `dw stats`. Overrun and underrun counters exist wherever a queue
  exists.
- **Capture and replay.** `dw capture on <file>` appends every byte
  `dw_feed` receives (and, tagged, every byte sent) to a file on
  storage. File format: chunks of `dir (0 = from CoCo, 1 = to CoCo),
  len_lo, len_hi, bytes`. `picoco-host --replay` feeds the dir-0 chunks
  into the server on the Mac and `test_replay` checks the dir-1 bytes
  match; `tests/fixtures/` keeps captures as regression tests.
- **Logging.** `LOG_E/I/D(module, fmt, ...)` writes a formatted line
  with a microsecond timestamp into a 4 KB ring; core0 drains it to
  CDC1. Full ring drops the line and bumps `log_dropped`. Compile-time
  floor `PICOCO_LOG_LEVEL`; runtime level per module. Never called from
  core1 or from inside `becker_*`.
- **Crash record.** `crash.c` installs a HardFault handler and a
  `panic` hook that store `{magic, reason, pc, lr, cfsr, mode, uptime}`
  into a `__uninitialized_ram` section, then trigger a watchdog reboot.
  On boot, `status` shows `last reset: <reason> pc=... mode=...`. The
  watchdog is enabled in every mode (8 s, fed from the main loop).
- **Isolation modes.** Each layer has a mode that does not depend on
  the layer above (rom pattern, becker loop, becker bridge). Future
  devices follow the same rule: a fixed-response or loopback mode
  first.
- **SWD.** Debug Probe on J_SWD with openocd + gdb for core0 and
  startup. Halting core1 under gdb stops bus service; use the flight
  recorder for timing questions.

## 10. Tests

All run via `ctest` on the host build; sanitizers on.

| File | Covers |
|---|---|
| `test_dw_disk.c` | detection of raw/JVC/VDK/OS9 synthetic images, byte offsets, max_lsn, EOF, sector size codes, HDB-DOS LSN split and growth |
| `test_dw_server.c` | every opcode's request/response byte-for-byte; READEX good/bad/missing checksum; WRITE bad checksum leaves data untouched; unmounted drive replies; payload timeout resets; unknown opcode; stats counters; time |
| `test_becker.c` | table entries follow queue state; hook pops exactly one byte; underrun/overrun counters; loopback |
| `test_bus.c` | table defaults, set_read_range bounds, write ring order and overrun, hook registration, trace ring wrap and freeze |
| `test_console.c` | parser tokens, each command's ok/err path against fakes, config load/save round trip, bad config lines ignored |
| `test_stack.c` | end to end through `sim_bus`: a scripted CoCo polls $FF41, reads $FF42, issues READEX and WRITE against a POSIX-backed image and verifies data, then loopback and bridge pumps |
| `test_replay` | every capture under `fixtures/` replays without error or timeout |

Integration: `picoco-host` on 65504 + `tools/dwtest.py` (mount, read
all sectors and compare with the file, write pattern and read back,
REREAD after forced bad checksum, unmounted drive). Documented manual
check: XRoar with `-becker` pointed at `picoco-host`, boot HDB-DOS,
`DIR`, `LOADM`. `dwtest.py` also runs against the Pico in bridge mode
(CDC0 via a socat bridge) and, once network exists, natively.

On target the pass checks in `docs/breadboard-plan.md` section 6 are
the acceptance tests, run from the console.

## 11. Order of work

1. Skeleton: CMake for both targets, `test.h`, blink main, ctest wired.
2. `dw_disk` + checksum + `dw_server` with tests; `picoco-host`;
   `dwtest.py`; XRoar boot on the Mac.
3. `bus.c`, `becker`, `rom`, `device`, `log`, `console`, `config` with
   tests and `sim_bus`; `test_stack`.
4. Pico side: `bus_core1`, USB (2x CDC), FatFS on flash + MSC export,
   crash record, watchdog. Verified on a bare Pico 2 with no CoCo:
   console, `fs export` from the Mac, and `dw selftest`, which mounts
   an image from flash and runs READ/WRITE through `dw_feed`
   internally, checking the replies.
5. Hardware: milestones 0.1 to native from the console, tagging each.
6. Separate specs later: SD, PIO/DMA engine, virtual serial + network,
   ideas-doc devices.

## 12. Changes to existing docs

When implemented, update `docs/firmware-architecture.md`: section 2
layout to section 3 here; section 3.1 core split (core1 is the bus,
core0 everything else, reversed from the doc); section 3.3/3.4 PIO/DMA
marked as the v2 engine; section 6 Becker filter uses the full 14-bit
index; section 7.2/7.3 replaced by sections 6 and 11 here. Also record
in CLAUDE.md: core1 must stay flash-free, and the console is the only
supported bring-up interface.
