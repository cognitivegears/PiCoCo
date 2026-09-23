#!/usr/bin/env python3
"""Build-time check that core1's BUS_HOT bus loop never branches into flash.

core1 (firmware/src/bus/bus_core1.c) runs flash-free from SRAM with
interrupts disabled: a branch into flash would either fault or (worse) hang
with PICO_FLASH_ASSUME_CORE1_SAFE=1 turning it into a silent lockup instead
of a build error (see CLAUDE.md "Firmware"). This script follows every
direct branch/call reachable from bus_core1_main, plus the Becker read hooks
it calls indirectly through bus_add_read_hook (found by grepping the source,
since an indirect call's target can't be read off the disassembly), and
fails if any of them lands outside SRAM.

Also fails if:
- a reachable symbol is a linker veneer (name ends in "_veneer"): an
  SRAM->SRAM call is always in Thumb branch range and never needs one, so a
  veneer's mere presence means the real target is out of range, i.e. flash.
- a reachable function's own body has a literal .word in the flash range
  (0x10000000-0x1FFFFFFF): the usual way a long-range indirect call (ldr
  r3,=flashfunc; blx r3) embeds its target, which no branch-target scan can
  follow.

Run as a POST_BUILD step on the `picoco` target (see firmware/CMakeLists.txt).
stdlib only; uses arm-none-eabi-nm/objdump via the paths CMake's toolchain
already found (CMAKE_NM / CMAKE_OBJDUMP).

ponytail: matches b/bl plus every ARM condition code (with an optional
.n/.w width suffix) as a single regex, rather than tracking local loop
backedges separately from cross-function tail calls — a conditional branch
back into its own function is a no-op once that function is already
visited, so treating every branch mnemonic the same way is both simpler and
correct. Indirect calls through a register (blx <reg>) still can't be
resolved from a branch target address at all; those are covered by listing
the callee as its own entry point (see discover_hook_symbols) or by the
literal-pool scan above, not by tracing the call site.
"""
import re
import subprocess
import sys
import glob

SRAM_LO = 0x20000000
SRAM_HI = 0x20082000  # RP2350: RAM (512K) + SCRATCH_X + SCRATCH_Y, see pico-sdk default_locations.ld
FLASH_LO = 0x10000000
FLASH_HI = 0x20000000

ALWAYS_ENTRIES = ["bus_core1_main"]
HOOK_RE = re.compile(r"bus_add_read_hook\([^,]+,\s*([A-Za-z_]\w*)\s*\)")
BRANCH_LINE_RE = re.compile(r"^\s*([0-9a-fA-F]+):\s+([A-Za-z][A-Za-z0-9.]*)\s+([0-9a-fA-F]+)\s*<([^>]+)>")
WORD_LINE_RE = re.compile(r"^\s*([0-9a-fA-F]+):\s+\.word\s+(?:0x)?([0-9a-fA-F]+)")
BRANCH_MNEMONIC_RE = re.compile(
    r"^b(l)?(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.[nw])?$")


def discover_hook_symbols(repo_root):
    names = set()
    for path in glob.glob(f"{repo_root}/firmware/src/**/*.c", recursive=True):
        with open(path, encoding="utf-8") as f:
            names.update(HOOK_RE.findall(f.read()))
    return sorted(names)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def load_functions(nm, elf):
    """name -> (addr, size) for defined code symbols: text (t/T) or weak (w/W)
    — a linker veneer can land in either bucket depending on toolchain
    version, so both are kept instead of just "t"."""
    out = run([nm, "-S", "-n", "--defined-only", elf])
    funcs = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4:
            addr_s, size_s, typ, name = parts
            size = int(size_s, 16)
        elif len(parts) == 3:
            addr_s, typ, name = parts
            size = 0
        else:
            continue
        if typ.lower() not in ("t", "w"):
            continue
        funcs[name] = (int(addr_s, 16), size)
    return funcs


def main():
    if len(sys.argv) != 5:
        print("usage: check_core1_flash_free.py <elf> <nm> <objdump> <repo_root>", file=sys.stderr)
        return 2
    elf, nm, objdump, repo_root = sys.argv[1:5]

    funcs = load_functions(nm, elf)
    entries = list(ALWAYS_ENTRIES) + discover_hook_symbols(repo_root)
    missing = [e for e in entries if e not in funcs]
    if missing:
        print(f"check_core1_flash_free: entry symbol(s) not found in {elf}: {missing}", file=sys.stderr)
        return 2

    lines = run([objdump, "-d", "--no-show-raw-insn", elf]).splitlines()

    violations = []
    visited = set()
    queue = list(entries)
    while queue:
        fname = queue.pop()
        if fname in visited:
            continue
        visited.add(fname)
        if fname.endswith("_veneer"):
            violations.append(f"{fname} is a linker veneer: an SRAM->SRAM call never needs one, "
                               f"so its real target must be out of range (flash)")
        if fname not in funcs:
            # Handled here rather than a KeyError: a branch target the disassembly
            # named but that isn't in the symbol table at all (e.g. a stripped
            # local) can't be verified, so treat it as a failure, not a crash.
            violations.append(f"{fname}: not in the symbol table, cannot verify it stays in SRAM")
            continue
        addr, size = funcs[fname]
        if not (SRAM_LO <= addr < SRAM_HI):
            violations.append(f"{fname} (0x{addr:08x}) is not in SRAM")
            continue
        hi = addr + size if size else addr + 1
        for line in lines:
            bm = BRANCH_LINE_RE.match(line)
            if bm:
                insn_addr = int(bm.group(1), 16)
                if addr <= insn_addr < hi and BRANCH_MNEMONIC_RE.match(bm.group(2)):
                    target_addr = int(bm.group(3), 16)
                    target_name = bm.group(4).split("+")[0]
                    if not (SRAM_LO <= target_addr < SRAM_HI):
                        violations.append(f"{fname} branches to {target_name} (0x{target_addr:08x}), outside SRAM")
                    if target_name not in visited:
                        queue.append(target_name)
                continue
            wm = WORD_LINE_RE.match(line)
            if wm:
                insn_addr = int(wm.group(1), 16)
                if addr <= insn_addr < hi:
                    word_val = int(wm.group(2), 16)
                    if FLASH_LO <= word_val < FLASH_HI:
                        violations.append(
                            f"{fname} has a literal 0x{word_val:08x} (flash range) at 0x{insn_addr:08x}")

    if violations:
        print("check_core1_flash_free: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        return 1

    print(f"check_core1_flash_free: ok ({len(visited)} function(s) reachable from {entries}, all in SRAM)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
