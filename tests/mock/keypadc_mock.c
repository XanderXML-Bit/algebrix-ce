/*
 * keypadc_mock.c — Shared keypad state for desktop tests
 */
#include "keypadc.h"

#include <string.h>

uint16_t kb_Data[8];

void kb_mock_reset(void)
{
    memset(kb_Data, 0, sizeof(kb_Data));
}

void kb_Scan(void) {}
