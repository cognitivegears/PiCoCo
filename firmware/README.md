# PiCoCo firmware

Pico 2 <-> Tandy CoCo cartridge firmware. See
`docs/superpowers/specs/2026-09-07-firmware-design.md` for the design.

## Host build (Mac/Linux, for tests and `picoco-host`)

```
brew install cmake ninja
cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware
ninja -C build-host
ctest --test-dir build-host --output-on-failure
```

## `picoco-host`

A TCP-socket DriveWire server for bring-up/testing on the Mac, built as part
of the host build above.

```
./build-host/picoco-host [--dir DIR] [--port 65504] [--mount N=FILE[,ro]]... \
    [--hdbdos on|off] [--replay FILE]
```

- `--dir DIR`: directory disk images resolve from (default `.`).
- `--mount N=FILE[,ro]`: mount `FILE` (relative to `--dir`) as drive `N`;
  repeat for multiple drives. Add `,ro` to mount read-only.
- `--hdbdos on|off`: HDB-DOS drive-by-LSN addressing (default: whatever
  `dw_init` sets — currently on). The effective value is printed at startup.
- `--replay FILE`: replay a `dw capture` recording (see
  `tests/fixtures/README.md` for the chunk format) — feeds only the dir-0
  (rx) chunks back into the server, ignoring the dir-1 (tx) chunks it
  recorded, prints a stats summary, and exits; no socket is opened. Useful
  for replaying a captured session without XRoar/CoCo hardware attached. A
  malformed (truncated) capture file prints an error and exits 1.
- With no `--replay`, listens on `--port` (default 65504, the standard
  Becker port), serving one client at a time; disconnect and reconnect is
  fine. `Ctrl-C` (SIGINT) prints the stats summary and exits.

### Host integration test

`firmware/tools/dwtest.py` is a stdlib-only DriveWire client that exercises
`picoco-host` end to end: DWINIT, a full-image READEX sector compare, a READ
checksum check, a corrupted READEX/WRITE (expect `E_CRC`), an unmounted-drive
read (expect `E_NOTRDY`), a scratch-drive WRITE/READEX round trip, and TIME.

```
mkdir -p /tmp/pc
python3 -c "open('/tmp/pc/t.dsk','wb').write(bytes([(i>>8)&255 for i in range(161280)]))"
cp /tmp/pc/t.dsk /tmp/pc/s.dsk
./build-host/picoco-host --dir /tmp/pc --mount 0=t.dsk --mount 1=s.dsk --hdbdos off &
python3 firmware/tools/dwtest.py --image /tmp/pc/t.dsk --drive 0 --scratch s.dsk
kill %1
```

Expected: every check prints `ok`, dwtest.py exits 0. Repeat with
`--hdbdos on` on both the server and `dwtest.py --hdbdos` for the second
addressing mode.

A new connection replaces a client already held (log line
`picoco-host: client replaced`), so a rebooted Pico is not blocked by its old
dead socket. The server listens on all interfaces, so any LAN peer can evict
the current client; it is a dev tool.

Note: on macOS under a sandboxed shell, binding the listening socket can
fail with `Operation not permitted`; run the server (and the client) with
the sandbox disabled.

### Manual check: XRoar

Not automated — requires a real HDB-DOS DriveWire ROM, which the user
supplies at `firmware/roms/` (not checked in). With `picoco-host` running
and a drive mounted on 0:

```
xroar -machine coco3 -cart becker -becker-ip 127.0.0.1 -becker-port 65504 \
    -cart-rom firmware/roms/hdbdos_dw.rom
```

(Flags per XRoar's manual for the Becker-port cartridge.) Expected: booting
into HDB-DOS and running `DIR` lists the files on the image mounted on
drive 0.

### `tools/tracedump.py`

Decodes `trace dump [n]` console output (lines of `t_us idx R|W data`) into
a readable log with CoCo addresses and a running DriveWire opcode decoder:

```
python3 firmware/tools/tracedump.py trace.txt      # or pipe via stdin
```

Each line becomes `t_us  $ADDR  R/W  data  note`, where `$ADDR = 0xC000 +
idx`. Notes: `ROM` for ROM-space reads, `BECKER_STATUS`/`BECKER_DATA` for
Becker-port accesses (status reads add `data avail`/`no data`), and for a
DriveWire request written to `$FF42`, the opcode name plus, once its 4
drive/LSN payload bytes arrive, `drive=N lsn=N` on the completing line.
Non-matching lines (e.g. the console's `ok`/`err ...` replies mixed into the
same log) are skipped.

## Pico build

Cross-builds `build-pico/picoco.uf2` for the Pico 2 (RP2350A) using the Pico
SDK, cloned outside the repo:

```
cd /Users/cognitivegears/projects
git clone --branch 2.3.1 --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/raspberrypi/pico-sdk.git

cmake -B build-pico -G Ninja -DPICO_SDK_PATH=/Users/cognitivegears/projects/pico-sdk firmware
ninja -C build-pico
```

For a Waveshare RP2350B-Plus-W build instead, add `-DPICOCO_BOARD=plusw`
(default is `pico2`); this selects `firmware/boards/plusw.h` (pin map) and
the SDK board `picoco_plusw` (RP2350B, 48 GPIO, 16 MB flash):

```
cmake -B build-pico-plusw -G Ninja -DPICO_SDK_PATH=/Users/cognitivegears/projects/pico-sdk \
    -DPICOCO_BOARD=plusw firmware
ninja -C build-pico-plusw
```

Both builds run `firmware/tools/check_core1_flash_free.py` as a POST_BUILD
step, failing the build if any code core1's bus loop can reach (directly, or
through a Becker read hook) lands outside SRAM.

Flash a Pico 2 that's in BOOTSEL mode (hold BOOTSEL while plugging it in):

```
firmware/tools/flash.sh build-pico/picoco.uf2
```

The console appears as the second of two `/dev/tty.usbmodem*` ports; use
`screen /dev/tty.usbmodemXXXX2` or `picocom` to talk to it.

### Console checks

`firmware/tools/pconsole.py` is a stdlib-only serial console driver: it opens
the console port raw at 115200, sends each command, and prints replies until
a line reading `ok` or starting with `err` (or a 6 s timeout). Use the
`/dev/cu.usbmodem*` device, not `/dev/tty.usbmodem*` — opening the `tty`
node non-interactively blocks waiting for carrier detect on macOS.

```
python3 firmware/tools/pconsole.py /dev/cu.usbmodemXXXX2 \
    'fs format' 'fs ls' 'dw selftest'
```

Bus-engine-specific commands: `bus selftest` runs the on-chip fake 6809
self-test (see "Self-test without a CoCo" below); `cart on|off|auto`
sets the firmware `/CART` pulse policy (`auto`, the default, pulses for
500 ms after `/HALT` release when a loaded ROM is not DK-signature DOS;
Plus-W only today — a Pico 2 build has no `PIN_CART_DRV`, so `cart on`
and `cart off` are refused there with `err cart: needs Plus-W (JP5 on
a Pico 2)`; `auto` and the bare query still work on both boards);
`rom load <file>` accepts a plain
8 KB/16 KB image or a banked Games Master Cartridge image (32 KB, 64 KB
or 128 KB, switched by writes to `$FF40`), refusing any other size with
`err rom load: size must be 8K, 16K, or banked 32K/64K/128K`.

#### Self-test without a CoCo

`bus selftest` drives the real core1 bus loop from an on-chip PIO1 state
machine at 6809 timing — no CoCo needed, and safe to run on an unplugged
board (see `docs/firmware-architecture.md` §3.2.5 for why). **Unplug the
Pico from the CoCo cart edge first; the guard below is a backstop, not
a licence to leave it plugged in.** Before touching any pins the command
refuses (`-2`, "bus is live") if either: `bus_stats.cycles` moves during
a 100 ms sleep (a running CoCo driving cart cycles), or the address/`R/W`
pins (plus `OE_BUS` on a Pico 2, or `E`/`Q` on a Plus-W) aren't perfectly
still for a 10 ms, 50 µs-interval sample — an idle CoCo running from RAM
produces no cart cycles at all, but those pins still toggle with the CPU
clock through always-enabled buffers, where an unplugged board just sits
on its pull-ups.

```
python3 firmware/tools/bench.py [--port /dev/cu.usbmodemXXXX2] [--skip-if-absent]
```

`bench.py` runs `version` then `bus selftest` over the console (auto-
detecting the second `/dev/cu.usbmodem*`/`/dev/ttyACM*` node if
`--port` is omitted), prints every line, and exits 0 on `selftest pass`,
1 on any failure or timeout, or 77 with `--skip-if-absent` when no Pico
console is found at all. It is also registered as the `bench` ctest
target, but only opt-in — configure with `-DPICOCO_BENCH=ON` to add it,
since it drives real bus pins and must not run against a plugged-in
board by accident. With `PICOCO_BENCH` off (the default), `ctest
--test-dir build-host` doesn't know about it at all. The self-test
clears whatever ROM was loaded (`rom off`) as part of running, so
`bench.py` sends `reboot` at the end to replay a saved `rom load` from
`picoco.cfg`.

### Filesystem

The on-flash FAT filesystem (`fs_flash.c`/`dw_store_fatfs.c`) lives at
`PICOCO_FS_OFFSET..` of flash, sized by the board header
(`firmware/boards/*.h`): `0x180000..0x3FFFFF` on a Pico 2 (4 MB flash, 640
FAT12 clusters), `0x180000..0xFFFFFF` on a Plus-W (16 MB flash, 3712
clusters). Reset safety across a CoCo `/RESET` (the Pico reboots on every
one, at any instant — see `docs/ADDITIONAL_ROADMAP.md` §6 item 1) comes
from `dw_server.c` syncing after every successful DriveWire write, and
`fs_flash_write_blocks` skipping the erase/program cycle for any 4 KB block
that didn't actually change (the common case for a resync of the root dir
or FAT). `picoco.cfg` at the root holds the saved console config (see
`dw save`/`console_run_config`); DriveWire disk images and `dw capture`
recordings also live at the root.

### Export

The on-flash FAT volume can be exported to the Mac (or any USB host) as a
removable drive, e.g. to drag `.dsk` images on/off without a DriveWire
transfer:

1. `dw eject 0`..`dw eject 3` (or whichever drives are mounted) — `fs export`
   refuses with `err eject all drives first` while any DriveWire drive is
   mounted, since FatFS can't be unmounted out from under an open `FIL`.
2. `fs export` — unmounts FatFS and presents the partition over USB MSC as a
   volume named `PICOCO`; the board's LED goes solid while exporting.
3. Drag disk images onto the `PICOCO` volume in Finder, then eject it there
   (or `diskutil eject /Volumes/PICOCO`).
4. `fs import` (or `reboot`) re-mounts FatFS for DriveWire/console use.

macOS writes `.fseventsd/` and `._*` AppleDouble files to the volume; `fs ls`
hides any name starting with `.`, so these don't show up and are harmless.

## Console commands added in 1.2

DW4-compatible disk commands (used by NitrOS-9's `dw` utility and the
`coco/` manager program equally):

- `dw disk show [n]`: with no argument, lists every mounted drive in DW4's
  wire format; with `n`, shows just that drive's file.
- `dw disk insert <n> <file>`: same as `dw mount n file`, but the file
  name is the rest of the line (spaces allowed) and an occupied drive is
  ejected first.
- `dw disk eject <n>`: ejects drive `n`.

`fs new <name>`: creates a blank, formatted 35-track (161,280-byte)
RS-DOS image, byte-for-byte the same as ToolShed's `decb dskini` (all
`$FF` except track 17 sectors 1 and 2, as DSKINI leaves them). Refuses an
existing name, a name 32 characters or longer, a name that fails
`dw_disk_name_ok` (empty, a control byte, a byte $80 and above, or `/`,
`\`, `:`), and `picoco.cfg` (see below).

`rom boot <file>`: records the ROM to load on the *next* boot only (no
live swap under a running DOS); checks the file exists and is 8192 or
16384 bytes. `status` now shows both `rom now` (what's currently driving
`/CTS`) and `rom next` (what `rom boot` + `save` will load at the next
reset).

`time` now also prints `clock kept|lost`, showing whether the RP2350's
always-on timer carried the clock across the last `/RESET` (see
`docs/superpowers/specs/2026-09-23-coco-manager-design.md` §4.5).

`picoco.cfg` can never be mounted (`dw mount`/`dw disk insert`) or
created (`fs new`) as a disk image, on the console or over DriveWire: it
replays at boot with full USB privilege (`fs format`, `smoke`, `becker
off`, ...), so letting a remote `WRITE` rewrite it would hand that
privilege to whatever wrote the disk. The guard (`dw_disk_name_ok` in
`firmware/src/dw/dw_disk.c`) refuses the name outright rather than
matching only a `picoco.cfg` path component: FatFS's `create_name` ends a
path at any byte below `$20` and silently drops a trailing separator, so
a name like `picoco.cfg\` or `picoco.cfg` followed by a control byte
would still open `picoco.cfg` on the Pico even though a component-based
check sees something else (or nothing) as the last component. Bytes $80
and above are refused too: FatFS's CP437 short-name upcase table (`ff.c`
`TBL_CT437`) folds several of them onto plain ASCII letters (`$A2` folds
to `O`), so `pic\xA2co.cfg` would otherwise open `picoco.cfg` on the Pico
without matching it byte-for-byte. So the guard refuses any name with a
byte below `$20`, `$7F`, or $80 and above, or a `/`, `\`, `:` at all, in
addition to matching `picoco.cfg` itself.

### WiFi (Plus-W)

Firmware 1.3. Plus-W builds only (`PICOCO_HAVE_NET`); a Pico 2 has no radio
and every `net` command that needs one answers `net: needs Plus-W`. The
CoCo reaches a DriveWire server (DriveWire 4, `picoco-host`, FujiNet with
BoIP) over one TCP connection, in place of USB.

Radio pins (verified 2026-09-29 from the Waveshare schematic and a live
scan): WL_ON GP36, WL_D/HOST_WAKE GP37, WL_CS GP38, WL_CLK GP39, LED on
WL_GPIO0, VBUS on WL_GPIO2, VSYS sense GP46.

| Command | Effect |
|---|---|
| `net [status]` | State, SSID, `net psk set\|unset` (never the passphrase), server, IP, bytes, retries, `net radio yes\|no`, `net mode <running> boot <next>`. |
| `net join <ssid>` | Store the SSID (spaces allowed). |
| `net psk [<psk>]` | Store the passphrase; empty clears it (open network). |
| `net server <host> [port]` | Server; port 1-65535, default 65504. DNS names work, `.local` names do not (no mDNS). |
| `net forget` | Clear all settings and stop the radio. |
| `net scan` | One `ssid <name> rssi <n> chan <n>` line per network. Blocks up to 5 s. |
| `net mode net\|native` | Sets the NEXT-BOOT mode only; `save` writes it. |
| `becker net` | Console-only live switch. Refused with `net: needs Plus-W` or `net: set ssid and server first`. |

`save` writes `net join`, `net psk` (if set) and `net server` before the
`becker` line, so replay has them first. Example `picoco.cfg`:

```
net join MyLan
net psk hunter2hunter2
net server 192.168.1.20 65504
becker net
```

Credentials are plain text in `picoco.cfg`. The config-replay error log
redacts `net psk` lines. The remote allowlist has the bare `net` verb.

**Boot.** With `becker net` saved, /HALT stays held until the socket is up
or 10 s pass. Then the board falls back to native mode
(`net failed (<reason>), native fallback`); the saved config still says net
and retries continue every 2 s. `becker net` re-arms after a fallback. After a fallback the link keeps
retrying in the background; `net status` can show `up` while the board is
running native, and `becker net` switches over immediately. A saved
`becker net` on firmware 1.2 is a usage error there and leaves Becker off,
so clear it before downgrading.
Reasons: `no such network`, `bad password`, `dhcp timeout`, `dns failed`,
`refused`, `link lost`, `not configured`, `join rejected`. Rings are cleared on every
reconnect. After joining, WiFi power save is off. Once per boot, on link-up,
SNTP seeds the clock only if none is running (`clock lost`); it never
overwrites a `time set` (SNTP is UTC, `time set` is local).

**Latency rule.** The server must answer within about 200 ms round trip.
Servers abandon a half-finished op after 250 ms (`picoco-host`) or 200 ms
(DW4 `ReadByteWait`). A link drop mid-transfer leaves the CoCo waiting
until reset (accepted limitation; reset reboots the Pico, which reconnects).

**`bus selftest net`** (Plus-W, needs `becker net` up, no CoCo attached):
pushes DWINIT (2 bytes, `5A FF`) and 20 OP_TIME requests through the fake
6809, core1, the Becker ring, the socket and back. Prints
`selftest net dwinit -> <byte> in <n> ms`,
`selftest net time (yr-1900) Y-M-D h:m:s in <n> ms` and
`selftest net time worst <n> ms over 20`; fails if the worst case exceeds
200 ms.

### DriveWire virtual-serial command channel

`firmware/src/dw/dw_vser.c` implements DW4's virtual-serial command mode
(channels 1-13; one session at a time) so any DW4 client — NitrOS-9's
`dw`, DwTerm, or the `coco/` manager program — can run console commands
over the same Becker/DriveWire link used for disk I/O, framed exactly as
DW4 Java 4.3.3p (`OK command successful` / `FAIL nnn <msg>`, both
terminated `\n\r`). Reopening a channel after a reply was already queued
starts a clean session rather than piling onto the stale one.

Remote commands go through a deny-by-default allowlist
(`console_exec_remote` in `firmware/src/console/console.c`); only these
run over DriveWire:

    status version help fs ls fs new dw mount dw eject dw hdbdos
    dw disk rom boot time save

Everything else (`smoke`, `halt`, `bus`, `becker`, `fs
format|rm|export|import`, `rom load|pattern|off`, `trace`, `crash`,
`log`, `stats`, `dw capture|selftest|stats`, `reboot`, `bootsel`, ...)
answers `FAIL 255 console only` — a new console command is USB-only
until someone adds it to the allowlist.

Firmware version is 1.2.

## Bring-up

Breadboard milestones from `docs/breadboard-plan.md` section 6, mapped to
console commands. Before anything else: on macOS, approve the Pico once
under System Settings -> Privacy & Security -> "Allow accessories to
connect", or it enumerates over USB but exposes no serial ports. The
console is the second `/dev/cu.usbmodem*` device, the Becker bridge port is
the first. To reflash: send `bootsel` on the console (see `flash.sh` above,
or `python3 firmware/tools/pconsole.py /dev/cu.usbmodemXXXX2 bootsel`), wait
for `/Volumes/RP2350` to mount, then `cp -X build-pico/picoco.uf2
/Volumes/RP2350/`.

Run commands with `pconsole.py` as shown in "Console checks" above. `fs
export`/`fs import` are the "Export" steps above; use them to move ROM
images and disk images on and off the board.

| Step | Console | Expected console output | Expected CoCo-side result | Tag |
|---|---|---|---|---|
| 3 | `status` | `bus drive off`, `last reset power-on` | Pico powered from the CoCo rail: LED blinks 1 Hz. USB power and CoCo 5 V share only GND on the breadboard (`VBUS` is NC on the final board): don't back-power the CoCo from USB, use a cable with VBUS cut, or accept USB power during console sessions. | fw-0.1-blink |
| 4 | `bus drive off`, `trace run`, on the CoCo `PEEK(&HC123)`, then `trace dump 8` | dump includes a line `<t_us> 0123 R ff` (idx = $C123 - $C000); `status` bus reads count goes up by the number of PEEKs | `PEEK(&HFF41)` triggers a trace line ending `3f41 R`; the Pico still drives nothing back at the CoCo | fw-0.4-bus-capture |
| 5 | (hardware only, no console) | - | LA: OE_BUS low only during the E-high half of cart cycles, never otherwise | - |
| 6 | `rom pattern`, `bus drive on`, `save` | `ok` for each | `PEEK(&HC000)` = 0, `PEEK(&HC001)` = 1, `FOR I=0 TO 255: PRINT PEEK(&HC000+I);: NEXT` counts up. Slowest CoCo first; on CoCo 3 repeat after `POKE 65497,0`. | fw-0.5-rom-static |
| 7 | `fs export`, drag `hdbdos_dw.rom` (8 KB) onto `PICOCO`, `fs import`, `rom load hdbdos_dw.rom`, `save` | `usb drive exported...`, then `ok` for import/load/save | Power-cycle: CoCo autostarts HDB-DOS (or `DOS` enters it); `DIR` fails cleanly (no server yet) | fw-0.6-rom-hdbdos |
| 8 | `log main debug`; after a reboot, `log dump` (once the console reconnects) | `log dump` shows `core1 up, halt released` | LA on /RESET and $C000: compare Pico cold-boot time to that log line against CoCo reset to first $C000 read; decide Q2/R7/R8 per breadboard-plan section 2.3 | fw-0.3-halt-ctrl |
| 9 | `becker loop`, `bus drive on` | `ok` | `POKE &HFF42,65: PRINT PEEK(&HFF41), PEEK(&HFF41), PEEK(&HFF42)` -> `0 2 65`, and `trace dump` shows `3f42 W 41` (the first `PEEK(&HFF41)` always returns the pre-POKE status: core1 refreshes the table only after a read, single-writer rule; the `W` byte is the bit-order check, a swapped pair still echoes 65) | fw-0.7-becker-loop |
| 10 | *(deferred, see docs/ADDITIONAL_ROADMAP.md §5)* `becker bridge`; host: `pyDriveWire --port /dev/tty.usbmodemXXXX1 --speed 115200 <image>` (the first `/dev/cu.usbmodem*`, the bridge port) | `ok` | `DIR` in HDB-DOS lists the image; `LOADM` a small program | fw-0.8-bridge |
| 11 | `fs export`, copy a DSK onto `PICOCO`, `fs import`, `dw mount 0 <dsk>`, `becker native`, `save` | `ok` for each; `dw stats` afterward | `DIR`, `LOADM`, `SAVE` a program, power-cycle, `DIR` still shows it; `dw stats` shows reads/writes with `crc_err 0` and `timeouts 0` | fw-1.0-native |

Three tools to reach for when a step doesn't pass:

- `trace dump [n]` piped through `tools/tracedump.py` (see above): the ring
  freezes by itself at the first Becker underrun or DriveWire CRC error, with
  that cycle as its last entry (on a Pico 2 build the faulting read is the
  second-to-last entry: the read hooks run after the cycle is recorded);
  `trace run` thaws it. Reach for
  this when a CoCo-side PEEK/POKE doesn't show the address or data you
  expect, mainly steps 4-7.
- `dw capture on <file>` (see "dw capture" in the console commands) plus
  `picoco-host --replay` (see "picoco-host" above): reach for this when the
  bridge or native DriveWire session (steps 10-11) is flaky and you want to
  replay the exact byte stream off the CoCo for debugging.
- `status`, and `dw stats`/`stats reset`: reach for these for a quick health
  check at any step, especially the halt-timing decision in step 8 and the
  error counters in step 11.

## Licensing

Firmware code is under the top-level project license. Vendored third-party
code: `third_party/fatfs/` is FatFs R0.15a by ChaN, under the FatFs license
(BSD-style; see `third_party/fatfs/LICENSE.txt`). The Pico build also links
the Raspberry Pi Pico SDK and TinyUSB, each under their own upstream
licenses (BSD-3-Clause and MIT respectively).
