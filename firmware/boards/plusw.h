#pragma once
/* PiCoCo wiring for a Waveshare RP2350B-Plus-W soldered flat on the v2.3.1
 * PCB. See docs/firmware-architecture.md sec 3.2.1 and docs/hardware-design.md
 * sec 3.2 for the header pin table and the pad-grid map this is drawn from.
 * Selected with -DPICOCO_BOARD=plusw (firmware/CMakeLists.txt). */
#include "pico/stdlib.h"   /* pulls in boards/picoco_plusw.h (PICO_BOARD=picoco_plusw) */

#define PIN_D0      0    /* D0..D7  = GP0..GP7, same header pins as the Pico 2 */
#define PIN_A0      8    /* A0..A13 = GP8..GP21 */
#define PIN_RW      22
#define PIN_OE_BUS  40   /* header pin 31 = GP40 on this module (GP26 on a Pico 2) */
#define PIN_HALT    41   /* header pin 32 = GP41; HIGH holds /HALT low (Q2 on) */
#define PIN_CART_DRV 33  /* Q4 gate (populated, v2.3.1): drive HIGH to assert /CART.
                          * Keep LOW (released) at boot — no firmware /CART pulse yet
                          * (roadmap item 12). */
/* No PIN_LED: the Plus-W's LED pin is unverified — the pad-grid GP25 that a
 * naive port of PICO_DEFAULT_LED_PIN would reach is SCS_BUF, a U13 buffer
 * output, not an LED (see hardware-design.md sec 3.2). LED code in main.c and
 * plat_pico.c is #ifdef PIN_LED'd out without this define.
 * ponytail: add PIN_LED here once the physical module's LED pin is confirmed
 * safe to drive. */

/* Pad-grid inputs, capture only (no firmware use yet): GP24 CTS_BUF, GP25
 * SCS_BUF, GP26 E_BUF, GP27 Q_BUF, GP28 SLENB_BUF, GP29 A14_BUF, GP30
 * A15_BUF. GP31 (OE_FW), GP32 (NMI_DRV) and the audio pins (GP34, GP42) are
 * left untouched: not driven, not pulled. */
#define PICOCO_INPUT_MASK ((0xFFULL << PIN_D0) | (0x3FFFULL << PIN_A0) | (1ULL << PIN_RW) \
                           | (0x7FULL << 24) | (1ULL << PIN_OE_BUS))

/* core1's raw OE_BUS poll (bus_core1.c): GP40 is bit 8 of sio_hw->gpio_hi_in
 * (GP32..GP47's register), not sio_hw->gpio_in. */
#define BUS_OE_REG  gpio_hi_in
#define BUS_OE_MASK (1u << (PIN_OE_BUS - 32))

/* On-flash FAT filesystem: same firmware headroom as the Pico 2 build, rest
 * of the Plus-W's 16 MB flash goes to the partition (see fs_flash.h). */
#define PICOCO_FS_OFFSET 0x180000u
#define PICOCO_FS_SIZE   0xE80000u   /* FAT12, 3712 clusters at 4 KB each (well under the ~4084 ceiling) */
