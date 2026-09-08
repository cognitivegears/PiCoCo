#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* Platform functions. host/plat_host.c on the Mac, src/plat_pico.c on the RP2350 (Plan B). */
uint32_t plat_now_us(void);
uint32_t plat_now_ms(void);
int  plat_fs_list(void (*cb)(const char *name, uint32_t size, void *ctx), void *ctx); /* 0 ok */
int  plat_fs_remove(const char *name);
int  plat_fs_format(void);
int  plat_fs_export(bool on);                 /* USB MSC export; host: no-op returning -1 */
bool plat_usb_ejected(void);                  /* host has ejected the MSC volume; host: always false */
int  plat_cfg_read(char *buf, size_t max);    /* bytes read, <0 none */
int  plat_cfg_write(const char *buf, size_t n);
void plat_reboot(bool bootsel);
void plat_halt(bool assert_halt);
void plat_smoke(void);                        /* GPIO toggle test; host: no-op */
size_t plat_bridge_read(uint8_t *buf, size_t n);   /* CDC0 in bridge mode; host: 0 */
size_t plat_bridge_write(const uint8_t *buf, size_t n);
const char *plat_fs_dir(void);                /* host only: directory backing the "filesystem" */
void plat_host_set_dir(const char *dir);      /* host only */
