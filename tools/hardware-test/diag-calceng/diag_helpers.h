/*
 * diag_helpers.h — Shared diagnostic stage-marker API.
 * Included by launcher_diag.c, module_main_diag.c, module_runtime_diag.c.
 * DO NOT include in production builds.
 */
#ifndef DIAG_HELPERS_H
#define DIAG_HELPERS_H

#include <stdint.h>

/*
 * When set to 1, dbg_stage() logs to CEDBG but skips the screen/keypress
 * gate.  Set by pressing CLEAR at any stage prompt.  Natural reset: program
 * is reloaded from archive on every os_RunPrgm re-entry, so static
 * initialiser resets it to 0 each time.
 */
extern uint8_t g_dbg_skip;

/* Wipe the CEDBG AppVar and create a fresh empty one.
 * Call ONCE at the very start of CALCENG main() (L01).
 * Do NOT call from the worker — the worker appends to the same AppVar. */
void dbg_appvar_init(void);

/* Append label + newline to the CEDBG AppVar (append mode).
 * Safe to call even if g_dbg_skip == 1.
 * Called automatically by dbg_stage(); can also be called standalone. */
void dbg_appvar_append(const char *label);

/*
 * Show a stage gate on the home screen, log to CEDBG, and wait for input.
 *
 *   label  — short description, max ~20 chars (fits one LCD row).
 *            e.g. "L01 entry", "W06 pre dispatch"
 *   cur    — this stage's number within its module (1-based).
 *   total  — total stages in this module (25 for launcher, 11 for worker).
 *
 * Screen layout:
 *   Row 0: <label>
 *   Row 1: <cur>/<total>
 *   Row 2: ENTER=next  CLR=skip
 *
 * Keys:
 *   ENTER → advance to next stage.
 *   CLEAR → set g_dbg_skip=1 and return immediately; all subsequent
 *            dbg_stage() calls in this run will log but not gate.
 */
void dbg_stage(const char *label, uint8_t cur, uint8_t total);

#endif /* DIAG_HELPERS_H */
