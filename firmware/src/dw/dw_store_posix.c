#include "dw_store.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* ponytail: single static dir string as ctx; this backend only ever needs
 * one instance (the host filesystem root). Add a real ctx struct if a
 * second store is ever needed. */
static char s_dir[256];

static int posix_open(void *ctx, const char *name, bool write, dw_file *f) {
    (void)ctx;
    if (strchr(name, '/')) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    int fd = -1;
    int ret = 0;
    if (write) {
        fd = open(path, O_RDWR);
        if (fd < 0) {
            fd = open(path, O_RDONLY);
            ret = 1;
        }
    } else {
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) return -1;
    f->h = (void *)(intptr_t)fd;
    return ret;
}

static int posix_read(dw_file *f, uint32_t off, void *buf, uint32_t n) {
    int fd = (int)(intptr_t)f->h;
    ssize_t r = pread(fd, buf, n, off);
    return r < 0 ? -1 : (int)r;
}

static int posix_write(dw_file *f, uint32_t off, const void *buf, uint32_t n) {
    int fd = (int)(intptr_t)f->h;
    ssize_t r = pwrite(fd, buf, n, off);
    return r < 0 ? -1 : (int)r;
}

static int posix_size(dw_file *f, uint32_t *out) {
    int fd = (int)(intptr_t)f->h;
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    *out = (uint32_t)st.st_size;
    return 0;
}

static int posix_sync(dw_file *f) {
    int fd = (int)(intptr_t)f->h;
    return fsync(fd) < 0 ? -1 : 0;
}

static void posix_close(dw_file *f) {
    close((int)(intptr_t)f->h);
}

static const dw_store_ops s_posix_ops = {
    .open = posix_open, .read = posix_read, .write = posix_write,
    .size = posix_size, .sync = posix_sync, .close = posix_close,
};

void dw_store_posix_init(dw_store *s, const char *dir) {
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    s->ops = &s_posix_ops;
    s->ctx = NULL;
}
