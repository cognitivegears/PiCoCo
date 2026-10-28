* PiCoCo-DOS: the board's own boot ROM (docs/ADDITIONAL_ROADMAP.md section 1).
* Skeleton: the DK header, the $C002 init, the DSKCON vectors, one RAM hook,
* a banner, then back into BASIC's cold start so the CoCo prints OK.
*
* Copyright (c) 2026 Nathan Byrd
* SPDX-License-Identifier: MIT
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, subject to the conditions in LICENSE at
* the repository root. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
* ANY KIND.
*
* Clean room: written from the cartridge protocol's interfaces (Extended
* BASIC looks for DK at $C000 and jumps to $C002; the RAM hooks are 3-byte
* slots from $015E; ML programs call the disk routine through [$C004] with
* its variables at [$C006]; a CoCo 3 patches a DK cart copy at $C0C6-$C0DB
* and $C8B4-$C8BE, coco/stub/manager.asm) and from watching the cold start
* run in XRoar. No BASIC ROM code is copied, and no address inside those
* ROMs is hard-coded: see GOECB and EXIT.

CHROUT  equ $A002               Color BASIC's console-out vector
RVEC4   equ $016A               the CONSOLE IN RAM hook: 3 bytes, RTS until a DOS claims it
DCOPC   equ $EA                 DSKCON variables: op, drive, track, sector, buffer (2 bytes), status
DCSTA   equ $F0

        org $C000
        fcc "DK"                the cartridge signature Extended BASIC looks for
        bra INIT                $C002: the entry on a cold start, and EXEC 49154 from BASIC (2 bytes: $C004 follows)
        fdb DSKCON              $C004: the disk routine, JSR [$C004]; a high byte of $D6 would
        fdb DCOPC               $C006: its variable block     move the CoCo 3 patch to $C0C6

INIT    ldx #BANNER
INIT1   lda ,x+
        beq INIT2
        jsr [CHROUT]
        bra INIT1
INIT2   lda #$7E                JMP opcode: the hook slot becomes JMP HOOK
        sta RVEC4
        ldx #HOOK
        stx RVEC4+1
        jmp EXIT

* Zeros to $C0D8: on a CoCo 3, Super Extended BASIC copies a DK cart to RAM
* and patches the copy as it would Disk BASIC 1.1 ($C0C6-$C0DB, $C8B4-$C8BE).
        rmb $C0D9-*
* The 3 bytes at $C0D9 are the patched exit: a CoCo 3 overwrites them with its
* own jump back into its cold start, so this jump is ours only on a CoCo 1/2.
EXIT    jmp GOECB

* CoCo 1/2: Extended BASIC's cold start got here with LBEQ $C002 after its
* DK compare, and the instruction after that branch is its no-cartridge path
* (its banner, the warm-start setup, OK). Find the branch in the ROM rather
* than hard-code where: $10 $27 then the offset that lands on $C002 from there.
* No match means no Extended BASIC, so we were EXECed from BASIC: return.
* shortcut: only the LBEQ form is known (Extended BASIC 1.0 and 1.1 in
* XRoar); add the form another version uses if its cold start ends in our RTS.
GOECB   ldx #$8000
GOECB1  ldd ,x
        cmpd #$1027             LBEQ
        bne GOECB2
        pshs x
        ldd #$C002-4
        subd ,s++               the offset an LBEQ $C002 here would carry
        cmpd 2,x
        bne GOECB2
        jmp 4,x
GOECB2  leax 1,x
        cmpx #$A000
        blo GOECB1
        rts

* The CONSOLE IN hook, installed to prove the slot is ours; it passes through.
HOOK    rts

* DSKCON: no drive yet, so every call reports NOT READY ($80) in DCSTA.
DSKCON  lda #$80
        sta DCSTA
        rts

BANNER  fcc "PICOCO-DOS 0.1"
        fcb 13,0

        rmb $E000-*
        end
