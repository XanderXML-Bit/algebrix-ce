/*
 * list.c — List operations implementation
 *
 * Static storage for 6 lists (L1-L6), up to 50 elements each.
 * Provides scalar-returning, list-returning, and element-wise operations.
 */
#include "list.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* ── Global list storage ─────────────────────────────────── */
list_t g_lists[MAX_LIST_COUNT];

void list_init_all(void)
{
    memset(g_lists, 0, sizeof(g_lists));
}

void list_set(int idx, const ce_float_t *data, int len)
{
    int i;
    if (idx < 0 || idx >= MAX_LIST_COUNT) return;
    if (len < 0) len = 0;
    if (len > MAX_LIST_SIZE) len = MAX_LIST_SIZE;
    if (len > 0 && data == NULL) len = 0;
    g_lists[idx].len = (uint8_t)len;
    for (i = 0; i < len; i++)
        g_lists[idx].data[i] = data[i];
}

const list_t *list_get(int idx)
{
    if (idx < 0 || idx >= MAX_LIST_COUNT) return NULL;
    return &g_lists[idx];
}

/* ── Helpers ─────────────────────────────────────────────── */

static void isort(ce_float_t *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        ce_float_t key = a[i];
        j = i - 1;
        while (j >= 0 && a[j] > key) { a[j+1] = a[j]; j--; }
        a[j+1] = key;
    }
}

/* ── Scalar-returning functions ──────────────────────────── */

ce_float_t list_sum(const list_t *lst)
{
    ce_float_t s = 0;
    int i;
    for (i = 0; i < lst->len; i++) s += lst->data[i];
    return s;
}

ce_float_t list_prod(const list_t *lst)
{
    ce_float_t p = 1;
    int i;
    if (lst->len == 0) return 1;  /* empty product = 1 (mathematical convention) */
    for (i = 0; i < lst->len; i++) p *= lst->data[i];
    return p;
}

ce_float_t list_min(const list_t *lst)
{
    ce_float_t v;
    int i;
    if (lst->len == 0) return 0;
    v = lst->data[0];
    for (i = 1; i < lst->len; i++)
        if (lst->data[i] < v) v = lst->data[i];
    return v;
}

ce_float_t list_max(const list_t *lst)
{
    ce_float_t v;
    int i;
    if (lst->len == 0) return 0;
    v = lst->data[0];
    for (i = 1; i < lst->len; i++)
        if (lst->data[i] > v) v = lst->data[i];
    return v;
}

ce_float_t list_mean(const list_t *lst)
{
    if (lst->len == 0) return 0;
    return list_sum(lst) / (ce_float_t)lst->len;
}

ce_float_t list_median(const list_t *lst)
{
    /* static: 200 B off the 4 KB CE stack. Called below eval's stack
     * backstop (eval_list_op), and it never re-enters (isort only). */
    static ce_float_t tmp[MAX_LIST_SIZE];
    int n = lst->len;
    if (n == 0) return 0;
    memcpy(tmp, lst->data, n * sizeof(ce_float_t));
    isort(tmp, n);
    if (n % 2 == 1) return tmp[n/2];
    return (tmp[n/2-1] + tmp[n/2]) / 2.0f;
}

ce_float_t list_stdev(const list_t *lst)
{
    ce_float_t m, sum_sq = 0;
    int i, n = lst->len;
    if (n <= 1) return 0;
    m = list_mean(lst);
    for (i = 0; i < n; i++) {
        ce_float_t d = lst->data[i] - m;
        sum_sq += d * d;
    }
    return sqrtf(sum_sq / (ce_float_t)(n - 1));
}

int list_dim(const list_t *lst)
{
    return lst->len;
}

/* ── List-returning functions ────────────────────────────── */

void list_cumsum(const list_t *in, list_t *out)
{
    ce_float_t running = 0;
    int i;
    out->len = in->len;
    for (i = 0; i < in->len; i++) {
        running += in->data[i];
        out->data[i] = running;
    }
}

void list_deltalist(const list_t *in, list_t *out)
{
    int i;
    if (in->len <= 1) { out->len = 0; return; }
    out->len = (uint8_t)(in->len - 1);
    for (i = 0; i < out->len; i++)
        out->data[i] = in->data[i+1] - in->data[i];
}

void list_sorta(const list_t *in, list_t *out)
{
    memcpy(out->data, in->data, in->len * sizeof(ce_float_t));
    out->len = in->len;
    isort(out->data, out->len);
}

void list_sortd(const list_t *in, list_t *out)
{
    int i;
    list_sorta(in, out);
    /* reverse */
    for (i = 0; i < out->len / 2; i++) {
        ce_float_t tmp = out->data[i];
        out->data[i] = out->data[out->len - 1 - i];
        out->data[out->len - 1 - i] = tmp;
    }
}

/* ── Element-wise arithmetic ─────────────────────────────── */

ce_error_t list_add(const list_t *a, const list_t *b, list_t *out)
{
    int i;
    if (a->len != b->len) return CE_ERR_DOMAIN;
    out->len = a->len;
    for (i = 0; i < a->len; i++) out->data[i] = a->data[i] + b->data[i];
    return CE_OK;
}

ce_error_t list_sub(const list_t *a, const list_t *b, list_t *out)
{
    int i;
    if (a->len != b->len) return CE_ERR_DOMAIN;
    out->len = a->len;
    for (i = 0; i < a->len; i++) out->data[i] = a->data[i] - b->data[i];
    return CE_OK;
}

ce_error_t list_mul(const list_t *a, const list_t *b, list_t *out)
{
    int i;
    if (a->len != b->len) return CE_ERR_DOMAIN;
    out->len = a->len;
    for (i = 0; i < a->len; i++) out->data[i] = a->data[i] * b->data[i];
    return CE_OK;
}

ce_error_t list_div(const list_t *a, const list_t *b, list_t *out)
{
    int i;
    if (a->len != b->len) return CE_ERR_DOMAIN;
    /* Pre-scan for zeros before writing any output */
    for (i = 0; i < a->len; i++) {
        if (b->data[i] == 0) return CE_ERR_DIV_ZERO;
    }
    out->len = a->len;
    for (i = 0; i < a->len; i++) {
        out->data[i] = a->data[i] / b->data[i];
    }
    return CE_OK;
}

ce_error_t list_pow(const list_t *a, const list_t *b, list_t *out)
{
    int i;
    if (a->len != b->len) return CE_ERR_DOMAIN;
    out->len = a->len;
    for (i = 0; i < a->len; i++) out->data[i] = powf(a->data[i], b->data[i]);
    return CE_OK;
}

void list_scalar_mul(const list_t *a, ce_float_t s, list_t *out)
{
    int i;
    out->len = a->len;
    for (i = 0; i < a->len; i++) out->data[i] = a->data[i] * s;
}

void list_scalar_add(const list_t *a, ce_float_t s, list_t *out)
{
    int i;
    out->len = a->len;
    for (i = 0; i < a->len; i++) out->data[i] = a->data[i] + s;
}

/* ── Sequence generation ─────────────────────────────────── */

void list_seq(ce_float_t start, ce_float_t end, ce_float_t step, list_t *out)
{
    int i = 0;
    ce_float_t val;

    out->len = 0;
    if (step == 0) return;
    if (step > 0 && start > end) return;
    if (step < 0 && start < end) return;

    for (val = start; i < MAX_LIST_SIZE; val += step, i++) {
        if (step > 0 && val > end + 1e-9f) break;
        if (step < 0 && val < end - 1e-9f) break;
        out->data[i] = val;
    }
    out->len = (uint8_t)i;
}

/* ── Display ─────────────────────────────────────────────── */

int list_to_str(const list_t *lst, char *buf, int size)
{
    int pos = 0, i;
    if (size <= 0) return 0;
    buf[0] = '\0';
    if (pos < size - 1) buf[pos++] = '{';
    for (i = 0; i < lst->len; i++) {
        char tmp[16];
        int tlen;
        if (i > 0 && pos < size - 1) buf[pos++] = ',';
        sprintf(tmp, "%.6g", (double)lst->data[i]);
        tlen = (int)strlen(tmp);
        if (pos + tlen < size) { memcpy(buf + pos, tmp, tlen); pos += tlen; }
    }
    if (pos < size - 1) buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}
