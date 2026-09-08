#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t reads, writes, underrun, overrun; } becker_stats_t;
extern becker_stats_t becker_stats;

void   becker_init(void);            /* registers device 0x3F40..0x3F5F, read hook on 0x3F42, refreshes table */
void   becker_refresh(void);         /* rewrite table[0x3F41], table[0x3F42] from to_coco */
size_t becker_read(uint8_t *buf, size_t n);         /* core0: bytes the CoCo wrote */
size_t becker_write(const uint8_t *buf, size_t n);  /* core0: queue for CoCo; returns accepted count */
size_t becker_rx_avail(void);
size_t becker_tx_free(void);
void   becker_loopback_pump(void);   /* copy from_coco -> to_coco */
