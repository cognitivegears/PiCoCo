#pragma once
#include <stdbool.h>

/* True once the host has ejected the MSC volume (tud_msc_start_stop_cb);
 * informational only, surfaced by the console's "status" command. */
bool usb_msc_ejected(void);
void usb_msc_clear_ejected(void); /* called when a new export begins */
