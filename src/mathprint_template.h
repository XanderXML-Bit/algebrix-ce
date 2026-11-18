/*
 * mathprint_template.h — Visual MathPrint input template system
 *
 * Fill-in-the-blank templates for mathematical operations.  When the user
 * selects a function from the menu, or types a recognised prefix while
 * editing (e.g. "int("), an interactive template activates that renders the
 * proper notation (∫, d/dx, lim, Σ, …) with empty placeholder boxes.
 *
 * Navigation:
 *   LEFT / RIGHT   — move cursor within the active slot; at a slot boundary,
 *                    wrap to the previous / next slot.
 *   UP / DOWN      — jump between visually stacked slots (e.g. upper/lower
 *                    bounds for ∫ and Σ, numerator/denominator for fractions).
 *   TAB_LEFT/RIGHT — unconditional prev/next slot.
 *   ENTER          — when all mandatory slots are filled, assemble and submit.
 *   CLEAR          — cancel the template.
 */
#ifndef MATHPRINT_TEMPLATE_H
#define MATHPRINT_TEMPLATE_H

#include "calcengine.h"
#include "ui.h"

/* Maximum number of fillable slots in any template */
#define MP_TMPL_MAX_SLOTS 5

/* Template type identifiers (stored as uint8_t in menu_item_t) */
typedef enum {
    MP_TMPL_NONE           = 0,   /* not a template                       */
    MP_TMPL_INTEGRAL_INDEF = 1,   /* ∫ f d[x]  (2 slots: f, x)           */
    MP_TMPL_INTEGRAL_DEF   = 2,   /* ∫_a^b f d[x] (4 slots: f,x,a,b)    */
    MP_TMPL_DERIV          = 3,   /* d/d[x](f) (2 slots: f, x)           */
    MP_TMPL_LIMIT          = 4,   /* lim_{[x]→[a]} f (3 slots: f,x,a)   */
    MP_TMPL_SUM            = 5,   /* Σ_{[n]=[lo]}^[hi] f (4 slots)       */
    MP_TMPL_PRODUCT        = 6,   /* Π_{[n]=[lo]}^[hi] f (4 slots)       */
    MP_TMPL_FRACTION       = 7,   /* [num]/[den] (2 slots)                */
    MP_TMPL_TAYLOR         = 8,   /* taylor(f,[x],[ctr],[deg]) (4 slots)  */
    MP_TMPL_TANGENT        = 9,   /* tangent(f,[x],[pt]) (3 slots)        */
    MP_TMPL_NDERIV         = 10,  /* nderiv(f,[x],[n]) (3 slots)          */
    MP_TMPL_COUNT
} mp_tmpl_type_t;

/* A single fillable slot */
typedef struct {
    char        buf[MAX_INPUT_LEN]; /* content typed by the user          */
    uint8_t     len;                /* chars in buf                       */
    uint8_t     cursor;             /* cursor pos within buf              */
    const char *placeholder;        /* hint shown when empty ("expr" etc) */
    bool        mandatory;          /* must be non-empty to assemble      */
} mp_slot_t;

/* Full template state — zero-init equals "no template active" */
typedef struct {
    mp_tmpl_type_t  type;
    mp_slot_t       slots[MP_TMPL_MAX_SLOTS];
    uint8_t         num_slots;
    int8_t          active_slot;     /* 0-based; -1 = not active          */
    /* prefix: text already in input_buf BEFORE the template trigger      */
    char            prefix[MAX_INPUT_LEN];
    uint8_t         prefix_len;
} mp_template_state_t;

/* ── Life-cycle ──────────────────────────────────────────────── */

/* Initialise template state for the given type (clears all slots). */
void mp_tmpl_init(mp_template_state_t *tmpl, mp_tmpl_type_t type);

/* Cancel and clear. */
void mp_tmpl_cancel(mp_template_state_t *tmpl);

/* True if a template is currently active. */
bool mp_tmpl_active(const mp_template_state_t *tmpl);

/* ── Input handling ─────────────────────────────────────────── */

/*
 * Feed a key event to the template.
 * Returns true when the event was consumed.
 * Sets *done = true  when ENTER was pressed and all mandatory slots are full.
 * Sets *cancelled = true when CLEAR/QUIT was pressed.
 */
bool mp_tmpl_key(mp_template_state_t *tmpl,
                 const key_event_t   *evt,
                 bool                *done,
                 bool                *cancelled);

/* ── Assembly ────────────────────────────────────────────────── */

/* True when every mandatory slot has at least one character. */
bool mp_tmpl_ready(const mp_template_state_t *tmpl);

/*
 * Build the final expression string (prefix + function call).
 * Writes into out_buf (at most out_size bytes including '\0').
 * Returns true on success.
 */
bool mp_tmpl_assemble(const mp_template_state_t *tmpl,
                      char *out_buf, int out_size);

/* ── Drawing ─────────────────────────────────────────────────── */

/*
 * Render the template in the input strip area (expands upward if needed).
 * Clears its own background; does NOT call ui_flip().
 */
void mp_tmpl_draw(const mp_template_state_t *tmpl);

/* ── Live-typing trigger detection ──────────────────────────── */

/*
 * Inspect the current text input buffer (before the user pressed any key)
 * and the character/string just pressed.  If the combined buffer ends with a
 * recognised template trigger (e.g. "int("), fills *type with the template
 * type, *prefix_end with the byte offset in buf at which the trigger begins,
 * and returns true.
 *
 * The caller should:
 *   1. Accept the keypress into the buffer as normal.
 *   2. Then call this function.
 *   3. If true: extract buf[0..prefix_end) as the prefix, then call
 *      mp_tmpl_init() and possibly import any partial text after the trigger
 *      into slot 0.
 */
bool mp_tmpl_detect_trigger(const char     *buf,
                             uint8_t         len,
                             mp_tmpl_type_t *type_out,
                             uint8_t        *prefix_end_out);

#endif /* MATHPRINT_TEMPLATE_H */
