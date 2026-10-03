# PiCoCo - Breadboard-First Plan

The breadboard phase is finished. Its plan was executed between 2026-09-05
and 2026-09-29 and the PCB (v2.3.1) replaced the breadboard as the bench
board on 2026-09-30. The full plan is in git history; what came out of it
lives here:

- **Bench results:** `firmware/TEST_PLAN.md`, the dated logs from
  2026-09-15 (address buffers, decode, Becker loop), 2026-09-16 (HDB-DOS
  and native DriveWire) and 2026-09-29 (bridge mode, latency limit).
- **Design findings** (U10 on LVC245 with the Pico on the A side, the
  NAND-NAND decode, the finger trim, the /HALT and RUN decisions): folded
  into the v2.3 board, `docs/superpowers/specs/2026-09-17-main-board-v2.3-design.md`
  and `docs/hardware-design.md` §4.
- **The breakout and cobbler boards:** `breakout/README.md`. Never pair the
  breakout with a Raspberry Pi cobbler; use `PiCoCo-Cobbler`.
- **Bringing up a new board:** `docs/pcb-bringup.md`, then the bench rows in
  `firmware/TEST_PLAN.md` sections F-K.
