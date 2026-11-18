/*
 * userfunc.h — User-defined custom functions with workspace management
 *
 * Supports up to 10 custom functions per workspace, 4 workspaces total.
 * Functions defined as: define(f, x, x^2+3x) or f(x) = x^2+3x
 * Recalled in expressions: d/dx(f(x)), int(f(x),x,0,1), etc.
 *
 * Workspace management:
 *   ws(name)      — switch to or create workspace by name
 *   ws()          — show current workspace name
 *   clearfuncs()  — clear all functions in active workspace
 *   undef(name)   — delete a single function from active workspace
 *   funcs()       — list all defined functions in active workspace
 *
 * Functions persist in AppVar "CEUSERFN".
 */
#ifndef USERFUNC_H
#define USERFUNC_H

#include "calcengine.h"

/* ── Limits ──────────────────────────────────────────────── */
#define USERFUNC_MAX_PER_WS  10    /* functions per workspace             */
#define USERFUNC_NAME_MAX    12    /* max function name length            */
#define USERFUNC_PARAM_MAX    8    /* max parameter name length           */
#define USERFUNC_BODY_MAX    96    /* max body expression length          */
#define USERFUNC_WS_COUNT     4    /* number of workspaces                */
#define USERFUNC_WS_NAME_MAX 16    /* max workspace name length           */

/* ── Function definition ─────────────────────────────────── */
typedef struct {
    bool in_use;
    char name[USERFUNC_NAME_MAX];
    char param[USERFUNC_PARAM_MAX];  /* single parameter name, e.g. "x" */
    char body[USERFUNC_BODY_MAX];
} userfunc_def_t;

/* ── Command status ──────────────────────────────────────── */
typedef enum {
    UFUNC_CMD_NONE = 0,   /* not a user function command      */
    UFUNC_CMD_OK,         /* command executed successfully     */
    UFUNC_CMD_ERROR       /* command failed                    */
} userfunc_cmd_t;

/* ── Lifecycle ───────────────────────────────────────────── */

/* Initialize all workspaces to empty. */
void userfunc_init(void);

/* Get active workspace index (0-3). */
int userfunc_active_ws(void);

/* Get active workspace name. */
const char *userfunc_active_ws_name(void);

/* Switch to workspace by name (creates if not found). Returns false if full. */
bool userfunc_switch_ws(const char *name);

/* ── Command parser ──────────────────────────────────────── */

/*
 * Try to interpret input as a user-function command.
 * Handles: define(f, x, body), f(x)=body, undef(f),
 *          funcs(), clearfuncs(), ws(name), ws()
 * On UFUNC_CMD_OK:    writes success message to out_msg
 * On UFUNC_CMD_ERROR: writes error message to err_msg
 * Returns UFUNC_CMD_NONE if not a user function command.
 */
userfunc_cmd_t userfunc_try_command(const char *input,
                                    char *out_msg, int out_msg_size,
                                    char *err_msg, int err_msg_size);

/* ── Expansion ───────────────────────────────────────────── */

/*
 * If input contains a call to a user-defined function (e.g., "f(x+1)"),
 * expand it by substituting the parameter with the argument expression.
 * Returns true if any expansion happened, false otherwise.
 * out must be at least MAX_INPUT_LEN bytes.
 */
bool userfunc_expand(const char *input, char *out, int out_size,
                     char *err_msg, int err_msg_size);

/* ── Query ───────────────────────────────────────────────── */

/* Number of defined functions in active workspace. */
int userfunc_count(void);

/* Get definition by index (0-based) in active workspace. NULL if out of range. */
const userfunc_def_t *userfunc_get(int index);

/* Find function by name (case-insensitive). Returns NULL if not found. */
const userfunc_def_t *userfunc_find(const char *name);

/* Delete function by name. Returns false if not found. */
bool userfunc_delete(const char *name);

/* Clear all functions in active workspace. */
void userfunc_clear_active(void);

/* ── Persistence ─────────────────────────────────────────── */
/* Save/load all workspaces from AppVar "CEUSERFN". */
void userfunc_save(void);
void userfunc_load(void);

#endif /* USERFUNC_H */
