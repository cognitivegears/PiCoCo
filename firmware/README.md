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
- `--replay FILE`: feed `FILE` as a raw stream of CoCo->server bytes (no
  framing) in 64-byte chunks, print a stats summary, and exit — no socket is
  opened. Useful for replaying a captured session without XRoar/CoCo
  hardware attached.
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

## Pico build

Not wired up yet in this task; see Plan B for the RP2350 target.
