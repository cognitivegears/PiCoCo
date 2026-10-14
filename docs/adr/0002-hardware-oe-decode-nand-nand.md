# U10 /OE comes from hardware NAND-NAND decode, not firmware

A quad-NAND 74LVC00 (U15) derives `SEL_N = NAND(CTS_BUF, SCS_BUF)` and `OE_BUS_RAW = NAND(SEL_N, E_BUF)`, so the data buffer is enabled only while a cart address is selected and E is high. Gating on E closes a write-cycle contention window, and keeping the decode in hardware saves a GPIO the Pico 2 does not have; JP2 position 2-3 exists so a Plus-W build can take /OE from firmware GP31 instead. v2.3 chose NAND-NAND on a quad part over a 3-input AND (SN74LVC1G11); gates 3/4 are grounded and unused.
