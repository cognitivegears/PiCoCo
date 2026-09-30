#include "dw.h"

/* Becker port. ponytail: fixed address; probe at runtime once the SDC
 * profile moves Becker to $FF5x (spec §9). */
#define BSTAT (*(volatile u8 *)0xFF41)
#define BDATA (*(volatile u8 *)0xFF42)
#define TICKS (*(volatile u16 *)0x0112)   /* Extended BASIC 60 Hz TIMER */
#define CH 1
#define FIRST_REPLY_TICKS 600             /* 10 s: fs new writes 161 KB of flash */
#define BYTE_TICKS 60

static void put(u8 b) { BDATA = b; }

static int get(u16 ticks)
{
    u16 t0 = TICKS;
    while (!(BSTAT & 0x02))
        if ((u16)(TICKS - t0) > ticks) return -1;
    return BDATA;
}

static void drain(void)
{
    while (BSTAT & 0x02) (void)BDATA;
}

int picoco_cmd(const char *line, char *buf, u16 cap, char **body)
{
    u16 n = (u16)strlen(line), got = 0, i, t0;
    int b1, b2, c;
    if (n > 120) return PC_TOOLONG;
    drain();
    put(0xC4); put(CH); put(0x29);                  /* SERSETSTAT SS.Open */
    put(0x64); put(CH); put((u8)(n + 1));           /* SERWRITEM line + CR */
    for (i = 0; i < n; i++) put((u8)line[i]);
    put(13);
    t0 = TICKS;
    for (;;) {
        put(0x43);                                  /* SERREAD */
        b1 = get(FIRST_REPLY_TICKS);
        if (b1 < 0) return PC_TIMEOUT;
        b2 = get(BYTE_TICKS);
        if (b2 < 0) return PC_TIMEOUT;
        if (b1 == 0x10 && b2 == CH) break;          /* hangup: reply complete */
        if (b1 == CH + 1) {
            if (got < cap - 1) buf[got++] = (char)b2;
        } else if (b1 == CH + 17) {
            put(0x63); put(CH); put((u8)b2);        /* SERREADM */
            for (i = 0; i < (u16)b2; i++) {
                c = get(BYTE_TICKS);
                if (c < 0) return PC_TIMEOUT;
                if (got < cap - 1) buf[got++] = (char)c;
            }
        } else if ((u16)(TICKS - t0) > FIRST_REPLY_TICKS) {
            return PC_TIMEOUT;
        }
    }
    buf[got] = '\0';
    return parse_reply(buf, body);
}

int dw_read_sector(u8 drive, u32 lsn, u8 *sec)
{
    int c, i;
    u16 sum = 0, want;
    drain();
    put(0x52); put(drive);
    put((u8)(lsn >> 16)); put((u8)(lsn >> 8)); put((u8)lsn);
    c = get(300); if (c < 0) return PC_TIMEOUT;
    if (c != 0) return c;
    c = get(BYTE_TICKS); if (c < 0) return PC_TIMEOUT;
    want = (u16)((u16)c << 8);
    c = get(BYTE_TICKS); if (c < 0) return PC_TIMEOUT;
    want |= (u16)c;
    for (i = 0; i < 256; i++) {
        c = get(BYTE_TICKS);
        if (c < 0) return PC_TIMEOUT;
        sec[i] = (u8)c;
        sum += (u8)c;
    }
    return sum == want ? 0 : 0xF3;   /* E_CRC */
}
