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
     ROM images (the classic Program Pak trick). HDB-DOS does not need it.
5. **DNP parts** (not fitted by default): R2 R10 R15 R16 R17 R18 Q3 Q4 C12
   C15. Removing R7 disables the /HALT boot hold: a blank module then no
   longer halts the CoCo, at the cost of the boot-time race the hold exists to
   prevent. Fitting C15 (1 uF) and removing R24 gives AC-coupled sound.
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
   (`POKE &HFF7F` slot value); a CoCo 3 needs the upgraded MPI PAL. The 98 mm
   body may not fit some MPI slot openings; measure first.

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
