#include "dw_store.h"
#include "ff.h"
#include <stdint.h>
#include <string.h>

/* Pool of FIL objects: 4 drives + capture + rom load. Static: FIL is too big
 * (and FatFS's own FIL pool would be, too) for the 2 KB core0 stack, and
 * dw_file only carries a void* so the real FIL has to live somewhere. */
#define FATFS_STORE_SLOTS 6

typedef struct { FIL fil; bool used; } fatfs_slot;
static fatfs_slot s_pool[FATFS_STORE_SLOTS];

static int alloc_slot(void) {
    for (int i = 0; i < FATFS_STORE_SLOTS; i++) {
        if (!s_pool[i].used) { s_pool[i].used = true; return i; }
    }
    return -1;
}

static FIL *slot_fil(dw_file *f) { return &s_pool[(int)(intptr_t)f->h].fil; }

static int fatfs_open(void *ctx, const char *name, bool write, dw_file *f) {
    (void)ctx;
    if (strchr(name, '/')) return -1;
    int slot = alloc_slot();
    if (slot < 0) return -1;
    int ret = 0;
    FRESULT r;
    if (write) {
        r = f_open(&s_pool[slot].fil, name, FA_READ | FA_WRITE);
        if (r != FR_OK) {
            r = f_open(&s_pool[slot].fil, name, FA_READ);
            ret = 1;
        }
    } else {
        r = f_open(&s_pool[slot].fil, name, FA_READ);
    }
    if (r != FR_OK) { s_pool[slot].used = false; return -1; }
    f->h = (void *)(intptr_t)slot;
    return ret;
}

static int fatfs_create(void *ctx, const char *name, dw_file *f) {
    (void)ctx;
    if (strchr(name, '/')) return -1;
    int slot = alloc_slot();
    if (slot < 0) return -1;
    if (f_open(&s_pool[slot].fil, name, FA_CREATE_ALWAYS | FA_READ | FA_WRITE) != FR_OK) {
        s_pool[slot].used = false;
        return -1;
    }
    f->h = (void *)(intptr_t)slot;
    return 0;
}

static int fatfs_read(dw_file *f, uint32_t off, void *buf, uint32_t n) {
    FIL *fil = slot_fil(f);
    if (f_lseek(fil, off) != FR_OK) return -1;
    UINT br = 0;
    if (f_read(fil, buf, n, &br) != FR_OK) return -1;
    return (int)br;
}

static int fatfs_write(dw_file *f, uint32_t off, const void *buf, uint32_t n) {
    FIL *fil = slot_fil(f);
    if (f_lseek(fil, off) != FR_OK) return -1;
    UINT bw = 0;
    if (f_write(fil, buf, n, &bw) != FR_OK) return -1;
    return (int)bw;
}

static int fatfs_size(dw_file *f, uint32_t *out) {
    *out = (uint32_t)f_size(slot_fil(f));
    return 0;
}

static int fatfs_sync(dw_file *f) {
    return f_sync(slot_fil(f)) == FR_OK ? 0 : -1;
}

static void fatfs_close(dw_file *f) {
    int slot = (int)(intptr_t)f->h;
    f_close(&s_pool[slot].fil);
    s_pool[slot].used = false;
}

static const dw_store_ops s_fatfs_ops = {
    .open = fatfs_open, .create = fatfs_create, .read = fatfs_read, .write = fatfs_write,
    .size = fatfs_size, .sync = fatfs_sync, .close = fatfs_close,
};

/* Names are FAT paths at the root (FF_FS_RPATH=0: no subdirectories, no ".."). */
void dw_store_fatfs_init(dw_store *s) {
    s->ops = &s_fatfs_ops;
    s->ctx = NULL;
}
