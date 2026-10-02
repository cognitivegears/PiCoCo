# PiCoCo manager stub: 8 KB cart image, plus the C header the firmware links.
HDR = ../../firmware/src/ui/manager_rom.h

all: manager.rom $(HDR)

manager.rom: manager.asm
	lwasm --raw -o $@ --list=manager.lst $<
	@python3 -c "import sys; d=open('$@','rb').read(); \
	ok=len(d)==8192 and not any(d[0xC6:0xDC]) and not any(d[0x8B4:0x8BF]) and d[4]!=0xD6; \
	ok or sys.exit('manager.rom: must be 8192 bytes with zeros at \$$C0C6-\$$C0DB and \$$C8B4-\$$C8BE and byte 4 != \$$D6 (CoCo 3 Super ECB patches a DK cart copy there)')"

$(HDR): manager.rom
	xxd -i -n manager_rom $< | sed 's/^unsigned/static const unsigned/' > $@

clean:
	rm -f manager.rom manager.lst

.PHONY: all clean
