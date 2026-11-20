/*
 * calcengine.h — Common types and constants
 * CalcEngine — TI-84 Plus CE Math Suite
 * Zendren LLC
 */
#ifndef CALCENGINE_H
#define CALCENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "ce_float_class.h"   /* ce_isnan/ce_isinf/ce_isfinite: CEdev ce_isinf() is wrong */

/* ── MSVC compatibility ──────────────────────────────────── */
#ifdef _MSC_VER
#  define strncasecmp _strnicmp
#  define strcasecmp  _stricmp
#endif

/* POSIX strcasecmp/strncasecmp. The CE libc declares them in string.h. */
#if !defined(_MSC_VER) && defined(__has_include)
#  if __has_include(<strings.h>)
#    include <strings.h>
#  endif
#endif

/* ── Version ─────────────────────────────────────────────── */
#include "version.h"

/* ── Screen ──────────────────────────────────────────────── */
#define SCREEN_W   320
#define SCREEN_H   240

/* Layout regions (Y coordinates) */
#define HEADER_H   14
#define STATUS_H   12
#define INPUT_Y    (SCREEN_H - STATUS_H - 14)
#define WORK_Y     (HEADER_H + 2)
#define WORK_H     (INPUT_Y - WORK_Y - 2)

/* ── Palette color indices (CE default 8-bit palette) ───── */
/* Confirmed from graphx.h: gfx_black=0, gfx_white=0xFF,
   gfx_red=0xE0, gfx_green=0x03, gfx_blue=0x10            */
#define COL_WHITE    0xFF
#define COL_BLACK    0x00
#define COL_DKBLUE   0x10   /* gfx_blue   */
#define COL_BLUE     0x15
#define COL_RED      0xE0   /* gfx_red    */
#define COL_GREEN    0x03   /* gfx_green  */
#define COL_DKGRAY   0x49
#define COL_LTGRAY   0xB5
#define COL_YELLOW   0xE7   /* gfx_yellow */
#define COL_ORANGE   0xE3   /* gfx_orange */
#define COL_HIGHLIGHT  COL_YELLOW  /* selection highlight color */

/* ── Limits ──────────────────────────────────────────────── */
#define MAX_INPUT_LEN    128
#define MAX_TOKEN_LEN    16
#define MAX_TOKENS       128
#define MAX_RESULT_LEN   512

/* ── Error codes ─────────────────────────────────────────── */
typedef enum {
    CE_OK = 0,
    CE_ERR_SYNTAX,
    CE_ERR_MEMORY,
    CE_ERR_OVERFLOW,
    CE_ERR_DIV_ZERO,
    CE_ERR_DOMAIN,
    CE_ERR_UNDEFINED,
    CE_ERR_TOO_COMPLEX,
    CE_ERR_INTERNAL,
    CE_ERR_CANCELLED,      /* user pressed CLEAR during computation */
    CE_ERR_DEPTH,          /* recursion refusal, not arithmetic overflow */
    CE_ERR_PRECISION       /* float32 rounding swamps the result      */
} ce_error_t;

/* ── Math value type ─────────────────────────────────────── */
typedef float ce_float_t;

#define CE_PI  3.14159265358979f
#define CE_E   2.71828182845905f
#define CE_INF INFINITY

/* ── Complex number type ─────────────────────────────────── */
typedef struct {
    ce_float_t re;
    ce_float_t im;
} ce_complex_t;

/* ── Settings ────────────────────────────────────────────── */
typedef enum {
    ANGLE_RADIANS = 0,
    ANGLE_DEGREES
} angle_mode_t;

typedef enum {
    COMPLEX_OFF = 0,
    COMPLEX_RECT,
    COMPLEX_POLAR
} complex_mode_t;

typedef enum {
    STEP_ALL = 0,     /* show all steps at once (default) */
    STEP_COMPACT      /* show abbreviated/key steps only   */
} step_detail_t;

typedef enum {
    SOLVE_FULL_AUTO = 0,  /* solve completely, show all steps at once */
    SOLVE_STEP_BY_STEP    /* pause at operation-level checkpoints     */
} solve_mode_t;

typedef enum {
    METHOD_AUTO = 0,      /* engine picks optimal method              */
    METHOD_USUB,          /* force u-substitution                     */
    METHOD_PARTS,         /* force integration by parts               */
    METHOD_TRIGSUB,       /* force trig substitution                  */
    METHOD_PARTIAL_FRAC   /* force partial fractions                  */
} integ_method_t;

typedef struct {
    angle_mode_t   angle_mode;          /* radians (default) or degrees      */
    bool           show_steps;          /* step-by-step display on/off       */
    uint8_t        precision;           /* decimal digits (1-9, default 6)   */
    complex_mode_t complex_mode;        /* OFF (default) / RECT / POLAR      */
    bool           pmt_at_begin;        /* false=END (default), true=BEGIN   */
    step_detail_t  step_detail;         /* ALL (default) / COMPACT           */
    solve_mode_t   solve_mode;          /* FULL_AUTO (default) / STEP_BY_STEP */
    integ_method_t integ_method;        /* AUTO (default) / forced method    */
    bool           show_method_choices; /* OFF (default): show disambig menu
                                         * only for different-goal ops.
                                         * ON: also show same-result methods. */
    bool           segment_pause;       /* OFF (default): compute all stages
                                         * at once and show accumulated result.
                                         * ON: pause between major computation
                                         * stages (checkpoints), showing only
                                         * the current stage result at a time.
                                         * Each ENTER advances to next stage.  */
    bool           exact_mode;          /* Phase 3: OFF (default) = decimal output.
                                         * ON = prefer exact symbolic output
                                         * (fractions, sqrt, pi) over decimal
                                         * approximations when possible. Workers
                                         * check this to select their output format. */
} settings_t;

/* Global settings instance (defined in main.c) */
extern settings_t g_settings;

/* Previous answer (defined in main.c / test_main.c) */
extern ce_float_t g_last_answer;
/* Set by eval_expr() when it refuses to recurse: its CE_EVAL_MAX_DEPTH cap
 * on every build, or its eZ80 stack backstop (src/eval.c). Defined in
 * src/split/ipc.c; every CE program run starts with it clear, and the
 * desktop processor clears it at request entry (one request per run on the
 * CE, many per desktop test process). ce_response_from_runtime() never
 * reports success while it is set, because some callers map eval errors to
 * values that could otherwise be reported as successful results.
 * Only code that never turns an eval error into a value may restore the
 * value it saved on entry. Those sites:
 *   - limits.c compute_limit() and limits_continuity(). Each of the file's
 *     evaluations treats an error as "this method does not apply" (direct
 *     substitution, the 0/0 test, L'Hopital's numeric fallback...), so the
 *     result is a value from successful evaluations, "could not evaluate"
 *     or "could not determine".
 * Any other refused evaluation fails the request, including probes that
 * would have recovered (the 12-level algsolve workers' Newton starts, for
 * one); a desktop corpus of every worker config found no correct answer
 * lost that way, but those probes were checked by corpus, not audited. */
extern bool g_eval_refused;
extern ce_float_t g_eval_refused_ans;   /* Ans when it was first set */

/* ── Stored scalar variables ──────────────────────────────────── */
/*
 * Values kept by `expr->X` for the letters A..Z, persisted in CE_STATE so
 * they survive the worker hand-off and a relaunch of CALCENG.  Letter
 * 'A'+i holds a value when bit (i & 7) of mask[i >> 3] is set.  Lowercase
 * letters share the uppercase slot (the CE launcher types uppercase only).
 * Only modules built with CE_EVAL_USER_VARS=1 read or write the table
 * (eval.h); every other module carries CE_STATE through unchanged.
 */
#define CE_USER_VAR_COUNT 26
/* Packed (alignment 1): it sits inside the packed ce_shared_state_t, so a
 * pointer to it there must not require float alignment on desktop. */
#pragma pack(push, 1)
typedef struct {
    uint8_t    mask[4];
    ce_float_t value[CE_USER_VAR_COUNT];
} ce_user_vars_t;
#pragma pack(pop)

/* ── Computation cancellation ─────────────────────────────── */
/*
 * check_cancel() — scan keyboard for CLEAR press during long computation.
 * Returns true if user pressed CLEAR, meaning caller should abort.
 * On real hardware uses kb_Scan(); on desktop always returns false.
 */
bool check_cancel(void);

/* ── Progress indicator ──────────────────────────────────── */
/*
 * progress_start() — record computation start time.
 * progress_check() — if elapsed > ~2 seconds, draw "Computing..." indicator.
 * progress_clear() — clear the indicator (happens naturally on redraw).
 * On desktop builds these are no-ops.
 */
void progress_start(void);
void progress_check(void);

/* ── Forward declarations ────────────────────────────────── */
typedef struct ast_node ast_node_t;

#endif /* CALCENGINE_H */
