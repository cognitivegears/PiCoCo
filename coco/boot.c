#include "boot.h"
#include "ui.h"
#include "dw.h"

#define BOOT_TRACK_LSN 612u   /* track 34 sector 1 */
#define DIR_LSN        308u   /* track 17 sector 3 */

static u8 sec[256];
static rs_ent rs[72];
static file_ent pick[72];
static char cmdline[112];

extern char hook_text[];      /* hook.asm, spike 2026-09-23 */
void hook_install(void);

/* BASIC types the command itself: RVEC4 console-in hook (hook.asm). Armed
 * now, fed once the program returns to the OK prompt. */
static void basic_handoff(const char *cmd)
{
    strcpy(hook_text, cmd);
    strcat(hook_text, "\r");
    clear_screen();
    hook_install();
}

int boot_image(const char *name)
{
    char *body;
    int i, n = 0, end = 0;
    strcpy(cmdline, "dw disk insert 0 ");
    strcat(cmdline, name);
    if (ui_cmd(cmdline, &body) != 0) return 0;
    ui_dirty = 1;
    if (dw_read_sector(0, BOOT_TRACK_LSN, sec) != 0) { msg("CANNOT READ DISK"); return 0; }
    if (is_os9_boot(sec)) {
        /* As Disk BASIC's DOS command: track 34 to $2600, then $2602. The
         * program sits at $3800+, so nothing below is ours (this also
         * overwrites LSBUF at $2700, but the picked name above is already
         * copied into cmdline by now, so that's fine). */
        for (i = 0; i < 18; i++) {
            if (dw_read_sector(0, BOOT_TRACK_LSN + (u32)i, (u8 *)(0x2600 + i * 256)) != 0) {
                msg("BOOT FAILED: PRESS RESET");
                for (;;) ;
            }
        }
        asm {
            orcc    #$50
            jmp     $2602
        }
    }
    for (i = 0; i < 9 && !end; i++) {
        if (dw_read_sector(0, DIR_LSN + (u32)i, sec) != 0) { msg("CANNOT READ DIRECTORY"); return 0; }
        n += rsdos_dir(sec, rs + n, 72 - n, &end);
    }
    if (n == 0) { msg("NO BAS/BIN FILES. IN DRIVE 0"); return 0; }
    for (i = 0; i < n; i++) { pick[i].name = rs[i].name; pick[i].kb = 0; }
    i = pick_list("RUN WHICH FILE? (DRIVE 0)", pick, n);
    if (i < 0) return 0;
    if (rs[i].type == 0) { strcpy(cmdline, "RUN\""); strcat(cmdline, rs[i].name); strcat(cmdline, "\""); }
    else { strcpy(cmdline, "LOADM\""); strcat(cmdline, rs[i].name); strcat(cmdline, "\":EXEC"); }
    basic_handoff(cmdline);
    return 1;
}
