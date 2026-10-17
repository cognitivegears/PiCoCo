# PiCoCo Loader: lwasm builds PICOCO.BIN, ToolShed decb puts it on the .DSK
# with PICOCO.BAS. The Stub (stub/) is built too, and regenerates
# firmware/src/ui/manager_rom.h.
DECB ?= decb

all: PICOCO.DSK
	$(MAKE) -C stub

PICOCO.BIN: loader.asm
	lwasm --format=decb -o $@ --list=loader.lst $<

PICOCO.DSK: PICOCO.BIN PICOCO.BAS
	rm -f $@
	$(DECB) dskini $@
	$(DECB) copy -2 -b PICOCO.BIN $@,PICOCO.BIN
	$(DECB) copy -0 -a -t PICOCO.BAS $@,PICOCO.BAS

# 8 KB cart test ROM for a CoCo without Extended BASIC (carttest.asm header)
carttest.rom: carttest.asm
	lwasm --raw -o $@ $<

clean:
	rm -f PICOCO.BIN PICOCO.DSK *.o *.map *.lst *.s

.PHONY: all clean
