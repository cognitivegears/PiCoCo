# PiCoCo-DOS: lwasm builds the 8 KB cart image, smoke.py checks it on the host.
# `make xroar` also boots it headless in XRoar (needs the BASIC ROMs in ~/.xroar/roms).
# SPDX-License-Identifier: MIT
all: picoco-dos.rom

# a rejected image must not survive for the next make to accept
.DELETE_ON_ERROR:

picoco-dos.rom: picoco-dos.asm
	lwasm --raw -o $@ --list=picoco-dos.lst $<
	python3 smoke.py $@

xroar: picoco-dos.rom
	python3 smoke.py --xroar $<

clean:
	rm -f picoco-dos.rom picoco-dos.lst

.PHONY: all clean xroar
