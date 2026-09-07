#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { uint8_t *buf; uint32_t mask; volatile uint32_t head, tail; } ring_t;
static inline void ring_init(ring_t *r, uint8_t *buf, uint32_t size_pow2) { r->buf = buf; r->mask = size_pow2 - 1; r->head = r->tail = 0; }
static inline uint32_t ring_count(const ring_t *r) { return (r->head - r->tail) & r->mask; }
static inline uint32_t ring_free(const ring_t *r) { return r->mask - ring_count(r); }
static inline bool ring_push(ring_t *r, uint8_t b) {
    uint32_t h = r->head, n = (h + 1) & r->mask;
    if (n == r->tail) return false;
    r->buf[h] = b; __atomic_store_n(&r->head, n, __ATOMIC_RELEASE); return true;
}
static inline bool ring_peek(const ring_t *r, uint8_t *b) { if (r->head == r->tail) return false; *b = r->buf[r->tail]; return true; }
static inline bool ring_pop(ring_t *r, uint8_t *b) {
    uint32_t t = r->tail; if (__atomic_load_n(&r->head, __ATOMIC_ACQUIRE) == t) return false;
    *b = r->buf[t]; __atomic_store_n(&r->tail, (t + 1) & r->mask, __ATOMIC_RELEASE); return true;
}
