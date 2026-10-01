# PiCoCo manager stub: 8 KB cart image, plus the C header the firmware links.
HDR = ../../firmware/src/ui/manager_rom.h

all: manager.rom $(HDR)

manager.rom: manager.asm
	lwasm --raw -o $@ --list=manager.lst $<

$(HDR): manager.rom
	xxd -i -n manager_rom $< | sed 's/^unsigned/static const unsigned/' > $@

clean:
	rm -f manager.rom manager.lst

.PHONY: all clean
