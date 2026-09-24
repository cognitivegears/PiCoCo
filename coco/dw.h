#ifndef DW_H
#define DW_H
#include "parse.h"

#define PC_BAD     -1   /* reply was neither OK nor FAIL */
#define PC_TOOLONG -2
#define PC_TIMEOUT -3

/* Runs one PiCoCo console command over DW4 virtual serial channel 1.
 * Returns 0 (body = output) or the FAIL code (body = message), or PC_*. */
int picoco_cmd(const char *line, char *buf, u16 cap, char **body);
/* OP_READ of one 256-byte sector. 0 ok, DW error code, or PC_TIMEOUT. */
int dw_read_sector(u8 drive, u32 lsn, u8 *sec);
#endif
