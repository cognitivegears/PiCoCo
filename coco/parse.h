#ifndef PARSE_H
#define PARSE_H
/* Pure-C reply parsing shared by the CoCo program (CMOC) and the host test.
 * C89 style: CMOC wants declarations at the top of a block. */
#ifdef _CMOC_VERSION_
#include <cmoc.h>
typedef unsigned char u8;
typedef unsigned int u16;
typedef unsigned long u32;
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#define MAX_FILES 128

typedef struct { char *name; u16 kb; } file_ent;  /* name points into the ls text */
typedef struct { char name[13]; u8 type; } rs_ent; /* "NAME.EXT"; type 0 BASIC, 2 ML */

/* Firmware reply "OK ...\n\r<body>" -> 0, body; "FAIL nnn msg\n\r" -> nnn,
 * body = msg (terminated in place); anything else -> -1. */
int  parse_reply(char *buf, char **body);
/* "name size" lines, split in place. roms=1 keeps *.ROM only (and hides
 * names with a space, since "rom boot"/"rom load" take one token); roms=0
 * hides *.ROM and picoco.cfg. Returns entries stored. */
int  parse_ls(char *text, file_ent *out, int max, int roms);
/* 1 if text has a line that is exactly "...": the firmware's truncation
 * marker. Call before parse_ls, which rewrites text in place. */
int  list_truncated(const char *text);
void sort_files(file_ent *f, int n);
/* "dw disk show" -> names[d] per drive, "" when empty. */
void parse_disks(const char *text, char names[4][32]);
/* 1 if a line starts with key; copies the rest of that line into out. */
int  line_value(const char *text, const char *key, char *out, int cap);
u32  dec_to_u32(const char *s);
int  parse_scan(char *text, file_ent *out, int max);
void u32_to_dec(u32 v, char *out);
u32  civil_to_unix(int y, int mo, int d, int h, int mi);
void unix_to_civil(u32 t, int *y, int *mo, int *d, int *h, int *mi);
int  parse_datetime(const char *s, u32 *out);      /* "YYYY-MM-DD HH:MM": 0 ok, -1 bad */
int  is_os9_boot(const u8 *sec);                   /* track 34 sector 1 starts "OS" */
/* One RS-DOS directory sector (8 entries). Adds BASIC/ML entries to out (at
 * most max); sets *end when the $FF end marker is seen. Returns entries added. */
int  rsdos_dir(const u8 *sec, rs_ent *out, int max, int *end);
int  ends_with_ci(const char *s, const char *suffix);
#endif
