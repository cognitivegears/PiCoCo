#pragma once
#include <stdint.h>
#include <stddef.h>

/* DriveWire wire-protocol error codes (pyDriveWire). */
#define DW_E_OK      0
#define DW_E_EOF     0xD3
#define DW_E_WRPROT  0xF2
#define DW_E_CRC     0xF3
#define DW_E_READ    0xF4
#define DW_E_WRITE   0xF5
#define DW_E_NOTRDY  0xF6

/* 16-bit sum of bytes (DriveWire block checksum). */
uint16_t dw_checksum(const uint8_t *p, size_t n);

/* Big-endian 3-byte LSN unpack. */
uint32_t dw_lsn_unpack(const uint8_t p[3]);
