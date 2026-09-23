#pragma once
#include <stdint.h>
#include <stdbool.h>
#include PICOCO_BOARD_H   /* PICOCO_FS_OFFSET / PICOCO_FS_SIZE: board-specific flash layout */

#define FS_SECTOR 512u
#define FS_SECTORS (PICOCO_FS_SIZE / FS_SECTOR)

int  fs_flash_mount(void);        /* 0 ok; formats first if no valid volume; -1 on failure */
void fs_flash_unmount(void);
int  fs_flash_format(void);       /* f_mkfs FAT12 (cluster count set by the board's PICOCO_FS_SIZE,
                                    * 4 KB each), 2 FATs, then mount; 0 ok. Existing volumes formatted
                                    * before this change keep 1 FAT until reformatted. */
bool fs_flash_mounted(void);

/* USB MSC export: on unmounts FatFS and hands the raw blocks to the host;
 * off re-mounts FatFS. Caller (console) must refuse export while DriveWire
 * drives are mounted. */
int  fs_flash_export(bool on);
bool fs_flash_exporting(void);

/* Raw 512-byte block access shared by diskio.c's FatFS glue and the USB MSC
 * export (Task 4). */
int  fs_flash_read_blocks(uint32_t lba, uint8_t *buf, uint32_t n);       /* memcpy from XIP */
int  fs_flash_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t n); /* 4 KB read-modify-write */
