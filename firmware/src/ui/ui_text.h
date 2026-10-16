#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Line editor for the Manager (#59). Keys are POLCAT codes; the text is kept
 * as typed, so lowercase survives to the console verbs. core0 only. */
#define UI_TEXT_MAX 63

typedef void (*ui_text_cb)(const char *text);

/* max is capped at UI_TEXT_MAX. The editor closes before cb runs, so cb may begin another. */
void ui_text_begin(const char *prompt, const char *initial, bool masked, int max, ui_text_cb cb);
bool ui_text_active(void);
bool ui_text_key(uint8_t key);   /* true if consumed: every key while active */
void ui_text_draw(void);         /* overlays rows 12-15: prompt, two text rows, footer */
