#ifndef UI_H
#define UI_H
#include "parse.h"

extern u8 ui_dirty;
void ui_run(void);
u8   key(void);
void clear_screen(void);
void clear_row(u8 row);
void put_at(u8 row, u8 col, const char *s, u8 inv);
void msg(const char *s);
int  ui_cmd(const char *line, char **body);
int  input_line(const char *prompt, char *buf, int max);
int  pick_list(const char *title, file_ent *e, int n);
void settings_run(void);   /* Task 13 */
#endif
