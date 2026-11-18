/*
 * history.c — Calculation history stack
 *
 * Circular buffer of past calculations with navigation support.
 * Static storage only (no malloc).
 */
#include "history.h"
#include <string.h>

static history_entry_t entries[HISTORY_MAX_ENTRIES];
static int head = 0;   /* index of next write position */
static int count = 0;  /* number of valid entries */
static int nav_pos = -1; /* navigation cursor: -1 = at input line */

static const char *history_skip_result_prefix(const char *text)
{
    if (!text) return "";

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    if (*text == '=') {
        text++;
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
            text++;
    }
    return text;
}

void history_clear(void)
{
    memset(entries, 0, sizeof(entries));
    head = 0;
    count = 0;
    nav_pos = -1;
}

bool history_push(const char *input, const char *result,
                  ce_float_t numeric, bool has_numeric)
{
    if (!input || !result) return false;

    history_entry_t *e = &entries[head];
    strncpy(e->input, input, HISTORY_EXPR_LEN - 1);
    e->input[HISTORY_EXPR_LEN - 1] = '\0';
    strncpy(e->result, result, HISTORY_RESULT_LEN - 1);
    e->result[HISTORY_RESULT_LEN - 1] = '\0';
    e->numeric_result = numeric;
    e->has_numeric = has_numeric;
    e->in_use = true;

    head = (head + 1) % HISTORY_MAX_ENTRIES;
    if (count < HISTORY_MAX_ENTRIES) count++;

    nav_pos = -1;
    return true;
}

int history_count(void)
{
    return count;
}

const history_entry_t *history_get(int index)
{
    if (index < 0 || index >= count) return NULL;

    /* index 0 = most recent */
    int real = (head - 1 - index + HISTORY_MAX_ENTRIES) % HISTORY_MAX_ENTRIES;
    return &entries[real];
}

const char *history_get_input(int index)
{
    const history_entry_t *e = history_get(index);
    return e ? e->input : "";
}

const char *history_get_result(int index)
{
    const history_entry_t *e = history_get(index);
    return e ? e->result : "";
}

bool history_get_reentry_text(int index, char *out, int out_size)
{
    const history_entry_t *e;
    const char *src;
    int len;

    if (!out || out_size <= 0) return false;
    out[0] = '\0';

    e = history_get(index);
    if (!e) return false;

    src = history_skip_result_prefix(e->result);
    if (!src[0]) src = e->input;
    if (!src || !src[0]) return false;

    strncpy(out, src, (size_t)out_size - 1u);
    out[out_size - 1] = '\0';

    len = (int)strlen(out);
    while (len > 0) {
        char ch = out[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
        out[len - 1] = '\0';
        len--;
    }
    return out[0] != '\0';
}

int history_navigate_back(void)
{
    if (count == 0) return -1;

    if (nav_pos < 0) {
        nav_pos = 0;
    } else if (nav_pos < count - 1) {
        nav_pos++;
    }
    return nav_pos;
}

int history_navigate_forward(void)
{
    if (nav_pos < 0) return -1;

    nav_pos--;
    return nav_pos;
}

void history_navigate_reset(void)
{
    nav_pos = -1;
}

int history_navigate_position(void)
{
    return nav_pos;
}
