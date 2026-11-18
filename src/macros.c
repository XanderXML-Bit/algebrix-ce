/*
 * macros.c - User-defined expression shortcut macros.
 */
#include "macros.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    bool in_use;
    uint8_t param_count;
    char name[MACROS_NAME_MAX];
    char params[MACROS_PARAMS_MAX][MACROS_PARAM_NAME_MAX];
    char body[MACROS_BODY_MAX];
} macro_def_t;

static macro_def_t g_macros[MACROS_MAX_COUNT];

static bool is_ident_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static bool ci_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (!*s) return false;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
        s++;
        prefix++;
    }
    return true;
}

static void trim_span(const char **start, const char **end)
{
    while (*start < *end && isspace((unsigned char)**start)) (*start)++;
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) (*end)--;
}

static bool append_char(char *out, int out_size, int *pos, char c)
{
    if (*pos + 1 >= out_size) return false;
    out[*pos] = c;
    (*pos)++;
    out[*pos] = '\0';
    return true;
}

static bool append_str(char *out, int out_size, int *pos, const char *s)
{
    while (*s) {
        if (!append_char(out, out_size, pos, *s++)) return false;
    }
    return true;
}

static int find_macro(const char *name)
{
    int i;
    for (i = 0; i < MACROS_MAX_COUNT; i++) {
        if (g_macros[i].in_use && strcmp(g_macros[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int find_free_slot(void)
{
    int i;
    for (i = 0; i < MACROS_MAX_COUNT; i++) {
        if (!g_macros[i].in_use) return i;
    }
    return -1;
}

static bool parse_identifier(const char *src, int len, char *out, int out_size)
{
    int i;
    const char *start = src;
    const char *end = src + len;
    trim_span(&start, &end);
    if (start >= end) return false;
    if (!is_ident_start(*start)) return false;
    for (i = 1; start + i < end; i++) {
        if (!is_ident_char(start[i])) return false;
    }
    if (end - start >= out_size) return false;
    for (i = 0; start + i < end; i++) {
        out[i] = (char)tolower((unsigned char)start[i]);
    }
    out[i] = '\0';
    return true;
}

static void trim_in_place(char *s)
{
    char *start = s;
    char *end = s + strlen(s);
    while (*start && isspace((unsigned char)*start)) start++;
    while (end > start && isspace((unsigned char)*(end - 1))) end--;
    if (start != s) memmove(s, start, (size_t)(end - start));
    s[end - start] = '\0';
}

static bool parse_signature(const char *sig,
                            char *name_out,
                            char params_out[MACROS_PARAMS_MAX][MACROS_PARAM_NAME_MAX],
                            uint8_t *param_count_out,
                            char *err_msg, int err_msg_size)
{
    const char *start = sig;
    const char *end = sig + strlen(sig);
    const char *lp = NULL;
    const char *rp = NULL;
    uint8_t pcount = 0;
    int i;

    trim_span(&start, &end);
    if (start >= end) {
        snprintf(err_msg, err_msg_size, "Missing macro name");
        return false;
    }

    for (i = 0; start + i < end; i++) {
        if (start[i] == '(') {
            lp = start + i;
            break;
        }
    }

    if (!lp) {
        if (!parse_identifier(start, (int)(end - start), name_out, MACROS_NAME_MAX)) {
            snprintf(err_msg, err_msg_size, "Invalid macro name");
            return false;
        }
        *param_count_out = 0;
        return true;
    }

    rp = end - 1;
    if (*rp != ')' || rp <= lp) {
        snprintf(err_msg, err_msg_size, "Malformed macro signature");
        return false;
    }

    if (!parse_identifier(start, (int)(lp - start), name_out, MACROS_NAME_MAX)) {
        snprintf(err_msg, err_msg_size, "Invalid macro name");
        return false;
    }

    {
        const char *ps = lp + 1;
        const char *pe = rp;
        const char *cur = ps;
        while (cur <= pe) {
            const char *comma = cur;
            while (comma < pe && *comma != ',') comma++;

            if (comma == cur) {
                if (cur == pe) break; /* empty parameter list */
                snprintf(err_msg, err_msg_size, "Empty parameter name");
                return false;
            }

            if (pcount >= MACROS_PARAMS_MAX) {
                snprintf(err_msg, err_msg_size, "Too many macro parameters (max %d)",
                         MACROS_PARAMS_MAX);
                return false;
            }

            if (!parse_identifier(cur, (int)(comma - cur),
                                  params_out[pcount], MACROS_PARAM_NAME_MAX)) {
                snprintf(err_msg, err_msg_size, "Invalid parameter name");
                return false;
            }
            pcount++;

            if (comma == pe) break;
            cur = comma + 1;
        }
    }

    *param_count_out = pcount;
    return true;
}

static bool parse_call_args(const char *src,
                            char args[MACROS_PARAMS_MAX][MAX_INPUT_LEN],
                            int *argc,
                            int *consumed,
                            char *err_msg, int err_msg_size)
{
    int depth = 1;
    int i = 1;
    int argi = 0;
    int cur_len = 0;
    char current[MAX_INPUT_LEN];

    current[0] = '\0';
    if (src[0] != '(') {
        snprintf(err_msg, err_msg_size, "Malformed macro call");
        return false;
    }

    while (src[i] && depth > 0) {
        char c = src[i];

        if (c == '(') {
            depth++;
            if (depth > 1) {
                if (cur_len + 1 >= MAX_INPUT_LEN) {
                    snprintf(err_msg, err_msg_size, "Macro argument too long");
                    return false;
                }
                current[cur_len++] = c;
                current[cur_len] = '\0';
            }
            i++;
            continue;
        }

        if (c == ')') {
            depth--;
            if (depth == 0) {
                current[cur_len] = '\0';
                trim_in_place(current);
                if (argi == 0 && current[0] == '\0') {
                    *argc = 0;
                } else {
                    if (current[0] == '\0') {
                        snprintf(err_msg, err_msg_size, "Empty macro argument");
                        return false;
                    }
                    if (argi >= MACROS_PARAMS_MAX) {
                        snprintf(err_msg, err_msg_size,
                                 "Too many macro arguments (max %d)",
                                 MACROS_PARAMS_MAX);
                        return false;
                    }
                    strncpy(args[argi], current, MAX_INPUT_LEN - 1);
                    args[argi][MAX_INPUT_LEN - 1] = '\0';
                    *argc = argi + 1;
                }
                i++;
                *consumed = i;
                return true;
            }
            if (cur_len + 1 >= MAX_INPUT_LEN) {
                snprintf(err_msg, err_msg_size, "Macro argument too long");
                return false;
            }
            current[cur_len++] = c;
            current[cur_len] = '\0';
            i++;
            continue;
        }

        if (c == ',' && depth == 1) {
            current[cur_len] = '\0';
            trim_in_place(current);
            if (current[0] == '\0') {
                snprintf(err_msg, err_msg_size, "Empty macro argument");
                return false;
            }
            if (argi >= MACROS_PARAMS_MAX) {
                snprintf(err_msg, err_msg_size,
                         "Too many macro arguments (max %d)",
                         MACROS_PARAMS_MAX);
                return false;
            }
            strncpy(args[argi], current, MAX_INPUT_LEN - 1);
            args[argi][MAX_INPUT_LEN - 1] = '\0';
            argi++;
            cur_len = 0;
            current[0] = '\0';
            i++;
            continue;
        }

        if (cur_len + 1 >= MAX_INPUT_LEN) {
            snprintf(err_msg, err_msg_size, "Macro argument too long");
            return false;
        }
        current[cur_len++] = c;
        current[cur_len] = '\0';
        i++;
    }

    snprintf(err_msg, err_msg_size, "Unclosed macro argument list");
    return false;
}

static bool substitute_params(const macro_def_t *m,
                              char args[MACROS_PARAMS_MAX][MAX_INPUT_LEN],
                              char *out, int out_size,
                              char *err_msg, int err_msg_size)
{
    int i = 0;
    int pos = 0;

    out[0] = '\0';
    while (m->body[i]) {
        if (is_ident_start(m->body[i])) {
            int j = i + 1;
            int k;
            int repl = -1;
            char ident[MACROS_PARAM_NAME_MAX];
            int ident_len = 0;

            while (m->body[j] && is_ident_char(m->body[j])) j++;
            if (j - i >= MACROS_PARAM_NAME_MAX) {
                snprintf(err_msg, err_msg_size, "Macro parameter token too long");
                return false;
            }
            for (k = i; k < j; k++) {
                ident[ident_len++] = (char)tolower((unsigned char)m->body[k]);
            }
            ident[ident_len] = '\0';

            for (k = 0; k < m->param_count; k++) {
                if (strcmp(ident, m->params[k]) == 0) {
                    repl = k;
                    break;
                }
            }

            if (repl >= 0) {
                if (!append_char(out, out_size, &pos, '(') ||
                    !append_str(out, out_size, &pos, args[repl]) ||
                    !append_char(out, out_size, &pos, ')')) {
                    snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                    return false;
                }
            } else {
                for (k = i; k < j; k++) {
                    if (!append_char(out, out_size, &pos, m->body[k])) {
                        snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                        return false;
                    }
                }
            }
            i = j;
            continue;
        }

        if (!append_char(out, out_size, &pos, m->body[i])) {
            snprintf(err_msg, err_msg_size, "Macro expansion overflow");
            return false;
        }
        i++;
    }

    return true;
}

static bool expand_pass(const char *input, char *out, int out_size,
                        bool *changed, char *err_msg, int err_msg_size)
{
    int i = 0;
    int pos = 0;

    out[0] = '\0';
    *changed = false;

    while (input[i]) {
        if (is_ident_start(input[i])) {
            int j = i + 1;
            int k;
            int macro_idx;
            char ident[MACROS_NAME_MAX];
            int ident_len = 0;

            while (input[j] && is_ident_char(input[j])) j++;
            if (j - i >= MACROS_NAME_MAX) {
                /* Not a macro name length; copy as-is */
                for (k = i; k < j; k++) {
                    if (!append_char(out, out_size, &pos, input[k])) {
                        snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                        return false;
                    }
                }
                i = j;
                continue;
            }

            for (k = i; k < j; k++) {
                ident[ident_len++] = (char)tolower((unsigned char)input[k]);
            }
            ident[ident_len] = '\0';
            macro_idx = find_macro(ident);

            if (macro_idx < 0) {
                for (k = i; k < j; k++) {
                    if (!append_char(out, out_size, &pos, input[k])) {
                        snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                        return false;
                    }
                }
                i = j;
                continue;
            }

            {
                const macro_def_t *m = &g_macros[macro_idx];
                if (m->param_count == 0) {
                    /* Bare shortcut only; avoid clobbering function calls. */
                    if (input[j] == '(') {
                        for (k = i; k < j; k++) {
                            if (!append_char(out, out_size, &pos, input[k])) {
                                snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                                return false;
                            }
                        }
                        i = j;
                        continue;
                    }
                    if (!append_char(out, out_size, &pos, '(') ||
                        !append_str(out, out_size, &pos, m->body) ||
                        !append_char(out, out_size, &pos, ')')) {
                        snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                        return false;
                    }
                    *changed = true;
                    i = j;
                    continue;
                }

                if (input[j] != '(') {
                    for (k = i; k < j; k++) {
                        if (!append_char(out, out_size, &pos, input[k])) {
                            snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                            return false;
                        }
                    }
                    i = j;
                    continue;
                }

                {
                    char args[MACROS_PARAMS_MAX][MAX_INPUT_LEN];
                    int argc = 0;
                    int consumed = 0;
                    char substituted[MAX_INPUT_LEN];
                    if (!parse_call_args(input + j, args, &argc, &consumed,
                                         err_msg, err_msg_size)) {
                        return false;
                    }
                    if (argc != m->param_count) {
                        snprintf(err_msg, err_msg_size,
                                 "Macro '%s' expects %d args, got %d",
                                 m->name, (int)m->param_count, argc);
                        return false;
                    }
                    if (!substitute_params(m, args, substituted, sizeof(substituted),
                                           err_msg, err_msg_size)) {
                        return false;
                    }
                    if (!append_char(out, out_size, &pos, '(') ||
                        !append_str(out, out_size, &pos, substituted) ||
                        !append_char(out, out_size, &pos, ')')) {
                        snprintf(err_msg, err_msg_size, "Macro expansion overflow");
                        return false;
                    }
                    *changed = true;
                    i = j + consumed;
                    continue;
                }
            }
        }

        if (!append_char(out, out_size, &pos, input[i])) {
            snprintf(err_msg, err_msg_size, "Macro expansion overflow");
            return false;
        }
        i++;
    }

    return true;
}

void macros_init(void)
{
    memset(g_macros, 0, sizeof(g_macros));
}

int macros_count(void)
{
    int i, n = 0;
    for (i = 0; i < MACROS_MAX_COUNT; i++) {
        if (g_macros[i].in_use) n++;
    }
    return n;
}

macro_cmd_status_t macros_try_command(const char *input,
                                      char *out_msg, int out_msg_size,
                                      char *err_msg, int err_msg_size)
{
    char line[MAX_INPUT_LEN];
    const char *s = input;
    const char *e;

    if (!input || !out_msg || !err_msg) return MACRO_CMD_NONE;

    e = input + strlen(input);
    trim_span(&s, &e);
    if (s >= e) return MACRO_CMD_NONE;

    if ((int)(e - s) >= MAX_INPUT_LEN) {
        snprintf(err_msg, err_msg_size, "Input too long");
        return MACRO_CMD_ERROR;
    }

    memcpy(line, s, (size_t)(e - s));
    line[e - s] = '\0';

    if (ci_cmp(line, "macros()") == 0) {
        int i;
        int pos = 0;
        int count = macros_count();
        if (count == 0) {
            snprintf(out_msg, out_msg_size, "No macros defined");
            return MACRO_CMD_OK;
        }
        out_msg[0] = '\0';
        snprintf(out_msg, out_msg_size, "Macros (%d):", count);
        pos = (int)strlen(out_msg);
        for (i = 0; i < MACROS_MAX_COUNT; i++) {
            if (!g_macros[i].in_use) continue;
            if (!append_char(out_msg, out_msg_size, &pos, '\n') ||
                !append_str(out_msg, out_msg_size, &pos, g_macros[i].name)) {
                break;
            }
            if (g_macros[i].param_count > 0) {
                int p;
                if (!append_char(out_msg, out_msg_size, &pos, '(')) break;
                for (p = 0; p < g_macros[i].param_count; p++) {
                    if (p > 0 && !append_char(out_msg, out_msg_size, &pos, ',')) break;
                    if (!append_str(out_msg, out_msg_size, &pos, g_macros[i].params[p])) break;
                }
                if (!append_char(out_msg, out_msg_size, &pos, ')')) break;
            }
        }
        return MACRO_CMD_OK;
    }

    if (strlen(line) > 9 && ci_starts_with(line, "unmacro(") && line[strlen(line) - 1] == ')') {
        char name[MACROS_NAME_MAX];
        int idx;
        if (!parse_identifier(line + 8, (int)strlen(line) - 9, name, sizeof(name))) {
            snprintf(err_msg, err_msg_size, "Invalid macro name");
            return MACRO_CMD_ERROR;
        }
        idx = find_macro(name);
        if (idx < 0) {
            snprintf(err_msg, err_msg_size, "Macro '%s' not found", name);
            return MACRO_CMD_ERROR;
        }
        memset(&g_macros[idx], 0, sizeof(g_macros[idx]));
        snprintf(out_msg, out_msg_size, "Removed macro '%s'", name);
        return MACRO_CMD_OK;
    }

    if (strlen(line) > 8 && ci_starts_with(line, "macro(") && line[strlen(line) - 1] == ')') {
        char inside[MAX_INPUT_LEN];
        char signature[MAX_INPUT_LEN];
        char body[MAX_INPUT_LEN];
        char name[MACROS_NAME_MAX];
        char params[MACROS_PARAMS_MAX][MACROS_PARAM_NAME_MAX];
        uint8_t pcount = 0;
        int i, depth = 0, split = -1;
        int slot;
        const char *bs, *be;

        strncpy(inside, line + 6, sizeof(inside) - 1);
        inside[sizeof(inside) - 1] = '\0';
        inside[strlen(inside) - 1] = '\0'; /* drop trailing ')' */

        for (i = 0; inside[i]; i++) {
            if (inside[i] == '(') depth++;
            else if (inside[i] == ')') depth--;
            else if (inside[i] == ',' && depth == 0) {
                split = i;
                break;
            }
        }

        if (split <= 0) {
            snprintf(err_msg, err_msg_size, "Use macro(name, body)");
            return MACRO_CMD_ERROR;
        }

        strncpy(signature, inside, (size_t)split);
        signature[split] = '\0';
        strncpy(body, inside + split + 1, sizeof(body) - 1);
        body[sizeof(body) - 1] = '\0';
        trim_in_place(signature);
        trim_in_place(body);
        if (body[0] == '\0') {
            snprintf(err_msg, err_msg_size, "Macro body cannot be empty");
            return MACRO_CMD_ERROR;
        }
        if (strlen(body) >= MACROS_BODY_MAX) {
            snprintf(err_msg, err_msg_size, "Macro body too long (max %d chars)",
                     MACROS_BODY_MAX - 1);
            return MACRO_CMD_ERROR;
        }

        if (!parse_signature(signature, name, params, &pcount, err_msg, err_msg_size))
            return MACRO_CMD_ERROR;

        /* Trim body one more time via span to avoid internal buffer surprises */
        bs = body;
        be = body + strlen(body);
        trim_span(&bs, &be);
        if (be <= bs) {
            snprintf(err_msg, err_msg_size, "Macro body cannot be empty");
            return MACRO_CMD_ERROR;
        }

        slot = find_macro(name);
        if (slot < 0) slot = find_free_slot();
        if (slot < 0) {
            snprintf(err_msg, err_msg_size, "Macro table full (max %d)", MACROS_MAX_COUNT);
            return MACRO_CMD_ERROR;
        }

        memset(&g_macros[slot], 0, sizeof(g_macros[slot]));
        g_macros[slot].in_use = true;
        g_macros[slot].param_count = pcount;
        strncpy(g_macros[slot].name, name, MACROS_NAME_MAX - 1);
        g_macros[slot].name[MACROS_NAME_MAX - 1] = '\0';
        for (i = 0; i < pcount; i++) {
            strncpy(g_macros[slot].params[i], params[i], MACROS_PARAM_NAME_MAX - 1);
            g_macros[slot].params[i][MACROS_PARAM_NAME_MAX - 1] = '\0';
        }
        memcpy(g_macros[slot].body, bs, (size_t)(be - bs));
        g_macros[slot].body[be - bs] = '\0';

        if (pcount == 0)
            snprintf(out_msg, out_msg_size, "Macro '%s' saved", name);
        else
            snprintf(out_msg, out_msg_size, "Macro '%s' (%d args) saved",
                     name, (int)pcount);
        return MACRO_CMD_OK;
    }

    return MACRO_CMD_NONE;
}

bool macros_expand(const char *input, char *out, int out_size,
                   char *err_msg, int err_msg_size)
{
    char curr[MAX_INPUT_LEN];
    char next[MAX_INPUT_LEN];
    int pass;

    if (!input || !out || out_size <= 0) return false;
    if ((int)strlen(input) >= MAX_INPUT_LEN) {
        if (err_msg && err_msg_size > 0)
            snprintf(err_msg, err_msg_size, "Input too long");
        return false;
    }

    strncpy(curr, input, sizeof(curr) - 1);
    curr[sizeof(curr) - 1] = '\0';

    for (pass = 0; pass < MACROS_EXPAND_DEPTH; pass++) {
        bool changed = false;
        if (!expand_pass(curr, next, sizeof(next), &changed, err_msg, err_msg_size))
            return false;
        if (!changed) {
            if ((int)strlen(curr) >= out_size) {
                if (err_msg && err_msg_size > 0)
                    snprintf(err_msg, err_msg_size, "Expanded expression too long");
                return false;
            }
            strncpy(out, curr, (size_t)out_size - 1);
            out[out_size - 1] = '\0';
            return true;
        }
        strncpy(curr, next, sizeof(curr) - 1);
        curr[sizeof(curr) - 1] = '\0';
    }

    if (err_msg && err_msg_size > 0)
        snprintf(err_msg, err_msg_size,
                 "Macro expansion too deep (possible recursion)");
    return false;
}
