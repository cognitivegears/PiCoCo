# PiCoCo manager: CoCo-side program and command channel

Status: draft for review, 2026-09-23. Brainstormed with the user.

## 1. Goals

1. A CoCo program (`PICOCO.BIN` on `PICOCO.DSK`) that manages a PiCoCo from
   the CoCo keyboard: browse the disk images on the cartridge's flash,
   mount and eject them in drives 0-3, create blank images, boot a disk,
   choose the next-boot ROM, toggle HDB-DOS drive mode, set the clock,
   and save everything to `picoco.cfg`. SDC Explorer
   (cocosdc.blogspot.com/p/sdc-explorer.html) is the UX reference.
2. The program talks to the firmware over standard DriveWire 4 virtual
   serial channels. Any DW4 client (NitrOS-9 `dw`, DwTerm) can then use the
   same commands, and later features (WiFi, `tcp`) reuse the transport.
3. One command backend. The existing console dispatcher
   (`firmware/src/console/console.c`) stays the only place that acts. USB
   CDC1 and the virtual-serial channel are both thin front ends to it.
4. Runs on CoCo 1, 2 and 3 with 32K, using the 32x16 text screen.

Non-goals for v1: delete, rename, 40/80 columns, copying images from the
CoCo, WiFi, SDC register emulation, subdirectories. See section 9.

## 2. Decisions

| Topic | Decision | Why |
|---|---|---|
| Transport | DW4 virtual serial, command mode, one channel at a time | Standard, reusable by existing DW4 clients; user's pick over a custom opcode |
| Wire reference | Java DW4 server 4.3.3p where it differs from the spec, except SERINIT/SERTERM (see 4.1) | Every shipped client was tested against it |
| Command vocabulary | The console commands, plus DW4 `dw disk` aliases, plus `fs new` and `rom boot` | No second command set to maintain |
| Remote safety | Allowlist on the virtual-serial path; everything else is USB-only | Several commands can wreck the bus or the channel itself |
| Program language | C with CMOC; LWTOOLS assembler/linker; ToolShed `decb` for the .DSK | Same toolchain as FujiNet CONFIG |
| Screen | 32x16 uppercase text on all models | User's pick; one binary |
| Becker address | Compile-time constant $FF41/$FF42 in one place | Runtime probing arrives with the SDC profile (section 9) |
| Apply vs persist | Commands act live; `save` persists; BREAK returns to BASIC without a hardware reset | Every CoCo reset reboots the Pico and replays `picoco.cfg` |

## 3. Architecture

```
CoCo                                  Pico core0
PICOCO.BIN                            dw_server.c
  ui.c  --picoco_cmd(line)-->           opcode parser
  dw.c  (Becker $FF41/$FF42)  ======>   dw_vser.c  (new: port state, poll, command mode)
                                           | exec(line, buf, cap)  (callback)
                                        console.c
                                          console_exec_remote(): allowlist,
                                          output captured into a buffer
                                          dispatch()  <-- also USB CDC1
```

- `dw/` does not include `console/`. `dw_vser` receives an `exec` callback
  at init, which keeps the DriveWire code host-testable on its own.
- core1, the bus table and the single-writer Becker rule are untouched.
  Everything new runs on core0.

## 4. Firmware

### 4.1 Virtual serial on the wire (`src/dw/dw_vser.c`, new)

Channels: 1-13 are usable for commands. Port 0 is DW4's NTerm, 14 is
MIDI, and 15 cannot be encoded in the SERREAD reply. Opcodes on 0, 14 and
15 are consumed and ignored.

| Op | CoCo sends | Server replies | Behaviour |
|---|---|---|---|
| SERINIT $45 | `45 ch` | nothing | Same as SS.Open (spec). Java 4.3.3p ignores it; acting on it is a compatible superset |
| SERTERM $C5 | `C5 ch` | nothing | Same as SS.Close |
| SERSETSTAT $C4 | `C4 ch code` (+26 bytes if code $28) | nothing | $29 SS.Open: `opens++`, port enters command mode. $2A SS.Close: `opens--`; at 0 the port resets. $28: payload consumed and ignored. Others ignored |
| SERGETSTAT $44 | `44 ch code` | nothing | Ignored, as in Java |
| SERWRITE $C3 | `C3 ch b` | nothing | Byte into the port's line buffer |
| SERWRITEM $64 | `64 ch n` + n bytes (n=0 means 256) | nothing | Bytes into the line buffer |
| FASTWRITE $80+ch | `8x b` | nothing | As SERWRITE for ch 0-15 (so 0, 14, 15 are ignored). $90-$9F (Z/window ports) stay consumed and ignored |
| SERREAD $43 | `43` | 2 bytes | See poll order below |
| SERREADM $63 | `63 ch n` | n bytes from the queue | If the port has fewer than n bytes, send nothing (as Java; the client must only ask for what SERREAD advertised) |

SERREAD poll order (Java `DWVSerialPorts.serRead`):
1. A port whose close is pending: `[0x10, ch]`, then the port resets.
2. A port with 1-2 bytes queued: `[ch+1, byte]` (one byte dequeued).
3. A port with 3 or more queued: `[ch+17, min(n,255)]`, nothing dequeued.
4. Nothing: `[0x00, 0x00]`.

With one active session there is no fairness logic to port.

`OP_DWINIT`, `OP_RESET` ($F8/$FE/$FF) reset all ports, as Java does.
The existing parser length table already covers every opcode above; the
change replaces the stubs at `dw_server.c` SERREAD/SERREADM/SERWRITEM/
SERSETSTAT/SERINIT/SERTERM and the FASTWRITE default.

### 4.2 Command mode

- Writes to an open port collect into a 128-byte line buffer. CR ($0D)
  ends the line; LF is stripped; BS ($08) deletes; $00 is ignored. No echo.
  An overlong line is answered with `FAIL 010 line too long`.
- A blank line is ignored. Every other line runs through
  `exec(line, buf, 4096)`. DW4's other APIs (`AT`, `tcp`, `ser`, `ui`) are
  not special-cased: they fail as unknown commands until a later phase
  adds them.
- Reply framing, byte for byte as Java 4.3.3p (`DWVSerialPort.java`):
  - success: `OK command successful` `0A 0D`, then the captured output;
  - failure: `FAIL nnn <msg>` `0A 0D` (nnn = 3-digit code).
  The firmware's `ok` terminator line is not sent; an `err <msg>` from
  the console becomes `FAIL 255 <msg>` unless mapped below.
- After the reply is fully read, the port's close becomes pending, so the
  next SERREAD returns `[0x10, ch]`. This happens on both success and
  failure, as in Java.
- One session at a time. An SS.Open on a second channel while a command
  port is open gets an immediate hangup (`[0x10, ch]`) on the next SERREAD.
- Commands run synchronously on core0. The longest (`fs new`, section
  4.4) holds the CoCo's SERREAD reply for up to a few seconds; the CoCo
  side's timeout allows for it (section 5.2).
- Output longer than the 4096-byte buffer is truncated and ends with a
  line `...`.

Error-code mapping (DW4 `DWDefs`): messages starting `usage` are 010,
`bad drive` is 101, everything else is 255. The console's `cerr()` message
is the FAIL text (cut to 80 characters).

### 4.3 Remote allowlist (`console_exec_remote`)

`console_exec_remote(line, buf, cap)` swaps `g_out` to a buffer writer,
checks the allowlist, runs `dispatch()`, and restores `g_out`. USB behaviour
is unchanged.

Allowed from the CoCo (verb and first argument):

| Command | Notes |
|---|---|
| `status`, `version`, `help` | read-only |
| `fs ls` | |
| `fs new <name>` | new, 4.4 |
| `dw mount`, `dw eject`, `dw hdbdos` | |
| `dw disk show [n]`, `dw disk insert <n> <file>`, `dw disk eject <n>` | new DW4 aliases, 4.4 |
| `rom boot <file>` | new, 4.4 |
| `time`, `time set <unix>` | |
| `save` | |

Refused with `FAIL 255 console only` (everything else, including): `smoke`
(drives the data pins), `halt` (freezes the CoCo), `bus`, `becker` (cut the
program's own channel), `fs format|rm|export|import`, `rom load|pattern|off`
(live swap under a running DOS), `trace`, `crash`, `log`, `stats`, `dw
capture|selftest|stats`, `reboot`, `bootsel`.

The allowlist is deny-by-default: a new console command is USB-only until
someone adds it here.

### 4.4 New console commands (available on USB too)

- `dw disk show [n]`: DW4 format. No argument prints
  `\r\nCurrent DriveWire disks:\r\n\r\n` then per mounted drive
  `X%-3d` + (`*` if read-only else space) + filename + `\r\n`. With `n`
  prints `Details for disk in drive #n:\r\n\r\n<file>\r\n`.
- `dw disk insert <n> <file>`: same as `dw mount n file`, except the file
  name is the rest of the line re-joined with single spaces (DW4
  behaviour, allows spaces), and an occupied drive is ejected first.
  Replies `Disk inserted in drive n.`.
- `dw disk eject <n>`: replies `Disk ejected from drive n.\r\n`.
- `fs new <name>`: creates a formatted, empty 35-track RS-DOS image of
  161,280 bytes: every byte $FF except the FAT sector (track 17 sector 2,
  offset 0x13300) bytes 68-255, which are $00, as `DSKINI` leaves them.
  Check the result against a `DSKINI0` image from XRoar in the tests.
  Refuses if the file exists or the name contains `/`.
- `rom boot <file>`: checks the file exists and is 8192 or 16384 bytes,
  then records `load <file>` as the saved ROM command. It does not touch
  `bus_table`. `status` shows both the running ROM and the next-boot ROM.

The tokenizer limit (6 tokens, 127 chars) stays. `dw disk insert` is the
only command that re-joins its tail.

### 4.5 Clock spike

`time set` stores an offset from uptime, which is lost at every CoCo
reset because /RESET drives the Pico's RUN pin. A spike checks early
whether the RP2350 always-on (AON/powman) timer keeps counting across a
RUN-pin reset:
- If it does, `time set` also writes the AON timer and boot reads it
  back, so the time lasts until power-off.
- If it does not, the clock lasts until the next reset. The settings
  screen says so.

## 5. CoCo program (`coco/`, new)

### 5.1 Build and launch

- `coco/Makefile`: CMOC plus LWTOOLS build `PICOCO.BIN`; ToolShed `decb`
  builds `PICOCO.DSK` containing `PICOCO.BIN` and `PICOCO.BAS`
  (`10 LOADM"PICOCO":EXEC`).
- Load address at or above $3800, so the OS-9 boot track can load to
  $2600-$37FF without overwriting the program (5.5). The program must end
  below $7800, leaving room for BASIC's stack and string space.
- Launch: copy `PICOCO.DSK` to flash, mount it (for example
  `dw mount 3 PICOCO.DSK` plus `save`), then `DRIVE 3:RUN"PICOCO"`. The
  loader `LOADM`s from the default drive, hence `DRIVE` first. On a CoCo 3
  it also does `WIDTH 32` (guarded by `PEEK(65534)=140`).
- Toolchain install (CMOC, LWTOOLS, ToolShed, XRoar) is the first plan
  task; none are on the dev Mac today.

### 5.2 Modules

| File | Job |
|---|---|
| `dw.c` | Becker I/O: `put(b)`, `get(timeout)` polling $FF41 bit 1 and $FF42. `dw_read(drive, lsn, buf)` (OP_READ with checksum). `picoco_cmd(line, buf, cap)`: SS.Open ch 1, SERWRITEM the line + CR, poll SERREAD, SERREADM bulk, until `[0x10,1]`. Returns OK/FAIL, code and text |
| `parse.c` | Pure C, no hardware: parse `fs ls` lines (`name size`), `dw disk show`, `status`; RS-DOS directory entries; OS-9 detection |
| `ui.c` | Screens, keys, list scrolling |
| `boot.c` | Boot flows (5.5) |
| `main.c` | Startup: `version` check, first screen |

Timeouts: 1 s per byte while a reply is streaming; 10 s for the first
byte of a command's reply (covers `fs new`). On timeout the program shows
`PICOCO NOT RESPONDING` and `(BECKER NATIVE? FIRMWARE >= 1.2?)`, and offers retry
or exit.

### 5.3 Main screen

```
PICOCO MANAGER        FW 1.2
0 NITROS9.DSK      2 -
1 ZAXXON.DSK       3 PICOCO.DSK
--------------------------------
>DINORUN.DSK             161K
 GAMES1.VDK              161K
 ...
--------------------------------
0-3:MOUNT SHIFT+ E:EJECT B:BOOT
N:NEW S:SET V:SAVE BREAK:EXIT
```

- The list comes from `fs ls`, excluding `*.ROM` and `picoco.cfg`, shown
  uppercase and cut to fit. Mount names use the original case.
- Up/Down move, Shift+Up/Down page, letters jump to the first matching
  name (SDC Explorer style).
- 0-3 mount the selection (`dw disk insert`). Commands take SHIFT, as in
  SDC Explorer, so plain letters stay free for jumping: SHIFT+E then 0-3
  ejects; SHIFT+N asks for a name (`.DSK` appended when there is no
  extension) and runs `fs new`; SHIFT+B boots; SHIFT+S opens settings;
  SHIFT+V saves; BREAK returns to BASIC.
- Drive names come from `dw disk show`. The header refreshes after every
  change.
- FAIL messages show on the bottom line until the next key.
- Mount names of 32 characters or more fail in the firmware
  (`dw_disk.h`); the program shows the error text as usual.

### 5.4 Settings screen (S)

```
FIRMWARE 1.2
ROM NOW  HDBDW3BC3.ROM
ROM NEXT HDBDW3BC3.ROM
R:ROM  H:HDB-DOS MODE ON
T:CLOCK 2026-09-23 14:02
V:SAVE  BREAK:BACK
```

- R lists `*.ROM` files, runs `rom boot`, then shows `SAVE, THEN RESET`.
- H toggles `dw hdbdos on|off`.
- T asks for `YYYY-MM-DD HH:MM`, converts it to Unix seconds and runs
  `time set`. The note from 4.5 shows when the clock does not survive a
  reset.

### 5.5 Boot (B)

1. Mount the selection to drive 0 (`dw disk insert 0 <file>`).
2. Read LSN 612 (track 34, sector 1) with `dw_read(0, 612)`. In HDB-DOS
   mode drive 0's offset is 0, so the LSN is the same in both modes.
3. If it starts with `OS`: read all 18 sectors of track 34 to $2600-$37FF
   and `JMP $2602`, as Disk BASIC's `DOS` command does.
4. Otherwise: read the RS-DOS directory (track 17, sectors 3-11) and list
   the .BAS and .BIN entries. The user picks one; the program returns to
   BASIC and runs `RUN"NAME"` or `LOADM"NAME":EXEC`.
5. How to hand a command line to BASIC from machine code on Color/
   Extended/HDB-DOS ROMs is a spike. The fallback is to exit to BASIC and
   print the command for the user to type.

### 5.6 Exit

BREAK on the main screen returns to BASIC with a warm start and no
hardware reset. Live mounts stay in effect until the next reset; the
program says `V TO KEEP AFTER RESET` if anything changed since the last
save.

## 6. Error handling

- Firmware: every console error becomes a FAIL line; nothing in command
  mode can leave a port half open. A byte stream that desyncs the parser
  is handled by the existing 250 ms payload timeout.
- CoCo: every `picoco_cmd` failure shows its text; a timeout offers retry
  or exit. The program never writes to a DriveWire sector except through
  firmware commands.
- `fs new` on a full volume fails (`FAIL 255 write failed`), and the
  partial file is removed.

## 7. Testing

Firmware (host build, ctest):
- `test_dw_vser`: each opcode in 4.1; SERREAD encodings for 1, 2, 3 and 300
  queued bytes; SERREADM exact and short; hangup after the reply on OK
  and FAIL; SERINIT behaving as SS.Open; `opens` counting; busy second
  channel; DWINIT/RESET clearing ports; line buffer overflow, BS, LF.
- `test_console`: allowlist accepts every listed entry and refuses a
  sample of the rest; `dw disk` output formats; `fs new` bytes against a
  fixture; `rom boot` does not change `bus_table`.
- `tools/dwtest.py`: a scripted session against `picoco-host`: open, `fs
  ls`, `dw disk insert`, `dw disk show`, hangup.

CoCo program:
- `coco/test_parse.c`, compiled with the host C compiler: `fs ls` and
  `dw disk show` parsing, RS-DOS directory entries, OS-9 detection.
- End to end in XRoar with its Becker port pointed at `picoco-host`
  (TCP 65504), on CoCo 2 and CoCo 3 machine profiles.
- Final check on the bench CoCo 3.

## 8. Order of work

1. Toolchain install; hello-world `.DSK` running in XRoar.
2. Spikes: AON clock (4.5), BASIC command handoff (5.5).
3. Firmware `dw_vser` + command mode + allowlist, with tests.
4. New console commands (4.4), with tests.
5. `coco/dw.c` + `parse.c` with host tests; a CLI-ish smoke program in XRoar.
6. `ui.c` main and settings screens.
7. `boot.c`.
8. Bench run on the CoCo 3; update `firmware/README.md` and the console
   help text.

Firmware version goes to 1.2.

## 9. Roadmap (out of v1)

| Phase | Firmware | CoCo program |
|---|---|---|
| 1.1 polish | Allowlist `fs rm`; add `fs mv`; auto-save mounts (ADDITIONAL_ROADMAP item 2) | Delete with confirmation, rename, 40/80 columns on a CoCo 3, read-only mount toggle |
| WiFi (Plus-W) | cyw43 + lwIP per `docs/FUJINET_SETUP.md`; `wifi scan|join|status|off` added to the allowlist; NTP sets the clock | WiFi screen: scan, pick, enter the password |
| Network channels | DW4 `tcp connect|listen` on virtual serial ports | None needed; DW4 terminal programs work |
| Images without a Mac | `fs get <url>` (HTTP or TNFS) | Download screen, later remote browsing |
| SDC profile | SDC register emulation and SDC-DOS; Becker moves to $FF5x | Probe the Becker address at runtime; SDC Explorer covers SDC-side disk jobs |
| No-disk menu | Serve the program from the ROM window or a PiCoCo-DOS menu | Same program, ROM build |

Open questions for those phases:
- The WiFi password would be stored in plain text in `picoco.cfg`, readable by
  anyone with USB access (same as FujiNet). Decide explicitly when WiFi is built.
- Subdirectories need FatFS changes (`FF_FS_RPATH`, the `/` check in
  `dw_store_fatfs.c`). This is a separate item.

## 10. References

- DW4 wire facts: `/Users/cognitivegears/projects/dw/driveWire/DriveWire
  Specification.md`; Java 4.3.3p `DWProtocolHandler.java`,
  `virtualserial/DWVSerialPorts.java`, `DWVSerialPort.java`,
  `DWVPortHandler.java`, `DWUtilDWThread.java`, `DWDefs.java`.
- Current firmware surface: `firmware/src/console/console.c`,
  `firmware/src/dw/dw_server.c`, `firmware/src/console/mode.c`.
- Roadmap context: `docs/ADDITIONAL_ROADMAP.md` items 2 and 3 and section 2
  (SDC), `docs/FUJINET_SETUP.md`.
