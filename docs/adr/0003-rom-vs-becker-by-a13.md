# Firmware tells ROM from Becker cycles by OE_BUS plus A13, not by /CTS and /SCS

Only `OE_BUS` reaches the Pico; /CTS and /SCS are not routed to it as separate signals. The ROM window ($C000-$FEFF, /CTS) and the Becker window ($FF40-$FF5F, /SCS) differ in A13, so one `IN PINS` capture of A0-A13 plus /R/W on contiguous GP8-GP22 classifies the cycle in a single PIO instruction. Routing both selects would cost two GPIOs the Pico 2 does not have; the price is that pin assignments are PIO-shaped and cannot move without editing `docs/firmware-architecture.md` in the same change.
