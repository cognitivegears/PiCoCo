# rom/: PiCoCo-DOS, the board's own boot ROM

The small MIT-licensed 6809 ROM that will make a fresh board usable with no
third-party files (`docs/ADDITIONAL_ROADMAP.md` section 1). Today it is the
skeleton from #16: the `DK` cartridge header, the `$C002` init, the DSKCON
vectors at `$C004`/`$C006` (every call reports NOT READY), one RAM hook
(CONSOLE IN at `$016A`, a pass-through), the banner `PICOCO-DOS 0.1`, and
the return into Extended BASIC's cold start, so the CoCo prints the banner,
BASIC's own banner and `OK`. `DIR`, `LOADM`, `DOS` and the rest are the
next tickets.

## Build and test

```
make -C rom          # lwasm builds picoco-dos.rom, smoke.py checks it
make -C rom xroar    # also boots it headless in XRoar on a CoCo 1, 2 and 3
```

Needs `lwasm` from LWTOOLS (`brew install lwtools`). `smoke.py` asserts the
size, the `DK` header, the vector table, the two zones a CoCo 3 patches in a
`DK` cart copy, and the MIT header in every `.asm`. `--xroar` needs XRoar
and the BASIC ROMs in `~/.xroar/roms` (`coco/README.md`, plus `bas10.rom`
and `extbas10.rom` for the CoCo 1 run); it reads XRoar's
CPU trace, no network, and passes when the banner goes out through CHROUT
and BASIC then reads the keyboard through our hook, which is where it sits
at `OK`. To see the screen itself, use `coco/tools/xrscreen.py` with `-gdb`
as in `coco/README.md`.

`picoco-dos.rom` is committed so a board can load it without the toolchain:
rebuild and commit it with every source change.

## On the board

Copy `picoco-dos.rom` to the board's flash (`fs export`, drag it onto the
`PICOCO` volume, `fs import`; `firmware/README.md` "Export"), then either
`rom load picoco-dos.rom` on the console or ENTER on it in the Manager's
ROMS list. Both cold-restart the CoCo; Extended BASIC finds `DK`, runs the
init, and the screen reads `PICOCO-DOS 0.1` over the BASIC banner and `OK`.
`EXEC 49154` from BASIC re-runs the init and comes back to `OK`; on the
XRoar CoCo 2 a program in memory survives it (CoCo 3 not checked; bench:
`firmware/TEST_PLAN.md` section L). Color BASIC alone
(no Extended BASIC) never calls `$C002`; there `EXEC 49154` prints the
banner and returns, untested.

## How the exit works

Disk BASIC's init ends in a jump back into Extended BASIC's cold start. On a
CoCo 3, Super Extended BASIC copies a `DK` cart to RAM and overwrites the 3
bytes at `$C0D9` with its own jump, so our exit jump sits exactly there and
the CoCo 3 path costs nothing. On a CoCo 1/2 the ROM scans `$8000-$9FFF`
for the `LBEQ $C002` that brought it in and continues right after it, the
no-cartridge path; no address inside the BASIC ROMs is hard-coded. Extended
BASIC 1.0 and 1.1 both use that branch.

## Clean room

Everything here is written from the cartridge protocol's interfaces and
from watching BASIC run, never by copying ROM code: the `DK` check and
`$C002` entry, the RAM hook table at `$015E` and the DSKCON interface with
its variables at `$EA-$F0` (the memory maps in the Unravelled series and
the Disk BASIC manual, used for addresses only); the CoCo 3 patch zones
(found on the bench for the Stub, `coco/stub/manager.asm`); the `LBEQ $C002`
and the patched exit (XRoar's CPU trace of the cold start). Token numbers,
vector addresses and the memory map are functional interfaces, and none of
Microsoft's or Tandy's code is in this tree. Licence: MIT, the
same as the firmware (`LICENSE` at the root); every source carries the
header. Replace the ROM with HDB-DOS from Cloud-9 or ToolShed for full Disk
BASIC.
