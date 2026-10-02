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
                          * Keep LOW (released) at boot; toggled ~500 Hz for 500 ms
                          * after the /HALT release when rom_cart_wanted() (main.c). */
#define PIN_LED     23   /* LED2, a plain GPIO; not brought out, so no carrier net. LED1 is on the
                          * radio's WL_GPIO0 (needs the CYW43 driver, unused). Never GP25: that is
                          * SCS_BUF on the pad grid. Source: Zephyr rp2350b_plus_w.dtsi led0 and
                          * arduino-pico issue #3297 (Waveshare schematic, continuity-checked). */

#define PICOCO_BOARD_PLUSW 1
#define PICOCO_HAVE_NET 1   /* RM2 radio: firmware/src/net/net.c is compiled in */
/* Pad-grid signals (all in sio_hw->gpio_in, bits < 32). The PIO engine
 * (bus_core1.c) uses /R/W and, through its helper in a GPIO-base-16 PIO block,
 * OE_BUS (GP40); E only in its calibration mode. PIN_OE_FW stays high (U10
 * enabled by OE_BUS only: JP2 1-2; 2-3 is not supported by the engine). GP34/GP42 (audio) are
 * left untouched: not driven, not pulled. */
#define PIN_CTS     24
#define PIN_SCS     25
#define PIN_E       26
#define PIN_Q       27
#define PIN_SLENB   28
#define PIN_A14     29
#define PIN_A15     30
#define PIN_OE_FW   31   /* output, active low; high = U10 disabled */
/* GP32 (NMI_DRV) is left untouched: not driven, not pulled. */
#define PICOCO_INPUT_MASK ((0xFFULL << PIN_D0) | (0x3FFFULL << PIN_A0) | (1ULL << PIN_RW) \
                           | (0x7FULL << 24) | (1ULL << PIN_OE_BUS))


/* On-flash FAT filesystem: same firmware headroom as the Pico 2 build, rest
 * of the Plus-W's 16 MB flash goes to the partition (see fs_flash.h). */
#define PICOCO_FS_OFFSET 0x180000u
#define PICOCO_FS_SIZE   0xE80000u   /* FAT12, 3712 clusters at 4 KB each (well under the ~4084 ceiling) */
