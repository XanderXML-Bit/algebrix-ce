/* Desktop stand-in for CEdev <ti/screen.h>: a 10x26 text home screen that
 * tests/harness/launcher_view_harness.c records.  os_ClrHome is a function
 * here (a macro over os_ClrLCD/os_HomeUp/os_DrawStatusBar in CEdev). */
#ifndef LAUNCHER_MOCK_TI_SCREEN_H
#define LAUNCHER_MOCK_TI_SCREEN_H
#include <stdint.h>
typedef unsigned int uint24_t;
void os_ClrHome(void);
void os_SetCursorPos(uint8_t curRow, uint8_t curCol);
uint24_t os_PutStrFull(const char *string);
uint24_t os_PutStrLine(const char *string);
#endif
