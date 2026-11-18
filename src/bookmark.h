/*
 * bookmark.h — Expression bookmarks
 *
 * Allows users to permanently save named results or expressions.
 * Separate from Recents (which are automatic).
 * Accessible from the Bookmarks tab (4th tab) in the main menu.
 *
 * Bookmarks persist in AppVar "CEBKMRK".
 *
 * Commands:
 *   bookmark("name")    — save current result under name
 *   bookmark()          — list bookmarks
 *   delbm("name")       — delete bookmark by name
 *   clearbm()           — clear all bookmarks
 */
#ifndef BOOKMARK_H
#define BOOKMARK_H

#include "calcengine.h"

/* ── Limits ──────────────────────────────────────────────── */
#define BOOKMARK_MAX_COUNT  16
#define BOOKMARK_NAME_MAX   16
#define BOOKMARK_EXPR_MAX   64
#define BOOKMARK_RESULT_MAX 64

/* ── Bookmark entry ──────────────────────────────────────── */
typedef struct {
    bool in_use;
    char name[BOOKMARK_NAME_MAX];
    char expr[BOOKMARK_EXPR_MAX];
    char result[BOOKMARK_RESULT_MAX];
    ce_float_t numeric;
    bool has_numeric;
} bookmark_t;

/* ── Command status ──────────────────────────────────────── */
typedef enum {
    BM_CMD_NONE = 0,
    BM_CMD_OK,
    BM_CMD_ERROR
} bookmark_cmd_t;

/* Initialize bookmarks (clear all). */
void bookmark_init(void);

/* Number of active bookmarks. */
int bookmark_count(void);

/* Get bookmark by index (0-based). NULL if out of range. */
const bookmark_t *bookmark_get(int index);

/* Find bookmark by name (case-insensitive). NULL if not found. */
const bookmark_t *bookmark_find(const char *name);

/*
 * Save a new bookmark or overwrite existing one with same name.
 * Returns false if at capacity.
 */
bool bookmark_save(const char *name, const char *expr, const char *result,
                   ce_float_t numeric, bool has_numeric);

/* Delete bookmark by name. Returns false if not found. */
bool bookmark_delete(const char *name);

/* Clear all bookmarks. */
void bookmark_clear(void);

/*
 * Try to interpret input as a bookmark command.
 * Handles: bookmark("name"), bookmark(), delbm("name"), clearbm()
 * current_expr: current input buffer (used for bookmark("name"))
 * current_result: current result string
 */
bookmark_cmd_t bookmark_try_command(const char *input,
                                    const char *current_expr,
                                    const char *current_result,
                                    ce_float_t current_numeric,
                                    bool has_numeric,
                                    char *out_msg, int out_msg_size,
                                    char *err_msg, int err_msg_size);

/* Recall bookmark into the input buffer (copies expr to out). */
bool bookmark_recall(const char *name, char *out, int out_size);

/* Persist/load bookmarks from AppVar. */
void bookmark_save_file(void);
void bookmark_load_file(void);

#endif /* BOOKMARK_H */
