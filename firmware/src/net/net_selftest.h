#pragma once
int net_selftest(void (*line)(const char *s));   /* 0 pass, -1 fail, -2 refused */
