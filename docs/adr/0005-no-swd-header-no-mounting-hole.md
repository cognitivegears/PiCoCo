# No SWD header and no mounting hole on the v2.3 board

`J_SWD` was dropped in v2.3 because its pads at header pins 41-43 collide with the Plus-W pad grid, and a flat-mounted Pico 2 cannot expose its own SWD castellations on this footprint. `MTG1` was dropped at the same time because the module's flat, horizontal placement leaves nowhere for a hole to clear. Debug is via TP1-TP8 and the USB console; a future shell-fit board gets holes at shell-dictated positions.
