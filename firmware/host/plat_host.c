#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static char s_dir[512] = ".";

const char *plat_fs_dir(void) { return s_dir; }
void plat_host_set_dir(const char *dir) { snprintf(s_dir, sizeof(s_dir), "%s", dir); }

uint32_t plat_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000u + ts.tv_nsec / 1000u);
}

uint32_t plat_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

int plat_fs_list(void (*cb)(const char *name, uint32_t size, void *ctx), void *ctx) {
    DIR *d = opendir(s_dir);
    if (!d) return -1;
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", s_dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        cb(ent->d_name, (uint32_t)st.st_size, ctx);
    }
    closedir(d);
    return 0;
}

int plat_fs_remove(const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    return unlink(path) == 0 ? 0 : -1;
}

int plat_fs_format(void) {
    DIR *d = opendir(s_dir);
    if (!d) return -1;
    struct dirent *ent;
    char path[1024];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s", s_dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) unlink(path);
    }
    closedir(d);
    return 0;
}

int plat_fs_export(bool on) { (void)on; return -1; }
bool plat_fs_exporting(void) { return false; }
bool plat_usb_ejected(void) { return false; }

int plat_cfg_read(char *buf, size_t max) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/picoco.cfg", s_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, max, f);
    fclose(f);
    return (int)n;
}

int plat_cfg_write(const char *buf, size_t n) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/picoco.cfg", s_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t written = fwrite(buf, 1, n, f);
    fclose(f);
    return written == n ? 0 : -1;
}

void plat_reboot(bool bootsel) { (void)bootsel; exit(0); }

void plat_halt(bool assert_halt) {
    fprintf(stderr, "plat_halt(%d)\n", (int)assert_halt);
}

void plat_smoke(void) {
    fprintf(stderr, "plat_smoke\n");
}

void plat_crash_test(void) { /* ponytail: no hardfault path on host, no-op */ }
const char *plat_last_reset(void) { return "host"; }
void plat_reset_latch(void) { }

size_t plat_bridge_read(uint8_t *buf, size_t n) { (void)buf; (void)n; return 0; }
size_t plat_bridge_write(const uint8_t *buf, size_t n) { (void)buf; (void)n; return 0; }

bool plat_rtc_get(int64_t *unix_secs) { (void)unix_secs; return false; }
void plat_rtc_set(int64_t unix_secs) { (void)unix_secs; }
