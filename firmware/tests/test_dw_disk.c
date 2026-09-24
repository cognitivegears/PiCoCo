#include "test.h"
#include "dw_disk.h"
#include "dw_store.h"
#include "dw_util.h"
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static char g_dir[256];
static dw_store st;

/* Writes hdr (hdrlen bytes) then nsect sectors where sector n's byte 0 is
 * n & 0xFF and byte 1 is n >> 8, rest zero. */
static void mk(const char *name, const uint8_t *hdr, int hdrlen, int nsect) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *fp = fopen(path, "wb");
    if (hdr && hdrlen > 0) fwrite(hdr, 1, (size_t)hdrlen, fp);
    uint8_t sec[256];
    for (int n = 0; n < nsect; n++) {
        memset(sec, 0, sizeof(sec));
        sec[0] = (uint8_t)(n & 0xFF);
        sec[1] = (uint8_t)(n >> 8);
        fwrite(sec, 1, sizeof(sec), fp);
    }
    fclose(fp);
}

TEST(raw_detect) {
    mk("raw.dsk", NULL, 0, 630);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "raw.dsk", false, &d), 0);
    ASSERT_EQ(d.fmt, DW_FMT_RAW);
    ASSERT_EQ(d.byte_offset, 0);
    ASSERT_EQ(d.sectors, 630);
    dw_disk_close(&d);
}

TEST(jvc_detect) {
    uint8_t h[1] = {18};
    mk("j.dsk", h, 1, 630);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "j.dsk", false, &d), 0);
    ASSERT_EQ(d.fmt, DW_FMT_JVC);
    ASSERT_EQ(d.byte_offset, 1);
    uint8_t s[256];
    ASSERT_EQ(dw_disk_read(&d, 5, s), 0);
    ASSERT_EQ(s[0], 5);
    dw_disk_close(&d);
}

TEST(jvc_bad_sector_size) {
    uint8_t h[3] = {18, 1, 2};
    mk("j512.dsk", h, 3, 10);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "j512.dsk", false, &d), -2);
}

TEST(jvc_shift_code_out_of_range) {
    uint8_t h[3] = {18, 1, 40};
    mk("jshift.dsk", h, 3, 10);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "jshift.dsk", false, &d), -2);
}

TEST(vdk_detect) {
    uint8_t h[12] = {'d', 'k', 12, 0, 1, 1, 0, 0, 35, 1, 0, 0};
    mk("v.vdk", h, 12, 630);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "v.vdk", false, &d), 0);
    ASSERT_EQ(d.fmt, DW_FMT_VDK);
    ASSERT_EQ(d.byte_offset, 12);
    uint8_t s[256];
    ASSERT_EQ(dw_disk_read(&d, 7, s), 0);
    ASSERT_EQ(s[0], 7);
    dw_disk_close(&d);
}

TEST(vdk_header_larger_than_file) {
    uint8_t h[12] = {'d', 'k', 0xD0, 0x07, 1, 1, 0, 0, 35, 1, 0, 0}; /* hsize = 2000 LE */
    mk("vbad.vdk", h, 12, 2); /* file size = 12 + 2*256 = 524 < 2000 */
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "vbad.vdk", false, &d), -2);
}

TEST(vdk_write_protect_flag) {
    uint8_t h[12] = {'d', 'k', 12, 0, 1, 1, 0, 0, 35, 1, 0x01, 0}; /* flags byte (offset 10) bit 0 set */
    mk("vwp.vdk", h, 12, 10);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "vwp.vdk", false, &d), 0);
    uint8_t s[256] = {0};
    ASSERT_EQ(dw_disk_write(&d, 0, s), DW_E_WRPROT);
    dw_disk_close(&d);
}

TEST(os9_detect) {
    uint8_t h[0x13];
    memset(h, 0, sizeof(h));
    /* DD_TOT = 630 = 0x000276, big-endian 24-bit at offset 0 */
    h[0] = 0x00; h[1] = 0x02; h[2] = 0x76;
    h[3] = 18;            /* DD_TKS */
    h[0x10] = 0;          /* DD_FMT: sides = (fmt & 1) + 1 = 1 */
    h[0x11] = 0x00; h[0x12] = 0x12; /* DD_SPT = 18, big-endian u16 */
    mk("o.os9", h, sizeof(h), 630);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "o.os9", false, &d), 0);
    ASSERT_EQ(d.fmt, DW_FMT_OS9);
    uint8_t s[256] = {0};
    ASSERT_EQ(dw_disk_write(&d, 630, s), DW_E_EOF);
    dw_disk_close(&d);
}

TEST(os9_needs_full_sector) {
    uint8_t h[0x13];
    memset(h, 0, sizeof(h));
    h[0] = 0x00; h[1] = 0x02; h[2] = 0x76; /* DD_TOT = 630 */
    h[3] = 18;                             /* DD_TKS */
    h[0x10] = 0;                           /* DD_FMT */
    h[0x11] = 0x00; h[0x12] = 0x12;        /* DD_SPT = 18 */
    mk("oshort.os9", h, sizeof(h), 0);     /* only 19 bytes, no sectors */
    dw_disk d;
    int r = dw_disk_open(&st, "oshort.os9", false, &d);
    if (r == 0) {
        ASSERT(d.fmt != DW_FMT_OS9);
        dw_disk_close(&d);
    }
}

TEST(read_eof_and_zero_fill) {
    mk("raw10.dsk", NULL, 0, 10);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "raw10.dsk", false, &d), 0);
    uint8_t s[256];
    ASSERT_EQ(dw_disk_read(&d, 10, s), DW_E_EOF);
    /* truncate to 9.5 sectors: 9 full sectors + 128 bytes */
    char path[512];
    snprintf(path, sizeof(path), "%s/raw10.dsk", g_dir);
    ASSERT_EQ(truncate(path, 9 * 256 + 128), 0);
    memset(s, 0xAA, sizeof(s));
    ASSERT_EQ(dw_disk_read(&d, 9, s), 0);
    ASSERT_EQ(s[0], 9);
    for (int i = 128; i < 256; i++) ASSERT_EQ(s[i], 0);
    dw_disk_close(&d);
}

TEST(write_extends_and_wrprot) {
    mk("raw10w.dsk", NULL, 0, 10);
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "raw10w.dsk", false, &d), 0);
    uint8_t s[256];
    memset(s, 0, sizeof(s));
    s[0] = 0x77;
    ASSERT_EQ(dw_disk_write(&d, 20, s), 0);
    ASSERT_EQ(d.sectors, 21);
    uint8_t rb[256];
    ASSERT_EQ(dw_disk_read(&d, 20, rb), 0);
    ASSERT_MEMEQ(rb, s, 256);
    dw_disk_close(&d);

    dw_disk d2;
    ASSERT_EQ(dw_disk_open(&st, "raw10w.dsk", true, &d2), 0);
    ASSERT_EQ(dw_disk_write(&d2, 0, s), DW_E_WRPROT);
    dw_disk_close(&d2);
}

TEST(checksum) {
    uint8_t z[256] = {0};
    ASSERT_EQ(dw_checksum(z, 256), 0);
    uint8_t o[3] = {0xFF, 0xFF, 0x02};
    ASSERT_EQ(dw_checksum(o, 3), 0x200);
    uint8_t l[3] = {0x01, 0x02, 0x03};
    ASSERT_EQ(dw_lsn_unpack(l), 0x010203);
}

TEST(missing_file) {
    dw_disk d;
    ASSERT_EQ(dw_disk_open(&st, "nope.dsk", false, &d), -1);
}

/* dw_disk_is_config_name splits the name on '/', ':' and '\\' and matches
 * the LAST component, so a name ending in a separator (empty last
 * component) or holding a control byte that FatFS's create_name would
 * truncate on doesn't match it -- but dw_disk_name_ok must still refuse
 * all of these at the choke point, since the POSIX store used on host
 * treats these bytes literally where FatFS on the Pico would not. */
TEST(name_ok_refuses_bypass_bytes) {
    ASSERT_EQ(dw_disk_name_ok("picoco.cfg\\"), false);
    ASSERT_EQ(dw_disk_name_ok("picoco.cfg\\\\"), false);
    ASSERT_EQ(dw_disk_name_ok("picoco.cfg\x01"), false);
    ASSERT_EQ(dw_disk_name_ok("picoco.cfg\x1fjunk"), false);
    ASSERT_EQ(dw_disk_name_ok("a\x01b.dsk"), false);
    ASSERT_EQ(dw_disk_name_ok("0:x.dsk"), false);
    ASSERT_EQ(dw_disk_name_ok(""), false);
    ASSERT_EQ(dw_disk_name_ok("PICOCO.CFG. "), false);
}

TEST(name_ok_allows_ordinary_names) {
    ASSERT_EQ(dw_disk_name_ok("ok name.dsk"), true);
    ASSERT_EQ(dw_disk_name_ok("xpicoco.cfg"), true);
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/dwdiskXXXXXX", tmpdir);
    char *d = mkdtemp(tmpl);
    strcpy(g_dir, d);
    dw_store_posix_init(&st, g_dir);

    RUN(raw_detect);
    RUN(jvc_detect);
    RUN(jvc_bad_sector_size);
    RUN(jvc_shift_code_out_of_range);
    RUN(vdk_detect);
    RUN(vdk_header_larger_than_file);
    RUN(vdk_write_protect_flag);
    RUN(os9_detect);
    RUN(os9_needs_full_sector);
    RUN(read_eof_and_zero_fill);
    RUN(write_extends_and_wrprot);
    RUN(checksum);
    RUN(missing_file);
    RUN(name_ok_refuses_bypass_bytes);
    RUN(name_ok_allows_ordinary_names);
    TEST_MAIN_END
}
