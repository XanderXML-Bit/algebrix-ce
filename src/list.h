/*
 * list.h — List operations for CalcEngine
 * TI-84 style L1-L6 list storage with element-wise arithmetic.
 */
#ifndef LIST_H
#define LIST_H

#include "calcengine.h"

#define MAX_LIST_SIZE  50
#define MAX_LIST_COUNT 6

/* ── List type ───────────────────────────────────────────── */
typedef struct {
    ce_float_t data[MAX_LIST_SIZE];
    uint8_t    len;
} list_t;

/* ── List function identifiers ───────────────────────────── */
typedef enum {
    LIST_SUM, LIST_PROD, LIST_MIN, LIST_MAX,
    LIST_MEAN, LIST_MEDIAN, LIST_STDEV,
    LIST_DIM,
    LIST_CUMSUM, LIST_DELTALIST,
    LIST_SORTA, LIST_SORTD,
    LIST_SEQ
} list_fn_t;

/* ── Global list storage (L1-L6) ─────────────────────────── */
extern list_t g_lists[MAX_LIST_COUNT];

/* ── List storage API ────────────────────────────────────── */
void list_init_all(void);
void list_set(int idx, const ce_float_t *data, int len);
const list_t *list_get(int idx);

/* ── Scalar-returning functions ──────────────────────────── */
ce_float_t list_sum(const list_t *lst);
ce_float_t list_prod(const list_t *lst);
ce_float_t list_min(const list_t *lst);
ce_float_t list_max(const list_t *lst);
ce_float_t list_mean(const list_t *lst);
ce_float_t list_median(const list_t *lst);
ce_float_t list_stdev(const list_t *lst);
int        list_dim(const list_t *lst);

/* ── List-returning functions ────────────────────────────── */
void list_cumsum(const list_t *in, list_t *out);
void list_deltalist(const list_t *in, list_t *out);
void list_sorta(const list_t *in, list_t *out);
void list_sortd(const list_t *in, list_t *out);

/* ── Sequence generation ─────────────────────────────────── */
void list_seq(ce_float_t start, ce_float_t end, ce_float_t step, list_t *out);

/* ── Element-wise arithmetic ─────────────────────────────── */
ce_error_t list_add(const list_t *a, const list_t *b, list_t *out);
ce_error_t list_sub(const list_t *a, const list_t *b, list_t *out);
ce_error_t list_mul(const list_t *a, const list_t *b, list_t *out);
ce_error_t list_div(const list_t *a, const list_t *b, list_t *out);
ce_error_t list_pow(const list_t *a, const list_t *b, list_t *out);
void list_scalar_mul(const list_t *a, ce_float_t s, list_t *out);
void list_scalar_add(const list_t *a, ce_float_t s, list_t *out);

/* ── Display ─────────────────────────────────────────────── */
int list_to_str(const list_t *lst, char *buf, int size);

#endif /* LIST_H */
