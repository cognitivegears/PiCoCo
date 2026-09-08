#pragma once
/* PiCoCo breadboard bring-up wiring on Pico 2 (RP2350 module).
 * See docs/firmware-architecture.md sec 3.2 for the pin table and PIO rationale. */
#include "pico/stdlib.h"   /* pulls in the SDK's boards/pico2.h, defining PICO_DEFAULT_LED_PIN */

#define PIN_D0      0    /* D0..D7  = GP0..GP7 */
#define PIN_A0      8    /* A0..A13 = GP8..GP21 */
#define PIN_RW      22
#define PIN_OE_BUS  26
#define PIN_HALT    27   /* HALT_GATE: HIGH holds /HALT low (Q2 on) */
#define PIN_E       28
#define PIN_LED     PICO_DEFAULT_LED_PIN
