#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Storage backend abstraction: dw_disk talks to files only through this,
 * so the Pico build can swap in a littlefs-backed dw_store_pico later. */
typedef struct { void *h; } dw_file;

typedef struct dw_store_ops {
    /* 0 opened read-write, 1 opened read-only (write requested but denied), <0 error/not found */
    int  (*open)(void *ctx, const char *name, bool write, dw_file *f);
    /* Creates (or truncates) name for read-write; 0 ok, <0 error. Distinct
     * from open(write=true) so mounting a missing disk still fails with
     * -1 (dw_disk_open relies on that); only used for creating new files
     * (e.g. console "dw capture on"). */
    int  (*create)(void *ctx, const char *name, dw_file *f);
    int  (*read)(dw_file *f, uint32_t off, void *buf, uint32_t n);       /* bytes read (0 at EOF), <0 error */
    int  (*write)(dw_file *f, uint32_t off, const void *buf, uint32_t n); /* bytes written, <0 error */
    int  (*size)(dw_file *f, uint32_t *out);
    int  (*sync)(dw_file *f);
    void (*close)(dw_file *f);
} dw_store_ops;

typedef struct { const dw_store_ops *ops; void *ctx; } dw_store;

/* Names resolve to dir/name; rejects names containing '/'. */
void dw_store_posix_init(dw_store *s, const char *dir);
