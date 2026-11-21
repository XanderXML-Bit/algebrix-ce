/*
 * ce_float_ops.h - Range-checked float arithmetic that does not trust the
 * TI-84 Plus CE's float routines at the edges of the float range.
 *
 * The following behavior was measured in CEmu running the OS 5.8 ROM.
 * These are the cases the helpers below check:
 *
 *  - The boot-code __fmul/__fdiv (CEdev lib/crt/os.src -> 0x288/0x278) round
 *    correctly in range. On overflow they return a NaN pattern (7FFFFFFF,
 *    7F96769B) or a wrapped *finite* value (1E38*10 -> 00FFFFFF, 2.35e-38).
 *    Underflow gives 0.
 *  - __fadd/__fsub (0x270/0x290) truncate: the shifted-out bits of the
 *    smaller operand are dropped, so about 45% of random sums are 1 ulp off
 *    and 16777216-1 gives 16777216. A same-sign overflow gives a NaN pattern
 *    (3E38+3E38 -> 7FE1B1ED).
 *  - None of them handles inf: inf*2 -> 2.35e-38, inf+inf -> 2.35e-38,
 *    inf-inf -> 0, 0.5*inf -> 1.7e38.
 *  - TI-OS expf/powf/sinhf/coshf (PREFER_OS_LIBC) return a junk value
 *    (003565C3, 4.9e-39) or 0 on overflow, and powf is 1..96 ulp off even
 *    for exact cases (2^30 -> 1073743360, 1.5^2 -> 2.2499998).
 *
 * So these helpers decide overflow from the operands' exponent bits (and,
 * within one binade of the limit, a float compare) before calling the
 * native operation, which then only runs when its result is in range. They
 * return +-inf on overflow on every platform, so the desktop build runs the
 * same decisions; tests/test_ce_float_ops.c compares them with the host's
 * IEEE arithmetic bit for bit, and checks that no native + - * / they call
 * on finite operands overflows (powf near its limit is checked by result
 * instead; see ce_fpow).
 *
 * Underflow is left to the native operations (the CE flushes to 0, desktop
 * gives a subnormal).
 */
#ifndef CE_FLOAT_OPS_H
#define CE_FLOAT_OPS_H

#include "calcengine.h"
#include "ast.h"
#include <math.h>

/*
 * CE_SAFE_FLOAT selects these helpers (1) or the plain native operations
 * (0). The helpers cost about 3.2 KB of eZ80 code per module, more than
 * most modules have left under the 51,200-byte ceiling, so CE modules opt
 * in from their split/Makefile.* (CEBASE, which evaluates plain arithmetic,
 * does). Every eZ80 build, and every build with a split module's flags
 * (HW_BUILD_SPLIT), defaults to 0, and with 0 the code compiles to exactly
 * what it was before. Desktop builds default to 1, so the suite runs the
 * helpers; desktop
 * builds of a module's own flags (tests/split_cebase_test.c,
 * tests/module_parity) get that module's setting.
 */
#ifndef CE_SAFE_FLOAT
#if defined(__ez80__) || defined(_EZ80) || defined(HW_BUILD_SPLIT)
#define CE_SAFE_FLOAT 0
#else
#define CE_SAFE_FLOAT 1
#endif
#endif

/*
 * CE_SAFE_FLOAT_TINY (only with CE_SAFE_FLOAT; default 0, CEBASE sets 1)
 * refuses tiny arithmetic rather than letting the CE lose precision.
 * On the OS 5.8 ROM, __fadd/__fsub discard an
 * exponent-zero operand and emit a non-IEEE value for a difference below
 * FLT_MIN, __fmul/__fdiv/sqrtf flush, OS powf flushes, and OS logf/powf
 * read exponent-zero bytes with a hidden bit. The supported range is
 * +-0 and |x| >= FLT_MIN (2^-126). With the flag:
 *  - an operand whose exponent field is 0 and mantissa is not (a
 *    "tiny" value) is refused;
 *  - a nonzero result below FLT_MIN is refused, decided from the
 *    operands' exponent bytes before the native call (an add near the
 *    lower boundary runs on operands scaled by 2^64, so an exact
 *    cancellation, which is +-0, is told apart from an underflow);
 *  - zero is classified from the sign-masked bytes, never a native
 *    x != 0 (the CE's compare treats -0 as negative and nonzero).
 * A refusal returns 0 and sets g_ce_fops_tiny; eval.c reports it as
 * CE_FOPS_TINY_MSG. The flag is sticky until eval_init(). Code that drops
 * a refusal it can prove harmless (a power of 1, cos of a tiny angle, a
 * Kahan correction) clears only a refusal it made itself: one already set
 * may come from an argument whose error was not reported yet.
 * Samples of int, sum, d/dx, lim and tangent are internal values: one
 * whose own root operation (through *, / and unary minus) fell below
 * FLT_MIN is taken as 0 when the dropped samples cannot move the
 * sampler's result by half an ulp; otherwise the result is refused
 * (eval.c, ft_sample_ and ft_drops_ok_).
 * This is not IEEE gradual underflow: the CE has no representation that
 * every native routine agrees on, so nothing below FLT_MIN is computed.
 */
#ifndef CE_SAFE_FLOAT_TINY
#define CE_SAFE_FLOAT_TINY 0
#endif
#if CE_SAFE_FLOAT_TINY && !CE_SAFE_FLOAT
#error "CE_SAFE_FLOAT_TINY needs CE_SAFE_FLOAT"
#endif
/* At most 26 characters: the launcher's status row and home column. */
#define CE_FOPS_TINY_MSG "Tiny value unsupported"

#if CE_SAFE_FLOAT

/* Largest arguments whose IEEE single-precision result is finite:
 * expf(0x42B17217), coshf/sinhf(0x42B2D4FC). On the ROM, OS expf is finite
 * there too (7F7FFF4C) and NaN from 0x42B17218; OS sinhf/coshf are finite up
 * to 0x42B2D4FB and return 0 from 0x42B2D4FC, which ce_fexpfn's result check
 * turns into inf. */
#define CE_FOPS_EXP_MAX  88.7228317f
#define CE_FOPS_HYP_MAX  89.4159851f

/* a op b for op = OP_ADD, OP_SUB, OP_MUL or OP_DIV (ast.h); one function
 * rather than four keeps the eZ80 code small. With finite operands it
 * returns +-inf on overflow (also for a product or quotient within about
 * 2 ulp of FLT_MAX) and exact integer sums where CE __fadd truncates.
 * inf/NaN operands follow IEEE rules without calling the CE routines. */
ce_float_t ce_farith(ce_float_t a, ce_float_t b, int op);
#define ce_fadd(a, b) ce_farith((a), (b), OP_ADD)
#define ce_fsub(a, b) ce_farith((a), (b), OP_SUB)
#define ce_fmul(a, b) ce_farith((a), (b), OP_MUL)
#define ce_fdiv(a, b) ce_farith((a), (b), OP_DIV)
/* base^expo. Integer exponents up to 32 use repeated multiplication (exact
 * for integer bases while the result fits in 24 bits, so 2^30 and 3^15 are
 * exact). Other cases use powf after a range check on expo*ln|base|. A
 * negative base with a non-integer exponent gives NaN. */
ce_float_t ce_fpow(ce_float_t base, ce_float_t expo);
/* expf, sinhf or coshf for fn = FN_EXP, FN_SINH or FN_COSH (ast.h), +-inf
 * on overflow. */
ce_float_t ce_fexpfn(ce_float_t x, int fn);
#define ce_fexp(x)  ce_fexpfn((x), FN_EXP)
#define ce_fsinh(x) ce_fexpfn((x), FN_SINH)
#define ce_fcosh(x) ce_fexpfn((x), FN_COSH)

#if CE_SAFE_FLOAT_TINY
/* Set by a refused tiny operand or result; cleared by eval_init(). */
extern unsigned char g_ce_fops_tiny;
/* Class of x from its bytes: 0 for +-0, 1 for a tiny value (exponent
 * field 0, mantissa not), 2 otherwise (normal, inf or NaN). */
unsigned ce_fops_class(ce_float_t x);
#endif

#else /* !CE_SAFE_FLOAT: the native operations, as before */

#define ce_fadd(a, b) ((a) + (b))
#define ce_fsub(a, b) ((a) - (b))
#define ce_fmul(a, b) ((a) * (b))
#define ce_fdiv(a, b) ((a) / (b))
#define ce_fpow(a, b) powf((a), (b))
#define ce_fexp(x)  expf(x)
#define ce_fsinh(x) sinhf(x)
#define ce_fcosh(x) coshf(x)

#endif /* CE_SAFE_FLOAT */

#endif /* CE_FLOAT_OPS_H */
