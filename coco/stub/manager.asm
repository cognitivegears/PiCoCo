* PiCoCo manager stub (spec 2026-10-01-rom-manager-design.md section 4).
* Thin client: the Pico's ui module decides what is on the screen. This ROM
* polls it with the last key, checks the reply's checksum, then carries out
* the action list: draw text, clear, or leave (jump, restart, return and
* type a line, or load a boot track and jump).
*
* Entry is $C002 on every path (the image starts with "DK"):
*   Extended BASIC cold start, EXEC 49154 from Color BASIC, or the Loader
*   (coco/loader.asm) from a running DOS, marked by X = $5043 and A = $4C.
* Rules: interrupts masked, no BASIC ROM call except POLCAT [$A000] (it is
* not hooked through RAM), never CLR a Becker register (CLR reads first).
* Y is the frame pointer for the whole session; nothing else may use Y.

BSTAT   equ $FF41
BDATA   equ $FF42
UICTL   equ $FF43               write $A5 begin session, $5A end
SCREEN  equ $0400
RSTSW   equ $71                 BASIC warm-start flag
RVEC4   equ $016A               CONSOLE IN hook (3-byte JMP in the RAM hook table)
CINBFL  equ $0070               console-in EOF flag
DEVNUM  equ $006F               current I/O device, 0 = keyboard
BUFSZ   equ 600                 largest reply the firmware sends (UI_REPLY_MAX)
RETRIES equ 5

* frame at Y: the reply buffer, then the variables
FLAGS   equ BUFSZ               bit 0 resend, bit 1 first poll (caps follow)
KEY     equ BUFSZ+1
CAPS    equ BUFSZ+2
LEN     equ BUFSZ+3             2 bytes
ACC     equ BUFSZ+5             2 bytes, running sum of the reply
CNT     equ BUFSZ+7             2 bytes
TRIES   equ BUFSZ+9
LOADER  equ BUFSZ+10            1 = entered from the Loader (caps bit 3; 13/18 return into it)
SECU    equ BUFSZ+11            2 bytes: where the sector being read goes (15 retries restart here)
ARGS    equ BUFSZ+13            9 bytes: the operands of 15 (drive, LSN, count, load, jump), or 14's hook address
FRAME   equ BUFSZ+22

        org $C000
        fcc "DK"
* $C002 is the entry on every path. Only a jump lives here: on a CoCo 3,
* Super Extended BASIC copies a DK cart to RAM and patches the copy as if it
* were Disk BASIC (coco3.rom $C321): 3 bytes at $C0D9 and 11 NOPs at $C8B4,
* or at $C0C6 if the byte at $C004 is $D6. Nothing of ours may sit there, and
* $C004 must never be $D6 (it is the low byte of BEGIN's address, the jump target).
START   jmp BEGIN

        rmb $C100-*
BEGIN   orcc #$50
        leas -FRAME,s
        tfr s,y
        clr LOADER,y
        cmpx #$5043             the Loader's mark, checked before any register is touched
        bne BEG1
        cmpa #$4C
        bne BEG1
        inc LOADER,y
BEG1    lda #$A5
        sta UICTL
        jsr DRAIN
        jsr DETECT
        sta CAPS,y
        lda #2
        sta FLAGS,y
        clr KEY,y
MAIN    jsr POLL
KEYW    pshs y
        jsr [$A000]
        puls y                  PULS leaves the flags alone: Z is still POLCAT's
        beq KEYW
        sta KEY,y
        clr FLAGS,y
        bra MAIN

* A = capability bits: 0 32K (a $7FFF write must take and not mirror $3FFF), 1 64K, 2 Extended BASIC,
* 4 CoCo 3, 6-7 CoCo 3 RAM size (MEM3: 00 128K, 01 512K, 10 1 MB, 11 2 MB; 0 unless bit 4)
DETECT  clrb
        ldx $8000
        cmpx #$4558
        bne DET1
        orb #$04
DET1    lda $FFFE
        cmpa #$8C
        bne DET2
        orb #$12                CoCo 3, and 64K with it
        pshs b
        jsr MEM3
        ora ,s+
        tfr a,b
DET2    pshs b
        lda $7FFF
        ldb $3FFF
        coma
        sta $7FFF
        cmpa $7FFF              the write took?
        bne DET2N
        cmpb $3FFF              and it was not a mirror of $3FFF?
        bne DET2N
        coma
        sta $7FFF
        puls b
        orb #$01
        bra DET3
DET2N   coma
        sta $7FFF               put back whatever we changed
        puls b
DET3    tst LOADER,y
        beq DET4
        orb #$08                bit 3: Disk BASIC live, entered from the Loader
DET4    tfr b,a
        rts

* CoCo 3 RAM size, in caps bits 6-7: A = $00 128K, $40 512K, $80 1 MB, $C0 2 MB.
* MMU block numbers wrap at the amount of RAM fitted, so block $00 is the same
* RAM as $30 on 128K, as $40 on 512K, as $80 on 1 MB. Tested through slot 2
* ($4000-$5FFF, register $FFA2): RAM in ROM mode too (a Loader entry), while our
* code runs at $C100 (slot 6), the stack is in slot 3 and the Loader in slot 1.
* If the MMU is off or task 1 is selected the writes do not move the CPU's map,
* the probe reads its own cell back and the answer is 2M: display only, no damage.
MEM3    lda $FFA2
        pshs a                  what was mapped there; put back at the end
        lda $4000
        pshs a                  the byte there now, to check that restore
        ldb #$30
        bsr SAME
        beq MEM128
        ldb #$40
        bsr SAME
        beq MEM512
        ldb #$80
        bsr SAME
        beq MEM1M
        lda #$C0
        bra MEM3X
MEM1M   lda #$80
        bra MEM3X
MEM512  lda #$40
        bra MEM3X
MEM128  clra
MEM3X   ldb 1,s
        stb $FFA2
        ldb ,s                  the same byte again means the read-back value was the
        cmpb $4000              real block number; otherwise use BASIC's default map
        leas 2,s
        beq MEM3Y
        ldb #$3A
        stb $FFA2
MEM3Y   rts

* Z set if MMU blocks $00 and B are the same RAM. Restores both bytes it
* touches (the saved bytes are the originals, so the order does not matter).
SAME    stb ,-s                 S: blk
        clra
        sta $FFA2
        lda $4000
        sta ,-s                 S: byte0, blk
        lda 1,s
        sta $FFA2
        lda $4000
        sta ,-s                 S: byteB, byte0, blk
        ldb #$5A
        bsr PROBE
        bne SAME9
        ldb #$A5
        bsr PROBE
SAME9   tfr cc,b                keep the answer (and the interrupt masks) while restoring
        lda 2,s
        sta $FFA2
        lda ,s
        sta $4000
        clra
        sta $FFA2
        lda 1,s
        sta $4000
        leas 3,s
        tfr b,cc
        rts

* Write B to block blk, then see whether block $00 shows it. Z set if it does.
* Called from SAME: the stack is return (2), byteB, byte0, blk.
PROBE   lda 4,s
        sta $FFA2
        stb $4000
        clra
        sta $FFA2
        cmpb $4000
        rts

* discard whatever the Pico had queued
DRAIN   lda BSTAT
        bita #2
        beq DRAINX
        lda BDATA
        bra DRAIN
DRAINX  rts

* one byte in A, carry set on timeout (~1.3 s at 0.89 MHz)
GETB    pshs x
        ldx #0
GETB1   lda BSTAT
        bita #2
        bne GETB2
        leax -1,x
        bne GETB1
        orcc #1
        puls x,pc
GETB2   lda BDATA
        andcc #$FE
        puls x,pc

* send A to the Pico and add it to the poll checksum in B
PUTB    sta BDATA
        pshs a
        addb ,s+
        rts

* send a poll, receive and check the reply, run it. A bad reply is asked for
* again with the resend flag; after RETRIES show the banner and wait for a
* key, then start a fresh session.
POLL    lda #RETRIES
        sta TRIES,y
POLL1   clrb
        lda #'P
        jsr PUTB
        lda FLAGS,y
        jsr PUTB
        lda KEY,y
        jsr PUTB
        lda FLAGS,y
        bita #2
        beq POLL2
        lda CAPS,y
        jsr PUTB
POLL2   stb BDATA
        jsr GETB
        bcs POLLBAD
        sta LEN,y
        jsr GETB
        bcs POLLBAD
        sta LEN+1,y
        ldd LEN,y
        cmpd #BUFSZ
        bhi POLLBAD
        std CNT,y
        ldx #0
        stx ACC,y
        tfr y,x
POLL3   ldd CNT,y
        beq POLL4
        subd #1
        std CNT,y
        jsr GETB
        bcs POLLBAD
        sta ,x+
        tfr a,b
        clra
        addd ACC,y
        std ACC,y
        bra POLL3
POLL4   jsr GETB
        bcs POLLBAD
        tfr a,b
        jsr GETB
        bcs POLLBAD
        exg a,b
        cmpd ACC,y
        beq RUNLIST
POLLBAD jsr DRAIN
        lda FLAGS,y
        ora #1
        sta FLAGS,y
        dec TRIES,y
        lbne POLL1
BANNER  leax NORESP,pcr
        ldu #SCREEN
POLLB1  lda ,x+
        beq POLLB2
        sta ,u+
        bra POLLB1
POLLB2  pshs y
        jsr [$A000]
        puls y
        beq POLLB2
        lda #$A5
        sta UICTL
        lda #2
        sta FLAGS,y
        clr KEY,y
        lbra POLL

* run the action list in the buffer: X walks it, CNT holds the end address
RUNLIST tfr y,x
        ldd LEN,y
        leau d,x
        stu CNT,y
RUN1    cmpx CNT,y
        bhs RUNX
        lda ,x+
        beq RUNX
        cmpa #$01
        beq ATEXT
        cmpa #$02
        beq ACLR
        cmpa #$10
        bhs ALEAVE
RUNX    rts

* 01 offset(2) length(1) bytes: screen codes straight to video RAM
ATEXT   ldd ,x++
        cmpd #$0200
        bhs RUNX
        addd #SCREEN
        tfr d,u
        ldb ,x+
        beq RUN1
ATEXT1  cmpu #SCREEN+$0200
        bhs RUNX
        lda ,x+
        sta ,u+
        decb
        bne ATEXT1
        bra RUN1

* 02: clear to spaces (screen code $60)
ACLR    ldu #SCREEN
        stu >$88                BASIC's CURPOS: a launched program prints from the top-left
        lda #$60
ACLR1   sta ,u+
        cmpu #SCREEN+$0200
        blo ACLR1
        bra RUN1

* leave: copy LEAVER into the reply buffer and run it there, so nothing
* executes from the cart while the Pico swaps the ROM. 14 and 15 carry
* operands in the buffer, saved first: LEAVER is copied over them.
ALEAVE  pshs a
        cmpa #$15
        bne ALV14Q
        leau ARGS,y             15: the nine operands
        ldb #9
ALVC    lda ,x+
        sta ,u+
        decb
        bne ALVC
        bra ALV0
ALV14Q  cmpa #$14
        bne ALV0
        leau -HOOKSZ-32,y       14: the hook and its text go below the frame (S is at Y: the
        stu ARGS,y              32 bytes clear the pushes until the leave); BASIC's stack
        pshs x                  does not reach them before the line is typed
        leax HOOK,pcr
        ldb #HTEXT
ALVH    lda ,x+
        sta ,u+
        decb
        bne ALVH
        puls x
        ldb ,x+                 length, then the text (ends in CR); a NUL after it
ALVT    lda ,x+
        sta ,u+
        decb
        bne ALVT
        clr ,u
ALV0    leax LEAVER,pcr
        tfr y,u
        ldb #LEAVEND-LEAVER
ALV1    lda ,x+
        sta ,u+
        decb
        bne ALV1
        puls a
        jmp ,y

* position-independent (no absolute references to its own labels; copied to the
* reply buffer, copy count < 256); A = action code ($10 jump, $11 cold, $12 DOS init,
* $13 warm, $14 warm and type, $15 boot track, $16 CoCo 3 ROM-mode jump, $17 CoCo 3
* cold, $18 CoCo 3 warm). Sends 'G', waits for $06.
LEAVER  tfr a,b
        lda #'G
        sta BDATA
        ldx #0
LV1     lda BSTAT
        bita #2
        bne LV2
        leax -1,x
        bne LV1
        lbra LVFAIL
LV2     lda BDATA
        cmpa #$06
        lbne LVFAIL
        lda #$5A
        sta UICTL
        cmpb #$15
        beq LV15
        leas FRAME,y
        cmpb #$10
        beq LVJMP
        cmpb #$12
        beq LV12
        cmpb #$13
        beq LVWARM
        cmpb #$14
        beq LV14
        cmpb #$16
        beq LVJMP3
        cmpb #$17
        beq LVCOLD3
        cmpb #$18
        beq LVWARM3
        clr RSTSW               11: cold restart
LVWARM  tst LOADER,y
        bne LVRET
        jmp [$FFFE]
LVJMP   andcc #$AF
        jmp $C000
LV12    andcc #$AF              12: the swapped DOS ROM's init, as EXEC &HC002 would
        jmp $C002
* 14: arm the hook copied below the frame (ARGS holds its address): save
* RVEC4 into it, point it at its text, then return into the Loader as 13.
LV14    ldu ARGS,y
        ldd RVEC4
        std HSAVE,u
        lda RVEC4+2
        sta HSAVE+2,u
        leax HTEXT,u
        stx HPTR,u
        lda #$7E
        sta RVEC4
        stu RVEC4+1
        bra LVWARM
* 15: no ROM swap, so this cart is still here and the frame is still needed
LV15    jmp BOOT15
* CoCo 3 exits. All three first select 0.89 MHz ($FFD8): the board cannot
* serve cartridge reads on consecutive cycles at 1.79 MHz, and the CoCo 3's
* DK check and cart copy are exactly that (bench 2026-10-02).
LVJMP3  sta $FFD8
        lda #$CC                16K internal + 16K cartridge ROM, as $8C28 does
        sta $FF90
        sta $FFDE               ROM mode
        jmp $C000
LVCOLD3 sta $FFD8
        clr $FEED               INT.FLAG: not $55 forces the init's cold path, which recopies the cart
        clr RSTSW
        jmp $8C1B
LVWARM3 tst LOADER,y
        bne LVRET
        sta $FFD8
        jmp $8C1B
* 13 or 18 on a Loader entry: the previous ROM is back in the window; return
* into the Loader, which returns to BASIC. No DOS init runs on this path.
LVRET   rts
LVFAIL  ldx #0
        tst LOADER,y
        beq LVFAIL1
        ldx #$5043              re-enter as from the Loader: its frame is still below ours
        lda #$4C
LVFAIL1 leas FRAME,y
        jmp START
LEAVEND

* RVEC4 hook (the logic of coco/hook.asm), position-independent: copied
* below the frame with its text (offset HTEXT), where it outlives the
* session until BASIC has read the CR. Each CONSOLE IN call from the line
* input is answered with the next character; the CR puts the saved vector
* back. B, X and U are kept; a NUL is handed over as CR so the text's end
* is never run past; another device (DEVNUM not 0) passes straight through.
HOOK    pshs b,x
        ldb DEVNUM
        bne HPASS
        clr CINBFL
        ldx HPTRL,pcr
        lda ,x+
        stx HPTRL,pcr
        tsta
        bne HNZ
        lda #13
HNZ     cmpa #13
        bne HOUT
        ldx HSAVEL,pcr
        stx RVEC4
        ldb HSAVEL+2,pcr
        stb RVEC4+2
HOUT    puls b,x
        leas 2,s                drop the return into $A176 (the keyboard path)...
        rts                     ...and return to $A173 with A = the character, as HDB-DOS's own handler does
HPASS   puls b,x
        jmp HSAVEL,pcr          the saved 3 bytes are the original JMP: run it in place
HPTRL   fdb 0
HSAVEL  fcb 0,0,0
HOOKEND
HPTR    equ HPTRL-HOOK
HSAVE   equ HSAVEL-HOOK
HTEXT   equ HOOKEND-HOOK
HOOKSZ  equ HTEXT+64            room for the text (the firmware sends under 48 bytes)

* 15: load the sectors named at ARGS (drive, LSN, count, load address, jump
* address) with plain DriveWire OP_READ, the session over and the Becker
* port the server's again. Each sector is checksummed and retried up to
* RETRIES times; a failure shows the banner and starts a fresh session.
BOOT15  ldu ARGS+5,y
B15S    stu SECU,y
        lda #RETRIES
        sta TRIES,y
B15R    ldu SECU,y
        jsr DRAIN
        lda #$52                OP_READ drive lsn(3)
        sta BDATA
        lda ARGS,y
        sta BDATA
        lda ARGS+1,y
        sta BDATA
        lda ARGS+2,y
        sta BDATA
        lda ARGS+3,y
        sta BDATA
        jsr GETB
        lbcs B15BAD
        tsta                    error code: anything but 0 is a failed read
        lbne B15BAD
        jsr GETB
        lbcs B15BAD
        sta ACC,y
        jsr GETB
        lbcs B15BAD
        sta ACC+1,y
        clr LEN,y
        clr LEN+1,y
        clrb                    256 bytes
B15L    jsr GETB
        lbcs B15BAD
        sta ,u+
        adda LEN+1,y
        sta LEN+1,y
        bcc B15N
        inc LEN,y
B15N    decb
        bne B15L
        ldd LEN,y
        cmpd ACC,y
        lbne B15BAD
        inc ARGS+3,y            next LSN
        bne B15X
        inc ARGS+2,y
        bne B15X
        inc ARGS+1,y
B15X    dec ARGS+4,y
        lbne B15S
        ldx ARGS+7,y
        lda $FFFE
        cmpa #$8C
        bne B15GO
* CoCo 3: the DOS command runs the boot track in RAM mode; this code may be
* in the cart (Loader entry), so the switch runs from the reply buffer
        tfr y,u
        ldd #$B7FF              sta $FFDF
        std ,u++
        lda #$DF
        sta ,u+
        ldd #$6E84              jmp ,x
        std ,u
        leas FRAME,y
        jmp ,y
B15GO   leas FRAME,y
        jmp ,x
B15BAD  dec TRIES,y
        lbne B15R
        jmp BANNER

* "PICOCO NOT RESPONDING" in screen codes
NORESP  fcb $50,$49,$43,$4F,$43,$4F,$60,$4E,$4F,$54,$60
        fcb $52,$45,$53,$50,$4F,$4E,$44,$49,$4E,$47,0

        rmb $E000-*
        end START
