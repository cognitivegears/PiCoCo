#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* DW4 virtual serial, command mode only (spec 2026-09-23 §4.1-4.2). One
 * session at a time on channels 1..13; wire behaviour follows DW4 Java
 * 4.3.3p DWVSerialPorts.serRead / DWVSerialPort. */
#define VSER_CH_MIN   1
#define VSER_CH_MAX   13
#define VSER_LINE_MAX 128
#define VSER_HDR_MAX  96
#define VSER_BODY_MAX 4096

/* Runs one command line. Returns 0 for OK with out[0..*outn) as the reply
 * body, or a DW4 result code with out[0..*outn) as a one-line message. */
typedef int (*vser_exec_fn)(void *ctx, const char *line, char *out, size_t cap, size_t *outn);

typedef struct {
    vser_exec_fn exec; void *exec_ctx;
    uint8_t ch, opens, reject_ch;
    bool replied, closing, overflow;
    char line[VSER_LINE_MAX]; uint16_t linelen;
    /* Reply queue: the body is written at q+VSER_HDR_MAX and the status
     * line is placed just before it, so neither is copied. */
    uint8_t q[VSER_HDR_MAX + VSER_BODY_MAX]; uint16_t qhead, qlen;
} dw_vser;

void   vser_init(dw_vser *v, vser_exec_fn exec, void *ctx);
void   vser_reset(dw_vser *v);                  /* DWINIT, RESET */
void   vser_open(dw_vser *v, uint8_t ch);       /* SERINIT, SS.Open */
void   vser_close(dw_vser *v, uint8_t ch);      /* SERTERM, SS.Close */
void   vser_write(dw_vser *v, uint8_t ch, const uint8_t *b, size_t n);
void   vser_serread(dw_vser *v, uint8_t out[2]);
size_t vser_serreadm(dw_vser *v, uint8_t ch, size_t n, uint8_t *out); /* n bytes, or 0 */
