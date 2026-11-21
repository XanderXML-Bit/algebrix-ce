/*
 * simplify.c — Algebraic simplification
 *
 * Bottom-up recursive pass that applies:
 *   - Constant folding  (2+3 → 5)
 *   - Zero identities   (0+x → x, 0*x → 0)
 *   - One identities    (1*x → x, x^1 → x)
 *   - Power identities  (x^0 → 1, (x^a)^b → x^(a*b))
 *   - Self-cancellation (x-x → 0, x/x → 1)
 *   - Negation cleanup  (-0 → 0, --x → x, -(a-b) → b-a)
 *   - Negation hoisting (a+(-b) → a-b, a-(-b) → a+b, (-a)+b → b-a)
 *   - Constant propagation  (a*(b*x) → (a*b)*x when a,b numbers)
 *   - Distribution      (n*(a+b) → n*a+n*b, n*(a-b) → n*a-n*b)
 *   - Associative folding ((e+a)+b → e+(a+b), (a*x)*b → (a*b)*x)
 *   - Sqrt identities   (sqrt(x)^2 → x, sqrt(x^2) → |x|)
 *   - Trig parity       (sin(-x) → -sin(x), cos(-x) → cos(x))
 *   - Trig sum-to-prod  (sin(a)+sin(b) → 2sin((a+b)/2)cos((a-b)/2), etc.)
 *   - Trig prod-to-sum  (via ast_prod_to_sum(), not auto-applied)
 *   - Half-angle        (sin(E/2)^2 → (1-cos(E))/2, cos(E/2)^2 → (1+cos(E))/2)
 *   - Log laws          (ln(a*b) → ln(a)+ln(b), ln(a/b) → ln(a)-ln(b), ln(a^n) → n*ln(a))
 */
#include "simplify.h"
#include "ce_float_ops.h"
#include "steps.h"
#include "memory.h"
#include <math.h>
#include <stddef.h>

/* ── Helpers ─────────────────────────────────────────────── */
/* ast_equal() and ast_is_num() now live in ast.c as public API.
 * Local is_num() wrapper maps to ast_is_num() for brevity. */
#define is_num(n, v)  ast_is_num((n), (v))

/* is_num(n, 0) accepts |n| < 1e-10.  Rules that drop or return an
 * operand require an exact zero instead: an annihilating rule would turn
 * 1e-30*x into 1e-30, and an additive identity would drop a
 * typed constant that is not negligible next to x, so ln(x+5e-11)' became
 * 1/x. Of the is_num(n, 0) zero tests, only x^c -> 1 keeps
 * the tolerance: for |c| < 1e-10, x^c rounds to exactly 1.0f for every
 * positive finite float x.  (Like-term collection below has its own
 * |a+b| < 1e-10 threshold; that is a separate rule.) */
static bool is_exact_zero(const ast_node_t *n)
{
    return n && n->type == NODE_NUMBER && n->d.number == 0;
}

/* Check if a node is func(E)^2, returning the function arg E and id.
 * Returns true if match, sets *arg and *fn_id. */
static bool is_func_squared(const ast_node_t *n, const ast_node_t **arg, func_id_t *fn_id)
{
    if (n && n->type == NODE_BINOP && n->d.binop.op == OP_POW &&
        is_num(n->d.binop.right, 2) &&
        n->d.binop.left->type == NODE_FUNC) {
        *arg = n->d.binop.left->d.func.arg;
        *fn_id = n->d.binop.left->d.func.id;
        return true;
    }
    return false;
}

/* Check if a node is a negation, return the inner operand. */
static ast_node_t *as_neg(ast_node_t *n)
{
    if (n && n->type == NODE_UNOP && n->d.unop.op == UOP_NEG)
        return n->d.unop.operand;
    return NULL;
}

/*
 * Extract coefficient and "core" from a term for like-term collection.
 * E.g.: 3*x → coeff=3, core=x;  x → coeff=1, core=x;
 * -(2*x) → coeff=-2, core=x.
 * Returns false if cannot extract (non-simple form).
 */
static bool split_coeff(const ast_node_t *n, ce_float_t *coeff,
                        const ast_node_t **core)
{
    if (!n) return false;
    /* -(...) */
    if (n->type == NODE_UNOP && n->d.unop.op == UOP_NEG) {
        if (split_coeff(n->d.unop.operand, coeff, core)) {
            *coeff = -(*coeff);
            return true;
        }
        return false;
    }
    /* number * expr */
    if (n->type == NODE_BINOP && n->d.binop.op == OP_MUL &&
        n->d.binop.left->type == NODE_NUMBER) {
        *coeff = n->d.binop.left->d.number;
        *core  = n->d.binop.right;
        return true;
    }
    /* expr * number */
    if (n->type == NODE_BINOP && n->d.binop.op == OP_MUL &&
        n->d.binop.right->type == NODE_NUMBER) {
        *coeff = n->d.binop.right->d.number;
        *core  = n->d.binop.left;
        return true;
    }
    /* bare expression → coefficient 1 */
    if (n->type != NODE_NUMBER) {
        *coeff = 1;
        *core  = n;
        return true;
    }
    return false;
}

/* ── Simplify a binary operation (children already simplified) */

static ast_node_t *simp_binop(ast_node_t *node)
{
    ast_node_t *L = node->d.binop.left;
    ast_node_t *R = node->d.binop.right;
    binop_t     op = node->d.binop.op;

    /* ── Constant folding ─────────────────────────────── */
    if (L->type == NODE_NUMBER && R->type == NODE_NUMBER) {
        ce_float_t v;
        switch (op) {
        /* ce_f* (ce_float_ops.h) return inf on overflow on the CE too, where
         * the native float routines wrap (1E38*10 gave 2.35e-38). */
        case OP_ADD: v = ce_fadd(L->d.number, R->d.number); break;
        case OP_SUB: v = ce_fsub(L->d.number, R->d.number); break;
        case OP_MUL: v = ce_fmul(L->d.number, R->d.number); break;
        case OP_DIV:
            if (R->d.number == 0) return node;
            v = ce_fdiv(L->d.number, R->d.number); break;
        case OP_POW: v = ce_fpow(L->d.number, R->d.number); break;
        default: return node;
        }
        if (!ce_isfinite(v)) return node;  /* don't fold NaN/Inf */
        mem_free_node(L);
        mem_free_node(R);
        node->type     = NODE_NUMBER;
        node->d.number = v;
        return node;
    }

    switch (op) {

    /* ── Addition ─────────────────────────────────────── */
    case OP_ADD: {
        ast_node_t *r, *inner;
        if (is_exact_zero(L)) { mem_free_node(L); r = R; mem_free_node(node); return r; }
        if (is_exact_zero(R)) { mem_free_node(R); r = L; mem_free_node(node); return r; }
        /* a + (-b) → a - b */
        inner = as_neg(R);
        if (inner) {
            R->d.unop.operand = NULL;
            mem_free_node(R);
            node->d.binop.op    = OP_SUB;
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* (-a) + b → b - a */
        inner = as_neg(L);
        if (inner) {
            L->d.unop.operand = NULL;
            mem_free_node(L);
            node->d.binop.op    = OP_SUB;
            node->d.binop.left  = R;
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* sin(E)^2 + cos(E)^2 → 1  (or cos^2 + sin^2) */
        {
            const ast_node_t *argL, *argR;
            func_id_t fnL, fnR;
            if (is_func_squared(L, &argL, &fnL) &&
                is_func_squared(R, &argR, &fnR) &&
                ast_equal(argL, argR) &&
                ((fnL == FN_SIN && fnR == FN_COS) ||
                 (fnL == FN_COS && fnR == FN_SIN))) {
                ast_free(L); ast_free(R);
                node->type = NODE_NUMBER; node->d.number = 1;
                return node;
            }
        }
        /* 1 + tan(E)^2 → sec(E)^2  (or tan^2 + 1) */
        {
            const ast_node_t *targ;
            func_id_t tfn;
            const ast_node_t *one_node = NULL, *sq_node = NULL;
            if (is_num(L, 1)) { one_node = L; sq_node = R; }
            else if (is_num(R, 1)) { one_node = R; sq_node = L; }
            if (sq_node && is_func_squared(sq_node, &targ, &tfn) && tfn == FN_TAN) {
                ast_node_t *arg_clone = ast_clone(targ);
                ast_free(L); ast_free(R);
                node->d.binop.op    = OP_POW;
                node->d.binop.left  = ast_func(FN_SEC, arg_clone);
                node->d.binop.right = ast_number(2);
                return node;
            }
        }
        /* 1 + cot(E)^2 → csc(E)^2  (or cot^2 + 1) */
        {
            const ast_node_t *targ;
            func_id_t tfn;
            const ast_node_t *one_node = NULL, *sq_node = NULL;
            if (is_num(L, 1)) { one_node = L; sq_node = R; }
            else if (is_num(R, 1)) { one_node = R; sq_node = L; }
            if (sq_node && is_func_squared(sq_node, &targ, &tfn) && tfn == FN_COT) {
                ast_node_t *arg_clone = ast_clone(targ);
                ast_free(L); ast_free(R);
                node->d.binop.op    = OP_POW;
                node->d.binop.left  = ast_func(FN_CSC, arg_clone);
                node->d.binop.right = ast_number(2);
                return node;
            }
        }
        /* Associative constant folding: (E + a) + b → E + (a+b) */
        if (R->type == NODE_NUMBER &&
            L->type == NODE_BINOP && L->d.binop.op == OP_ADD &&
            L->d.binop.right->type == NODE_NUMBER) {
            ce_float_t ab = L->d.binop.right->d.number + R->d.number;
            inner = L->d.binop.left;
            mem_free_node(L->d.binop.right);
            mem_free_node(R);
            mem_free_node(L);
            node->d.binop.left  = inner;
            node->d.binop.right = ast_number(ab);
            return ast_simplify(node);
        }
        /* Associative constant folding: a + (b + E) → (a+b) + E */
        if (L->type == NODE_NUMBER &&
            R->type == NODE_BINOP && R->d.binop.op == OP_ADD &&
            R->d.binop.left->type == NODE_NUMBER) {
            ce_float_t ab = L->d.number + R->d.binop.left->d.number;
            inner = R->d.binop.right;
            mem_free_node(R->d.binop.left);
            mem_free_node(L);
            mem_free_node(R);
            node->d.binop.left  = ast_number(ab);
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* Associative constant folding: a + (E + b) → (a+b) + E */
        if (L->type == NODE_NUMBER &&
            R->type == NODE_BINOP && R->d.binop.op == OP_ADD &&
            R->d.binop.right->type == NODE_NUMBER) {
            ce_float_t ab = L->d.number + R->d.binop.right->d.number;
            inner = R->d.binop.left;
            mem_free_node(R->d.binop.right);
            mem_free_node(L);
            mem_free_node(R);
            node->d.binop.left  = ast_number(ab);
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* Associative constant folding: (a + E) + b → (a+b) + E */
        if (R->type == NODE_NUMBER &&
            L->type == NODE_BINOP && L->d.binop.op == OP_ADD &&
            L->d.binop.left->type == NODE_NUMBER) {
            ce_float_t ab = L->d.binop.left->d.number + R->d.number;
            inner = L->d.binop.right;
            mem_free_node(L->d.binop.left);
            mem_free_node(R);
            mem_free_node(L);
            node->d.binop.left  = ast_number(ab);
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* Like-term collection: a*E + b*E → (a+b)*E  (includes E+E → 2*E) */
        {
            ce_float_t cL, cR;
            const ast_node_t *coreL, *coreR;
            if (split_coeff(L, &cL, &coreL) &&
                split_coeff(R, &cR, &coreR) &&
                ast_equal(coreL, coreR)) {
                ce_float_t sum = cL + cR;
                ast_node_t *new_core = ast_clone(coreL);
                ast_free(L); ast_free(R);
                if (fabsf(sum) < 1e-10f) {
                    mem_free_node(node);
                    ast_free(new_core);
                    return ast_number(0);
                }
                if (fabsf(sum - 1.0f) < 1e-10f) {
                    mem_free_node(node);
                    return new_core;
                }
                node->d.binop.op    = OP_MUL;
                node->d.binop.left  = ast_number(sum);
                node->d.binop.right = new_core;
                return node;
            }
        }
        /* sin(a) + sin(b) → 2*sin((a+b)/2)*cos((a-b)/2) */
        if (L->type == NODE_FUNC && R->type == NODE_FUNC &&
            L->d.func.id == FN_SIN && R->d.func.id == FN_SIN &&
            !ast_equal(L->d.func.arg, R->d.func.arg)) {
            ast_node_t *a = ast_clone(L->d.func.arg);
            ast_node_t *b = ast_clone(R->d.func.arg);
            if (a && b) {
                ast_node_t *half_sum  = ast_binop(OP_DIV, ast_binop(OP_ADD, a, b), ast_number(2));
                ast_node_t *a2 = ast_clone(L->d.func.arg);
                ast_node_t *b2 = ast_clone(R->d.func.arg);
                ast_node_t *half_diff = ast_binop(OP_DIV, ast_binop(OP_SUB, a2, b2), ast_number(2));
                ast_free(L); ast_free(R);
                node->d.binop.op    = OP_MUL;
                node->d.binop.left  = ast_number(2);
                node->d.binop.right = ast_binop(OP_MUL,
                    ast_func(FN_SIN, half_sum),
                    ast_func(FN_COS, half_diff));
                return ast_simplify(node);
            }
            ast_free(a); ast_free(b);
        }
        /* cos(a) + cos(b) → 2*cos((a+b)/2)*cos((a-b)/2) */
        if (L->type == NODE_FUNC && R->type == NODE_FUNC &&
            L->d.func.id == FN_COS && R->d.func.id == FN_COS &&
            !ast_equal(L->d.func.arg, R->d.func.arg)) {
            ast_node_t *a = ast_clone(L->d.func.arg);
            ast_node_t *b = ast_clone(R->d.func.arg);
            if (a && b) {
                ast_node_t *half_sum  = ast_binop(OP_DIV, ast_binop(OP_ADD, a, b), ast_number(2));
                ast_node_t *a2 = ast_clone(L->d.func.arg);
                ast_node_t *b2 = ast_clone(R->d.func.arg);
                ast_node_t *half_diff = ast_binop(OP_DIV, ast_binop(OP_SUB, a2, b2), ast_number(2));
                ast_free(L); ast_free(R);
                node->d.binop.op    = OP_MUL;
                node->d.binop.left  = ast_number(2);
                node->d.binop.right = ast_binop(OP_MUL,
                    ast_func(FN_COS, half_sum),
                    ast_func(FN_COS, half_diff));
                return ast_simplify(node);
            }
            ast_free(a); ast_free(b);
        }
        break;
    }

    /* ── Subtraction ──────────────────────────────────── */
    case OP_SUB: {
        ast_node_t *r, *inner;
        if (is_exact_zero(R)) { mem_free_node(R); r = L; mem_free_node(node); return r; }
        if (is_exact_zero(L)) {
            /* 0 - R → -R */
            mem_free_node(L);
            node->type           = NODE_UNOP;
            node->d.unop.op      = UOP_NEG;
            node->d.unop.operand = R;
            return ast_simplify(node);
        }
        /* x - x → 0 */
        if (ast_equal(L, R)) {
            ast_free(L); ast_free(R);
            node->type = NODE_NUMBER; node->d.number = 0;
            return node;
        }
        /* a - (-b) → a + b */
        inner = as_neg(R);
        if (inner) {
            R->d.unop.operand = NULL;
            mem_free_node(R);
            node->d.binop.op    = OP_ADD;
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* Like-term collection: a*E - b*E → (a-b)*E */
        {
            ce_float_t cL, cR;
            const ast_node_t *coreL, *coreR;
            if (split_coeff(L, &cL, &coreL) &&
                split_coeff(R, &cR, &coreR) &&
                ast_equal(coreL, coreR)) {
                ce_float_t diff = cL - cR;
                ast_node_t *new_core = ast_clone(coreL);
                ast_free(L); ast_free(R);
                if (fabsf(diff) < 1e-10f) {
                    mem_free_node(node);
                    ast_free(new_core);
                    return ast_number(0);
                }
                if (fabsf(diff - 1.0f) < 1e-10f) {
                    mem_free_node(node);
                    return new_core;
                }
                if (fabsf(diff + 1.0f) < 1e-10f) {
                    mem_free_node(node);
                    return ast_unop(UOP_NEG, new_core);
                }
                node->d.binop.op    = OP_MUL;
                node->d.binop.left  = ast_number(diff);
                node->d.binop.right = new_core;
                return node;
            }
        }
        /* sin(a) - sin(b) → 2*cos((a+b)/2)*sin((a-b)/2) */
        if (L->type == NODE_FUNC && R->type == NODE_FUNC &&
            L->d.func.id == FN_SIN && R->d.func.id == FN_SIN &&
            !ast_equal(L->d.func.arg, R->d.func.arg)) {
            ast_node_t *a = ast_clone(L->d.func.arg);
            ast_node_t *b = ast_clone(R->d.func.arg);
            if (a && b) {
                ast_node_t *half_sum  = ast_binop(OP_DIV, ast_binop(OP_ADD, a, b), ast_number(2));
                ast_node_t *a2 = ast_clone(L->d.func.arg);
                ast_node_t *b2 = ast_clone(R->d.func.arg);
                ast_node_t *half_diff = ast_binop(OP_DIV, ast_binop(OP_SUB, a2, b2), ast_number(2));
                ast_free(L); ast_free(R);
                node->d.binop.op    = OP_MUL;
                node->d.binop.left  = ast_number(2);
                node->d.binop.right = ast_binop(OP_MUL,
                    ast_func(FN_COS, half_sum),
                    ast_func(FN_SIN, half_diff));
                return ast_simplify(node);
            }
            ast_free(a); ast_free(b);
        }
        /* cos(a) - cos(b) → -2*sin((a+b)/2)*sin((a-b)/2) */
        if (L->type == NODE_FUNC && R->type == NODE_FUNC &&
            L->d.func.id == FN_COS && R->d.func.id == FN_COS &&
            !ast_equal(L->d.func.arg, R->d.func.arg)) {
            ast_node_t *a = ast_clone(L->d.func.arg);
            ast_node_t *b = ast_clone(R->d.func.arg);
            if (a && b) {
                ast_node_t *half_sum  = ast_binop(OP_DIV, ast_binop(OP_ADD, a, b), ast_number(2));
                ast_node_t *a2 = ast_clone(L->d.func.arg);
                ast_node_t *b2 = ast_clone(R->d.func.arg);
                ast_node_t *half_diff = ast_binop(OP_DIV, ast_binop(OP_SUB, a2, b2), ast_number(2));
                ast_free(L); ast_free(R);
                node->d.binop.op    = OP_MUL;
                node->d.binop.left  = ast_number(-2);
                node->d.binop.right = ast_binop(OP_MUL,
                    ast_func(FN_SIN, half_sum),
                    ast_func(FN_SIN, half_diff));
                return ast_simplify(node);
            }
            ast_free(a); ast_free(b);
        }
        break;
    }

    /* ── Multiplication ───────────────────────────────── */
    case OP_MUL: {
        ast_node_t *r;
        /* 0 * R → 0 */
        if (is_exact_zero(L)) { ast_free(R); r = L; mem_free_node(node); return r; }
        if (is_exact_zero(R)) { ast_free(L); r = R; mem_free_node(node); return r; }
        /* 1 * R → R */
        if (is_num(L, 1)) { mem_free_node(L); r = R; mem_free_node(node); return r; }
        if (is_num(R, 1)) { mem_free_node(R); r = L; mem_free_node(node); return r; }
        /* (-1) * R → -R */
        if (is_num(L, -1)) {
            mem_free_node(L);
            node->type           = NODE_UNOP;
            node->d.unop.op      = UOP_NEG;
            node->d.unop.operand = R;
            return node;
        }
        /* a * (b * X) → (a*b) * X   where a,b are numbers */
        if (L->type == NODE_NUMBER &&
            R->type == NODE_BINOP && R->d.binop.op == OP_MUL &&
            R->d.binop.left->type == NODE_NUMBER) {
            ce_float_t ab = L->d.number * R->d.binop.left->d.number;
            ast_node_t *inner = R->d.binop.right;
            mem_free_node(L);
            mem_free_node(R->d.binop.left);
            mem_free_node(R);
            node->d.binop.left  = ast_number(ab);
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* a * (X * b) → (a*b) * X   where a,b are numbers */
        if (L->type == NODE_NUMBER &&
            R->type == NODE_BINOP && R->d.binop.op == OP_MUL &&
            R->d.binop.right->type == NODE_NUMBER) {
            ce_float_t ab = L->d.number * R->d.binop.right->d.number;
            ast_node_t *inner = R->d.binop.left;
            mem_free_node(L);
            mem_free_node(R->d.binop.right);
            mem_free_node(R);
            node->d.binop.left  = ast_number(ab);
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* (a * X) * b → (a*b) * X   where a,b are numbers */
        if (R->type == NODE_NUMBER &&
            L->type == NODE_BINOP && L->d.binop.op == OP_MUL &&
            L->d.binop.left->type == NODE_NUMBER) {
            ce_float_t ab = L->d.binop.left->d.number * R->d.number;
            ast_node_t *inner = L->d.binop.right;
            mem_free_node(L->d.binop.left);
            mem_free_node(R);
            mem_free_node(L);
            node->d.binop.left  = ast_number(ab);
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* (X * a) * b → (a*b) * X   where a,b are numbers */
        if (R->type == NODE_NUMBER &&
            L->type == NODE_BINOP && L->d.binop.op == OP_MUL &&
            L->d.binop.right->type == NODE_NUMBER) {
            ce_float_t ab = L->d.binop.right->d.number * R->d.number;
            ast_node_t *inner = L->d.binop.left;
            mem_free_node(L->d.binop.right);
            mem_free_node(R);
            mem_free_node(L);
            node->d.binop.left  = ast_number(ab);
            node->d.binop.right = inner;
            return ast_simplify(node);
        }
        /* x * x → x^2 */
        if (ast_equal(L, R)) {
            ast_free(R);
            node->d.binop.op    = OP_POW;
            node->d.binop.right = ast_number(2);
            return node;
        }
        /* x * x^n → x^(n+1) */
        if (R->type == NODE_BINOP && R->d.binop.op == OP_POW &&
            R->d.binop.right->type == NODE_NUMBER &&
            ast_equal(L, R->d.binop.left)) {
            ce_float_t n_val = R->d.binop.right->d.number + 1;
            ast_free(R->d.binop.left); R->d.binop.left = NULL;
            mem_free_node(R->d.binop.right);
            mem_free_node(R);
            node->d.binop.op    = OP_POW;
            node->d.binop.right = ast_number(n_val);
            return node;
        }
        /* x^n * x → x^(n+1) */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_POW &&
            L->d.binop.right->type == NODE_NUMBER &&
            ast_equal(L->d.binop.left, R)) {
            ce_float_t n_val = L->d.binop.right->d.number + 1;
            ast_node_t *base = L->d.binop.left;
            L->d.binop.left = NULL;
            mem_free_node(L->d.binop.right);
            mem_free_node(L);
            ast_free(R);
            node->d.binop.left  = base;
            node->d.binop.op    = OP_POW;
            node->d.binop.right = ast_number(n_val);
            return node;
        }
        /* x^a * x^b → x^(a+b) */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_POW &&
            R->type == NODE_BINOP && R->d.binop.op == OP_POW &&
            L->d.binop.right->type == NODE_NUMBER &&
            R->d.binop.right->type == NODE_NUMBER &&
            ast_equal(L->d.binop.left, R->d.binop.left)) {
            ce_float_t ab = L->d.binop.right->d.number + R->d.binop.right->d.number;
            ast_node_t *base = L->d.binop.left;
            L->d.binop.left = NULL; L->d.binop.right->d.number = 0;
            mem_free_node(L->d.binop.right);
            mem_free_node(L);
            ast_free(R->d.binop.left); R->d.binop.left = NULL;
            mem_free_node(R->d.binop.right);
            mem_free_node(R);
            node->d.binop.op    = OP_POW;
            node->d.binop.left  = base;
            node->d.binop.right = ast_number(ab);
            return ast_simplify(node);
        }
        /* Double-angle: 2*sin(E)*cos(E) → sin(2*E)
         * Tree forms after constant propagation:
         *   (2*sin(E)) * cos(E)  or  (2*cos(E)) * sin(E)
         */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_MUL &&
            is_num(L->d.binop.left, 2)) {
            ast_node_t *inner = L->d.binop.right;
            if (inner->type == NODE_FUNC && R->type == NODE_FUNC) {
                if ((inner->d.func.id == FN_SIN && R->d.func.id == FN_COS &&
                     ast_equal(inner->d.func.arg, R->d.func.arg)) ||
                    (inner->d.func.id == FN_COS && R->d.func.id == FN_SIN &&
                     ast_equal(inner->d.func.arg, R->d.func.arg))) {
                    ast_node_t *two_arg = ast_binop(OP_MUL, ast_number(2),
                                                     ast_clone(inner->d.func.arg));
                    ast_free(L); ast_free(R);
                    node->type       = NODE_FUNC;
                    node->d.func.id  = FN_SIN;
                    node->d.func.arg = two_arg;
                    return node;
                }
            }
        }
        /* Distribution: num*(a+b) → num*a + num*b  (or num*(a-b) → num*a - num*b) */
        if (L->type == NODE_NUMBER && R->type == NODE_BINOP &&
            (R->d.binop.op == OP_ADD || R->d.binop.op == OP_SUB)) {
            ce_float_t c = L->d.number;
            ast_node_t *a = R->d.binop.left;
            ast_node_t *b = R->d.binop.right;
            binop_t sub_op = R->d.binop.op;
            R->d.binop.left  = NULL;
            R->d.binop.right = NULL;
            mem_free_node(L);
            mem_free_node(R);
            node->d.binop.op    = sub_op;
            node->d.binop.left  = ast_binop(OP_MUL, ast_number(c), a);
            node->d.binop.right = ast_binop(OP_MUL, ast_number(c), b);
            return ast_simplify(node);
        }
        /* Distribution: (a+b)*num → num*a + num*b  (or (a-b)*num → num*a - num*b) */
        if (R->type == NODE_NUMBER && L->type == NODE_BINOP &&
            (L->d.binop.op == OP_ADD || L->d.binop.op == OP_SUB)) {
            ce_float_t c = R->d.number;
            ast_node_t *a = L->d.binop.left;
            ast_node_t *b = L->d.binop.right;
            binop_t sub_op = L->d.binop.op;
            L->d.binop.left  = NULL;
            L->d.binop.right = NULL;
            mem_free_node(R);
            mem_free_node(L);
            node->d.binop.op    = sub_op;
            node->d.binop.left  = ast_binop(OP_MUL, ast_number(c), a);
            node->d.binop.right = ast_binop(OP_MUL, ast_number(c), b);
            return ast_simplify(node);
        }
        /* sin(E)*cos(E) or cos(E)*sin(E) → sin(2*E)/2 */
        if (L->type == NODE_FUNC && R->type == NODE_FUNC &&
            ((L->d.func.id == FN_SIN && R->d.func.id == FN_COS) ||
             (L->d.func.id == FN_COS && R->d.func.id == FN_SIN)) &&
            ast_equal(L->d.func.arg, R->d.func.arg)) {
            ast_node_t *two_arg = ast_binop(OP_MUL, ast_number(2),
                                             ast_clone(L->d.func.arg));
            ast_free(L); ast_free(R);
            node->d.binop.op    = OP_DIV;
            node->d.binop.left  = ast_func(FN_SIN, two_arg);
            node->d.binop.right = ast_number(2);
            return node;
        }
        break;
    }

    /* ── Division ─────────────────────────────────────── */
    case OP_DIV: {
        ast_node_t *r;
        if (is_exact_zero(L)) { ast_free(R); r = L; mem_free_node(node); return r; }
        if (is_num(R, 1)) { mem_free_node(R); r = L; mem_free_node(node); return r; }
        /* x / x → 1 */
        if (ast_equal(L, R)) {
            ast_free(L); ast_free(R);
            node->type = NODE_NUMBER; node->d.number = 1;
            return node;
        }
        /* x^a / x^b → x^(a-b) */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_POW &&
            R->type == NODE_BINOP && R->d.binop.op == OP_POW &&
            ast_equal(L->d.binop.left, R->d.binop.left)) {
            ast_node_t *base = L->d.binop.left;
            ast_node_t *exp_a = L->d.binop.right;
            ast_node_t *exp_b = R->d.binop.right;
            L->d.binop.left = NULL; L->d.binop.right = NULL;
            mem_free_node(L);
            ast_free(R->d.binop.left); R->d.binop.left = NULL;
            R->d.binop.right = NULL;
            mem_free_node(R);
            node->d.binop.op    = OP_POW;
            node->d.binop.left  = base;
            node->d.binop.right = ast_binop(OP_SUB, exp_a, exp_b);
            return ast_simplify(node);
        }
        /* x / x^b → x^(1-b) */
        if (R->type == NODE_BINOP && R->d.binop.op == OP_POW &&
            ast_equal(L, R->d.binop.left)) {
            ast_node_t *base = L;
            ast_node_t *exp_b = R->d.binop.right;
            ast_free(R->d.binop.left); R->d.binop.left = NULL;
            R->d.binop.right = NULL;
            mem_free_node(R);
            node->d.binop.op    = OP_POW;
            node->d.binop.left  = base;
            node->d.binop.right = ast_binop(OP_SUB, ast_number(1), exp_b);
            return ast_simplify(node);
        }
        /* x^a / x → x^(a-1) */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_POW &&
            ast_equal(L->d.binop.left, R)) {
            ast_node_t *base = L->d.binop.left;
            ast_node_t *exp_a = L->d.binop.right;
            L->d.binop.left = NULL; L->d.binop.right = NULL;
            mem_free_node(L);
            ast_free(R);
            node->d.binop.op    = OP_POW;
            node->d.binop.left  = base;
            node->d.binop.right = ast_binop(OP_SUB, exp_a, ast_number(1));
            return ast_simplify(node);
        }
        /* (a / b) / c → a / (b * c)  (flatten nested division) */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_DIV) {
            ast_node_t *a = L->d.binop.left;
            ast_node_t *b = L->d.binop.right;
            L->d.binop.left  = NULL;
            L->d.binop.right = NULL;
            mem_free_node(L);
            node->d.binop.left  = a;
            node->d.binop.right = ast_binop(OP_MUL, b, R);
            return ast_simplify(node);
        }
        /* a / (b / c) → (a * c) / b  (flatten nested division) */
        if (R->type == NODE_BINOP && R->d.binop.op == OP_DIV) {
            ast_node_t *b = R->d.binop.left;
            ast_node_t *c = R->d.binop.right;
            R->d.binop.left  = NULL;
            R->d.binop.right = NULL;
            mem_free_node(R);
            node->d.binop.left  = ast_binop(OP_MUL, L, c);
            node->d.binop.right = b;
            return ast_simplify(node);
        }
        /* GCD-based fraction reduction: n / m → (n/g) / (m/g) for integer n,m */
        if (L->type == NODE_NUMBER && R->type == NODE_NUMBER) {
            int ni = (int)L->d.number;
            int mi = (int)R->d.number;
            if (fabsf(L->d.number - (float)ni) < 1e-6f &&
                fabsf(R->d.number - (float)mi) < 1e-6f &&
                mi != 0 && ni != 0) {
                int a_val = ni < 0 ? -ni : ni;
                int b_val = mi < 0 ? -mi : mi;
                int g;
                /* Euclidean GCD */
                while (b_val) { int t = b_val; b_val = a_val % b_val; a_val = t; }
                g = a_val;
                if (g > 1) {
                    int rn = ni / g, rm = mi / g;
                    if (rm < 0) { rn = -rn; rm = -rm; } /* normalize sign */
                    if (rm == 1) {
                        mem_free_node(R);
                        L->d.number = (ce_float_t)rn;
                        r = L;
                        mem_free_node(node);
                        return r;
                    }
                    L->d.number = (ce_float_t)rn;
                    R->d.number = (ce_float_t)rm;
                    return node;
                }
            }
        }
        break;
    }

    /* ── Power ────────────────────────────────────────── */
    case OP_POW: {
        ast_node_t *r;
        if (is_num(R, 0)) { ast_free(L); mem_free_node(R);
            node->type = NODE_NUMBER; node->d.number = 1; return node; }
        if (is_num(R, 1)) { mem_free_node(R); r = L; mem_free_node(node); return r; }
        if (is_exact_zero(L)) { ast_free(R); r = L; mem_free_node(node); return r; }
        if (is_num(L, 1)) { ast_free(R); r = L; mem_free_node(node); return r; }
        /* sqrt(x)^2 → x */
        if (is_num(R, 2) && L->type == NODE_FUNC && L->d.func.id == FN_SQRT) {
            ast_node_t *inner = L->d.func.arg;
            L->d.func.arg = NULL;
            mem_free_node(L);
            mem_free_node(R);
            mem_free_node(node);
            return inner;
        }
        /* sin(E/2)^2 → (1 - cos(E)) / 2  (half-angle identity) */
        if (is_num(R, 2) && L->type == NODE_FUNC &&
            L->d.func.id == FN_SIN &&
            L->d.func.arg->type == NODE_BINOP &&
            L->d.func.arg->d.binop.op == OP_DIV &&
            is_num(L->d.func.arg->d.binop.right, 2)) {
            ast_node_t *E = ast_clone(L->d.func.arg->d.binop.left);
            if (E) {
                ast_free(L); mem_free_node(R);
                node->d.binop.op    = OP_DIV;
                node->d.binop.left  = ast_binop(OP_SUB, ast_number(1),
                                                  ast_func(FN_COS, E));
                node->d.binop.right = ast_number(2);
                return ast_simplify(node);
            }
        }
        /* cos(E/2)^2 → (1 + cos(E)) / 2  (half-angle identity) */
        if (is_num(R, 2) && L->type == NODE_FUNC &&
            L->d.func.id == FN_COS &&
            L->d.func.arg->type == NODE_BINOP &&
            L->d.func.arg->d.binop.op == OP_DIV &&
            is_num(L->d.func.arg->d.binop.right, 2)) {
            ast_node_t *E = ast_clone(L->d.func.arg->d.binop.left);
            if (E) {
                ast_free(L); mem_free_node(R);
                node->d.binop.op    = OP_DIV;
                node->d.binop.left  = ast_binop(OP_ADD, ast_number(1),
                                                  ast_func(FN_COS, E));
                node->d.binop.right = ast_number(2);
                return ast_simplify(node);
            }
        }
        /* e^(ln(x)) → x */
        if (L->type == NODE_CONSTANT && L->d.constant == CONST_E &&
            R->type == NODE_FUNC && R->d.func.id == FN_LN) {
            ast_node_t *x = R->d.func.arg;
            R->d.func.arg = NULL;
            mem_free_node(R);
            mem_free_node(L);
            mem_free_node(node);
            return x;
        }
        /* (x^a)^b → x^(a*b) when a,b are numbers */
        if (L->type == NODE_BINOP && L->d.binop.op == OP_POW &&
            L->d.binop.right->type == NODE_NUMBER &&
            R->type == NODE_NUMBER) {
            ce_float_t ab = L->d.binop.right->d.number * R->d.number;
            ast_node_t *base = L->d.binop.left;
            mem_free_node(L->d.binop.right);
            mem_free_node(R);
            mem_free_node(L);
            node->d.binop.left  = base;
            node->d.binop.right = ast_number(ab);
            return ast_simplify(node);
        }
        break;
    }

    default:
        break;
    }

    return node;
}

/* ── Simplify a unary operation ──────────────────────────── */

static ast_node_t *simp_unop(ast_node_t *node)
{
    ast_node_t *child = node->d.unop.operand;

    if (node->d.unop.op == UOP_NEG) {
        /* -0 → 0 */
        if (is_exact_zero(child)) {
            mem_free_node(node);
            return child;
        }
        /* -(number) → fold */
        if (child->type == NODE_NUMBER) {
            child->d.number = -child->d.number;
            mem_free_node(node);
            return child;
        }
        /* -(-x) → x */
        if (child->type == NODE_UNOP && child->d.unop.op == UOP_NEG) {
            ast_node_t *inner = child->d.unop.operand;
            mem_free_node(child);
            mem_free_node(node);
            return inner;
        }
        /* -(a - b) → b - a */
        if (child->type == NODE_BINOP && child->d.binop.op == OP_SUB) {
            ast_node_t *a = child->d.binop.left;
            ast_node_t *b = child->d.binop.right;
            child->d.binop.left  = b;
            child->d.binop.right = a;
            mem_free_node(node);
            return ast_simplify(child);
        }
    }
    return node;
}

/* ── Main simplification entry point ─────────────────────── */

/* Each nesting level costs one ast_simplify + simp_binop frame pair (about
 * 65 B on eZ80), so CE modules with a tight stack may lower this from the
 * build line.  Past the limit a subtree is returned unsimplified, which is
 * still a correct expression. */
#ifndef SIMPLIFY_MAX_DEPTH
#define SIMPLIFY_MAX_DEPTH 50
#endif
static int g_simplify_depth = 0;
/* mem_alloc_failures() when the outermost ast_simplify call started. */
static uint16_t g_simplify_fail_mark = 0;

/* True once a node allocation has failed during this simplification.  A
 * rewrite that ran out of nodes can leave a NULL operand anywhere below the
 * node it rewrote, and the rules dereference operands and their children,
 * so after a failure every remaining node is returned as it is.  The caller
 * sees the hole (deriv.c checks the finished tree). */
static bool simplify_pool_failed(void)
{
    return mem_alloc_failures() != g_simplify_fail_mark;
}

static ast_node_t *simplify_inner(ast_node_t *node)
{
    if (!node) return NULL;
    if (simplify_pool_failed()) return node;

    switch (node->type) {
    /* A child can be NULL when the node pool ran out while the tree was
     * built or rewritten (ast_binop keeps a NULL operand).  The rules below
     * dereference their operands, so such a node, and every node once an
     * allocation has failed in this call, is returned unchanged; callers
     * that need a complete tree check for the hole themselves. */
    case NODE_BINOP:
        node->d.binop.left  = ast_simplify(node->d.binop.left);
        node->d.binop.right = ast_simplify(node->d.binop.right);
        if (!node->d.binop.left || !node->d.binop.right ||
            simplify_pool_failed()) return node;
        return simp_binop(node);

    case NODE_UNOP:
        node->d.unop.operand = ast_simplify(node->d.unop.operand);
        if (!node->d.unop.operand || simplify_pool_failed()) return node;
        return simp_unop(node);

    case NODE_FUNC:
        node->d.func.arg = ast_simplify(node->d.func.arg);
        if (!node->d.func.arg || simplify_pool_failed()) return node;
        /* ── Log / exp simplification rules ────────────── */
        {
            ast_node_t *arg = node->d.func.arg;
            func_id_t fn = node->d.func.id;

            /* ln(1) -> 0 */
            if (fn == FN_LN && is_num(arg, 1)) {
                ast_free(arg);
                node->type = NODE_NUMBER;
                node->d.number = 0;
                return node;
            }
            /* ln(e) -> 1 */
            if (fn == FN_LN && arg->type == NODE_CONSTANT &&
                arg->d.constant == CONST_E) {
                ast_free(arg);
                node->type = NODE_NUMBER;
                node->d.number = 1;
                return node;
            }
            /* log(1) -> 0 */
            if (fn == FN_LOG && is_num(arg, 1)) {
                ast_free(arg);
                node->type = NODE_NUMBER;
                node->d.number = 0;
                return node;
            }
            /* log(10) -> 1 */
            if (fn == FN_LOG && is_num(arg, 10)) {
                ast_free(arg);
                node->type = NODE_NUMBER;
                node->d.number = 1;
                return node;
            }
            /* ln(e^x) -> x */
            if (fn == FN_LN && arg->type == NODE_BINOP &&
                arg->d.binop.op == OP_POW &&
                arg->d.binop.left->type == NODE_CONSTANT &&
                arg->d.binop.left->d.constant == CONST_E) {
                ast_node_t *x = arg->d.binop.right;
                arg->d.binop.right = NULL;
                ast_free(arg);
                mem_free_node(node);
                return x;
            }
            /* ln(exp(x)) -> x */
            if (fn == FN_LN && arg->type == NODE_FUNC &&
                arg->d.func.id == FN_EXP) {
                ast_node_t *x = arg->d.func.arg;
                arg->d.func.arg = NULL;
                mem_free_node(arg);
                mem_free_node(node);
                return x;
            }
            /* exp(ln(x)) -> x */
            if (fn == FN_EXP && arg->type == NODE_FUNC &&
                arg->d.func.id == FN_LN) {
                ast_node_t *x = arg->d.func.arg;
                arg->d.func.arg = NULL;
                mem_free_node(arg);
                mem_free_node(node);
                return x;
            }
            /* ln(a * b) -> ln(a) + ln(b) */
            if (fn == FN_LN && arg->type == NODE_BINOP &&
                arg->d.binop.op == OP_MUL) {
                ast_node_t *a = arg->d.binop.left;
                ast_node_t *b = arg->d.binop.right;
                arg->d.binop.left  = NULL;
                arg->d.binop.right = NULL;
                mem_free_node(arg);
                node->type          = NODE_BINOP;
                node->d.binop.op    = OP_ADD;
                node->d.binop.left  = ast_func(FN_LN, a);
                node->d.binop.right = ast_func(FN_LN, b);
                return ast_simplify(node);
            }
            /* ln(a / b) -> ln(a) - ln(b) */
            if (fn == FN_LN && arg->type == NODE_BINOP &&
                arg->d.binop.op == OP_DIV) {
                ast_node_t *a = arg->d.binop.left;
                ast_node_t *b = arg->d.binop.right;
                arg->d.binop.left  = NULL;
                arg->d.binop.right = NULL;
                mem_free_node(arg);
                node->type          = NODE_BINOP;
                node->d.binop.op    = OP_SUB;
                node->d.binop.left  = ast_func(FN_LN, a);
                node->d.binop.right = ast_func(FN_LN, b);
                return ast_simplify(node);
            }
            /* ln(a^n) -> n * ln(a) */
            if (fn == FN_LN && arg->type == NODE_BINOP &&
                arg->d.binop.op == OP_POW) {
                ast_node_t *a = arg->d.binop.left;
                ast_node_t *n_exp = arg->d.binop.right;
                arg->d.binop.left  = NULL;
                arg->d.binop.right = NULL;
                mem_free_node(arg);
                node->type          = NODE_BINOP;
                node->d.binop.op    = OP_MUL;
                node->d.binop.left  = n_exp;
                node->d.binop.right = ast_func(FN_LN, a);
                return ast_simplify(node);
            }
            /* sqrt(n) -> a*sqrt(b) where n = a^2 * b (radical simplification) */
            if (fn == FN_SQRT && arg->type == NODE_NUMBER &&
                arg->d.number > 1 && arg->d.number < 1e6f) {
                int n = (int)arg->d.number;
                if (fabsf(arg->d.number - (float)n) < 1e-6f && n > 1) {
                    /* Find largest perfect square factor */
                    int a = 1, b = n, k;
                    for (k = (int)sqrtf((float)n); k >= 2; k--) {
                        if (n % (k * k) == 0) {
                            a = k;
                            b = n / (k * k);
                            break;
                        }
                    }
                    if (a > 1) {
                        if (b == 1) {
                            /* Perfect square: sqrt(9) → 3 */
                            ast_free(arg);
                            node->type = NODE_NUMBER;
                            node->d.number = (ce_float_t)a;
                            return node;
                        }
                        /* sqrt(12) → 2*sqrt(3) */
                        arg->d.number = (ce_float_t)b;
                        /* node is already sqrt(b), wrap in a*node */
                        return ast_binop(OP_MUL, ast_number((ce_float_t)a), node);
                    }
                }
            }
            /* sqrt(x^2) -> |x| */
            if (fn == FN_SQRT && arg->type == NODE_BINOP &&
                arg->d.binop.op == OP_POW && is_num(arg->d.binop.right, 2)) {
                ast_node_t *x = arg->d.binop.left;
                arg->d.binop.left = NULL;
                mem_free_node(arg->d.binop.right);
                mem_free_node(arg);
                node->d.func.id  = FN_ABS;
                node->d.func.arg = x;
                return node;
            }
            /* sin(-x) -> -sin(x) */
            if (fn == FN_SIN && arg->type == NODE_UNOP &&
                arg->d.unop.op == UOP_NEG) {
                ast_node_t *inner = arg->d.unop.operand;
                arg->d.unop.operand = NULL;
                mem_free_node(arg);
                node->d.func.arg = inner;
                return ast_unop(UOP_NEG, node);
            }
            /* cos(-x) -> cos(x) */
            if (fn == FN_COS && arg->type == NODE_UNOP &&
                arg->d.unop.op == UOP_NEG) {
                ast_node_t *inner = arg->d.unop.operand;
                arg->d.unop.operand = NULL;
                mem_free_node(arg);
                node->d.func.arg = inner;
                return node;
            }
            /* tan(-x) -> -tan(x) */
            if (fn == FN_TAN && arg->type == NODE_UNOP &&
                arg->d.unop.op == UOP_NEG) {
                ast_node_t *inner = arg->d.unop.operand;
                arg->d.unop.operand = NULL;
                mem_free_node(arg);
                node->d.func.arg = inner;
                return ast_unop(UOP_NEG, node);
            }
        }
        return node;

    default:
        return node;
    }
}

ast_node_t *ast_simplify(ast_node_t *node)
{
    ast_node_t *result;
    if (!node) return NULL;
    if (g_simplify_depth >= SIMPLIFY_MAX_DEPTH) {
        steps_addf("Warning: simplification depth limit (%d) reached - expression left unsimplified",
                   SIMPLIFY_MAX_DEPTH);
        return node;
    }
    if (g_simplify_depth == 0) g_simplify_fail_mark = mem_alloc_failures();
    g_simplify_depth++;
    result = simplify_inner(node);
    g_simplify_depth--;
    return result;
}

/* ── Product-to-sum (standalone, not auto-applied) ────────── */

ast_node_t *ast_prod_to_sum(const ast_node_t *node)
{
    const ast_node_t *L, *R;
    func_id_t fnL, fnR;

    if (!node || node->type != NODE_BINOP || node->d.binop.op != OP_MUL)
        return NULL;

    L = node->d.binop.left;
    R = node->d.binop.right;

    /* Need both sides to be trig functions */
    if (!L || !R || L->type != NODE_FUNC || R->type != NODE_FUNC)
        return NULL;

    fnL = L->d.func.id;
    fnR = R->d.func.id;

    /* Only handle sin/cos products */
    if ((fnL != FN_SIN && fnL != FN_COS) ||
        (fnR != FN_SIN && fnR != FN_COS))
        return NULL;

    /* Don't apply if arguments are equal (double-angle handles that) */
    if (ast_equal(L->d.func.arg, R->d.func.arg))
        return NULL;

    {
        ast_node_t *a1 = ast_clone(L->d.func.arg);
        ast_node_t *b1 = ast_clone(R->d.func.arg);
        ast_node_t *a2 = ast_clone(L->d.func.arg);
        ast_node_t *b2 = ast_clone(R->d.func.arg);
        ast_node_t *sum_ab, *dif_ab, *result;

        if (!a1 || !b1 || !a2 || !b2) {
            ast_free(a1); ast_free(b1); ast_free(a2); ast_free(b2);
            return NULL;
        }

        sum_ab = ast_binop(OP_ADD, a1, b1);
        dif_ab = ast_binop(OP_SUB, a2, b2);

        if (fnL == FN_SIN && fnR == FN_COS) {
            /* sin(a)*cos(b) → [sin(a+b) + sin(a-b)] / 2 */
            result = ast_binop(OP_DIV,
                ast_binop(OP_ADD,
                    ast_func(FN_SIN, sum_ab),
                    ast_func(FN_SIN, dif_ab)),
                ast_number(2));
        } else if (fnL == FN_COS && fnR == FN_SIN) {
            /* cos(a)*sin(b) → [sin(a+b) - sin(a-b)] / 2 */
            result = ast_binop(OP_DIV,
                ast_binop(OP_SUB,
                    ast_func(FN_SIN, sum_ab),
                    ast_func(FN_SIN, dif_ab)),
                ast_number(2));
        } else if (fnL == FN_COS && fnR == FN_COS) {
            /* cos(a)*cos(b) → [cos(a+b) + cos(a-b)] / 2 */
            result = ast_binop(OP_DIV,
                ast_binop(OP_ADD,
                    ast_func(FN_COS, sum_ab),
                    ast_func(FN_COS, dif_ab)),
                ast_number(2));
        } else {
            /* sin(a)*sin(b) → [cos(a-b) - cos(a+b)] / 2 */
            result = ast_binop(OP_DIV,
                ast_binop(OP_SUB,
                    ast_func(FN_COS, dif_ab),
                    ast_func(FN_COS, sum_ab)),
                ast_number(2));
        }

        return result;
    }
}
