/*
 * tice.h — Mock TI CE header for desktop compilation
 * Shared state for timing and battery APIs used by CalcEngine.
 */
#ifndef TICE_H
#define TICE_H

#include <stdint.h>
#include <stdbool.h>

void delay(unsigned int ms);
void msleep(unsigned int ms);
uint8_t boot_GetBatteryStatus(void);
void tice_mock_set_battery_status(uint8_t level);
uint8_t tice_mock_get_battery_status(void);

/* Mock cancel checking for tests */
void tice_mock_set_cancel(bool val);
void tice_mock_cancel_once(void);
unsigned int tice_mock_cancel_calls(void);

#endif /* TICE_H */
