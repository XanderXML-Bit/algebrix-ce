/*
 * strategy_rank.h — CEISTR (intstrategy) ranking state that survives
 * os_RunPrgm re-entry.
 *
 * On the TI-84 Plus CE, os_RunPrgm never returns to its caller: when a child
 * program returns, the CEdev runprgm stub reloads CEISTR from its variable
 * (all statics zeroed) and re-enters main() through the forward callback.
 * An in-process loop over the four strategy children therefore reached only
 * the first child on hardware and never built a ranking.
 * CEISTR now keeps its progress in the scratch
 * AppVar CEISTRS: which strategy is pending, plus each finished strategy's
 * matched flag and reason. The IPC schema (CE_REQ/CE_RESP/CE_STATE) is
 * unchanged, so CE_IPC_VERSION stays as it is.
 *
 * The strategy children accept a plain indefinite int(expr,var) (or their
 * own trigsub/usub/intparts op) and answer "Unsupported advanced integral
 * request" to an intstrategy node, so CEISTR forwards the request rewritten
 * as int(expr,var); see ce_istr_integral_request().
 *
 * Header-only on purpose: report_main.c (the CEISTR module) and the desktop
 * test runner both include it, so no build script needs a new source file.
 */
#ifndef CALC_INT_STRATEGY_RANK_H
#define CALC_INT_STRATEGY_RANK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <fileioc.h>

#include "memory.h"
#include "split/astwire.h"
#include "split/ipc.h"

#define CE_ISTR_SCRATCH_APPVAR  "CEISTRS"
#define CE_ISTR_SCRATCH_MAGIC   0x5253u /* "SR" */
/* Version 2 expands reason[] from 96 to 128 bytes. A v1
 * CEISTRS left by a run interrupted across the upgrade is smaller than the
 * new struct, so ce_istr_scratch_load() rejects it: a re-entry then answers
 * "Strategy report lost its progress" once, and a fresh dispatch overwrites
 * it. It is not migrated. */
#define CE_ISTR_SCRATCH_VERSION 2u
#define CE_ISTR_COUNT           4u
/* A child's reason comes from ce_response_t.error (128 bytes) or from the
 * forward error buffer (128 bytes), so it is at most 127 characters and is
 * stored whole. The ranking text is what has to fit a budget: all of it goes
 * into ce_response_t.result (MAX_RESULT_LEN, 512 bytes), and four 127-char
 * reasons plus their line prefixes would not. ce_istr_format() therefore
 * shares the room left after the fixed text between the reasons and clips
 * only the ones that do not fit, at a word boundary, with "...". The old
 * 96-byte cut could split a word, as in "or recipro" on the CE. */
#define CE_ISTR_REASON_LEN      128u
#define CE_ISTR_ELLIPSIS        "..."
#define CE_ISTR_ELLIPSIS_LEN    3u

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
    uint8_t version;
    /* Strategies finished so far. While a child runs, done is the index of
     * that child in ce_istr_programs[]. */
    uint8_t done;
    uint8_t matched[CE_ISTR_COUNT];
    char reason[CE_ISTR_COUNT][CE_ISTR_REASON_LEN];
} ce_istr_scratch_t;
#pragma pack(pop)

/* Try order. It is also the order of the ranking table below. */
static const char *const ce_istr_programs[CE_ISTR_COUNT] = {
    "CEIPFR", "CEITRG", "CEIUSB", "CEIPRT"
};
static const char *const ce_istr_labels[CE_ISTR_COUNT] = {
    "partial fractions", "trig substitution", "u-substitution",
    "integration by parts"
};
static const int ce_istr_base_scores[CE_ISTR_COUNT] = { 48, 44, 42, 40 };
#define CE_ISTR_MATCH_BONUS 40

static void ce_istr_scratch_reset(ce_istr_scratch_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->magic = CE_ISTR_SCRATCH_MAGIC;
    s->version = CE_ISTR_SCRATCH_VERSION;
}

static bool ce_istr_scratch_valid(const ce_istr_scratch_t *s)
{
    return s && s->magic == CE_ISTR_SCRATCH_MAGIC &&
           s->version == CE_ISTR_SCRATCH_VERSION && s->done <= CE_ISTR_COUNT;
}

/* Index of the strategy that should run next, CE_ISTR_COUNT when all ran. */
static size_t ce_istr_next(const ce_istr_scratch_t *s)
{
    if (!ce_istr_scratch_valid(s)) return CE_ISTR_COUNT;
    return s->done;
}

/* Copies src into dst (which holds at least max + 1 bytes) as at most max
 * characters and returns the length written. A src longer than max is clipped
 * at a word and ends in "...": the kept text ends at the last space that
 * leaves room for the ellipsis, without trailing ' ' , ; : . ( or -. When that
 * would keep less than half of max (one long token such as an expression),
 * the text is cut at max - 3 characters instead (same trailing cleanup), so a
 * clip never throws most of the room away. Only ASCII is written.
 *
 * Callers: ce_istr_format() with its shared budget cap, which is where
 * clipping happens in practice, and ce_istr_copy_reason() with max 127, which
 * clips only a source longer than any CE_RESP error (a desktop caller, or an
 * unterminated error field). */
/* Length of src[0..keep) without trailing separators. */
static size_t ce_istr_trim_tail(const char *src, size_t keep)
{
    while (keep > 0) {
        char c = src[keep - 1];
        if (c != ' ' && c != ',' && c != ';' && c != ':' && c != '.' &&
            c != '(' && c != '-') {
            break;
        }
        keep--;
    }
    return keep;
}

static size_t ce_istr_clip_words(char *dst, const char *src, size_t max)
{
    size_t len = 0;
    size_t keep;
    size_t i;

    if (!dst) return 0;
    if (!src) src = "";
    /* Bound first: src may be an unterminated max + 1 byte field, so
     * src[max + 1] must never be read. */
    while (len <= max && src[len]) len++;
    if (len <= max) {
        memcpy(dst, src, len);
        dst[len] = '\0';
        return len;
    }
    if (max <= CE_ISTR_ELLIPSIS_LEN) {
        for (i = 0; i < max; i++) dst[i] = '.';
        dst[max] = '\0';
        return max;
    }

    /* src[0..max] all exist here, because src is longer than max. */
    keep = 0;
    for (i = max - CE_ISTR_ELLIPSIS_LEN; i > 0; i--) {
        if (src[i] == ' ') {
            keep = i;
            break;
        }
    }
    keep = ce_istr_trim_tail(src, keep);
    if (keep * 2u < max) keep = ce_istr_trim_tail(src, max - CE_ISTR_ELLIPSIS_LEN);
    /* The trim may keep less than half (a run of separators); that is
     * fine. Only when it leaves nothing is the plain max - 3 cut used. */
    if (keep == 0) keep = max - CE_ISTR_ELLIPSIS_LEN;

    memcpy(dst, src, keep);
    memcpy(dst + keep, CE_ISTR_ELLIPSIS, CE_ISTR_ELLIPSIS_LEN);
    dst[keep + CE_ISTR_ELLIPSIS_LEN] = '\0';
    return keep + CE_ISTR_ELLIPSIS_LEN;
}

static void ce_istr_copy_reason(char *dst, const char *src)
{
    (void)ce_istr_clip_words(dst, src, CE_ISTR_REASON_LEN - 1u);
}

/* Records the result of the strategy at index done and advances. err may be
 * NULL or empty. Returns false (and changes nothing) when the scratch is
 * invalid or every strategy already ran. */
static bool ce_istr_record(ce_istr_scratch_t *s, bool ok, const char *err)
{
    size_t i;
    if (!ce_istr_scratch_valid(s) || s->done >= CE_ISTR_COUNT) return false;
    i = s->done;
    s->matched[i] = ok ? 1u : 0u;
    if (ok) {
        char msg[24];
        snprintf(msg, sizeof(msg), "matched by %s", ce_istr_programs[i]);
        ce_istr_copy_reason(s->reason[i], msg);
    } else if (err && err[0]) {
        ce_istr_copy_reason(s->reason[i], err);
    } else {
        ce_istr_copy_reason(s->reason[i], "no match");
    }
    s->done++;
    return true;
}

/* Like ce_istr_record(), but only when child names the strategy that was
 * pending. A different name means the scratch does not belong to the chain
 * that just returned, so nothing is recorded. */
static bool ce_istr_record_child(ce_istr_scratch_t *s, const char *child,
                                 bool ok, const char *err)
{
    if (!child || ce_istr_next(s) >= CE_ISTR_COUNT) return false;
    if (strcmp(child, ce_istr_programs[s->done]) != 0) return false;
    return ce_istr_record(s, ok, err);
}

static int ce_istr_score(const ce_istr_scratch_t *s, size_t i)
{
    return ce_istr_base_scores[i] + (s->matched[i] ? CE_ISTR_MATCH_BONUS : 0);
}

static const char *ce_istr_reason(const ce_istr_scratch_t *s, size_t k)
{
    return s->reason[k][0] ? s->reason[k] : "not run";
}

/* The largest per-reason length cap such that the reasons, each cut to the
 * cap, add up to at most avail characters. len[] holds their full lengths
 * (each at most CE_ISTR_REASON_LEN - 1), so the starting cap clips nothing
 * when they already fit. The loop always ends: at cap 0 the total is 0. */
static size_t ce_istr_reason_cap(const size_t *len, size_t avail)
{
    size_t cap = CE_ISTR_REASON_LEN - 1u;
    for (;;) {
        size_t total = 0;
        size_t i;
        for (i = 0; i < CE_ISTR_COUNT; i++) {
            total += len[i] < cap ? len[i] : cap;
        }
        if (total <= avail) return cap;
        cap--;
    }
}

/* A summary line, then one line per strategy, highest score first.
 *
 * The CE launcher's home row shows only the first line, clipped to its 26
 * columns, so the summary names the winner within 26 columns ("Best:
 * integration by parts" is exactly 26). UP opens the full text in the
 * launcher's result viewer (PR #103).
 *
 * The whole text must fit out (MAX_RESULT_LEN in CEISTR). The summary and the
 * four "\nN) label (score S): " prefixes are kept whole; the reasons share
 * what is left. When they do not all fit, every reason longer than a common
 * cap is clipped at a word with "..." (ce_istr_clip_words), so the last line
 * is never cut off mid-word by the end of the buffer. A buffer too small for
 * the fixed text is filled and terminated, and the text stops there. */
static void ce_istr_format(const ce_istr_scratch_t *s, char *out, size_t out_size)
{
    size_t order[CE_ISTR_COUNT];
    size_t len[CE_ISTR_COUNT];
    size_t i, j, pos, fixed, cap;
    char prefix[48];
    int n;

    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!ce_istr_scratch_valid(s)) return;

    for (i = 0; i < CE_ISTR_COUNT; i++) order[i] = i;
    for (i = 0; i < CE_ISTR_COUNT; i++) {
        for (j = i + 1; j < CE_ISTR_COUNT; j++) {
            if (ce_istr_score(s, order[j]) > ce_istr_score(s, order[i])) {
                size_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    if (s->matched[order[0]]) {
        n = snprintf(out, out_size, "Best: %s", ce_istr_labels[order[0]]);
    } else {
        n = snprintf(out, out_size, "No strategy matched");
    }
    if (n < 0) return;
    pos = (size_t)n;
    if (pos >= out_size) return;

    /* Budget: what the prefixes leave of out_size - 1 characters. */
    fixed = pos;
    for (i = 0; i < CE_ISTR_COUNT; i++) {
        size_t k = order[i];
        n = snprintf(prefix, sizeof(prefix), "\n%u) %s (score %d): ",
                     (unsigned)(i + 1), ce_istr_labels[k], ce_istr_score(s, k));
        if (n < 0) return;
        fixed += (size_t)n;
        len[i] = strlen(ce_istr_reason(s, k));
    }
    cap = ce_istr_reason_cap(len, fixed < out_size - 1u ? out_size - 1u - fixed : 0u);

    for (i = 0; i < CE_ISTR_COUNT; i++) {
        size_t k = order[i];
        size_t room;
        n = snprintf(out + pos, out_size - pos, "\n%u) %s (score %d): ",
                     (unsigned)(i + 1), ce_istr_labels[k], ce_istr_score(s, k));
        if (n < 0) return;
        if ((size_t)n >= out_size - pos) return;  /* truncated, terminated */
        pos += (size_t)n;
        room = out_size - 1u - pos;
        pos += ce_istr_clip_words(out + pos, ce_istr_reason(s, k),
                                  cap < room ? cap : room);
    }
}

/* Rewrites req's AST as the indefinite integral the strategy children
 * accept: intstrategy(expr,var) -> int(expr,var). An indefinite NODE_INTEGRAL
 * root is forwarded as it is. Returns false (req unchanged) for anything
 * else, including a definite integral (every child rejects bounds), when the
 * node pool is exhausted or when the integral does not fit the payload. */
static bool ce_istr_integral_request(const ast_node_t *root, ce_request_t *req)
{
    ast_node_t *integral;
    uint16_t size = 0;
    bool ok;

    if (!root || !req) return false;
    if (root->type == NODE_INTEGRAL) {
        return !root->d.integral.lo && !root->d.integral.hi;
    }
    if (root->type != NODE_POLAR || root->d.polar.op != POLOP_INTSTRATEGY ||
        !root->d.polar.expr1) {
        return false;
    }

    integral = mem_alloc_node();
    if (!integral) return false;
    integral->type = NODE_INTEGRAL;
    integral->d.integral.var = root->d.polar.var ? root->d.polar.var : 'x';
    /* Borrowed, not cloned: detached again before the node is freed. */
    integral->d.integral.expr = root->d.polar.expr1;
    integral->d.integral.lo = NULL;
    integral->d.integral.hi = NULL;
    ok = ce_ast_serialize(integral, req->ast_payload,
                          (uint16_t)sizeof(req->ast_payload), &size);
    integral->d.integral.expr = NULL;
    ast_free(integral);
    if (!ok || size == 0) return false;
    req->ast_size = size;
    req->flags = (uint8_t)(req->flags | CE_REQ_FLAG_HAS_AST);
    return true;
}

static bool ce_istr_scratch_save(const ce_istr_scratch_t *s)
{
    uint8_t slot;
    if (!ce_istr_scratch_valid(s)) return false;
    slot = ti_Open(CE_ISTR_SCRATCH_APPVAR, "w");
    if (!slot) return false;
    if (ti_Write(s, sizeof(*s), 1, slot) != 1) {
        ti_Close(slot);
        return false;
    }
    ti_Close(slot);
    return true;
}

static bool ce_istr_scratch_load(ce_istr_scratch_t *s)
{
    uint8_t slot;
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    slot = ti_Open(CE_ISTR_SCRATCH_APPVAR, "r");
    if (!slot) return false;
    if (ti_GetSize(slot) < sizeof(*s) || ti_Read(s, sizeof(*s), 1, slot) != 1) {
        ti_Close(slot);
        memset(s, 0, sizeof(*s));
        return false;
    }
    ti_Close(slot);
    return ce_istr_scratch_valid(s);
}

static void ce_istr_scratch_delete(void)
{
    ti_Delete(CE_ISTR_SCRATCH_APPVAR);
}

#endif /* CALC_INT_STRATEGY_RANK_H */
