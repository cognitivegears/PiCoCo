#include "fs_flash.h"
#include "ff.h"
#include "diskio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"
#include "hardware/watchdog.h"
#include "log.h"
#include <string.h>

static FATFS s_fatfs;   /* static: too big for the 4 KB core0 stack */
static bool s_mounted;
static bool s_exporting;

int fs_flash_read_blocks(uint32_t lba, uint8_t *buf, uint32_t n) {
    if (n == 0) return 0;
    if (lba >= FS_SECTORS || n > FS_SECTORS - lba) return -1;
    memcpy(buf, (const uint8_t *)(XIP_BASE + PICOCO_FS_OFFSET + lba * FS_SECTOR), (size_t)n * FS_SECTOR);
    return 0;
}

/* ponytail: RMW per 4 KB erase, ~50 ms; capture-to-flash and heavy DW writes
 * feel it. Buffering or a log-structured layer is the upgrade. */
int fs_flash_write_blocks(uint32_t lba, const uint8_t *buf, uint32_t n) {
    if (n == 0) return 0;
    if (lba >= FS_SECTORS || n > FS_SECTORS - lba) return -1;
    static uint8_t blk[4096]; /* static: too big for the 4 KB core0 stack */
    uint32_t first_block = (lba * FS_SECTOR) / 4096u;
    uint32_t last_block = ((lba + n - 1) * FS_SECTOR) / 4096u;
    for (uint32_t b = first_block; b <= last_block; b++) {
        uint32_t block_off = b * 4096u;
        memcpy(blk, (const uint8_t *)(XIP_BASE + PICOCO_FS_OFFSET + block_off), sizeof(blk));
        uint32_t blk_first_lba = block_off / FS_SECTOR;
        for (uint32_t s = 0; s < 4096u / FS_SECTOR; s++) {
            uint32_t sector = blk_first_lba + s;
            if (sector < lba || sector >= lba + n) continue;
            memcpy(blk + s * FS_SECTOR, buf + (sector - lba) * FS_SECTOR, FS_SECTOR);
        }
        uint32_t irq = save_and_disable_interrupts();
        flash_range_erase(PICOCO_FS_OFFSET + block_off, sizeof(blk));
        flash_range_program(PICOCO_FS_OFFSET + block_off, blk, sizeof(blk));
        restore_interrupts(irq);
        watchdog_update();   /* one 4 KB erase/program can run long; keep petting between blocks */
    }
    return 0;
}

/* diskio.h glue: FatFS talks in 512-byte sectors of "drive 0", which is this
 * whole flash partition. */
DSTATUS disk_status(BYTE pdrv) { (void)pdrv; return 0; }
DSTATUS disk_initialize(BYTE pdrv) { (void)pdrv; return 0; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    return fs_flash_read_blocks((uint32_t)sector, buff, (uint32_t)count) == 0 ? RES_OK : RES_PARERR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    return fs_flash_write_blocks((uint32_t)sector, buff, (uint32_t)count) == 0 ? RES_OK : RES_PARERR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = FS_SECTORS; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff = FS_SECTOR; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 8; return RES_OK; /* 4 KB erase / 512 B sector */
        default: return RES_PARERR;
    }
}

bool fs_flash_mounted(void) { return s_mounted; }

int fs_flash_format(void) {
    static uint8_t work[4096]; /* static: f_mkfs's work buffer, too big for the stack */
    s_mounted = false;
    /* n_fat=2: the Pico reboots on every CoCo /RESET, at any instant (roadmap
     * item 1); a second FAT survives a reset that corrupts the FAT FatFS was
     * mid-write on. */
    MKFS_PARM parm = { .fmt = FM_FAT, .n_fat = 2, .align = 0, .n_root = 0, .au_size = 4096 };
    if (f_mkfs("", &parm, work, sizeof(work)) != FR_OK) return -1;
    if (f_mount(&s_fatfs, "", 1) != FR_OK) return -1;
    if (f_setlabel("PICOCO") != FR_OK) LOG_E(LOG_M_FS, "f_setlabel failed");
    s_mounted = true;
    return 0;
}

int fs_flash_mount(void) {
    FRESULT r = f_mount(&s_fatfs, "", 1);
    if (r == FR_NO_FILESYSTEM) return fs_flash_format();
    if (r != FR_OK) return -1;
    s_mounted = true;
    return 0;
}

void fs_flash_unmount(void) {
    f_mount(NULL, "", 0);
    s_mounted = false;
}

bool fs_flash_exporting(void) { return s_exporting; }

int fs_flash_export(bool on) {
    if (on) {
        fs_flash_unmount();
        s_exporting = true;
        return 0;
    }
    s_exporting = false;
    return fs_flash_mount();
}
