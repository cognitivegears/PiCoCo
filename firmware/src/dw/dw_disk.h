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

/* True if name (final path component after '/', ':' or '\\', trailing
 * '.'/' ' trimmed, case-insensitive) is picoco.cfg: the boot config replays
 * with USB-console privilege at boot, so a CoCo must never be able to mount
 * or create it. */
bool dw_disk_is_config_name(const char *name);

/* True if name is safe to open/create as a DriveWire disk image or "fs new"
 * target on this flat (no-subdirectory) volume: non-empty; no byte < 0x20
 * or == 0x7F (FatFS's create_name ends a path at any byte < ' ' and
 * silently drops a trailing separator -- ff.c ~2900 -- so "picoco.cfg\" and
 * "picoco.cfg\x01" must be caught here, not by matching a trailing
 * separator); no '/', '\\' or ':' (there are no subdirectories or drive
 * prefixes here); and not picoco.cfg per dw_disk_is_config_name. This is
 * the one choke point every mount/create path (console "dw mount"/"fs new",
 * remote "dw mount"/"dw disk insert"/"fs new") goes through. */
bool dw_disk_name_ok(const char *name);

/* 0 ok, -1 not found, -2 unsupported (e.g. JVC sector size != 256) */
int  dw_disk_open(dw_store *store, const char *name, bool read_only, dw_disk *d);
void dw_disk_close(dw_disk *d);
/* 0 ok, DW_E_EOF, DW_E_READ; short read past end returns zeros with 0 */
int  dw_disk_read(dw_disk *d, uint32_t lsn, uint8_t buf[256]);
/* 0, DW_E_WRPROT, DW_E_EOF (OS9 only), DW_E_WRITE; extends non-OS9 images and updates d->sectors */
int  dw_disk_write(dw_disk *d, uint32_t lsn, const uint8_t buf[256]);
