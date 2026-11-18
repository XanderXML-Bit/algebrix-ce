/*
 * steps.h — Step-by-step recording for differentiation display
 */
#ifndef STEPS_H
#define STEPS_H

#include "calcengine.h"

#define MAX_STEPS       32
#define MAX_STEP_LEN    120
#define MAX_CHECKPOINTS 16
#define MAX_CKPT_LEN    60

#if defined(__GNUC__) || defined(__clang__)
#define STEPS_PRINTF(f, a) __attribute__((format(__printf__, f, a)))
#else
#define STEPS_PRINTF(f, a)
#endif

#ifdef CE_DISABLE_STEP_RECORDING

static inline void steps_clear(void) { }
static inline bool steps_add(const char *line) { (void)line; return false; }
static inline bool steps_addf(const char *fmt, ...) STEPS_PRINTF(1, 2);
static inline bool steps_addf(const char *fmt, ...) { (void)fmt; return false; }
static inline int steps_count(void) { return 0; }
static inline const char *steps_get(int index) { (void)index; return ""; }
static inline int steps_save(void) { return 0; }
static inline void steps_restore(int saved_count) { (void)saved_count; }
static inline bool steps_full(void) { return false; }
static inline bool steps_add_checkpoint(const char *desc) { (void)desc; return false; }
static inline bool steps_set_checkpoint_result(const char *result) { (void)result; return false; }
static inline bool steps_set_checkpoint_resultf(const char *fmt, ...) STEPS_PRINTF(1, 2);
static inline bool steps_set_checkpoint_resultf(const char *fmt, ...) { (void)fmt; return false; }
static inline int steps_checkpoint_count(void) { return 0; }
static inline const char *steps_get_checkpoint(int i) { (void)i; return ""; }
static inline int steps_get_checkpoint_step(int i) { (void)i; return 0; }

#else

void        steps_clear(void);
bool        steps_add(const char *line);
bool        steps_addf(const char *fmt, ...) STEPS_PRINTF(1, 2);
int         steps_count(void);
const char *steps_get(int index);
int         steps_save(void);
void        steps_restore(int saved_count);
bool        steps_full(void);

/* ── Operation-level checkpoints ─────────────────────────── */
/* Checkpoints mark meaningful mathematical operations like
 * "Rewrite as partial fractions" or "Apply chain rule".
 * Each checkpoint records its description and the step index
 * at which it was inserted, enabling step-by-step navigation
 * to pause at operation boundaries rather than every line.   */
bool        steps_add_checkpoint(const char *desc);
bool        steps_set_checkpoint_result(const char *result);
bool        steps_set_checkpoint_resultf(const char *fmt, ...) STEPS_PRINTF(1, 2);
int         steps_checkpoint_count(void);
const char *steps_get_checkpoint(int index);
int         steps_get_checkpoint_step(int index);
int         steps_get_checkpoint_end_step(int index);
const char *steps_get_checkpoint_result(int index);

#endif

#endif /* STEPS_H */
