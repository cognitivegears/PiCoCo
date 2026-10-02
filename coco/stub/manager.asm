* PiCoCo manager stub (spec 2026-10-01-rom-manager-design.md section 4).
* Thin client: the Pico's ui module decides what is on the screen. This ROM
* polls it with the last key, checks the reply's checksum, then carries out
* the action list: draw text, clear, or leave (jump / restart).
*
* Entry is $C002 on every path (the image starts with "DK"):
*   Extended BASIC cold start, or EXEC 49154 from Color BASIC.
* Rules: interrupts masked, no BASIC ROM call except POLCAT [$A000] (it is
* not hooked through RAM), never CLR a Becker register (CLR reads first).
* Y is the frame pointer for the whole session; nothing else may use Y.

BSTAT   equ $FF41
BDATA   equ $FF42
UICTL   equ $FF43               write $A5 begin session, $5A end
SCREEN  equ $0400
RSTSW   equ $71                 BASIC warm-start flag
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
FRAME   equ BUFSZ+10

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
        lda #$A5
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

* A = capability bits: 0 32K (a $7FFF write must take and not mirror $3FFF), 1 64K, 2 Extended BASIC, 4 CoCo 3
DETECT  clrb
        ldx $8000
        cmpx #$4558
        bne DET1
        orb #$04
DET1    lda $FFFE
        cmpa #$8C
        bne DET2
        orb #$12                CoCo 3, and 64K with it
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
DET3    tfr b,a
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
        leax NORESP,pcr
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
        lda #$60
ACLR1   sta ,u+
        cmpu #SCREEN+$0200
        blo ACLR1
        bra RUN1

* leave: copy LEAVER into the reply buffer and run it there, so nothing
* executes from the cart while the Pico swaps the ROM
ALEAVE  pshs a
        leax LEAVER,pcr
        tfr y,u
        ldb #LEAVEND-LEAVER
ALV1    lda ,x+
        sta ,u+
        decb
        bne ALV1
        puls a
        jmp ,y

* position-independent (no absolute references to its own labels; copied to the
* reply buffer, copy count < 256); A = action code ($10 jump, $11 cold, $13 warm, $16 CoCo 3 ROM-mode
* jump, $17 CoCo 3 cold, $18 CoCo 3 warm). Sends 'G', waits for $06.
LEAVER  tfr a,b
        lda #'G
        sta BDATA
        ldx #0
LV1     lda BSTAT
        bita #2
        bne LV2
        leax -1,x
        bne LV1
        bra LVFAIL
LV2     lda BDATA
        cmpa #$06
        bne LVFAIL
        lda #$5A
        sta UICTL
        leas FRAME,y
        cmpb #$10
        beq LVJMP
        cmpb #$13
        beq LVWARM
        cmpb #$16
        beq LVJMP3
        cmpb #$17
        beq LVCOLD3
        cmpb #$18
        beq LVWARM3
        clr RSTSW               11: cold restart
LVWARM  jmp [$FFFE]
LVJMP   andcc #$AF
        jmp $C000
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
LVWARM3 sta $FFD8
        jmp $8C1B
LVFAIL  leas FRAME,y
        jmp START
LEAVEND

* "PICOCO NOT RESPONDING" in screen codes
NORESP  fcb $50,$49,$43,$4F,$43,$4F,$60,$4E,$4F,$54,$60
        fcb $52,$45,$53,$50,$4F,$4E,$44,$49,$4E,$47,0

        rmb $E000-*
        end START
