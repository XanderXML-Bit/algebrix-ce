/*
 * bookmark.c — Expression bookmarks
 */
#include "bookmark.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <fileioc.h>

#define BOOKMARK_APPVAR "CEBKMRK"
#define BOOKMARK_MAGIC  0xBB

static bookmark_t g_bm[BOOKMARK_MAX_COUNT];
static uint8_t    g_count = 0;

/* ── Internal helpers ────────────────────────────────────── */

static bool ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return !*a && !*b;
}

static const char *skip_ws(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Parse a quoted string like "name" or 'name'. Returns end pointer or NULL. */
static const char *parse_quoted(const char *s, char *out, int out_size)
{
    char quote;
    int n = 0;
    s = skip_ws(s);
    if (*s != '"' && *s != '\'') {
        /* Unquoted: read until ) or , or whitespace */
        while (*s && *s != ')' && *s != ',' && !isspace((unsigned char)*s) && n < out_size - 1)
            out[n++] = *s++;
        out[n] = '\0';
        return s;
    }
    quote = *s++;
    while (*s && *s != quote && n < out_size - 1)
        out[n++] = *s++;
    out[n] = '\0';
    if (*s == quote) s++;
    return s;
}

/* ── Lifecycle ───────────────────────────────────────────── */

void bookmark_init(void)
{
    memset(g_bm, 0, sizeof(g_bm));
    g_count = 0;
}

int bookmark_count(void) { return g_count; }

const bookmark_t *bookmark_get(int index)
{
    int found = 0, i;
    if (index < 0 || index >= g_count) return NULL;
    for (i = 0; i < BOOKMARK_MAX_COUNT; i++) {
        if (g_bm[i].in_use) {
            if (found == index) return &g_bm[i];
            found++;
        }
    }
    return NULL;
}

const bookmark_t *bookmark_find(const char *name)
{
    int i;
    if (!name) return NULL;
    for (i = 0; i < BOOKMARK_MAX_COUNT; i++) {
        if (g_bm[i].in_use && ci_eq(g_bm[i].name, name))
            return &g_bm[i];
    }
    return NULL;
}

bool bookmark_save(const char *name, const char *expr, const char *result,
                   ce_float_t numeric, bool has_numeric)
{
    int i, free_slot = -1;
    if (!name || !*name) return false;

    for (i = 0; i < BOOKMARK_MAX_COUNT; i++) {
        if (g_bm[i].in_use && ci_eq(g_bm[i].name, name)) {
            /* Overwrite */
            strncpy(g_bm[i].expr,   expr   ? expr   : "", BOOKMARK_EXPR_MAX - 1);
            strncpy(g_bm[i].result, result ? result : "", BOOKMARK_RESULT_MAX - 1);
            g_bm[i].numeric     = numeric;
            g_bm[i].has_numeric = has_numeric;
            return true;
        }
        if (!g_bm[i].in_use && free_slot < 0) free_slot = i;
    }

    if (free_slot < 0) return false;  /* full */

    memset(&g_bm[free_slot], 0, sizeof(g_bm[free_slot]));
    g_bm[free_slot].in_use = true;
    strncpy(g_bm[free_slot].name,   name,   BOOKMARK_NAME_MAX - 1);
    strncpy(g_bm[free_slot].expr,   expr   ? expr   : "", BOOKMARK_EXPR_MAX - 1);
    strncpy(g_bm[free_slot].result, result ? result : "", BOOKMARK_RESULT_MAX - 1);
    g_bm[free_slot].numeric     = numeric;
    g_bm[free_slot].has_numeric = has_numeric;
    g_count++;
    return true;
}

bool bookmark_delete(const char *name)
{
    int i;
    if (!name) return false;
    for (i = 0; i < BOOKMARK_MAX_COUNT; i++) {
        if (g_bm[i].in_use && ci_eq(g_bm[i].name, name)) {
            memset(&g_bm[i], 0, sizeof(g_bm[i]));
            g_count--;
            return true;
        }
    }
    return false;
}

void bookmark_clear(void)
{
    memset(g_bm, 0, sizeof(g_bm));
    g_count = 0;
}

/* ── Command parser ──────────────────────────────────────── */

bookmark_cmd_t bookmark_try_command(const char *input,
                                    const char *current_expr,
                                    const char *current_result,
                                    ce_float_t current_numeric,
                                    bool has_numeric,
                                    char *out_msg, int out_msg_size,
                                    char *err_msg, int err_msg_size)
{
    const char *p = skip_ws(input);
    if (!p || !*p) return BM_CMD_NONE;

    /* --- bookmark("name") or bookmark() --- */
    if (strncasecmp(p, "bookmark", 8) == 0) {
        const char *q = p + 8;
        q = skip_ws(q);
        if (*q == '(' || *q == '\0') {
            if (*q == '(') q++;
            q = skip_ws(q);
            if (*q == ')' || *q == '\0') {
                /* bookmark() — list all */
                int i, pos = 0;
                pos += snprintf(out_msg + pos, (size_t)(out_msg_size - pos),
                                "Bookmarks (%d):\n", g_count);
                if (g_count == 0) {
                    snprintf(out_msg + pos, (size_t)(out_msg_size - pos), "  (none)");
                } else {
                    for (i = 0; i < BOOKMARK_MAX_COUNT && pos < out_msg_size - 4; i++) {
                        if (!g_bm[i].in_use) continue;
                        pos += snprintf(out_msg + pos, (size_t)(out_msg_size - pos),
                                        "  [%s] %s = %s\n",
                                        g_bm[i].name,
                                        g_bm[i].expr,
                                        g_bm[i].result);
                    }
                }
                return BM_CMD_OK;
            } else {
                /* bookmark("name") — save */
                char name[BOOKMARK_NAME_MAX];
                q = parse_quoted(q, name, sizeof(name));
                if (!name[0]) {
                    /* Auto-generate name from expr */
                    if (current_expr && *current_expr) {
                        strncpy(name, current_expr, BOOKMARK_NAME_MAX - 1);
                        name[BOOKMARK_NAME_MAX - 1] = '\0';
                    } else {
                        snprintf(name, sizeof(name), "bm%d", g_count + 1);
                    }
                }
                if (!bookmark_save(name, current_expr, current_result,
                                    current_numeric, has_numeric)) {
                    snprintf(err_msg, (size_t)err_msg_size, "bookmark: storage full (max %d)", BOOKMARK_MAX_COUNT);
                    return BM_CMD_ERROR;
                }
                snprintf(out_msg, (size_t)out_msg_size,
                         "Saved bookmark: [%s] %s = %s",
                         name, current_expr ? current_expr : "",
                         current_result ? current_result : "");
                bookmark_save_file();
                return BM_CMD_OK;
            }
        }
        return BM_CMD_NONE;
    }

    /* --- delbm("name") --- */
    if (strncasecmp(p, "delbm", 5) == 0 && p[5] == '(') {
        char name[BOOKMARK_NAME_MAX];
        const char *q = parse_quoted(p + 6, name, sizeof(name));
        (void)q;
        if (!bookmark_delete(name)) {
            snprintf(err_msg, (size_t)err_msg_size, "delbm: '%s' not found", name);
            return BM_CMD_ERROR;
        }
        snprintf(out_msg, (size_t)out_msg_size, "Deleted bookmark: %s", name);
        bookmark_save_file();
        return BM_CMD_OK;
    }

    /* --- clearbm() --- */
    if (strncasecmp(p, "clearbm", 7) == 0) {
        int was = g_count;
        bookmark_clear();
        snprintf(out_msg, (size_t)out_msg_size, "Cleared %d bookmark(s)", was);
        bookmark_save_file();
        return BM_CMD_OK;
    }

    return BM_CMD_NONE;
}

bool bookmark_recall(const char *name, char *out, int out_size)
{
    const bookmark_t *bm = bookmark_find(name);
    if (!bm) return false;
    if (bm->result[0]) {
        /* Prefer the result, stripping leading "= " */
        const char *r = bm->result;
        if (r[0] == '=' && r[1] == ' ') r += 2;
        strncpy(out, r, (size_t)(out_size - 1));
    } else {
        strncpy(out, bm->expr, (size_t)(out_size - 1));
    }
    out[out_size - 1] = '\0';
    return true;
}

/* ── Persistence ─────────────────────────────────────────── */

#pragma pack(push, 1)
typedef struct {
    uint8_t magic;
    uint8_t count;
    bookmark_t entries[BOOKMARK_MAX_COUNT];
} bm_store_t;
#pragma pack(pop)

void bookmark_save_file(void)
{
    bm_store_t store;
    ti_var_t slot = ti_Open(BOOKMARK_APPVAR, "w");
    if (!slot) return;
    store.magic = BOOKMARK_MAGIC;
    store.count = g_count;
    memcpy(store.entries, g_bm, sizeof(g_bm));
    ti_Write(&store, sizeof(store), 1, slot);
    ti_Close(slot);
}

void bookmark_load_file(void)
{
    bm_store_t store;
    ti_var_t slot = ti_Open(BOOKMARK_APPVAR, "r");
    if (!slot) return;
    if (ti_Read(&store, sizeof(store), 1, slot) != 1) goto fail;
    if (store.magic != BOOKMARK_MAGIC) goto fail;
    if (store.count > BOOKMARK_MAX_COUNT) goto fail;
    memcpy(g_bm, store.entries, sizeof(g_bm));
    g_count = store.count;
    /* Ensure null termination */
    {
        int i;
        for (i = 0; i < BOOKMARK_MAX_COUNT; i++) {
            g_bm[i].name[BOOKMARK_NAME_MAX - 1]     = '\0';
            g_bm[i].expr[BOOKMARK_EXPR_MAX - 1]     = '\0';
            g_bm[i].result[BOOKMARK_RESULT_MAX - 1] = '\0';
        }
    }
    ti_Close(slot);
    return;
fail:
    ti_Close(slot);
}
