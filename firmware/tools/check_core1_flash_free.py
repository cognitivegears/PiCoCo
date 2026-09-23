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

Run as a POST_BUILD step on the `picoco` target (see firmware/CMakeLists.txt).
stdlib only; uses arm-none-eabi-nm/objdump via the paths CMake's toolchain
already found (CMAKE_NM / CMAKE_OBJDUMP).

ponytail: only follows bl/b/b.w/b.n (calls and direct/tail-call jumps).
Conditional branches (beq, bne, ...) are how the compiler expresses local
control flow (loops, if/else) and stay within the same function in practice,
so they're not traced. Indirect calls (blx <reg>) can't be resolved from a
branch target address at all; those are covered by listing the callee as its
own entry point instead (see discover_hook_symbols), not by tracing the call
site.
"""
import re
import subprocess
import sys
import glob

SRAM_LO = 0x20000000
SRAM_HI = 0x20082000  # RP2350: RAM (512K) + SCRATCH_X + SCRATCH_Y, see pico-sdk default_locations.ld

ALWAYS_ENTRIES = ["bus_core1_main"]
HOOK_RE = re.compile(r"bus_add_read_hook\([^,]+,\s*([A-Za-z_]\w*)\s*\)")
BRANCH_LINE_RE = re.compile(r"^\s*([0-9a-fA-F]+):\s+([A-Za-z][A-Za-z0-9.]*)\s+([0-9a-fA-F]+)\s*<([^>]+)>")
BRANCH_MNEMONICS = {"b", "b.n", "b.w", "bl", "bl.w"}


def discover_hook_symbols(repo_root):
    names = set()
    for path in glob.glob(f"{repo_root}/firmware/src/**/*.c", recursive=True):
        with open(path, encoding="utf-8") as f:
            names.update(HOOK_RE.findall(f.read()))
    return sorted(names)


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def load_functions(nm, elf):
    """name -> (addr, size) for defined text (code) symbols."""
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
        if typ.lower() != "t":  # text/code symbols only
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
        addr, size = funcs[fname]
        if not (SRAM_LO <= addr < SRAM_HI):
            violations.append(f"{fname} (0x{addr:08x}) is not in SRAM")
            continue
        hi = addr + size if size else addr + 1
        for line in lines:
            m = BRANCH_LINE_RE.match(line)
            if not m:
                continue
            insn_addr = int(m.group(1), 16)
            if not (addr <= insn_addr < hi):
                continue
            if m.group(2) not in BRANCH_MNEMONICS:
                continue
            target_addr = int(m.group(3), 16)
            target_name = m.group(4).split("+")[0]
            if not (SRAM_LO <= target_addr < SRAM_HI):
                violations.append(f"{fname} branches to {target_name} (0x{target_addr:08x}), outside SRAM")
            if target_name not in visited:
                queue.append(target_name)

    if violations:
        print("check_core1_flash_free: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        return 1

    print(f"check_core1_flash_free: ok ({len(visited)} function(s) reachable from {entries}, all in SRAM)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
