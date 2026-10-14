# PiCoCo

A Raspberry Pi Pico 2 or RP2350B-Plus-W carrier in the cartridge slot of a Tandy Color Computer. It stands in for a cartridge ROM and a DriveWire disk server at the same time. This glossary is the project's vocabulary; design detail lives in `docs/`.

## Language

### The CoCo side

**CoCo**:
The Tandy Color Computer the board plugs into (CoCo 1, 2 or 3). The CPU, bus and reset belong to it.
_Avoid_: host (that word means the development machine, see below)

**Cartridge bus**:
The 40-pin edge-connector signals the board sees: D0-D7, A0-A15, /R/W, E, Q, /CTS, /SCS, /CART, /HALT, /NMI, /RESET.
_Avoid_: slot bus, cart port

**Fingers**:
The hard-gold edge-connector pads on the board's finger tongue, the 10.16 mm strip that enters the slot.
_Avoid_: edge pins, contacts

**ROM window**:
The $C000-$FEFF address range the CoCo selects with /CTS. The board answers reads there with the ROM image it is serving.
_Avoid_: CTS range, cart space (the latter is the whole 32 KB region)

**Becker port**:
The two-register DriveWire peripheral at $FF41 (status) and $FF42 (data), selected by /SCS. Named after the Becker interface in the CoCo emulators.
_Avoid_: Becker window, DriveWire port

**Cycle**:
One CoCo bus access, read or write, as seen from the board. A cycle is a ROM cycle or a Becker cycle; nothing else is answered.

### The board side

**Module**:
The RP2350 board soldered onto the carrier: a Pico 2 or a Waveshare RP2350B-Plus-W. Many design facts depend on which one.
_Avoid_: Pico (ambiguous between the two), MCU board

**Pico 2**:
The Raspberry Pi Pico 2 module. Exposes 26 GPIO, all of which the design uses.

**Plus-W**:
The Waveshare RP2350B-Plus-W module. Adds a pad grid of extra GPIOs and Wi-Fi; the only module on which the Plus-W-only features work.
_Avoid_: RP2350B, Waveshare board

**Pad grid**:
The Plus-W's hidden underside pads that carry its extra GPIOs onto the carrier. A Pico 2 has none.

**Buffered signal**:
A cartridge-bus signal after it has passed through one of the four SN74LVC245A buffers. Its net name ends in `_BUF` (`RW_BUF`, `CTS_BUF`, `E_BUF`).
_Avoid_: `_B`, `_DBG` suffixes (banned by the schematic generator)

**OE_BUS**:
The hardware decode output that is low only while a cart address is selected and E is high. It enables the data buffer and tells firmware a cycle is in progress.

**Halt hold**:
The hardware circuit that holds the CoCo's /HALT low from power-on until firmware releases it through `HALT_GATE` (GP27).
_Avoid_: reset hold, boot hold

**Jumper**:
A solder-bridge option on the board (JP2-JP5). Each has a shipped default; the README lists them.
_Avoid_: strap, header jumper

**DNP**:
A footprint on the board that is not populated by default.

### The firmware side

**Bus engine**:
The PIO and DMA programs, with core1's event loop and hooks, that answer cycles in real time.
_Avoid_: bus watcher, bus loop

**Bus table**:
The 16 KB in-SRAM image of what the board presents across the cart space ($C000-$FFFF): ROM bytes plus the Becker registers.
_Avoid_: ROM buffer, memory map

**BUS_HOT**:
Code and data that core1 may execute: resident in SRAM, never flash.

**Host**:
The development machine. The firmware also builds as a host program for its test suites.
_Avoid_: PC, desktop

**Console**:
The USB CDC command interface on the module. The only supported bring-up interface; `picoco.cfg` replays console commands at boot.
_Avoid_: shell, CLI, serial monitor

**Bring-up**:
The staged process of proving a new board or build works on a real CoCo, milestone by milestone.
_Avoid_: testing (too broad), commissioning
