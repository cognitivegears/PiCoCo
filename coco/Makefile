# PiCoCo manager: CMOC + LWTOOLS build of PICOCO.BIN, ToolShed decb for the .DSK.
CMOC ?= cmoc
DECB ?= decb
ORG  ?= 0x3800
SRCS  = main.c

all: PICOCO.DSK

PICOCO.BIN: $(SRCS) $(wildcard *.h)
	$(CMOC) --coco --org=$(ORG) -o $@ $(SRCS)

PICOCO.DSK: PICOCO.BIN PICOCO.BAS
	rm -f $@
	$(DECB) dskini $@
	$(DECB) copy -2 -b PICOCO.BIN $@,PICOCO.BIN
	$(DECB) copy -0 -a -t PICOCO.BAS $@,PICOCO.BAS

test:
	@echo "no host tests yet"

clean:
	rm -f PICOCO.BIN PICOCO.DSK *.o *.map *.lst *.s test_parse

.PHONY: all test clean
