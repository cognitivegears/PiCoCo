# Replay fixtures

Files here are `dw capture` recordings ("console.c" `console_capture`) of a
real DriveWire transaction: a stream of length-prefixed chunks, one per
`dw_feed`/`dw_send` call while the capture was on:

```
byte 0    dir       0 = bytes the server received (rx), 1 = bytes it sent (tx)
byte 1-2  len       chunk length, little-endian u16
byte 3.. len bytes  the chunk contents
```

`test_replay.c` reads every `*.cap` file in this directory, feeds the dir-0
(rx) chunks through a fresh `dw_server` (mounting `raw.dsk`, a 630-sector
image where sector `n`'s byte 0 is `n & 0xFF` and byte 1 is `n >> 8`, same as
`test_stack.c`), and checks the bytes the server sends back match the dir-1
(tx) chunks recorded in the file, byte for byte. It also requires
`stats.timeouts == 0` and `stats.unknown_op == 0`. This is a regression test:
if a protocol change alters the reply for a request captured here, the test
fails.

`picoco-host --replay <file>` (see `host/picoco_host.c`) plays a capture
file the same way: it feeds the dir-0 chunks into a live `dw_server` (with
`dw_tick` between each) and ignores the dir-1 chunks, then prints stats.

## Fixtures

- `readex_boot.cap`: one READEX (`0xD2`) of drive 0, LSN 9, against
  `raw.dsk`, mounted with `hdbdos off` (matches `coco_readex_sector` in
  `test_stack.c`).
- `trace_readex.txt`: not a capture file, a `trace dump 64` text log (see
  `tools/tracedump.py`) taken right after `coco_readex_sector` sends the
  READEX opcode + drive/LSN bytes (before the 256-byte data phase, whose
  hundreds of status/data polls would otherwise scroll the 5-byte request
  out of a 64-entry dump).

## Adding a fixture

1. In `test_stack.c`, temporarily bracket the transaction you want to
   capture with `console_exec("dw capture on NAME.cap")` /
   `console_exec("dw capture off")`.
2. Build and run `test_stack`; find the temp directory it used (printed by
   `mkdtemp`, or the newest `$TMPDIR/stackXXXXXX` directory) and copy
   `NAME.cap` from there into `tests/fixtures/`.
3. Remove the temporary capture calls from `test_stack.c`.
4. Rebuild; `test_replay.c` picks up any `*.cap` file in this directory
   automatically.
