/*
 * complex.h — Complex number operations for CalcEngine
 * Parallel evaluator for complex mode (RECT/POLAR).
 */
#ifndef COMPLEX_H
#define COMPLEX_H

#include "calcengine.h"
#include "ast.h"
#include "eval.h"

/* ── Complex arithmetic ──────────────────────────────────── */
ce_complex_t cadd(ce_complex_t a, ce_complex_t b);
ce_complex_t csub(ce_complex_t a, ce_complex_t b);
ce_complex_t cmul(ce_complex_t a, ce_complex_t b);
ce_complex_t cdiv(ce_complex_t a, ce_complex_t b);
ce_complex_t ce_cpow(ce_complex_t base, ce_complex_t exp);

/* ── Complex functions ───────────────────────────────────── */
ce_float_t   cabs_val(ce_complex_t z);
ce_float_t   ce_carg(ce_complex_t z);
ce_complex_t cconj(ce_complex_t z);
ce_complex_t cexp_z(ce_complex_t z);
ce_complex_t cln_z(ce_complex_t z);
ce_complex_t csqrt_z(ce_complex_t z);
ce_complex_t csin_z(ce_complex_t z);
ce_complex_t ccos_z(ce_complex_t z);
ce_complex_t ctan_z(ce_complex_t z);
ce_complex_t csec_z(ce_complex_t z);
ce_complex_t ccsc_z(ce_complex_t z);
ce_complex_t ccot_z(ce_complex_t z);
ce_complex_t casin_z(ce_complex_t z);
ce_complex_t cacos_z(ce_complex_t z);
ce_complex_t catan_z(ce_complex_t z);
ce_complex_t csinh_z(ce_complex_t z);
ce_complex_t ccosh_z(ce_complex_t z);
ce_complex_t ctanh_z(ce_complex_t z);
ce_complex_t casec_z(ce_complex_t z);
ce_complex_t cacsc_z(ce_complex_t z);
ce_complex_t cacot_z(ce_complex_t z);
ce_complex_t casinh_z(ce_complex_t z);
ce_complex_t cacosh_z(ce_complex_t z);
ce_complex_t catanh_z(ce_complex_t z);
ce_complex_t csech_z(ce_complex_t z);
ce_complex_t ccsch_z(ce_complex_t z);
ce_complex_t ccoth_z(ce_complex_t z);

/* ── Complex evaluator ───────────────────────────────────── */
ce_complex_t ceval_expr(eval_ctx_t *ctx, const ast_node_t *node);

/* ── Display ─────────────────────────────────────────────── */
int complex_to_str(ce_complex_t z, char *buf, int size);

#endif /* COMPLEX_H */
