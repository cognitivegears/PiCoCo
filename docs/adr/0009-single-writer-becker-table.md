# Only core1's read hooks write the Becker entries of the bus table

`bus_table[0x3F41]` (status) and `[0x3F42]` (data) are written only by core1's read hooks after the CoCo has consumed a byte; core0 only pushes bytes into the queue. One writer per entry avoids a cross-core race on a table the PIO engine is reading in real time. Do not add a core0 path that writes those entries. Details: `docs/firmware-architecture.md` §6.3.
