* PiCoCo Loader (#60): enters the Manager from a running DOS.
* PICOCO.BAS does LOADM"PICOCO":EXEC. Runs from RAM, because a CoCo 1/2
* executes Disk BASIC from the cart and the cart is swapped away below.
* Asks the firmware for the manager (L + sum8 on the Becker port), then
* jumps into the Stub at $C002 with the mark it checks (X = $5043, A = $4C).
* BREAK in the Manager puts the DOS ROM back and returns here (Stub LVRET),
* and this returns to BASIC's EXEC: no DOS init runs, so RAM hooks survive.
BSTAT   equ $FF41
BDATA   equ $FF42
UICTL   equ $FF43               write $A5 begin session, $5A end
SCREEN  equ $0400

        org $3800
START   orcc #$50
        lda #$A5
        sta UICTL
        lda #'L
        sta BDATA               the request
        sta BDATA               and its sum8
        ldx #0                  ~1.3 s at 0.89 MHz
WAIT    lda BSTAT
        bita #2
        bne GOT
        leax -1,x
        bne WAIT
        bra FAIL
GOT     lda BDATA
        cmpa #$06
        bne FAIL
        lda $FFFE
        cmpa #$8C               CoCo 3: the DOS runs from a RAM copy, so select
        bne GO                  ROM mode (the cart is at $C002) and 0.89 MHz
        sta $FFD8               (the board cannot serve cart reads back to back)
        clr $FFDE
GO      ldx #$5043
        lda #$4C
        jsr $C002               back here on BREAK, the DOS ROM restored
        lda $FFFE
        cmpa #$8C
        bne DONE
        clr $FFDF               RAM mode again: Super ECB's copy of the DOS
DONE    andcc #$AF
        rts                     to EXEC
FAIL    lda #$5A
        sta UICTL
        leax NORESP,pcr
        ldu #SCREEN
FAIL1   lda ,x+
        beq DONE
        sta ,u+
        bra FAIL1
* "PICOCO NOT RESPONDING" in screen codes
NORESP  fcb $50,$49,$43,$4F,$43,$4F,$60,$4E,$4F,$54,$60
        fcb $52,$45,$53,$50,$4F,$4E,$44,$49,$4E,$47,0
        end START
