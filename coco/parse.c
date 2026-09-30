#include "parse.h"

static const u16 mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static char upc(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int cmp_ci(const char *a, const char *b)
{
    while (*a && upc(*a) == upc(*b)) { a++; b++; }
    return (int)(u8)upc(*a) - (int)(u8)upc(*b);
}

int ends_with_ci(const char *s, const char *suf)
{
    u16 a = (u16)strlen(s), b = (u16)strlen(suf);
    if (b > a) return 0;
    return cmp_ci(s + a - b, suf) == 0;
}

/* "rom boot"/"rom load" take one token, so a ROM name with a space can't be
 * booted; keep it off the picker rather than let it be picked and fail. */
static int has_space(const char *s)
{
    while (*s) { if (*s == ' ') return 1; s++; }
    return 0;
}

int parse_reply(char *buf, char **body)
{
    char *p;
    int code = 0, i;
    if (buf[0] == 'O' && buf[1] == 'K') {
        p = buf;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (*p == '\r') p++;
        *body = p;
        return 0;
    }
    if (strncmp(buf, "FAIL ", 5) != 0) return -1;
    for (i = 5; i < 8; i++) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        code = code * 10 + (buf[i] - '0');
    }
    p = buf + 8;
    if (*p == ' ') p++;
    *body = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    *p = '\0';
    return code ? code : 255;
}

u32 dec_to_u32(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10UL + (u32)(*s++ - '0');
    return v;
}

void u32_to_dec(u32 v, char *out)
{
    char tmp[11];
    int n = 0;
    do { tmp[n++] = (char)('0' + (int)(v % 10UL)); v /= 10UL; } while (v);
    while (n) *out++ = tmp[--n];
    *out = '\0';
}

/* True if text has a line that is exactly "...": the firmware's marker for
 * a "fs ls" reply body too big for its remote reply cap (console.c
 * remote_output_truncates). Must run before parse_ls, which rewrites line
 * terminators to NUL in place. */
int list_truncated(const char *text)
{
    const char *p = text, *line;
    while (*p) {
        line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (p - line == 3 && line[0] == '.' && line[1] == '.' && line[2] == '.') return 1;
        while (*p == '\n' || *p == '\r') p++;
    }
    return 0;
}

int parse_ls(char *text, file_ent *out, int max, int roms)
{
    int n = 0, keep;
    char *p = text, *line, *end, *sp, *q;
    u32 size;
    while (*p && n < max) {
        line = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        end = p;
        while (*p == '\n' || *p == '\r') { *p = '\0'; p++; }
        sp = 0;
        for (q = line; q < end; q++) if (*q == ' ') sp = q;
        if (!sp || sp == line || sp[1] < '0' || sp[1] > '9') continue;
        *sp = '\0';
        size = dec_to_u32(sp + 1);
        if (roms) keep = ends_with_ci(line, ".ROM") && has_space(line) == 0;
        /* ponytail: CMOC miscompiles `!func(...)` inline (verified on-device);
         * use `== 0` instead of `!` on a direct function-call result. */
        else keep = (ends_with_ci(line, ".ROM") == 0) && cmp_ci(line, "picoco.cfg") != 0;
        if (!keep) continue;
        out[n].name = line;
        out[n].kb = (u16)((size + 1023UL) / 1024UL);
        n++;
    }
    return n;
}

void sort_files(file_ent *f, int n)
{
    int i, j;
    file_ent t;
    for (i = 1; i < n; i++) {
        t = f[i];
        for (j = i; j > 0 && cmp_ci(f[j - 1].name, t.name) > 0; j--) f[j] = f[j - 1];
        f[j] = t;
    }
}

void parse_disks(const char *text, char names[4][32])
{
    int i, d, k;
    const char *p = text;
    for (i = 0; i < 4; i++) names[i][0] = '\0';
    while (*p) {
        if (p[0] == 'X' && p[1] >= '0' && p[1] <= '3') {
            d = p[1] - '0';
            for (k = 0; k < 5 && *p; k++) p++;   /* "X%-3d" then the '*'/' ' flag */
            i = 0;
            while (*p && *p != '\r' && *p != '\n' && i < 31) names[d][i++] = *p++;
            names[d][i] = '\0';
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

int line_value(const char *text, const char *key, char *out, int cap)
{
    u16 kl = (u16)strlen(key);
    const char *p = text;
    int i;
    while (*p) {
        if (strncmp(p, key, kl) == 0) {
            p += kl;
            i = 0;
            while (*p && *p != '\n' && *p != '\r' && i < cap - 1) out[i++] = *p++;
            out[i] = '\0';
            return 1;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return 0;
}

u32 civil_to_unix(int y, int mo, int d, int h, int mi)
{
    u32 days = 0;
    int i;
    for (i = 1970; i < y; i++) days += leap(i) ? 366UL : 365UL;
    for (i = 1; i < mo; i++) days += mdays[i - 1] + (u16)(i == 2 && leap(y));
    days += (u32)(d - 1);
    return days * 86400UL + (u32)h * 3600UL + (u32)mi * 60UL;
}

void unix_to_civil(u32 t, int *y, int *mo, int *d, int *h, int *mi)
{
    u32 days = t / 86400UL, rem = t % 86400UL;
    u16 len;
    int yy = 1970, m = 1;
    *h = (int)(rem / 3600UL);
    *mi = (int)((rem % 3600UL) / 60UL);
    for (;;) { len = leap(yy) ? 366 : 365; if (days < len) break; days -= len; yy++; }
    for (;;) { len = mdays[m - 1] + (u16)(m == 2 && leap(yy)); if (days < len) break; days -= len; m++; }
    *y = yy; *mo = m; *d = (int)days + 1;
}

static int num(const char *s, int n, int *out)
{
    int v = 0, i;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

int parse_datetime(const char *s, u32 *out)
{
    int y, mo, d, h, mi;
    if (strlen(s) != 16 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':') return -1;
    if (num(s, 4, &y) || num(s + 5, 2, &mo) || num(s + 8, 2, &d) || num(s + 11, 2, &h) || num(s + 14, 2, &mi))
        return -1;
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59) return -1;
    if (d > (int)(mdays[mo - 1] + (mo == 2 && leap(y)))) return -1;
    *out = civil_to_unix(y, mo, d, h, mi);
    return 0;
}

int is_os9_boot(const u8 *sec) { return sec[0] == 'O' && sec[1] == 'S'; }

int rsdos_dir(const u8 *sec, rs_ent *out, int max, int *end)
{
    int n = 0, e, i, k;
    const u8 *d;
    for (e = 0; e < 8; e++) {
        d = sec + e * 32;
        if (d[0] == 0xFF) { *end = 1; return n; }
        if (d[0] == 0x00) continue;                 /* killed */
        if (d[11] != 0 && d[11] != 2) continue;     /* BASIC and ML only */
        if (n >= max) return n;
        k = 0;
        for (i = 0; i < 8 && d[i] != ' '; i++) out[n].name[k++] = (char)d[i];
        out[n].name[k++] = '.';
        for (i = 8; i < 11 && d[i] != ' '; i++) out[n].name[k++] = (char)d[i];
        out[n].name[k] = '\0';
        out[n].type = d[11];
        n++;
    }
    return n;
}

/* "ssid <name> rssi <-n> chan <c>" lines from `net scan`; name may hold
 * spaces, so cut at the last " rssi ". kb carries -rssi (a small positive
 * number the list shows next to the name); a positive rssi gives 0. */
int parse_scan(char *text, file_ent *out, int max)
{
    int n = 0;
    char *p = text;
    while (*p && n < max) {
        char *eol = p;
        char save;
        while (*eol && *eol != '\n') eol++;
        save = *eol; *eol = '\0';
        if (strncmp(p, "ssid ", 5) == 0) {
            char *r = NULL, *q = p;
            for (; q < eol; q++) if (strncmp(q, " rssi ", 6) == 0) r = q;
            if (r && r > p + 5) {   /* skip hidden (empty) SSIDs */
                *r = '\0';
                out[n].name = p + 5;
                out[n].kb = (r[6] == '-') ? (u16)dec_to_u32(r + 7) : (u16)0;
                n++;
            }
        }
        *eol = save;
        p = *eol ? eol + 1 : eol;
    }
    return n;
}
