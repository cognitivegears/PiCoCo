# PiCoCo Loader: lwasm builds PICOCO.BIN, ToolShed decb puts it on the .DSK
# with PICOCO.BAS. The CoCo-side manager's C sources are retired by #61.
DECB ?= decb

all: PICOCO.DSK

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

test: test_parse
	./test_parse

test_parse: test_parse.c parse.c parse.h
	cc -std=c99 -Wall -Wextra -fsanitize=address,undefined -o $@ test_parse.c parse.c

clean:
	rm -f PICOCO.BIN PICOCO.DSK *.o *.map *.lst *.s test_parse

.PHONY: all test clean
