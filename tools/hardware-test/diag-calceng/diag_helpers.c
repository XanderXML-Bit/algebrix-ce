/*
 * diag_helpers.c — Shared diagnostic stage-marker implementation.
 * Compiled into both CALCENG (diagnostic) and CEBASE (diagnostic).
 * DO NOT use in production builds.
 */
#include "diag_helpers.h"

#include <tice.h>
#include <ti/getcsc.h>
#include <ti/screen.h>
#include <fileioc.h>
#include <string.h>
#include <stdio.h>

/* 0 = stepping enabled, 1 = skip gates (log only).
 * Re-initialised to 0 each time the .8xp is loaded from archive. */
uint8_t g_dbg_skip = 0;

void dbg_appvar_init(void) {
    ti_var_t av;
    /* Delete any residual AppVar from a previous session */
    ti_Delete("CEDBG");
    /* Create a fresh empty AppVar */
    av = ti_Open("CEDBG", "w");
    if (av) ti_Close(av);
}

void dbg_appvar_append(const char *label) {
    ti_var_t av;
    uint8_t nl = '\n';
    size_t sz;

    if (!label) return;
    sz = strlen(label);
    if (sz == 0) return;

    av = ti_Open("CEDBG", "a");
    if (av) {
        ti_Write(label, 1, sz, av);
        ti_Write(&nl, 1, 1, av);
        ti_Close(av);
    }
}

void dbg_stage(const char *label, uint8_t cur, uint8_t total) {
    char counter[20];
    volatile int i;
    uint8_t k;

    /* Always log — even in skip mode — so post-mortem trace is complete */
    dbg_appvar_append(label);

    if (g_dbg_skip) return;

    /* Row 0: stage label (pad to clear previous content) */
    os_SetCursorPos(0, 0);
    os_PutStrFull("                          ");
    os_SetCursorPos(0, 0);
    os_PutStrFull(label);

    /* Row 1: counter */
    snprintf(counter, sizeof(counter), "%u/%u", (unsigned)cur, (unsigned)total);
    os_SetCursorPos(1, 0);
    os_PutStrFull("                          ");
    os_SetCursorPos(1, 0);
    os_PutStrFull(counter);

    /* Row 2: instructions */
    os_SetCursorPos(2, 0);
    os_PutStrFull("ENTER=next  CLR=skip all  ");

    /* Gate: wait for ENTER or CLEAR */
    while (1) {
        k = os_GetCSC();
        if (k == sk_Enter) break;
        if (k == sk_Clear) {
            g_dbg_skip = 1;
            break;
        }
    }

    /* Brief hardware debounce */
    for (i = 0; i < 10000; i++) {}
}
