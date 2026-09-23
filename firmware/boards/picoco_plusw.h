/*
 * PiCoCo SDK board header for the Waveshare RP2350B-Plus-W (RP2350B, 48 GPIO,
 * 16 MB flash). Modelled on the Pico SDK's boards/pico2.h and
 * boards/waveshare_core2350b.h — neither matches this module exactly, so
 * this repo carries its own. Selected via -DPICOCO_BOARD=plusw, which sets
 * PICO_BOARD=picoco_plusw and adds this directory to PICO_BOARD_HEADER_DIRS
 * (firmware/CMakeLists.txt). Not to be confused with firmware/boards/plusw.h,
 * PiCoCo's own pin map (PICOCO_BOARD_H), which includes this file indirectly
 * via pico/stdlib.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_PICOCO_PLUSW_H
#define _BOARDS_PICOCO_PLUSW_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define PICOCO_PLUSW

// --- RP2350 VARIANT ---
#define PICO_RP2350A 0   // RP2350B: 48 GPIO

// --- UART ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 0
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 1
#endif

// --- LED ---
#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 23   // LED2; never the Pico 2's GP25 (SCS_BUF here), see plusw.h
#endif
// no PICO_DEFAULT_WS2812_PIN

// --- FLASH ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
