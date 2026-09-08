#pragma once
#include <stdint.h>
#include <stdbool.h>

/* On-flash FAT filesystem: firmware occupies 0x000000..0x17FFFF, this
 * partition is the rest of the Pico 2's 4 MB flash. See docs/CLAUDE.md /
 * the firmware spec for the layout. */
#define PICOCO_FS_OFFSET 0x180000u
#define PICOCO_FS_SIZE   0x280000u
#define FS_SECTOR 512u
#define FS_SECTORS (PICOCO_FS_SIZE / FS_SECTOR)

int  fs_flash_mount(void);        /* 0 ok; formats first if no valid volume; -1 on failure */
void fs_flash_unmount(void);
int  fs_flash_format(void);       /* f_mkfs FAT16 with 4 KB clusters, then mount; 0 ok */
bool fs_flash_mounted(void);

/* Raw 512-byte block access shared by diskio.c's FatFS glue and the USB MSC
 * export (Task 4). */
int  fs_flash_read_blocks(uint32_t lba, uint8_t *buf, uint32_t n);       /* memcpy from XIP */
int  fs_flash_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t n); /* 4 KB read-modify-write */
