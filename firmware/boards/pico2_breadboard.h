#pragma once
/* PiCoCo wiring for a Raspberry Pi Pico 2 (RP2350A module) — the breadboard
 * bring-up rig and also the v2.3.1 PCB's Pico 2 build.
 * See docs/firmware-architecture.md sec 3.2 for the pin table and PIO rationale. */
#include "pico/stdlib.h"   /* pulls in the SDK's boards/pico2.h, defining PICO_DEFAULT_LED_PIN */

#define PIN_D0      0    /* D0..D7  = GP0..GP7 */
#define PIN_A0      8    /* A0..A13 = GP8..GP21 */
#define PIN_RW      22
#define PIN_OE_BUS  26
#define PIN_HALT    27   /* HALT_GATE: HIGH holds /HALT low (Q2 on) */
#define PIN_LED     PICO_DEFAULT_LED_PIN
/* GP28 is AUDIO_PWM by default as of v2.3.1 (JP3 1-2) — no sound firmware yet
 * (roadmap item 11), so it is left uninitialised: neither driven nor pulled.
 * GP26/GP8-22 are taken over by PIO1 during `bus selftest` only (fake6809.c). */

/* Pull-up input pins for main.c's gpio_setup(): D0-7, A0-13, /R/W, OE_BUS. */
#define PICOCO_INPUT_MASK ((0xFFULL << PIN_D0) | (0x3FFFULL << PIN_A0) | (1ULL << PIN_RW) | (1ULL << PIN_OE_BUS))


/* On-flash FAT filesystem: firmware occupies 0x000000..0x17FFFF, this
 * partition is the rest of the Pico 2's 4 MB flash (see fs_flash.h). */
#define PICOCO_FS_OFFSET 0x180000u
#define PICOCO_FS_SIZE   0x280000u   /* FAT12, 640 clusters at 4 KB each */
