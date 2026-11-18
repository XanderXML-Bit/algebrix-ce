/*
 * tice_mock.c — Shared tice state for desktop tests
 */
#include "tice.h"
#include "calcengine.h"
#include <ti/vars.h>

static uint8_t g_battery_status = 4;

/* ── Mock: cancel checking (always false on desktop) ─────── */
static bool g_mock_cancel = false;
static bool g_mock_cancel_once = false;
static unsigned int g_mock_cancel_calls = 0;

bool check_cancel(void)
{
    g_mock_cancel_calls++;
    if (g_mock_cancel_once) {
        g_mock_cancel_once = false;
        return true;
    }
    return g_mock_cancel;
}

void tice_mock_set_cancel(bool val)
{
    g_mock_cancel = val;
    g_mock_cancel_once = false;
    g_mock_cancel_calls = 0;
}

void tice_mock_cancel_once(void)
{
    tice_mock_set_cancel(false);
    g_mock_cancel_once = true;
}

unsigned int tice_mock_cancel_calls(void)
{
    return g_mock_cancel_calls;
}

/* ── Mock: progress indicator (no-ops on desktop) ────────── */
void progress_start(void) {}
void progress_check(void) {}

void delay(unsigned int ms)
{
    (void)ms;
}

void msleep(unsigned int ms)
{
    (void)ms;
}

uint8_t boot_GetBatteryStatus(void)
{
    return g_battery_status;
}

void tice_mock_set_battery_status(uint8_t level)
{
    if (level > 4) level = 4;
    g_battery_status = level;
}

uint8_t tice_mock_get_battery_status(void)
{
    return g_battery_status;
}

int os_RunPrgm(const char *prgm, void *data, size_t size, os_runprgm_callback_t callback)
{
    (void)prgm;
    (void)data;
    (void)size;
    (void)callback;
    return 0;
}
