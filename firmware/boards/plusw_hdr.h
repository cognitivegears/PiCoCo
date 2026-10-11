#pragma once
/* PiCoCo wiring for a Waveshare RP2350B-Plus-W on 2x20 pin headers over a
 * v2.3.1 PCB: the module's underside flash keeps it from sitting flat there,
 * and on headers its 15 pad-grid pins reach nothing. Only the 40 header pins
 * are wired, the same nets as plusw.h. Selected with -DPICOCO_BOARD=plusw_hdr
 * (firmware/CMakeLists.txt).
 *
 * Differences from plusw.h:
 *  - E (GP26) floats, so the event SM ends a cycle on the OE_BUS rise
 *    instead (bus_event_ph / bus_sel_ph, bus_engine.pio).
 *  - No CART_DRV (GP33): `cart on/auto` are refused, as on a Pico 2.
 * The other pad-grid pins (PIN_CTS..PIN_OE_FW below, from plusw.h) stay
 * defined for the bare-module selftest (fake6809.c), which drives them on the
 * module itself; on the carrier they are inputs (or GP31, an output held high)
 * with no connection. */
#include "plusw.h"

#undef PIN_CART_DRV
#define PICOCO_PLUSW_HEADERS 1
