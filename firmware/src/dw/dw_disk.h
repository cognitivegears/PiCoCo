#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "dw_store.h"
#include "dw_util.h"

typedef enum { DW_FMT_RAW, DW_FMT_JVC, DW_FMT_VDK, DW_FMT_OS9 } dw_fmt;

typedef struct {
    dw_store *store;
    dw_file f;
    bool mounted, read_only;
    dw_fmt fmt;
    uint32_t byte_offset;
    uint32_t sectors;      /* sectors in the image now */
    char name[32];
} dw_disk;

/* 0 ok, -1 not found, -2 unsupported (e.g. JVC sector size != 256) */
int  dw_disk_open(dw_store *store, const char *name, bool read_only, dw_disk *d);
void dw_disk_close(dw_disk *d);
/* 0 ok, DW_E_EOF, DW_E_READ; short read past end returns zeros with 0 */
int  dw_disk_read(dw_disk *d, uint32_t lsn, uint8_t buf[256]);
/* 0, DW_E_WRPROT, DW_E_EOF (OS9 only), DW_E_WRITE; extends non-OS9 images and updates d->sectors */
int  dw_disk_write(dw_disk *d, uint32_t lsn, const uint8_t buf[256]);
