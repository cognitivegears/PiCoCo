# PiCoCo ROM manager: thin-client manager for every CoCo

Status: draft for review, 2026-10-01. Brainstormed with the user.
Supersedes the CoCo-side half of `2026-09-23-coco-manager-design.md` once
it ships; that spec's firmware half (virtual serial, console allowlist)
stays as it is.

## 1. Goals

1. One manager that is the board's front door on every CoCo, from a 16K
   CoCo 2 with plain Color BASIC to a CoCo 3. Prompted by the 2026-10-01
   bench CoCo 2 (26-3026, 16K, no Extended BASIC), which cannot start
   HDB-DOS and so cannot load today's `PICOCO.BIN`.
2. The 16K machine must not cap the manager. Today's `PICOCO.BIN` sits at
   its `$7B80` ceiling with no headroom on a 32K machine.
3. The board boots its saved default (HDB-DOS, a game ROM, or the
   manager). The manager comes up on request.
4. First version: launch ROM images, mount and create disk images, boot a
   disk (NitrOS-9 or an RS-DOS program), settings, WiFi, save.
   `PICOCO.BIN` is retired when it ships.

Non-goals for v1 are in section 10.

## 2. Decisions

| Topic | Decision | Why |
|---|---|---|
| Shape | Thin client: screens and logic in the firmware, a small stub ROM on the CoCo | Only shape where 16K and the full manager do not compete for space; UI becomes host-testable C |
| Rejected | One flat 16K ROM with runtime gating | 15.2 KB of 16.1 KB before launch code; ceiling stays |
| Rejected | Banked ROM, a module per bank | CMOC has no banking; data still has to fit 16K RAM |
| Role | One ROM for every machine | User's pick |
| Power-on | Saved default; manager on request | Keeps today's HDB-DOS boot time and RESET behaviour |
| No-ECB launch scope | ROM images only | Loading programs without a DOS is a later module |
| Boot from disk | In v1 | Needed to retire `PICOCO.BIN` |
| Ways in | `DK` autostart, BASIC loader, `EXEC 49154`, double RESET | Double RESET is the only one that works once a ROM owns the machine |
| Transport | Becker port, in a UI session the firmware handles itself | Must work in bridge and net modes, where DriveWire bytes go to an external server |

## 3. Architecture

```
CoCo                                   Pico (core0)
stub ROM (manager.rom, asm)            ui module: screens, 32x16 model, diff
  screen writes, key scan   <--------  action list (draw / launch / ...)
  detect RAM, ECB, DOS, CoCo 3 ----->  capabilities, last key
  actions run from RAM                 firmware APIs: fs, mounts, rom, net,
                                       time, save
```

core1 and the bus loop are not changed. Everything new on the Pico runs on
core0, and reaches the CoCo through the existing Becker rings.

## 4. The stub (`coco/stub/`, new; `manager.rom`, 8 KB image)

6809 assembly, built with lwasm, target about 1.5 KB of code.

### 4.1 Rules

- Self-contained console. It writes the text screen at `$0400-$05FF`
  directly. Keys come from `POLCAT` (`JSR [$A000]`), the one BASIC ROM
  routine it calls: it is not reached through a RAM hook, so a swapped-out
  DOS ROM cannot break it, and it handles SHIFT+0 lowercase itself. No
  other BASIC routine is used.
- Interrupts masked (`ORCC #$50`) for the whole session. Timeouts are
  counted loops (as in `coco/carttest.asm`), not the Extended BASIC timer.
- Working storage: a few bytes in the cassette buffer (`$01DA`) plus the
  stack it was entered with. Nothing depends on RAM size.
- Any step that swaps the ROM runs from a routine copied to RAM.
- Never `CLR` the Becker data port (a `CLR` reads first and eats a byte).

### 4.2 Entry

- Image starts with `DK`; code starts at `$C002`. Extended BASIC calls
  `$C002` on a cold start when the manager is the loaded ROM.
- No Extended BASIC: `EXEC 49154`.
- From BASIC under a DOS: the loader (section 7.2) jumps to `$C002`.
- The stub records which way it came in (a flag the loader sets in a
  register; cold start otherwise), because leaving differs (section 7.3).

### 4.3 Detection, reported once at session start

| Item | How |
|---|---|
| RAM | Non-destructive write test at `$7FFF` (32K) and, in all-RAM map, 64K |
| Extended BASIC | `EX` at `$8000` |
| Disk BASIC live | Entry flag from the loader (a DOS ROM cannot be probed once it is swapped out) |
| CoCo 3 | `PEEK($FFFE) = $8C`, as `PICOCO.BAS` does today |

On a CoCo 3 the stub forces the 32-column compatibility screen first.

### 4.4 Actions

| Code | Action | Notes |
|---|---|---|
| `01` | Write text: offset (2), length (1), bytes | Bytes are screen codes, firmware does the mapping |
| `02` | Clear screen | |
| `03` | Cursor at offset (2), or off | Stub blinks it while waiting for a key |
| `10` | Jump to `$C000` | After a ROM swap; Program Paks |
| `11` | Cold restart | After a ROM swap; clears the warm-start flag, jumps through the reset vector |
| `12` | Continue boot | After a ROM swap, power-on entry only: `JMP $C002` of the new `DK` ROM |
| `13` | Return to BASIC | Loader entry only |
| `14` | Return to BASIC and type a line: length (1), text | Loader entry only; RVEC4 hook from `coco/hook.asm`, copied to RAM |
| `15` | Load sectors and jump: drive (1), LSN (3), count (1), load address (2), jump address (2) | NitrOS-9 boot track; plain DriveWire OP_READ |
| `00` | End of list | Stub goes back to reading the keyboard |

Before carrying out any of `10`-`15` the stub ends the UI session (section
5.1), so the Becker port is back with the DriveWire server, bridge or net
relay: action `15` reads its sectors from wherever the disk is mounted.
Actions `10`-`15` are preceded by the firmware's "ROM is swapped" byte
when a swap is part of them. No byte within the timeout: the stub stays in
the manager and the next poll reports the failure.

## 5. Protocol

### 5.1 UI session

In `becker bridge` and `becker net` the firmware relays Becker bytes to an
external DriveWire server, so the manager cannot be a DriveWire operation.
Instead:

- New write-only control register at `$FF43` (free in the
  `RP2350B_IDEAS.md` section 12.1 map). It needs no core1 hook: writes in
  `$FF40-$FF5F` already reach the Becker device on core0 through the write
  ring, in order with the `$FF42` bytes around them.
- Write `$A5`: begin a UI session. Write `$5A`: end it.
- While the flag is set, `mode_pump` hands the Becker rings to the `ui`
  module instead of the DriveWire server, the bridge or the net relay.
  Both rings are flushed at begin and end.
- A session also ends on a Pico reboot. A stub left without a session gets
  no reply, times out and shows `PICOCO NOT RESPONDING`, `R=RETRY`.

### 5.2 Exchange

The CoCo always asks.

```
stub -> pico : 'P' flags key sum8
pico -> stub : len_hi len_lo  <actions...>  sum_hi sum_lo
```

- `flags` bit 0: resend the previous reply. Bit 1: first poll of a session,
  followed by one capabilities byte (bit 0 32K, 1 64K, 2 ECB, 3 Disk BASIC
  live, 4 CoCo 3, 5 entered at power-on).
- `key`: 0 for none, else the code `POLCAT` returned.
- The reply checksum is the 16-bit byte sum, as DriveWire uses. On a
  mismatch or a timeout the stub polls again with the resend flag and the
  firmware repeats the stored reply. Replies are capped at 600 bytes (a
  full screen plus headers).
- The firmware keeps the model of what the CoCo acknowledged (a poll
  without the resend flag acknowledges the previous reply) and sends only
  changed runs.

## 6. Firmware

### 6.1 `firmware/src/ui/` (new, core0, host-buildable)

| File | Does |
|---|---|
| `ui.c` / `ui.h` | `ui_poll(key, flags, caps, out, cap)`: runs the current screen, returns the action list. Owns the 32x16 model and the diff |
| `ui_screens.c` | ROMs, Disks, Boot, Settings, WiFi. Each is a draw function and a key function |
| `ui_text.c` | Line editor (two rows, about 57 characters), lowercase as `POLCAT` reports it, ASCII to screen-code mapping |

The screens call firmware functions directly (store listing, `dw_disk`
mounts, `rom_load_file`, `net_*`, `plat` time, the `save` writer in
`console.c`, which moves behind a function both can call). No reply
parsing.

### 6.2 Screens

Keys as today (`coco/README.md`): plain letters, arrows, SHIFT+arrows to
page, `G`+letter to jump, BREAK back.

| Screen | Does | Shown when |
|---|---|---|
| ROMs | List `.ROM`; ENTER launches, `D` sets the saved default | Always |
| Disks | List images; `0`-`3` mount, `E` eject, `N` new | Always |
| Boot (`B` on a disk) | Mounts it in drive 0, reads LSN 612. `OS`: action `15`. Else lists `.BAS`/`.BIN` from the directory (track 17), hands the pick to BASIC with action `14` | NitrOS-9 needs 64K; RS-DOS needs Disk BASIC |
| Settings | HDB-DOS mode, clock, timezone, save | Always |
| WiFi | Scan, PSK, server, next-boot mode | Radio present; else one line |

### 6.3 Capability rules (one function, `ui_can`)

- `DK` ROM without Extended BASIC: `NEEDS EXTENDED BASIC`.
- File that is not 8K, 16K or a banked size: refused with the firmware's
  existing message.
- RS-DOS program without Disk BASIC live: on a power-on entry with ECB the
  manager offers to start the DOS (action `12`) and says to bring the
  manager up again; without ECB it is refused. (If section 8's emulator
  check shows the typed line survives the DOS start-up, this becomes a
  direct launch.)
- NitrOS-9 boot without 64K: `NEEDS 64K`.

### 6.4 Launch is one-shot

Launching swaps the ROM for this session only (`rom load`). `D` sets the
next-boot default (`rom boot`) and marks the config unsaved. A power cycle
therefore returns to the saved default.

### 6.5 Other firmware changes

- **Built-in manager image.** `manager.rom` is linked into the firmware.
  `rom load manager` (no extension) selects it; a file of that name on the
  flash filesystem is not needed. With no ROM in `picoco.cfg`, the
  firmware loads it.
- **Previous ROM.** The firmware remembers what was loaded before the
  manager so BREAK can put it back.
- **Double RESET.** A marker in `__uninitialized_ram` (as `crash.c`
  already uses) set at the very start of `main`, cleared after
  `PICOCO_DOUBLE_RESET_MS` (3000) of uptime. Found set at boot: load the
  manager for this boot only, leave `picoco.cfg` alone, log it. The window
  is a named constant to tune on the bench.
- **Control register.** `$FF43`, handled in the Becker device's core0
  write path. core1 is unchanged.
- **`rom off` is a saved choice.** It is written to `picoco.cfg`; only a
  config with no `rom` line at all falls back to the manager.

## 7. Getting in and out

### 7.1 In

| Situation | How |
|---|---|
| ECB, manager is the default | `DK` autostart |
| ECB, another default, DOS running | `DRIVE 3:RUN"PICOCO"` (loader) |
| No ECB | Manager is the default; `EXEC 49154` |
| Blank config | Firmware falls back to the manager |
| Any ROM owns the machine | RESET twice within 3 s. On a machine without ECB, then `EXEC 49154` |

### 7.2 Loader

`PICOCO.BIN` shrinks to a few dozen bytes, loaded where today's
`PICOCO.BAS` loader puts it and clear of the RAM the action `14` hook
uses. It masks
interrupts, writes `$A5` to `$FF43`, sends a "load the manager" poll,
waits for the swap byte and jumps to `$C002` with the loader flag set. It
runs from RAM: on a CoCo 1/2 Disk BASIC executes from the cart and cannot
survive its ROM being swapped.

### 7.3 Out

| Choice | Action |
|---|---|
| Program Pak | swap, `10` |
| `DK` ROM | swap, `12` if entered at power-on, else `11` |
| NitrOS-9 disk | `15` |
| RS-DOS program | swap the DOS back, `14` |
| BREAK at the top screen | loader entry: swap back, `13`. Power-on entry: start the saved default (or stay, if the manager is the default) |

Unsaved changes at BREAK: offer save-then-exit or stay, as today.

## 8. Checks to settle in the emulator before relying on them

1. Cold restart on a CoCo 3 from the stub (ROM mode has to be restored
   before the reset vector is used).
2. Forcing the 32-column screen from a 40/80-column session.
3. Whether a line typed through the RVEC4 hook survives the DOS start-up
   after action `12` (section 6.3).
4. `$4B` is not executed anywhere: all entries are at `$C002`.

And on the bench:

5. The `__uninitialized_ram` marker survives a CoCo RESET (the firmware
   reports that reset as power-on today). Fallback: a POWMAN scratch
   register in the always-on domain the clock already uses.
6. The double-RESET window by feel.

## 9. Testing

- **Host (`firmware/tests/test_ui.c`, new suite):** drive `ui_poll` with
  key sequences and capability sets; assert the model text and the action
  list. Covers each screen, every `ui_can` refusal, the diff (only changed
  runs sent), the resend path, the 600-byte cap, and the line editor.
- **Emulator:** stub against `picoco-host` in XRoar, `coco2bus
  -no-extbas`, `coco2bus`, `coco3`, checked with `coco/tools/xrscreen.py`.
  `picoco-host` gains the UI session (it already links the firmware's
  DriveWire code).
- **Bench (`firmware/TEST_PLAN.md`, new section J):** 16K CoCo 2: launch
  `carttest.rom` from the manager, settings, save, double RESET. CoCo 3:
  enter from HDB-DOS, mount, run DINORUN from disk, boot NitrOS-9, WiFi
  screen on the Plus-W, double RESET out of a game ROM.
- core1 budget: the `$FF43` hook is a write hook; re-run
  `coco/carttest.asm` on the CoCo 2 after it lands.

## 10. Later versions

- **Firmware-pulsed `/CART` autostart** for machines without Extended
  BASIC, so no `EXEC` is typed. Needs JP5 at 1-2 on a Pico 2 (never with
  JP3 bridged), `cart on`, and the firmware serving a jump in place of the
  `DK` header while it pulses, since the FIRQ entry is `$C000`.
- Loading `.BIN` and BASIC programs from a disk image without a DOS.
- Serving Extended BASIC at `$8000` from a Plus-W
  (`docs/RP2350B_IDEAS.md` section 15).
- 40/80 columns on a CoCo 3; delete and rename.

## 11. Order of work

Two plans. The first (`docs/superpowers/plans/2026-10-01-rom-manager-foundation.md`)
is steps 1-3 plus cold/warm restart on a CoCo 1/2; the second is the rest,
written with the first one's emulator and bench results in hand.

1. `ui` module with the ROMs screen, host tests.
2. Stub with draw, key, poll, action `10`; UI session in `picoco-host`;
   emulator on the no-ECB profile.
3. `$FF43` register, built-in image, fallback default; bench on the 16K
   CoCo 2.
4. Remaining screens (Disks, Settings, WiFi) and save.
5. Loader, actions `11`-`14`, previous-ROM restore; emulator checks 1-3.
6. Boot screen, action `15`.
7. Double RESET; bench checks 5-6.
8. Retire `PICOCO.BIN` and update `coco/README.md`.

## 12. Depends on

The core1 read-path change of 2026-10-01 (uncommitted at the time of
writing) and its open CoCo 3 regression check. Bench steps 3 and 7 need a
board that passes both machines.
