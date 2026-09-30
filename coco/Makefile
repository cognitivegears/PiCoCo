# PiCoCo manager: CMOC + LWTOOLS build of PICOCO.BIN, ToolShed decb for the .DSK.
CMOC ?= cmoc
DECB ?= decb
ORG  ?= 0x3800
SRCS  = main.c ui.c dw.c parse.c boot.c hook.asm

all: PICOCO.DSK

PICOCO.BIN: $(SRCS) $(wildcard *.h)
	$(CMOC) --coco --org=$(ORG) --limit=7800 -o $@ $(SRCS)

PICOCO.DSK: PICOCO.BIN PICOCO.BAS
	rm -f $@
	$(DECB) dskini $@
	$(DECB) copy -2 -b PICOCO.BIN $@,PICOCO.BIN
	$(DECB) copy -0 -a -t PICOCO.BAS $@,PICOCO.BAS

test: test_parse
	./test_parse

test_parse: test_parse.c parse.c parse.h
	cc -std=c99 -Wall -Wextra -fsanitize=address,undefined -o $@ test_parse.c parse.c

clean:
	rm -f PICOCO.BIN PICOCO.DSK *.o *.map *.lst *.s test_parse

.PHONY: all test clean
