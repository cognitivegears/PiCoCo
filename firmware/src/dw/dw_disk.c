#include "dw_disk.h"
#include <string.h>
#include <stdio.h>

static uint32_t u16le(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t u16be(const uint8_t *p) { return ((uint32_t)p[0] << 8) | (uint32_t)p[1]; }
static uint32_t u24be(const uint8_t *p) { return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2]; }

int dw_disk_open(dw_store *store, const char *name, bool read_only, dw_disk *d) {
    if (strlen(name) >= sizeof(d->name)) return -1;
    memset(d, 0, sizeof(*d));
    dw_file f;
    int oret = store->ops->open(store->ctx, name, !read_only, &f);
    if (oret < 0) return -1;
    uint32_t size = 0;
    if (store->ops->size(&f, &size) < 0) { store->ops->close(&f); return -1; }

    uint8_t hdr[512] = {0};
    uint32_t hn = size < sizeof(hdr) ? size : (uint32_t)sizeof(hdr);
    if (hn > 0) {
        int rn = store->ops->read(&f, 0, hdr, hn);
        if (rn < 0) { store->ops->close(&f); return -1; }
        if ((uint32_t)rn < hn) memset(hdr + rn, 0, hn - (uint32_t)rn);
    }

    dw_fmt fmt = DW_FMT_RAW;
    uint32_t byte_offset = 0, sectors = 0;
    bool vdk_wp = false;

    if (hn >= 4 && hdr[0] == 'd' && hdr[1] == 'k') {
        byte_offset = u16le(&hdr[2]);
        if (byte_offset > size) { store->ops->close(&f); return -2; }
        fmt = DW_FMT_VDK;
        sectors = (size - byte_offset) / 256;
        vdk_wp = (hdr[10] & 1) != 0; /* header flags byte, bit 0 = write protect */
    } else if (size % 256 != 0 && size % 256 <= 5) {
        uint32_t hdrlen = size % 256;
        uint8_t code = hdrlen >= 3 ? hdr[2] : 1;
        if (code != 1) { store->ops->close(&f); return -2; }
        fmt = DW_FMT_JVC;
        byte_offset = hdrlen;
        sectors = (size - byte_offset) / 256;
    } else if (size >= 256 && hn >= 0x13) {
        uint32_t tot = u24be(&hdr[0]);
        uint32_t spt = u16be(&hdr[0x11]);
        uint32_t sides = (uint32_t)(hdr[0x10] & 1) + 1;
        if (tot != 0 && spt != 0 && hdr[3] == spt &&
            tot == (tot / spt / sides) * spt * sides) {
            fmt = DW_FMT_OS9;
            byte_offset = 0;
            sectors = tot;
        }
    }
    if (fmt == DW_FMT_RAW) {
        byte_offset = 0;
        sectors = size / 256;
    }

    d->store = store;
    d->f = f;
    d->mounted = true;
    d->read_only = read_only || (oret == 1) || vdk_wp;
    d->fmt = fmt;
    d->byte_offset = byte_offset;
    d->sectors = sectors;
    snprintf(d->name, sizeof(d->name), "%s", name);
    return 0;
}

void dw_disk_close(dw_disk *d) {
    if (!d->mounted) return;
    d->store->ops->close(&d->f);
    d->mounted = false;
}

int dw_disk_read(dw_disk *d, uint32_t lsn, uint8_t buf[256]) {
    if (lsn >= d->sectors) return DW_E_EOF;
    uint32_t off = d->byte_offset + lsn * 256;
    int rn = d->store->ops->read(&d->f, off, buf, 256);
    if (rn < 0) return DW_E_READ;
    if ((uint32_t)rn < 256) memset(buf + rn, 0, 256 - (uint32_t)rn);
    return DW_E_OK;
}

int dw_disk_write(dw_disk *d, uint32_t lsn, const uint8_t buf[256]) {
    if (d->read_only) return DW_E_WRPROT;
    if (d->fmt == DW_FMT_OS9 && lsn >= d->sectors) return DW_E_EOF;
    uint32_t off = d->byte_offset + lsn * 256;
    int wn = d->store->ops->write(&d->f, off, buf, 256);
    if (wn < 0 || (uint32_t)wn != 256) return DW_E_WRITE;
    if (lsn + 1 > d->sectors) d->sectors = lsn + 1;
    return DW_E_OK;
}
