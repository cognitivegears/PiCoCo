# NitrOS-9 and Ease of Use on PiCoCo

How to boot NitrOS-9 on a CoCo 3 through a PiCoCo cartridge: first the stock
NitrOS-9 disk straight from the Pico's flash, then the Ease of Use (EOU)
desktop served from a computer over USB.

Tested 2026-10-01 on PCB v2.3.1 with a Pico 2, a CoCo 3 with a 6309 and
2 MB, macOS. The bench log is `firmware/TEST_PLAN.md` section H.

## What works and what does not

| | State |
|---|---|
| Stock NitrOS-9 L2 (6309), served from the Pico's flash | works |
| EOU 1.0.1 (6309), served from a Mac over USB (bridge mode) | works: boots, gshell, apps, 40 minute soak |
| EOU 6809 build | not tested; the remaster script handles it by file name |
| EOU over WiFi (Plus-W, `becker net`) | not tested |
| EOU served by DriveWire 4 or pyDriveWire instead of `picoco-host` | not tested, see "Other servers" |
| EOU from the Pico's flash | not possible: the image is 128 MB, flash holds 2.5 MB (Pico 2) or 14.5 MB (Plus-W) |
| SWAPBOOT to another boot set | breaks the boot; rebuild the image |
| `/MIDI`, `/P`, `/N` DriveWire channels | not tested |

NitrOS-9 Level 2 needs a CoCo 3. EOU needs 512 K or more.

## Part 1: stock NitrOS-9 from flash

No computer needed after the copy. Good first test, because nothing but the
PiCoCo is involved.

1. Get a `coco3_becker.dsk` image from a NitrOS-9 release that matches your
   CPU: `nos96309l2v030300coco3_becker.dsk` for a 6309,
   `nos96809l2v030300coco3_becker.dsk` for a 6809. Not the `_headless` one
   (that puts the console on a DriveWire terminal).
2. Copy it to the Pico. On the PiCoCo console (the second
   `/dev/cu.usbmodem*` port):

   ```
   dw eject 0
   fs export
   ```

   A `PICOCO` drive appears. Copy the image onto it under a short name
   (`NOS9.DSK`), eject the drive, then:

   ```
   fs import
   dw mount 0 NOS9.DSK
   time set <unix seconds, local time>
   save
   ```

3. On the CoCo, at the HDB-DOS `OK` prompt: `DOS`.

You should see `NITROS9 BOOT`, the module list, then a shell prompt. The
date shown during startup is the disk's build date; `date -t` afterwards
shows the Pico's clock.

RESET inside NitrOS-9 restarts NitrOS-9. To get back to HDB-DOS, power the
CoCo off and on.

## Part 2: Ease of Use

EOU is distributed as a CoCo SDC hard-disk image. It does not boot on a
PiCoCo as shipped: its boot track and drivers talk to a CoCo SDC. A script
swaps in the DriveWire-over-Becker modules that are already on the image.

### One-time setup

You need:

- The EOU zip for your CPU from the Ease of Use project
  (lcurtisboyle.com/nitros9/nitros9.html), which contains `63SDC.VHD` (6309)
  or `68SDC.VHD` (6809).
- ToolShed's `os9` command on your PATH.
- This repository, with the host tools built:

  ```
  cmake -B build-host -G Ninja -DPICOCO_HOST=ON firmware
  ninja -C build-host
  ```

Then build the Becker image (it copies; the original is left alone):

```
python3 firmware/tools/eou_becker.py 63SDC.VHD 63BECKER.VHD
```

What the script changes, all from modules on the image itself:

- kernel track: `boot_dw_becker` instead of the SDC boot module;
- boot file: `dwio_becker` and `/DD` = DriveWire drive 0; the SDC driver
  modules removed (their registers sit on the Becker port);
- `startup`: the DriveWire one, which does not stop to ask the time;
- `COCO3FPGA=1` in the env files.

### Every time

Three things run on the computer; the order does not matter much.

1. The DriveWire server, with HDB-DOS translation off:

   ```
   ./build-host/picoco-host --dir . --mount 0=63BECKER.VHD --hdbdos off
   ```

2. The relay between the Pico's bridge port (the first
   `/dev/cu.usbmodem*`) and the server:

   ```
   python3 firmware/tools/becker_relay.py /dev/cu.usbmodemXXXX1 --reconnect
   ```

   `--reconnect` matters: every CoCo reset or power cycle reboots the Pico,
   which drops the USB port for a moment.

3. Bridge mode on the Pico, once (the console is the second port):

   ```
   becker bridge
   save
   ```

Then on the CoCo: power on, wait for `OK`, type `DOS`.

In bridge mode HDB-DOS waits at its banner until the relay is attached, so
if `OK` does not appear, check steps 1 and 2.

### What you should see

1. `NITROS9 BOOT` and the module list.
2. The EOU banner: memory size, `Cpu type`, `DriveWire - (Installed)
   (Active)`, then "Coco SDC - Not detected".
3. Fonts loading, then a shell prompt.
4. `gshell` starts the desktop. CLEAR switches between windows; EOU opens
   two extra shell windows at startup.

`dcheck /dd` reports "file structure is not intact" with 117 clusters in the
allocation map only and 129 previously allocated clusters. The image ships
that way; it is not damage.

### Going back to flash disks

```
becker native
save
```

and stop the relay and the server.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `KREL Boot Krn tb0....bt*j` then `NITROS9 6309 FAILED` | HDB-DOS translation is on, so the boot file reads go to a drive that is not mounted | `--hdbdos off` on the server (`dw hdbdos off` for a flash image) |
| HDB-DOS banner, no `OK`, in bridge mode | nothing is listening on the bridge port | start the relay and the server; replug USB if the Pico is not listed |
| Boot stops after "Gime-X - Not found." | a server that mis-parses EOU's CoCo SDC probe | use a current `picoco-host`; see "Other servers" |
| Boot hangs at `i2x`, or `?IO ERROR` in HDB-DOS at 1.79 MHz | firmware older than 2026-10-01 | reflash |
| Hangs on a 6809 CoCo with a `63...` image | wrong CPU build | use the `68...` files |
| EOU asks for the time | an image built before the `startup` change, or the server has no clock | rebuild with `eou_becker.py` |
| `picoco-host: mount drive 0 = ... failed (-1)` | image name too long for the server | rename it to something short (`NOS9.DSK`) |
| Pico missing from USB after a CoCo power cycle | seen once, cause unknown | unplug and replug the USB cable |

`status` on the console has the counters that matter: `becker underrun` (52
per EOU boot is the SDC probe and is normal), `bus addr_resample` and
`bus oe_glitch` (both should be 0).

## Other servers

`picoco-host` is the only server tested with EOU. Two things it does that
another DriveWire server may not:

- It ignores EOU's CoCo SDC probe. EOU's hardware detection writes
  `64 64 00 64 00 ...` to `$FF42`, which on a PiCoCo is the DriveWire data
  port, and a server that reads that as an `OP_SERWRITEM` swallows the next
  request. `COCO3FPGA=1` in EOU's env file is documented to skip the
  detection but did not on the image tested.
- It answers within the 200 ms the firmware allows a server.

A server reached through `becker bridge` or `becker net` sees exactly what
the CoCo writes, so the probe fix in the Pico's own server does not apply.
