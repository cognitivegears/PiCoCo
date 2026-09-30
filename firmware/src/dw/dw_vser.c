#include "dw_vser.h"
#include <stdio.h>
#include <string.h>

static const char OK_HDR[] = "OK command successful\n\r";

static void session_clear(dw_vser *v) {
    v->ch = 0; v->opens = 0;
    v->replied = v->closing = v->overflow = false;
    v->linelen = 0; v->qhead = v->qlen = 0;
}

void vser_init(dw_vser *v, vser_exec_fn exec, void *ctx) {
    memset(v, 0, sizeof(*v));
    v->exec = exec;
    v->exec_ctx = ctx;
}

void vser_reset(dw_vser *v) {
    session_clear(v);
    v->reject_ch = 0;
}

void vser_open(dw_vser *v, uint8_t ch) {
    if (ch < VSER_CH_MIN || ch > VSER_CH_MAX) return;
    if (v->ch == 0) {
        session_clear(v);
        v->ch = ch;
        v->opens = 1;
    } else if (v->ch == ch) {
        /* Client reopens after timeout without close; if reply already
         * queued, start fresh. Otherwise just increment opens count. */
        if (v->replied) {
            session_clear(v);
            v->ch = ch;
            v->opens = 1;
        } else {
            v->opens++;
        }
    } else {
        /* ponytail: one session at a time; a second channel gets an
         * immediate hangup. Per-port state when tcp/WiFi channels need it.
         * reject_ch holds one pending; two rejects before SERREAD lose first. */
        v->reject_ch = ch;
    }
}

void vser_close(dw_vser *v, uint8_t ch) {
    if (v->ch == 0 || ch != v->ch) return;
    if (v->opens > 0) v->opens--;
    if (v->opens == 0) session_clear(v);
}

static void queue_fail(dw_vser *v, int code, const char *msg) {
    char *h = (char *)v->q;
    /* snprintf with msg argument triggers -Wrestrict; build manually instead. */
    int n = snprintf(h, VSER_HDR_MAX - 2, "FAIL %03d ", (uint8_t)code);
    if (n < 0) n = 0;
    if (n > VSER_HDR_MAX - 82) n = VSER_HDR_MAX - 82;  /* room for 80 msg + \n\r */
    size_t msglen = strlen(msg);
    if (msglen > 80) msglen = 80;
    memcpy(h + n, msg, msglen);
    n += msglen;
    h[n++] = '\n';
    h[n++] = '\r';
    v->qhead = 0;
    v->qlen = (uint16_t)n;
}

static void run_line(dw_vser *v) {
    char *l = v->line;
    l[v->linelen] = '\0';
    bool overflow = v->overflow;
    v->linelen = 0;
    v->overflow = false;
    while (*l == ' ') l++;
    size_t n = strlen(l);
    while (n && l[n - 1] == ' ') l[--n] = '\0';
    if (!overflow && n == 0) return;
    v->replied = true;
    if (overflow) { queue_fail(v, 10, "line too long"); return; }
    if (!v->exec) { queue_fail(v, 255, "no command handler"); return; }
    char *body = (char *)v->q + VSER_HDR_MAX;
    size_t bodyn = 0;
    int rc = v->exec(v->exec_ctx, l, body, VSER_BODY_MAX, &bodyn);
    if (bodyn > VSER_BODY_MAX) bodyn = VSER_BODY_MAX;
    if (rc != 0) {
        if (bodyn >= VSER_BODY_MAX) bodyn = VSER_BODY_MAX - 1;
        body[bodyn] = '\0';
        queue_fail(v, rc, body);
        return;
    }
    size_t hl = sizeof(OK_HDR) - 1;
    memcpy(v->q + VSER_HDR_MAX - hl, OK_HDR, hl);
    v->qhead = (uint16_t)(VSER_HDR_MAX - hl);
    v->qlen = (uint16_t)(hl + bodyn);
}

void vser_write(dw_vser *v, uint8_t ch, const uint8_t *b, size_t n) {
    if (v->ch == 0 || ch != v->ch) return;
    for (size_t i = 0; i < n && !v->replied; i++) {
        uint8_t c = b[i];
        if (c == '\r') run_line(v);
        else if (c == '\n' || c == 0) continue;
        else if (c == 0x08) { if (v->linelen) v->linelen--; }
        else if (v->linelen < VSER_LINE_MAX - 1) v->line[v->linelen++] = (char)c;
        else v->overflow = true;
    }
}

static void pop(dw_vser *v, size_t n) {
    v->qhead = (uint16_t)(v->qhead + n);
    v->qlen = (uint16_t)(v->qlen - n);
    if (v->qlen == 0 && v->replied) v->closing = true;
}

void vser_serread(dw_vser *v, uint8_t out[2]) {
    out[0] = out[1] = 0;
    if (v->reject_ch) {
        out[0] = 0x10; out[1] = v->reject_ch;
        v->reject_ch = 0;
        return;
    }
    if (v->ch == 0) return;
    if (v->closing) {
        out[0] = 0x10; out[1] = v->ch;
        session_clear(v);
        return;
    }
    if (v->qlen == 0) return;
    if (v->qlen < 3) {   /* DW4 VSerial_MultiReadLimit */
        out[0] = (uint8_t)(v->ch + 1);
        out[1] = v->q[v->qhead];
        pop(v, 1);
        return;
    }
    out[0] = (uint8_t)(v->ch + 17);
    out[1] = (uint8_t)(v->qlen > 255 ? 255 : v->qlen);
}

size_t vser_serreadm(dw_vser *v, uint8_t ch, size_t n, uint8_t *out) {
    if (v->ch == 0 || ch != v->ch || n == 0 || n > v->qlen) return 0;
    memcpy(out, v->q + v->qhead, n);
    pop(v, n);
    return n;
}
