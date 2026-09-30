#include "parse.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void t_reply(void) {
    char ok[] = "OK command successful\n\rversion 1.2\n";
    char fail[] = "FAIL 101 bad drive\n\r";
    char junk[] = "hello";
    char *b;
    CHECK(parse_reply(ok, &b) == 0 && strcmp(b, "version 1.2\n") == 0);
    CHECK(parse_reply(fail, &b) == 101 && strcmp(b, "bad drive") == 0);
    CHECK(parse_reply(junk, &b) == -1);
}

static void t_ls(void) {
    char text[] = "zaxxon.dsk 161280\nHDBDW3BC3.ROM 8192\npicoco.cfg 120\nmy game.dsk 1024\nbad\n";
    file_ent f[8];
    int n = parse_ls(text, f, 8, 0);
    CHECK(n == 2);
    sort_files(f, n);
    CHECK(strcmp(f[0].name, "my game.dsk") == 0 && f[0].kb == 1);
    CHECK(strcmp(f[1].name, "zaxxon.dsk") == 0 && f[1].kb == 158);
    {
        char t2[] = "zaxxon.dsk 161280\nHDBDW3BC3.ROM 8192\n";
        CHECK(parse_ls(t2, f, 8, 1) == 1 && strcmp(f[0].name, "HDBDW3BC3.ROM") == 0);
    }
    {
        /* "rom boot"/"rom load" take one token, so a ROM name with a space
         * can't be booted -- the picker must hide it. */
        char t3[] = "HDBDW3BC3.ROM 8192\nold color basic.ROM 8192\n";
        CHECK(parse_ls(t3, f, 8, 1) == 1 && strcmp(f[0].name, "HDBDW3BC3.ROM") == 0);
    }
}

static void t_ls_truncated_tail(void) {
    char text[] = "a.dsk 100\nb.dsk 2\n...\n";
    file_ent f[8];
    CHECK(list_truncated(text));            /* must run before parse_ls rewrites text */
    CHECK(parse_ls(text, f, 8, 0) == 2);
    {
        char t3[] = "a.dsk 100\nb.dsk 2\nc.dsk 3\n";
        CHECK(!list_truncated(t3));
        CHECK(parse_ls(t3, f, 2, 0) == 2);   /* max respected */
    }
}

static void t_disks(void) {
    const char *t = "\r\nCurrent DriveWire disks:\r\n\r\nX0   raw.dsk\r\nX3  *nitros9 boot.dsk\r\n";
    char d[4][32];
    parse_disks(t, d);
    CHECK(strcmp(d[0], "raw.dsk") == 0);
    CHECK(d[1][0] == 0 && d[2][0] == 0);
    CHECK(strcmp(d[3], "nitros9 boot.dsk") == 0);
}

static void t_line_value(void) {
    const char *t = "mode native\nrom now load hdbdw3bc3.rom\nrom next none\ndw hdbdos on\n";
    char v[40];
    CHECK(line_value(t, "rom now ", v, sizeof v) && strcmp(v, "load hdbdw3bc3.rom") == 0);
    CHECK(line_value(t, "dw hdbdos ", v, sizeof v) && strcmp(v, "on") == 0);
    CHECK(!line_value(t, "nope ", v, sizeof v));
}

static void t_time(void) {
    u32 t;
    int y, mo, d, h, mi;
    char s[12];
    CHECK(civil_to_unix(2026, 1, 1, 0, 0) == 1767225600UL);
    CHECK(civil_to_unix(2024, 2, 29, 23, 59) == 1709251140UL);
    unix_to_civil(1709251140UL, &y, &mo, &d, &h, &mi);
    CHECK(y == 2024 && mo == 2 && d == 29 && h == 23 && mi == 59);
    CHECK(parse_datetime("2026-09-23 14:02", &t) == 0 && t == 1790172120UL);
    CHECK(parse_datetime("2026-02-29 00:00", &t) == -1);
    CHECK(parse_datetime("2026-9-23 14:02", &t) == -1);
    CHECK(parse_datetime("2026-09-23 24:00", &t) == -1);
    u32_to_dec(1790172120UL, s);
    CHECK(strcmp(s, "1790172120") == 0);
    CHECK(dec_to_u32("1790172120") == 1790172120UL);
    u32_to_dec(0, s);
    CHECK(strcmp(s, "0") == 0);
}

static void t_disk_detect(void) {
    u8 sec[256];
    rs_ent e[16];
    int end = 0;
    memset(sec, 0, sizeof sec);
    sec[0] = 'O'; sec[1] = 'S';
    CHECK(is_os9_boot(sec));
    memset(sec, 0xFF, sizeof sec);
    memcpy(sec, "HELLO   BAS", 11); sec[11] = 0;
    memcpy(sec + 32, "GAME    BIN", 11); sec[43] = 2;
    memcpy(sec + 64, "DATA    DAT", 11); sec[75] = 1;
    sec[96] = 0;                                   /* killed entry */
    CHECK(rsdos_dir(sec, e, 16, &end) == 2);
    CHECK(end == 1);
    CHECK(strcmp(e[0].name, "HELLO.BAS") == 0 && e[0].type == 0);
    CHECK(strcmp(e[1].name, "GAME.BIN") == 0 && e[1].type == 2);
}

static void t_scan(void) {
    char text[] = "ssid Oar5 rssi -36 chan 1\nssid My Net rssi -70 chan 9\nssid  rssi -50 chan 2\nssid Pos rssi 12 chan 3\nbad line\n";
    file_ent f[4];
    int n = parse_scan(text, f, 4);
    CHECK(n == 3);
    CHECK(strcmp(f[0].name, "Oar5") == 0 && f[0].kb == 36);
    CHECK(strcmp(f[1].name, "My Net") == 0 && f[1].kb == 70);
    CHECK(strcmp(f[2].name, "Pos") == 0 && f[2].kb == 0);
}
static void t_net_radio(void) {
    char no[] = "net state off\nnet radio no\nnet mode off boot native\n";
    char v[24];
    CHECK(line_value(no, "net radio ", v, 24) > 0 && strcmp(v, "no") == 0);
    CHECK(line_value(no, "net mode ", v, 24) > 0 && strcmp(v, "off boot native") == 0);
}

int main(void) {
    t_reply(); t_ls(); t_ls_truncated_tail(); t_disks(); t_line_value(); t_time(); t_disk_detect();
    t_scan(); t_net_radio();
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
