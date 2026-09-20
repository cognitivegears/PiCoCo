# PiCoCo

A Raspberry Pi Pico 2 (or Waveshare RP2350B-Plus-W) carrier that plugs into
the cartridge slot of a Tandy Color Computer (CoCo 1/2/3). It boots an
HDB-DOS ROM, serves DriveWire disk images through a Becker port at
$FF41/$FF42, and can drive the cartridge sound input. Licence: CERN-OHL-S-2.0
(hardware), MIT (firmware), CC-BY-SA-4.0 (docs); see `LICENSE`.

Board revision: **v2.3.1** (98 x 77.2 mm, 2 layers, hard-gold fingers).
Design docs: `docs/hardware-design.md`, `docs/firmware-architecture.md`,
`docs/kicad-workflow.md`. Ordering: `fab/main/READ-BEFORE-ORDERING.txt`.

## If you have a bare board

This is a **bare 98 x 77 mm board with no shell**. It fits no Program Pak
case — the 53.34 x 44.45 mm outline mentioned in the roadmap docs is the
cased, future-variant footprint, not this board.

1. **Orientation.** The board has no shell and no key. The component side is
   the top; the front silkscreen says `THIS SIDE UP` near the finger tongue
   and the back says `OTHER SIDE UP`. Inserted upside down, +5 V lands on the
   CoCo's /CTS line. There is no reverse protection.
2. **Power off before inserting or removing.** Every insertion.
3. **Module.** Solder a Pico 2 flat on the castellations or on 2x20 headers;
   a Plus-W goes flat (castellations plus the 15 hidden pads; use the
   `stencil-module` paste stencil for the 15 grid pads only and hand-solder
   the castellations). A flat-mounted Pico 2 rests its three underside debug
   pads on grid pads GP29/GP32/GP35: put Kapton tape over those three pads
   first, or use headers. Nothing else goes under the module.
4. **Jumpers (solder bridges, defaults as shipped).**
   - `JP2` 1-2: U10 data-buffer /OE from the hardware decode (default).
     2-3: /OE from firmware GP31 (Plus-W only).
   - `JP3` 1-2: audio PWM to header pin 34 (default; sound works on a Pico 2).
     2-3: E clock to header pin 34 instead. Never bridge 2-3 and drive GP34 on
     a Plus-W at the same time.
   - `JP4` open (default): bridge to tie Q to /CART for cartridge autostart of
     ROM images (the classic Program Pak trick). **Bridging JP4 breaks
     HDB-DOS and any DK-signature DOS ROM** — it pulses /CART on every cycle,
     so the CPU jumps to $C000 as code continuously, and a DOS ROM's first
     bytes ("DK") are not a valid instruction. Only bridge it for a
     cartridge-style ROM that expects the classic autostart hookup.
   - `J2` / `J3` (DNP 1x3 pin headers) are twins of JP3 and JP5 on the same
     nets: fit one and use a shunt if you expect to flip the setting often.
     Cut the solder jumper first — a bridged JP and a shunt in the other
     position would short two signals.
   - `JP5` open (default): shares header pin 34 with JP3, so bridge at most
     one of them. 1-2 gives a Pico 2 a firmware-pulsed /CART (the Q4 stage,
     populated); 2-3 gives a firmware-pulsed /NMI instead (the Q3 stage —
     fit R15/R17 too). No firmware drives either position yet.
5. **DNP parts** (not fitted by default): R2 R10 R15 R17 Q3 C12 C15 J1 J2 J3. These
   are all Plus-W provisions or optional stages, by design, not leftover
   indecision — a Pico 2 build works correctly without them. Removing R7
   disables the /HALT boot hold: a blank or dead module then no longer
   halts the CoCo, so a dead Pico presents to the user as a dead CoCo — that
   is the intended failure mode of the hold, not a fault to debug around.
   Fitting C15 (1 uF) and removing R24 gives AC-coupled sound. `J1` ("EXP
   (Plus-W)") breaks out four pad-grid pins that only exist on a Plus-W.
6. **Test points.** TP1 OE_BUS, TP2 RW_BUF, TP3 CTS_BUF, TP4 SCS_BUF, TP5 E_BUF,
   TP6 +3V3, TP7 SND_CART (sound output to the cart), TP8 GND.
7. **Flashing.** Hold BOOTSEL on the module and plug USB in; copy the UF2
   from `firmware/` (see `firmware/README.md`). With the board in the CoCo,
   holding BOOTSEL and pressing the CoCo's own RESET button also enters the
   bootloader, because cart /RESET is tied to the module's RUN pin (R9).
8. **Disk images.** The firmware keeps images in the module's flash and is
   driven from the USB console (`firmware/README.md`, `docs/breadboard-plan.md`
   §6 for the milestone commands). Wi-Fi DriveWire needs the Plus-W and later
   firmware.
9. **Multi-Pak Interface.** Select the PiCoCo slot for both /CTS and /SCS
   (`POKE &HFF7F` slot value) — the slot register ghosts at `$FF9F` as well
   as `$FF7F`; a CoCo 3 needs the upgraded MPI PAL. The 98 mm body may not
   fit some MPI slot openings; measure first.
10. **Is it alive?** On a CoCo 3, BASIC `PEEK(&HC000)` never reaches the
    cart — check `$FF41`/`$FF42` (the Becker status/data registers) instead.
11. **HDB-DOS.** Not shipped with this board — build it yourself with a
    single `make` from ToolShed's `hdbdos/` tree, or get a prebuilt image
    from Cloud-9 (see `docs/ADDITIONAL_ROADMAP.md` §3 for why binaries
    aren't checked in here). Credit to the DriveWire and HDB-DOS authors and
    maintainers, whose protocol and ROM this board depends on entirely.

## Repository map

```
PiCoCo/        KiCad 10 project (schematic is generated: edit tools/gen_schematic.py)
libraries/     local symbols and footprints (Pico-Carrier, COCO-CART)
tools/         generators and routing/fab tooling (see docs/kicad-workflow.md)
fab/main/      JLCPCB package: Gerber zip, BOM, CPL, module stencil, ordering checklist
breakout/      breadboard-phase boards (cart fingers to 2x20 header, cobbler)
firmware/      RP2350 firmware (host-testable core)
docs/          design documents and roadmap
```
