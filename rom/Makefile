# PiCoCo-DOS: lwasm builds the 8 KB cart image, smoke.py checks it on the host.
# `make xroar` also boots it headless in XRoar (needs the BASIC ROMs in ~/.xroar/roms).
# SPDX-License-Identifier: MIT
# picoco-dos.rom is committed, so on a fresh clone it is newer than the source:
# `check` assembles again and compares, then runs the smoke test every time.
all: picoco-dos.rom check

# a rejected image must not survive for the next make to accept
.DELETE_ON_ERROR:

picoco-dos.rom: picoco-dos.asm
	lwasm --raw -o $@ --list=picoco-dos.lst $<
	python3 smoke.py $@

check: picoco-dos.rom
	lwasm --raw -o $(or $(TMPDIR),/tmp)/picoco-dos.chk picoco-dos.asm
	cmp $(or $(TMPDIR),/tmp)/picoco-dos.chk $< || { echo "picoco-dos.rom is stale: rebuild and commit it"; exit 1; }
	python3 smoke.py $<

xroar: picoco-dos.rom
	python3 smoke.py --xroar $<

clean:
	rm -f picoco-dos.rom picoco-dos.lst

.PHONY: all check clean xroar
