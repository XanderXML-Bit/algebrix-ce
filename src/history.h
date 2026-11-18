/*
 * history.h — Calculation history stack
 *
 * Stores past calculation results so users can navigate back
 * with Up arrow and re-enter previous results. Static storage
 * only (no malloc).
 */
#ifndef HISTORY_H
#define HISTORY_H

#include "calcengine.h"

#define HISTORY_MAX_ENTRIES 16
#define HISTORY_EXPR_LEN 128
#define HISTORY_RESULT_LEN 128

typedef struct {
    char input[HISTORY_EXPR_LEN];    /* the expression that was entered */
    char result[HISTORY_RESULT_LEN]; /* the computed result string */
    ce_float_t numeric_result;       /* numeric value (if applicable) */
    bool has_numeric;                /* true if numeric_result is valid */
    bool in_use;                     /* true if this slot is occupied */
} history_entry_t;

/* ── History API ───────────────────────────────────────────── */

/*
 * Clear all history entries.
 */
void history_clear(void);

/*
 * Push a new entry onto the history stack.
 * Oldest entries are discarded when the stack is full.
 * Returns true on success.
 */
bool history_push(const char *input, const char *result,
                  ce_float_t numeric, bool has_numeric);

/*
 * Get the number of entries in the history.
 */
int history_count(void);

/*
 * Get a history entry by index (0 = most recent).
 * Returns NULL if index is out of range.
 */
const history_entry_t *history_get(int index);

/*
 * Get the input string of entry at index (0 = most recent).
 * Returns empty string if index is out of range.
 */
const char *history_get_input(int index);

/*
 * Get the result string of entry at index (0 = most recent).
 * Returns empty string if index is out of range.
 */
const char *history_get_result(int index);

/*
 * Copy the text that should be re-entered into the input buffer for
 * the history entry at index. Prefers the stored result, stripping a
 * leading "= " prompt marker, and falls back to the original input.
 */
bool history_get_reentry_text(int index, char *out, int out_size);

/*
 * Navigate: move backward in history (older).
 * Returns the index of the current position, or -1 if at end.
 */
int history_navigate_back(void);

/*
 * Navigate: move forward in history (newer).
 * Returns the index of the current position, or -1 if at beginning.
 */
int history_navigate_forward(void);

/*
 * Reset navigation position (called when user types new input).
 */
void history_navigate_reset(void);

/*
 * Get current navigation position.
 * Returns -1 if not navigating (at input line).
 */
int history_navigate_position(void);

#endif /* HISTORY_H */
