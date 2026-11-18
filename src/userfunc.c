/*
 * userfunc.c — User-defined custom functions with workspace management
 */
#include "userfunc.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <fileioc.h>

/* ── AppVar name ─────────────────────────────────────────── */
#define USERFUNC_APPVAR "CEUSERFN"

/* ── Workspace storage ───────────────────────────────────── */
typedef struct {
    char name[USERFUNC_WS_NAME_MAX];
    userfunc_def_t funcs[USERFUNC_MAX_PER_WS];
    uint8_t count;
} workspace_t;

static workspace_t g_ws[USERFUNC_WS_COUNT];
static int         g_active = 0;

/* ── Internal helpers ────────────────────────────────────── */

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static bool ci_eq(const char *a, const char *b)
{
    return ci_cmp(a, b) == 0;
}

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}
static bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static const char *skip_ws(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static bool parse_ident(const char *s, char *out, int out_size, const char **after)
{
    int n = 0;
    s = skip_ws(s);
    if (!is_ident_start(*s)) return false;
    while (is_ident_char(*s) && n < out_size - 1)
        out[n++] = *s++;
    out[n] = '\0';
    if (after) *after = s;
    return n > 0;
}

/* ── Lifecycle ───────────────────────────────────────────── */

void userfunc_init(void)
{
    int i;
    memset(g_ws, 0, sizeof(g_ws));
    for (i = 0; i < USERFUNC_WS_COUNT; i++) {
        snprintf(g_ws[i].name, USERFUNC_WS_NAME_MAX, "WS%d", i+1);
    }
    strncpy(g_ws[0].name, "Default", USERFUNC_WS_NAME_MAX - 1);
    g_active = 0;
}

int userfunc_active_ws(void)   { return g_active; }
const char *userfunc_active_ws_name(void) { return g_ws[g_active].name; }

bool userfunc_switch_ws(const char *name)
{
    int i;
    if (!name || !*name) return false;
    /* Find existing */
    for (i = 0; i < USERFUNC_WS_COUNT; i++) {
        if (ci_eq(g_ws[i].name, name)) { g_active = i; return true; }
    }
    /* Create in first empty slot (name == "WS1/2/3/4" = unused default) */
    for (i = 0; i < USERFUNC_WS_COUNT; i++) {
        char def[8];
        snprintf(def, sizeof(def), "WS%d", i+1);
        if (ci_eq(g_ws[i].name, def) && g_ws[i].count == 0) {
            strncpy(g_ws[i].name, name, USERFUNC_WS_NAME_MAX - 1);
            g_ws[i].name[USERFUNC_WS_NAME_MAX - 1] = '\0';
            g_active = i;
            return true;
        }
    }
    return false;  /* All workspaces in use */
}

/* ── Query ───────────────────────────────────────────────── */

int userfunc_count(void)
{
    return g_ws[g_active].count;
}

const userfunc_def_t *userfunc_get(int index)
{
    workspace_t *ws = &g_ws[g_active];
    if (index < 0 || index >= ws->count) return NULL;
    /* Find the nth in_use slot */
    int found = 0, i;
    for (i = 0; i < USERFUNC_MAX_PER_WS; i++) {
        if (ws->funcs[i].in_use) {
            if (found == index) return &ws->funcs[i];
            found++;
        }
    }
    return NULL;
}

const userfunc_def_t *userfunc_find(const char *name)
{
    workspace_t *ws = &g_ws[g_active];
    int i;
    if (!name) return NULL;
    for (i = 0; i < USERFUNC_MAX_PER_WS; i++) {
        if (ws->funcs[i].in_use && ci_eq(ws->funcs[i].name, name))
            return &ws->funcs[i];
    }
    return NULL;
}

bool userfunc_delete(const char *name)
{
    workspace_t *ws = &g_ws[g_active];
    int i;
    if (!name) return false;
    for (i = 0; i < USERFUNC_MAX_PER_WS; i++) {
        if (ws->funcs[i].in_use && ci_eq(ws->funcs[i].name, name)) {
            memset(&ws->funcs[i], 0, sizeof(ws->funcs[i]));
            ws->count--;
            return true;
        }
    }
    return false;
}

void userfunc_clear_active(void)
{
    workspace_t *ws = &g_ws[g_active];
    memset(ws->funcs, 0, sizeof(ws->funcs));
    ws->count = 0;
}

/* ── Define a new function ───────────────────────────────── */

static bool do_define(const char *name, const char *param, const char *body,
                      char *err, int err_size)
{
    workspace_t *ws = &g_ws[g_active];
    int i, free_slot = -1;

    if (!name[0]) {
        snprintf(err, (size_t)err_size, "define: empty function name");
        return false;
    }
    if (!param[0]) {
        snprintf(err, (size_t)err_size, "define: empty parameter name");
        return false;
    }
    if (!body[0]) {
        snprintf(err, (size_t)err_size, "define: empty function body");
        return false;
    }
    if (strlen(body) >= USERFUNC_BODY_MAX) {
        snprintf(err, (size_t)err_size, "define: body too long (max %d)", USERFUNC_BODY_MAX - 1);
        return false;
    }

    /* Update existing or find free slot */
    for (i = 0; i < USERFUNC_MAX_PER_WS; i++) {
        if (ws->funcs[i].in_use && ci_eq(ws->funcs[i].name, name)) {
            /* Update in place */
            strncpy(ws->funcs[i].param, param, USERFUNC_PARAM_MAX - 1);
            strncpy(ws->funcs[i].body,  body,  USERFUNC_BODY_MAX - 1);
            return true;
        }
        if (!ws->funcs[i].in_use && free_slot < 0) free_slot = i;
    }

    if (free_slot < 0) {
        snprintf(err, (size_t)err_size, "define: workspace full (max %d functions)", USERFUNC_MAX_PER_WS);
        return false;
    }

    ws->funcs[free_slot].in_use = true;
    strncpy(ws->funcs[free_slot].name,  name,  USERFUNC_NAME_MAX - 1);
    strncpy(ws->funcs[free_slot].param, param, USERFUNC_PARAM_MAX - 1);
    strncpy(ws->funcs[free_slot].body,  body,  USERFUNC_BODY_MAX - 1);
    ws->funcs[free_slot].name[USERFUNC_NAME_MAX - 1]  = '\0';
    ws->funcs[free_slot].param[USERFUNC_PARAM_MAX - 1] = '\0';
    ws->funcs[free_slot].body[USERFUNC_BODY_MAX - 1]  = '\0';
    ws->count++;
    return true;
}

/* ── Command parser ──────────────────────────────────────── */

userfunc_cmd_t userfunc_try_command(const char *input,
                                    char *out_msg, int out_msg_size,
                                    char *err_msg, int err_msg_size)
{
    const char *p = skip_ws(input);
    char ident[USERFUNC_NAME_MAX + 4];
    const char *after = NULL;

    if (!p || !*p) return UFUNC_CMD_NONE;

    /* --- define(name, param, body) --- */
    if (strncasecmp(p, "define", 6) == 0 && (p[6] == '(' || isspace((unsigned char)p[6]))) {
        char name[USERFUNC_NAME_MAX], param[USERFUNC_PARAM_MAX], body[USERFUNC_BODY_MAX];
        const char *q = p + 6;
        q = skip_ws(q);
        if (*q != '(') {
            snprintf(err_msg, (size_t)err_msg_size, "define: expected '('");
            return UFUNC_CMD_ERROR;
        }
        q++;
        if (!parse_ident(q, name, sizeof(name), &q)) {
            snprintf(err_msg, (size_t)err_msg_size, "define: expected function name");
            return UFUNC_CMD_ERROR;
        }
        q = skip_ws(q); if (*q != ',') {
            snprintf(err_msg, (size_t)err_msg_size, "define: expected ','");
            return UFUNC_CMD_ERROR;
        }
        q++;
        if (!parse_ident(q, param, sizeof(param), &q)) {
            snprintf(err_msg, (size_t)err_msg_size, "define: expected parameter name");
            return UFUNC_CMD_ERROR;
        }
        q = skip_ws(q); if (*q != ',') {
            snprintf(err_msg, (size_t)err_msg_size, "define: expected ',' before body");
            return UFUNC_CMD_ERROR;
        }
        q++;
        q = skip_ws(q);
        /* body = rest up to closing ')' (last one) */
        {
            int depth = 1;
            const char *body_start = q;
            const char *body_end = q;
            while (*q && depth > 0) {
                if (*q == '(') depth++;
                else if (*q == ')') { depth--; if (depth == 0) { body_end = q; break; } }
                q++;
            }
            if (depth != 0) {
                snprintf(err_msg, (size_t)err_msg_size, "define: unmatched parentheses");
                return UFUNC_CMD_ERROR;
            }
            int blen = (int)(body_end - body_start);
            if (blen <= 0) {
                snprintf(err_msg, (size_t)err_msg_size, "define: empty body");
                return UFUNC_CMD_ERROR;
            }
            if (blen >= (int)sizeof(body)) blen = (int)sizeof(body) - 1;
            memcpy(body, body_start, (size_t)blen);
            /* trim trailing whitespace */
            while (blen > 0 && isspace((unsigned char)body[blen-1])) blen--;
            body[blen] = '\0';
        }
        if (!do_define(name, param, body, err_msg, err_msg_size))
            return UFUNC_CMD_ERROR;
        snprintf(out_msg, (size_t)out_msg_size,
                 "Defined: %s(%s) = %s  [ws: %s]", name, param, body, g_ws[g_active].name);
        userfunc_save();
        return UFUNC_CMD_OK;
    }

    /* --- name(param) = body  (alternate define syntax) --- */
    {
        char name[USERFUNC_NAME_MAX], param[USERFUNC_PARAM_MAX];
        if (parse_ident(p, name, sizeof(name), &after)) {
            const char *q = skip_ws(after);
            if (*q == '(') {
                q++;
                if (parse_ident(q, param, sizeof(param), &q)) {
                    q = skip_ws(q);
                    if (*q == ')') {
                        q++;
                        q = skip_ws(q);
                        if (*q == '=') {
                            q++;
                            q = skip_ws(q);
                            if (*q) {
                                char body[USERFUNC_BODY_MAX];
                                strncpy(body, q, sizeof(body) - 1);
                                body[sizeof(body) - 1] = '\0';
                                /* trim trailing whitespace */
                                int blen = (int)strlen(body);
                                while (blen > 0 && isspace((unsigned char)body[blen-1])) blen--;
                                body[blen] = '\0';
                                if (!do_define(name, param, body, err_msg, err_msg_size))
                                    return UFUNC_CMD_ERROR;
                                snprintf(out_msg, (size_t)out_msg_size,
                                         "Defined: %s(%s) = %s  [ws: %s]",
                                         name, param, body, g_ws[g_active].name);
                                userfunc_save();
                                return UFUNC_CMD_OK;
                            }
                        }
                    }
                }
            }
        }
    }

    /* --- undef(name) --- */
    if (strncasecmp(p, "undef", 5) == 0 && p[5] == '(') {
        char name[USERFUNC_NAME_MAX];
        const char *q = p + 6;
        if (!parse_ident(q, name, sizeof(name), &q)) {
            snprintf(err_msg, (size_t)err_msg_size, "undef: expected function name");
            return UFUNC_CMD_ERROR;
        }
        if (!userfunc_delete(name)) {
            snprintf(err_msg, (size_t)err_msg_size, "undef: '%s' not defined", name);
            return UFUNC_CMD_ERROR;
        }
        snprintf(out_msg, (size_t)out_msg_size, "Removed: %s", name);
        userfunc_save();
        return UFUNC_CMD_OK;
    }

    /* --- clearfuncs() --- */
    if (strncasecmp(p, "clearfuncs", 10) == 0) {
        int was = g_ws[g_active].count;
        userfunc_clear_active();
        snprintf(out_msg, (size_t)out_msg_size,
                 "Cleared %d function(s) from '%s'", was, g_ws[g_active].name);
        userfunc_save();
        return UFUNC_CMD_OK;
    }

    /* --- funcs() --- */
    if (strncasecmp(p, "funcs", 5) == 0 && (p[5] == '(' || p[5] == '\0' || isspace((unsigned char)p[5]))) {
        workspace_t *ws = &g_ws[g_active];
        int i, pos = 0;
        pos += snprintf(out_msg + pos, (size_t)(out_msg_size - pos),
                        "Workspace: %s (%d fn)\n", ws->name, ws->count);
        for (i = 0; i < USERFUNC_MAX_PER_WS && pos < out_msg_size - 4; i++) {
            if (ws->funcs[i].in_use) {
                pos += snprintf(out_msg + pos, (size_t)(out_msg_size - pos),
                                "  %s(%s)=%s\n",
                                ws->funcs[i].name,
                                ws->funcs[i].param,
                                ws->funcs[i].body);
            }
        }
        if (ws->count == 0)
            snprintf(out_msg + pos, (size_t)(out_msg_size - pos), "  (none defined)");
        return UFUNC_CMD_OK;
    }

    /* --- ws(name) or ws() --- */
    if (strncasecmp(p, "ws", 2) == 0 && (p[2] == '(' || p[2] == '\0')) {
        const char *q = p + 2;
        q = skip_ws(q);
        if (*q == '(' && *(q+1) == ')') {
            /* ws() — show current */
            snprintf(out_msg, (size_t)out_msg_size,
                     "Active workspace: %s (%d/%d functions)",
                     g_ws[g_active].name, g_ws[g_active].count, USERFUNC_MAX_PER_WS);
            return UFUNC_CMD_OK;
        }
        if (*q == '(') {
            char ws_name[USERFUNC_WS_NAME_MAX];
            q++;
            if (!parse_ident(q, ws_name, sizeof(ws_name), &q)) {
                snprintf(err_msg, (size_t)err_msg_size, "ws: expected workspace name");
                return UFUNC_CMD_ERROR;
            }
            if (!userfunc_switch_ws(ws_name)) {
                snprintf(err_msg, (size_t)err_msg_size, "ws: all workspaces in use");
                return UFUNC_CMD_ERROR;
            }
            snprintf(out_msg, (size_t)out_msg_size,
                     "Switched to workspace: %s (%d functions)",
                     g_ws[g_active].name, g_ws[g_active].count);
            return UFUNC_CMD_OK;
        }
    }

    (void)ident;
    return UFUNC_CMD_NONE;
}

/* ── Expansion ───────────────────────────────────────────── */

bool userfunc_expand(const char *input, char *out, int out_size,
                     char *err_msg, int err_msg_size)
{
    workspace_t *ws = &g_ws[g_active];
    int i;
    bool found_any = false;
    char tmp[MAX_INPUT_LEN * 2];
    const char *src = input;

    strncpy(tmp, input, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Multiple passes to handle chained calls (up to 4 passes) */
    int pass;
    for (pass = 0; pass < 4; pass++) {
        bool changed = false;
        int out_pos = 0;
        src = tmp;

        while (*src && out_pos < out_size - 2) {
            /* Look ahead for a function call f( */
            bool matched = false;
            for (i = 0; i < USERFUNC_MAX_PER_WS; i++) {
                if (!ws->funcs[i].in_use) continue;
                const char *fname = ws->funcs[i].name;
                int flen = (int)strlen(fname);
                if (strncasecmp(src, fname, (size_t)flen) == 0 && src[flen] == '(') {
                    /* Match: extract argument */
                    const char *arg_start = src + flen + 1;
                    const char *arg_end = arg_start;
                    int depth = 1;
                    while (*arg_end && depth > 0) {
                        if (*arg_end == '(') depth++;
                        else if (*arg_end == ')') { depth--; if (!depth) break; }
                        arg_end++;
                    }
                    if (depth != 0) {
                        snprintf(err_msg, (size_t)err_msg_size,
                                 "userfunc: unmatched '(' in call to %s", fname);
                        return false;
                    }
                    int alen = (int)(arg_end - arg_start);
                    char arg_buf[MAX_INPUT_LEN];
                    if (alen >= (int)sizeof(arg_buf)) {
                        snprintf(err_msg, (size_t)err_msg_size, "userfunc: argument too long");
                        return false;
                    }
                    memcpy(arg_buf, arg_start, (size_t)alen);
                    arg_buf[alen] = '\0';

                    /* Substitute: replace param with arg in body */
                    const char *param = ws->funcs[i].param;
                    const char *body  = ws->funcs[i].body;
                    int plen = (int)strlen(param);

                    /* Build substituted body */
                    const char *bp = body;
                    out[out_pos++] = '(';
                    while (*bp && out_pos < out_size - 4) {
                        /* Check for param match (whole word) */
                        bool prev_ident = (bp > body && is_ident_char(*(bp-1)));
                        bool next_ident = is_ident_char(bp[plen]);
                        if (!prev_ident && strncasecmp(bp, param, (size_t)plen) == 0 && !next_ident) {
                            out[out_pos++] = '(';
                            int al2 = (int)strlen(arg_buf);
                            if (out_pos + al2 < out_size - 2) {
                                memcpy(out + out_pos, arg_buf, (size_t)al2);
                                out_pos += al2;
                            }
                            out[out_pos++] = ')';
                            bp += plen;
                        } else {
                            out[out_pos++] = *bp++;
                        }
                    }
                    out[out_pos++] = ')';

                    src = arg_end + 1; /* skip past closing ')' */
                    matched = true;
                    changed = true;
                    found_any = true;
                    break;
                }
            }
            if (!matched) {
                if (out_pos < out_size - 1) out[out_pos++] = *src++;
                else break;
            }
        }
        out[out_pos] = '\0';

        if (!changed) break;
        /* Copy out back to tmp for next pass */
        strncpy(tmp, out, sizeof(tmp) - 1);
    }

    if (!found_any) {
        /* No expansion needed — just copy input to out */
        strncpy(out, input, (size_t)(out_size - 1));
        out[out_size - 1] = '\0';
    }
    return true;
}

/* ── Persistence ─────────────────────────────────────────── */

#pragma pack(push, 1)
typedef struct {
    uint8_t magic;  /* 0xCF */
    workspace_t ws[USERFUNC_WS_COUNT];
    uint8_t active;
} userfunc_store_t;
#pragma pack(pop)

void userfunc_save(void)
{
    userfunc_store_t store;
    ti_var_t slot = ti_Open(USERFUNC_APPVAR, "w");
    if (!slot) return;
    store.magic  = 0xCF;
    store.active = (uint8_t)g_active;
    memcpy(store.ws, g_ws, sizeof(g_ws));
    ti_Write(&store, sizeof(store), 1, slot);
    ti_Close(slot);
}

void userfunc_load(void)
{
    userfunc_store_t store;
    ti_var_t slot = ti_Open(USERFUNC_APPVAR, "r");
    if (!slot) return;
    if (ti_Read(&store, sizeof(store), 1, slot) != 1) goto fail;
    if (store.magic != 0xCF) goto fail;
    if (store.active >= USERFUNC_WS_COUNT) goto fail;
    memcpy(g_ws, store.ws, sizeof(g_ws));
    g_active = store.active;
    /* Ensure null termination */
    {
        int i, j;
        for (i = 0; i < USERFUNC_WS_COUNT; i++) {
            g_ws[i].name[USERFUNC_WS_NAME_MAX - 1] = '\0';
            for (j = 0; j < USERFUNC_MAX_PER_WS; j++) {
                g_ws[i].funcs[j].name[USERFUNC_NAME_MAX - 1] = '\0';
                g_ws[i].funcs[j].param[USERFUNC_PARAM_MAX - 1] = '\0';
                g_ws[i].funcs[j].body[USERFUNC_BODY_MAX - 1] = '\0';
            }
        }
    }
    ti_Close(slot);
    return;
fail:
    ti_Close(slot);
}
