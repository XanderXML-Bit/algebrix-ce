/*
 * macros.h - User-defined expression shortcuts/macros.
 *
 * Uses fixed-size static storage suitable for TI-84 CE constraints.
 */
#ifndef MACROS_H
#define MACROS_H

#include "calcengine.h"

#define MACROS_MAX_COUNT      12
#define MACROS_NAME_MAX       16
#define MACROS_PARAMS_MAX      3
#define MACROS_PARAM_NAME_MAX 12
#define MACROS_BODY_MAX       96
#define MACROS_EXPAND_DEPTH    8

typedef enum {
    MACRO_CMD_NONE = 0,
    MACRO_CMD_OK,
    MACRO_CMD_ERROR
} macro_cmd_status_t;

/* Clear all stored macros. */
void macros_init(void);

/* Number of currently-defined macros. */
int macros_count(void);

/*
 * Handle macro-management commands.
 * Commands:
 *   macro(name, body)
 *   macro(name(p1,...), body)
 *   unmacro(name)
 *   macros()
 */
macro_cmd_status_t macros_try_command(const char *input,
                                      char *out_msg, int out_msg_size,
                                      char *err_msg, int err_msg_size);

/*
 * Expand user-defined macros in an expression.
 * Returns false on malformed macro usage, recursive expansion, or overflow.
 */
bool macros_expand(const char *input, char *out, int out_size,
                   char *err_msg, int err_msg_size);

#endif /* MACROS_H */
