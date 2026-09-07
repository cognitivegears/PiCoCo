#include "dw_util.h"

uint16_t dw_checksum(const uint8_t *p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += p[i];
    return (uint16_t)sum;
}

uint32_t dw_lsn_unpack(const uint8_t p[3]) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
