* PiCoCo cart test ROM for a CoCo with no Extended BASIC (TEST_PLAN I).
* Load with `rom load carttest.rom`, start with EXEC 49152.
*
* Runs from the cart ($C000), so every opcode fetch is a ROM read through
* U10, and reads drive 0 LSN 0-629 over the Becker port with DriveWire
* OP_READ in a loop. One line per pass:
*   PASS nnnn SUM ssss ERR eeee
* SUM is the 16-bit byte sum of the whole disk: same every pass, and equal
* to the sum of the image file. A bad sector prints Ecc llll (cc = error,
* F3 checksum, FF timeout; llll = LSN). Any key ends at the next pass.
* Needs only Color BASIC: CHROUT [$A002], POLCAT [$A000].

BSTAT   equ $FF41
BDATA   equ $FF42
NSEC    equ 630

* variables: cassette buffer, unused here
PASS    equ $01DA
ERRS    equ $01DC
SUM     equ $01DE
LSN     equ $01E0
WANT    equ $01E2
SSUM    equ $01E4

        org $C000
START   ldx #0
        stx PASS
        stx ERRS
        leax BANNER,pcr
        jsr PSTR
PLOOP   ldx #0
        stx SUM
        stx LSN
SLOOP   jsr RDSEC
        tsta
        beq SOK
        pshs a
        ldx ERRS
        leax 1,x
        stx ERRS
        lda #'E
        jsr PCH
        puls a
        jsr PHEX2
        lda #$20
        jsr PCH
        ldd LSN
        jsr PHEX4
        lda #13
        jsr PCH
        jsr QUIET
SOK     ldx LSN
        leax 1,x
        stx LSN
        cmpx #NSEC
        blo SLOOP
        ldx PASS
        leax 1,x
        stx PASS
        leax MPASS,pcr
        jsr PSTR
        ldd PASS
        jsr PHEX4
        leax MSUM,pcr
        jsr PSTR
        ldd SUM
        jsr PHEX4
        leax MERR,pcr
        jsr PSTR
        ldd ERRS
        jsr PHEX4
        lda #13
        jsr PCH
        jsr [$A000]
        beq PLOOP
        rts

* print the zero-terminated string at X
PSTR    lda ,x+
        beq PSTRX
        jsr PCH
        bra PSTR
PSTRX   rts

PHEX4   jsr PHEX2
        tfr b,a
PHEX2   pshs a
        lsra
        lsra
        lsra
        lsra
        jsr PNIB
        puls a
PNIB    anda #$0F
        adda #$90
        daa
        adca #$40
        daa
PCH     pshs d,x,y,u
        jsr [$A002]
        puls d,x,y,u,pc

* read sector LSN of drive 0. A = 0 ok (sector sum added to SUM),
* else the server's error code, $F3 checksum mismatch, $FF timeout.
RDSEC   lda BSTAT
        bita #2
        beq RDGO
        lda BDATA
        bra RDSEC
* never CLR BDATA: CLR reads the address first and eats a byte
RDGO    lda #$52
        sta BDATA
        clra
        sta BDATA
        sta BDATA
        lda LSN
        sta BDATA
        lda LSN+1
        sta BDATA
        jsr GETB
        bcs RDTMO
        tsta
        bne RDX
        jsr GETB
        bcs RDTMO
        sta WANT
        jsr GETB
        bcs RDTMO
        sta WANT+1
        ldx #0
        stx SSUM
        clrb
RDL     jsr GETB
        bcs RDTMO
        pshs b
        tfr a,b
        clra
        addd SSUM
        std SSUM
        puls b
        decb
        bne RDL
        ldd SSUM
        cmpd WANT
        bne RDBAD
        addd SUM
        std SUM
        clra
RDX     rts
RDBAD   lda #$F3
        rts
RDTMO   lda #$FF
        rts

* after an error: discard bytes until the port has been idle ~0.5 s, so the
* rest of a reply is gone and the server's 250 ms op timeout has reset it.
* Without this one slip desyncs every later sector.
QUIET   ldx #$6000
QUIET1  lda BSTAT
        bita #2
        beq QUIET2
        lda BDATA
        bra QUIET
QUIET2  leax -1,x
        bne QUIET1
        rts

* one Becker byte in A, carry set on timeout (~1.3 s at 0.89 MHz)
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

BANNER  fcc "PICOCO CART TEST, KEY TO STOP"
        fcb 13,0
MPASS   fcc "PASS "
        fcb 0
MSUM    fcc " SUM "
        fcb 0
MERR    fcc " ERR "
        fcb 0

        rmb $E000-*
        end START
