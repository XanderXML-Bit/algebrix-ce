/*
 * ast.c — AST node constructors, destructor, and pretty-printer
 */
#include "ast.h"
#include "memory.h"
#include "ce_float_class.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#ifndef CE_AST_ENABLE_STAT_NAMES
#define CE_AST_ENABLE_STAT_NAMES 1
#endif

/* ── Constructors ────────────────────────────────────────── */

ast_node_t *ast_number(ce_float_t val)
{
    ast_node_t *n = mem_alloc_node();
    if (!n) return NULL;
    n->type       = NODE_NUMBER;
    n->d.number   = val;
    return n;
}

ast_node_t *ast_variable(char var)
{
    ast_node_t *n = mem_alloc_node();
    if (!n) return NULL;
    n->type       = NODE_VARIABLE;
    n->d.variable = var;
    return n;
}

ast_node_t *ast_constant(const_id_t c)
{
    ast_node_t *n = mem_alloc_node();
    if (!n) return NULL;
    n->type       = NODE_CONSTANT;
    n->d.constant = c;
    return n;
}

ast_node_t *ast_binop(binop_t op, ast_node_t *left, ast_node_t *right)
{
    ast_node_t *n = mem_alloc_node();
    if (!n) { ast_free(left); ast_free(right); return NULL; }
    n->type          = NODE_BINOP;
    n->d.binop.op    = op;
    n->d.binop.left  = left;
    n->d.binop.right = right;
    return n;
}

ast_node_t *ast_unop(unop_t op, ast_node_t *operand)
{
    ast_node_t *n = mem_alloc_node();
    if (!n) { ast_free(operand); return NULL; }
    n->type           = NODE_UNOP;
    n->d.unop.op      = op;
    n->d.unop.operand = operand;
    return n;
}

ast_node_t *ast_func(func_id_t fn, ast_node_t *arg)
{
    ast_node_t *n = mem_alloc_node();
    if (!n) { ast_free(arg); return NULL; }
    n->type       = NODE_FUNC;
    n->d.func.id  = fn;
    n->d.func.arg = arg;
    return n;
}

/* ── Recursive destructor ────────────────────────────────── */

void ast_free(ast_node_t *node)
{
    if (!node) return;
    switch (node->type) {
    case NODE_BINOP:
        ast_free(node->d.binop.left);
        ast_free(node->d.binop.right);
        break;
    case NODE_UNOP:
        ast_free(node->d.unop.operand);
        break;
    case NODE_FUNC:
        ast_free(node->d.func.arg);
        break;
    case NODE_DERIV:
        ast_free(node->d.deriv.expr);
        break;
    case NODE_INTEGRAL:
        ast_free(node->d.integral.expr);
        ast_free(node->d.integral.lo);
        ast_free(node->d.integral.hi);
        break;
    case NODE_LIMIT:
        ast_free(node->d.limit.expr);
        ast_free(node->d.limit.point);
        break;
    case NODE_SUM:
        ast_free(node->d.sum.expr);
        ast_free(node->d.sum.lo);
        ast_free(node->d.sum.hi);
        break;
    case NODE_PRODUCT:
        ast_free(node->d.product.expr);
        ast_free(node->d.product.lo);
        ast_free(node->d.product.hi);
        break;
    case NODE_TAYLOR:
        ast_free(node->d.taylor.expr);
        ast_free(node->d.taylor.center);
        ast_free(node->d.taylor.degree);
        break;
    case NODE_FOURIER:
        ast_free(node->d.fourier.expr);
        ast_free(node->d.fourier.nterms);
        break;
    case NODE_EXPAND:
        ast_free(node->d.expand.expr);
        break;
    case NODE_FACTOR:
        ast_free(node->d.factor.expr);
        break;
    case NODE_SOLVE:
        ast_free(node->d.solve.expr);
        break;
    case NODE_TANGENT:
        ast_free(node->d.tangent.expr);
        ast_free(node->d.tangent.point);
        break;
    case NODE_PAIR:
        ast_free(node->d.pair.head);
        ast_free(node->d.pair.tail);
        break;
    case NODE_STAT:
        ast_free(node->d.stat.args);
        break;
    case NODE_MATRIX:
        break;
    case NODE_MATRIX_OP:
        ast_free(node->d.matrix_op.arg);
        break;
    case NODE_POLAR:
        ast_free(node->d.polar.expr1);
        ast_free(node->d.polar.expr2);
        ast_free(node->d.polar.lo);
        ast_free(node->d.polar.hi);
        break;
    case NODE_LIST_LITERAL:
        ast_free(node->d.list_literal.elems);
        break;
    case NODE_LIST_REF:
        break;
    case NODE_LIST_OP:
        ast_free(node->d.list_op.arg);
        break;
    case NODE_LIST_ELEM:
        ast_free(node->d.list_elem.index);
        break;
    case NODE_STORE:
        ast_free(node->d.store.expr);
        break;
    case NODE_PIECEWISE:
        ast_free(node->d.piecewise.pieces);
        break;
    default:
        break;
    }
    mem_free_node(node);
}

/* ── Deep copy ───────────────────────────────────────────── */

ast_node_t *ast_clone(const ast_node_t *n)
{
    ast_node_t *c;
    if (!n) return NULL;
    switch (n->type) {
    case NODE_NUMBER:   return ast_number(n->d.number);
    case NODE_VARIABLE: return ast_variable(n->d.variable);
    case NODE_CONSTANT: return ast_constant(n->d.constant);
    case NODE_BINOP: {
        ast_node_t *cl = ast_clone(n->d.binop.left);
        ast_node_t *cr = ast_clone(n->d.binop.right);
        if ((n->d.binop.left && !cl) || (n->d.binop.right && !cr)) {
            ast_free(cl); ast_free(cr); return NULL;
        }
        /* ast_binop/ast_unop/ast_func free their operands on failure. */
        return ast_binop(n->d.binop.op, cl, cr);
    }
    case NODE_UNOP: {
        ast_node_t *co = ast_clone(n->d.unop.operand);
        if (n->d.unop.operand && !co) return NULL;
        return ast_unop(n->d.unop.op, co);
    }
    case NODE_FUNC: {
        ast_node_t *ca = ast_clone(n->d.func.arg);
        if (n->d.func.arg && !ca) return NULL;
        return ast_func(n->d.func.id, ca);
    }
    case NODE_DERIV:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type          = NODE_DERIV;
        c->d.deriv.var   = n->d.deriv.var;
        c->d.deriv.expr  = ast_clone(n->d.deriv.expr);
        if (n->d.deriv.expr && !c->d.deriv.expr) { mem_free_node(c); return NULL; }
        return c;
    case NODE_INTEGRAL:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type              = NODE_INTEGRAL;
        c->d.integral.var    = n->d.integral.var;
        c->d.integral.expr   = ast_clone(n->d.integral.expr);
        c->d.integral.lo     = ast_clone(n->d.integral.lo);
        c->d.integral.hi     = ast_clone(n->d.integral.hi);
        if ((n->d.integral.expr && !c->d.integral.expr) ||
            (n->d.integral.lo && !c->d.integral.lo) ||
            (n->d.integral.hi && !c->d.integral.hi)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_LIMIT:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type              = NODE_LIMIT;
        c->d.limit.var       = n->d.limit.var;
        c->d.limit.expr      = ast_clone(n->d.limit.expr);
        c->d.limit.point     = ast_clone(n->d.limit.point);
        c->d.limit.direction = n->d.limit.direction;
        if ((n->d.limit.expr && !c->d.limit.expr) ||
            (n->d.limit.point && !c->d.limit.point)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_SUM:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type          = NODE_SUM;
        c->d.sum.var     = n->d.sum.var;
        c->d.sum.expr    = ast_clone(n->d.sum.expr);
        c->d.sum.lo      = ast_clone(n->d.sum.lo);
        c->d.sum.hi      = ast_clone(n->d.sum.hi);
        if ((n->d.sum.expr && !c->d.sum.expr) ||
            (n->d.sum.lo && !c->d.sum.lo) ||
            (n->d.sum.hi && !c->d.sum.hi)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_PRODUCT:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type              = NODE_PRODUCT;
        c->d.product.var     = n->d.product.var;
        c->d.product.expr    = ast_clone(n->d.product.expr);
        c->d.product.lo      = ast_clone(n->d.product.lo);
        c->d.product.hi      = ast_clone(n->d.product.hi);
        if ((n->d.product.expr && !c->d.product.expr) ||
            (n->d.product.lo && !c->d.product.lo) ||
            (n->d.product.hi && !c->d.product.hi)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_TAYLOR:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type              = NODE_TAYLOR;
        c->d.taylor.var      = n->d.taylor.var;
        c->d.taylor.expr     = ast_clone(n->d.taylor.expr);
        c->d.taylor.center   = ast_clone(n->d.taylor.center);
        c->d.taylor.degree   = ast_clone(n->d.taylor.degree);
        if ((n->d.taylor.expr && !c->d.taylor.expr) ||
            (n->d.taylor.center && !c->d.taylor.center) ||
            (n->d.taylor.degree && !c->d.taylor.degree)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_FOURIER:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type              = NODE_FOURIER;
        c->d.fourier.var     = n->d.fourier.var;
        c->d.fourier.expr    = ast_clone(n->d.fourier.expr);
        c->d.fourier.nterms  = ast_clone(n->d.fourier.nterms);
        if ((n->d.fourier.expr && !c->d.fourier.expr) ||
            (n->d.fourier.nterms && !c->d.fourier.nterms)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_EXPAND:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type            = NODE_EXPAND;
        c->d.expand.var    = n->d.expand.var;
        c->d.expand.expr   = ast_clone(n->d.expand.expr);
        if (n->d.expand.expr && !c->d.expand.expr) { mem_free_node(c); return NULL; }
        return c;
    case NODE_FACTOR:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type            = NODE_FACTOR;
        c->d.factor.var    = n->d.factor.var;
        c->d.factor.expr   = ast_clone(n->d.factor.expr);
        if (n->d.factor.expr && !c->d.factor.expr) { mem_free_node(c); return NULL; }
        return c;
    case NODE_SOLVE:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type           = NODE_SOLVE;
        c->d.solve.var    = n->d.solve.var;
        c->d.solve.expr   = ast_clone(n->d.solve.expr);
        if (n->d.solve.expr && !c->d.solve.expr) { mem_free_node(c); return NULL; }
        return c;
    case NODE_TANGENT:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type              = NODE_TANGENT;
        c->d.tangent.var     = n->d.tangent.var;
        c->d.tangent.expr    = ast_clone(n->d.tangent.expr);
        c->d.tangent.point   = ast_clone(n->d.tangent.point);
        if ((n->d.tangent.expr && !c->d.tangent.expr) ||
            (n->d.tangent.point && !c->d.tangent.point)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_PAIR:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type           = NODE_PAIR;
        c->d.pair.head    = ast_clone(n->d.pair.head);
        c->d.pair.tail    = ast_clone(n->d.pair.tail);
        if ((n->d.pair.head && !c->d.pair.head) ||
            (n->d.pair.tail && !c->d.pair.tail)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_STAT:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type           = NODE_STAT;
        c->d.stat.fn      = n->d.stat.fn;
        c->d.stat.args    = ast_clone(n->d.stat.args);
        if (n->d.stat.args && !c->d.stat.args) { ast_free(c); return NULL; }
        return c;
    case NODE_MATRIX:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type               = NODE_MATRIX;
        c->d.matrix.store_idx = n->d.matrix.store_idx;
        c->d.matrix.rows      = n->d.matrix.rows;
        c->d.matrix.cols      = n->d.matrix.cols;
        return c;
    case NODE_MATRIX_OP:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type              = NODE_MATRIX_OP;
        c->d.matrix_op.op    = n->d.matrix_op.op;
        c->d.matrix_op.arg   = ast_clone(n->d.matrix_op.arg);
        c->d.matrix_op.rows  = n->d.matrix_op.rows;
        c->d.matrix_op.cols  = n->d.matrix_op.cols;
        if (n->d.matrix_op.arg && !c->d.matrix_op.arg) { mem_free_node(c); return NULL; }
        return c;
    case NODE_POLAR:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type           = NODE_POLAR;
        c->d.polar.op     = n->d.polar.op;
        c->d.polar.var    = n->d.polar.var;
        c->d.polar.expr1  = ast_clone(n->d.polar.expr1);
        c->d.polar.expr2  = ast_clone(n->d.polar.expr2);
        c->d.polar.lo     = ast_clone(n->d.polar.lo);
        c->d.polar.hi     = ast_clone(n->d.polar.hi);
        if ((n->d.polar.expr1 && !c->d.polar.expr1) ||
            (n->d.polar.expr2 && !c->d.polar.expr2) ||
            (n->d.polar.lo && !c->d.polar.lo) ||
            (n->d.polar.hi && !c->d.polar.hi)) {
            ast_free(c); return NULL;
        }
        return c;
    case NODE_LIST_LITERAL:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type = NODE_LIST_LITERAL;
        c->d.list_literal.elems = ast_clone(n->d.list_literal.elems);
        if (n->d.list_literal.elems && !c->d.list_literal.elems) { mem_free_node(c); return NULL; }
        return c;
    case NODE_LIST_REF:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type = NODE_LIST_REF;
        c->d.list_ref.idx = n->d.list_ref.idx;
        return c;
    case NODE_LIST_OP:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type = NODE_LIST_OP;
        c->d.list_op.fn = n->d.list_op.fn;
        c->d.list_op.arg = ast_clone(n->d.list_op.arg);
        if (n->d.list_op.arg && !c->d.list_op.arg) { mem_free_node(c); return NULL; }
        return c;
    case NODE_LIST_ELEM:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type = NODE_LIST_ELEM;
        c->d.list_elem.list_idx = n->d.list_elem.list_idx;
        c->d.list_elem.index = ast_clone(n->d.list_elem.index);
        if (n->d.list_elem.index && !c->d.list_elem.index) { mem_free_node(c); return NULL; }
        return c;
    case NODE_STORE:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type = NODE_STORE;
        c->d.store.expr = ast_clone(n->d.store.expr);
        c->d.store.list_idx = n->d.store.list_idx;
        if (n->d.store.expr && !c->d.store.expr) { mem_free_node(c); return NULL; }
        return c;
    case NODE_PIECEWISE:
        c = mem_alloc_node();
        if (!c) return NULL;
        c->type = NODE_PIECEWISE;
        c->d.piecewise.pieces = ast_clone(n->d.piecewise.pieces);
        if (n->d.piecewise.pieces && !c->d.piecewise.pieces) { mem_free_node(c); return NULL; }
        return c;
    default:
        return NULL;
    }
}

/* ── Variable presence check ─────────────────────────────── */

/* Walks the left operand of a binop, and the single child of a unop, func
 * or deriv node, in a loop: a 60-term chain such as x*2*2*...*2 parses
 * left-leaning, and recursing down it took ~30 B of CE stack per term.
 * Right operands still recurse; the parser bounds how deep
 * those nest. */
bool ast_contains_var(const ast_node_t *n, char var)
{
    for (;;) {
    if (!n) return false;
    switch (n->type) {
    case NODE_NUMBER:
    case NODE_CONSTANT:
        return false;
    case NODE_VARIABLE:
        return n->d.variable == var;
    case NODE_BINOP:
        if (ast_contains_var(n->d.binop.right, var)) return true;
        n = n->d.binop.left;
        continue;
    case NODE_UNOP:
        n = n->d.unop.operand;
        continue;
    case NODE_FUNC:
        n = n->d.func.arg;
        continue;
    case NODE_DERIV:
        n = n->d.deriv.expr;
        continue;
    case NODE_INTEGRAL:
        return ast_contains_var(n->d.integral.expr, var) ||
               ast_contains_var(n->d.integral.lo, var) ||
               ast_contains_var(n->d.integral.hi, var);
    case NODE_LIMIT:
        return ast_contains_var(n->d.limit.expr, var) ||
               ast_contains_var(n->d.limit.point, var);
    case NODE_SUM:
        return ast_contains_var(n->d.sum.expr, var) ||
               ast_contains_var(n->d.sum.lo, var) ||
               ast_contains_var(n->d.sum.hi, var);
    case NODE_PRODUCT:
        return ast_contains_var(n->d.product.expr, var) ||
               ast_contains_var(n->d.product.lo, var) ||
               ast_contains_var(n->d.product.hi, var);
    case NODE_TAYLOR:
        return ast_contains_var(n->d.taylor.expr, var) ||
               ast_contains_var(n->d.taylor.center, var) ||
               ast_contains_var(n->d.taylor.degree, var);
    case NODE_FOURIER:
        return ast_contains_var(n->d.fourier.expr, var) ||
               ast_contains_var(n->d.fourier.nterms, var);
    case NODE_EXPAND:
        return ast_contains_var(n->d.expand.expr, var);
    case NODE_FACTOR:
        return ast_contains_var(n->d.factor.expr, var);
    case NODE_SOLVE:
        return ast_contains_var(n->d.solve.expr, var);
    case NODE_TANGENT:
        return ast_contains_var(n->d.tangent.expr, var) ||
               ast_contains_var(n->d.tangent.point, var);
    case NODE_PAIR:
        return ast_contains_var(n->d.pair.head, var) ||
               ast_contains_var(n->d.pair.tail, var);
    case NODE_STAT:
        return ast_contains_var(n->d.stat.args, var);
    case NODE_MATRIX:
        return false;
    case NODE_MATRIX_OP:
        return ast_contains_var(n->d.matrix_op.arg, var);
    case NODE_POLAR:
        return ast_contains_var(n->d.polar.expr1, var) ||
               ast_contains_var(n->d.polar.expr2, var) ||
               ast_contains_var(n->d.polar.lo, var) ||
               ast_contains_var(n->d.polar.hi, var);
    case NODE_LIST_LITERAL:
        return ast_contains_var(n->d.list_literal.elems, var);
    case NODE_LIST_REF:
        return false;
    case NODE_LIST_OP:
        return ast_contains_var(n->d.list_op.arg, var);
    case NODE_LIST_ELEM:
        return ast_contains_var(n->d.list_elem.index, var);
    case NODE_STORE:
        return ast_contains_var(n->d.store.expr, var);
    case NODE_PIECEWISE:
        return ast_contains_var(n->d.piecewise.pieces, var);
    default:
        return true;  /* conservative */
    }
    }
}

/* ── Pretty-printer ──────────────────────────────────────── */

/* Helpers — safe append to bounded buffer */
static int buf_remaining(int written, int size)
{
    return (written < size) ? size - written : 0;
}

/* Set when an append drops characters because the buffer is full.  The
 * display entry points then end the text in "..." so a cut result cannot
 * look complete (canonical text shows an unclosed '(' instead). */
static unsigned char s_ast_cut;

static int append_str(char *buf, int pos, int size, const char *s)
{
    int rem = buf_remaining(pos, size);
    int n   = (int)strlen(s);
    if (n > rem - 1) { n = rem - 1; s_ast_cut = 1; }
    if (n > 0) { memcpy(buf + pos, s, n); buf[pos + n] = '\0'; }
    return pos + (n > 0 ? n : 0);
}

/* noinline: ast_print and ast_print_punct call it from dozens of places, and
 * one shared copy is ~1 KB smaller per module than the inlined ones. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int append_char(char *buf, int pos, int size, char c)
{
    if (pos < size - 1) { buf[pos] = c; buf[pos + 1] = '\0'; return pos + 1; }
    s_ast_cut = 1;
    return pos;
}

static const char *func_name(func_id_t id)
{
    switch (id) {
    case FN_SIN:  return "sin";   case FN_COS:  return "cos";
    case FN_TAN:  return "tan";   case FN_SEC:  return "sec";
    case FN_CSC:  return "csc";   case FN_COT:  return "cot";
    case FN_ASIN: return "asin";  case FN_ACOS: return "acos";
    case FN_ATAN: return "atan";
    case FN_ASEC: return "asec";  case FN_ACSC: return "acsc";
    case FN_ACOT: return "acot";
    case FN_LN:   return "ln";    case FN_LOG:  return "log";
    case FN_SQRT: return "sqrt";  case FN_ABS:  return "abs";
    case FN_EXP:  return "exp";
    case FN_SINH: return "sinh";  case FN_COSH: return "cosh";
    case FN_TANH: return "tanh";  case FN_ASINH: return "asinh";
    case FN_ACOSH: return "acosh"; case FN_ATANH: return "atanh";
    case FN_SECH: return "sech";  case FN_CSCH: return "csch";
    case FN_COTH: return "coth";
    case FN_FLOOR: return "floor"; case FN_CEIL: return "ceil";
    case FN_ROUND: return "round"; case FN_IPART: return "ipart";
    case FN_FPART: return "fpart";
    case FN_DEC:  return "dec";   case FN_HEX:  return "hex";
    case FN_BIN:  return "bin";   case FN_OCT:  return "oct";
    case FN_REAL: return "real";  case FN_IMAG: return "imag";
    case FN_CONJ: return "conj";  case FN_ANGLE: return "angle";
    case FN_SIGN: return "sign";  case FN_HEAVISIDE: return "heaviside";
    default:      return "?";
    }
}

static const char *matrix_op_name(matrix_op_t op)
{
    switch (op) {
    case MAT_DET:       return "det";
    case MAT_INV:       return "inv";
    case MAT_RREF:      return "rref";
    case MAT_TRANSPOSE: return "transpose";
    default:            return "mat?";
    }
}

static const char *binop_str(binop_t op)
{
    switch (op) {
    case OP_ADD: return "+"; case OP_SUB: return "-";
    case OP_MUL: return "*"; case OP_DIV: return "/";
    case OP_POW: return "^";
    default:     return "?";
    }
}

/* ── Display (minimal-parentheses) printing ──────────────────
 *
 * CE_AST_DISPLAY_PRINT=0 retains canonical output in CECISM, CEIPFR, and
 * CECLMB. These workers cannot fit the display printer under 51,200 B.
 *
 * ast_print has two modes, chosen by s_ast_disp for the duration of one
 * ast_to_str/ast_to_display_str call.  Canonical mode (NULL) is the
 * historical format.  Display mode (ast_disp_mask) writes only the
 * parentheses parser.c needs:
 *
 *   level      parser.c         nodes
 *   AP_LOW     (assign/store)   NODE_STORE, NODE_PAIR, unknown binop
 *   AP_ADD     PREC_ADD         + -
 *   AP_MUL     PREC_MUL         * /
 *   AP_UNARY   PREC_UNARY       unary minus, and a NUMBER printed with '-'
 *   AP_POW     PREC_POW         ^ (right-associative in parser.c)
 *   AP_POST    PREC_POST        !
 *   AP_ATOM    -                everything printed as name(...) or a literal
 *
 * Left operand: parenthesised when it binds less tightly than the operator
 * (and for ^ also when it is another ^, so (A^B)^C stays explicit).
 * Right operand: parenthesised when it binds less tightly or at equal
 * precedence. This preserves the operator tree, including A+(B+C) and
 * A*(B/C), rather than regrouping finite-precision arithmetic. Negative
 * right operands keep parentheses for readability. Unary minus keeps a
 * product operand wrapped: -(2X) must not become (-2)*X.
 * Nested powers stay explicit because parser.c associates ^ rightward,
 * unlike the TI-OS.
 *
 * No sign rewrites occur here. Parsed scalar expressions retain their AST
 * when re-parsed, subject to the historical six-significant-digit number
 * formatting. A negative NUMBER leaf re-parses as NEG(positive NUMBER),
 * since parser.c represents negative input with a unary node. No shipped
 * code consumes result strings as input. See docs/AST-DISPLAY-AUDIT.md.
 *
 * Implicit products: '*' is dropped only after a finite NUMBER literal, or a
 * unary minus on a non-negative one (its text ends in a digit), and only
 * when the right operand's text starts with a variable other than e/E, pi,
 * a function name not starting with 'e', or '('.  This avoids every
 * tokenizer trap: "2e+X" would scan as an exponent, a letter after a letter
 * forms one unknown word ("Xsin"), and digit-leading keywords (2varstats,
 * 1propztest, ...) cannot form from a digit run plus a single variable or a
 * function name.
 *
 * Stack: ast_print stores traversal continuations in static arrays. Each
 * punctuation helper returns before the next node, so tree depth does not
 * multiply the OS stack frame.
 *
 * Size: s_ast_disp is a pointer to ast_disp_mask, set only by the display
 * entry points.  It is volatile so LTO keeps the indirect call instead of
 * proving the pointer has just two values and calling ast_disp_mask
 * directly; that way a module with no display caller (CECSEQ) lets the
 * linker drop the whole display decision code (about 1 KB). */
#ifndef CE_AST_DISPLAY_PRINT
#define CE_AST_DISPLAY_PRINT 1
#endif

#if CE_AST_DISPLAY_PRINT
/* The display mode: NULL in canonical mode, else ast_disp_mask.  Only
 * ast_to_display_str()/ast_to_display_term() take its address. */
typedef unsigned char (*ast_disp_fn_t)(const ast_node_t *n);
static ast_disp_fn_t volatile s_ast_disp;
#endif


enum {
    AP_LOW = 0, AP_ADD = 2, AP_MUL = 3, AP_UNARY = 4,
    AP_POW = 5, AP_POST = 6, AP_ATOM = 7
};

#define AP_OUTER 0x01   /* canonical: wrap the whole binop */
#define AP_L     0x02   /* wrap the left operand */
#define AP_R     0x04   /* wrap the right operand */
#define AP_IMPL  0x08   /* omit the '*' */


#if CE_AST_DISPLAY_PRINT
static int ast_number_is_neg(ce_float_t v)
{
    ce_float_bytes_t u;
    u.f = (float)v;
    return (u.b[3] & 0x80u) != 0u;   /* includes -0, which prints "-0" */
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static unsigned char ast_disp_prec(const ast_node_t *n)
{
    if (!n) return AP_ATOM;
    switch (n->type) {
    case NODE_BINOP:
        switch (n->d.binop.op) {
        case OP_ADD: case OP_SUB: return AP_ADD;
        case OP_MUL: case OP_DIV: return AP_MUL;
        case OP_POW:              return AP_POW;
        default:                  return AP_LOW;
        }
    case NODE_UNOP:
        return n->d.unop.op == UOP_NEG ? AP_UNARY : AP_POST;
    case NODE_NUMBER:
        return ast_number_is_neg(n->d.number) ? AP_UNARY : AP_ATOM;
    case NODE_STORE:
    case NODE_PAIR:
        return AP_LOW;
    default:
        return AP_ATOM;
    }
}

/* How does the display text of n start?  Used for the right operand of a
 * binop, the operand of a unary minus, and ast_to_display_term(). */
enum { LEAD_OTHER = 0, LEAD_PAREN, LEAD_WORD, LEAD_MINUS };

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static unsigned char ast_disp_lead(const ast_node_t *n)
{
    while (n) {
        switch (n->type) {
        case NODE_VARIABLE: {
            char c = n->d.variable;
            return (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) &&
                    c != 'e' && c != 'E') ? LEAD_WORD : LEAD_OTHER;
        }
        case NODE_CONSTANT:
            return n->d.constant == CONST_PI ? LEAD_WORD : LEAD_OTHER;
        case NODE_FUNC:
            /* exp is the only function name beginning with e. */
            return n->d.func.id >= FN_SIN && n->d.func.id <= FN_HEAVISIDE &&
                   n->d.func.id != FN_EXP ? LEAD_WORD : LEAD_OTHER;
        case NODE_NUMBER:
            return ast_number_is_neg(n->d.number) ? LEAD_MINUS : LEAD_OTHER;
        case NODE_BINOP: {
            unsigned char p = ast_disp_prec(n), pl;
            if (p == AP_LOW) return LEAD_PAREN;               /* canonical */
            pl = ast_disp_prec(n->d.binop.left);
            if (pl < p || (p == AP_POW && pl == p)) return LEAD_PAREN;
            n = n->d.binop.left;
            break;
        }
        case NODE_UNOP:
            if (n->d.unop.op == UOP_NEG) return LEAD_MINUS;
            if (ast_disp_prec(n->d.unop.operand) < AP_POST) return LEAD_PAREN;
            n = n->d.unop.operand;
            break;
        default:
            return LEAD_OTHER;
        }
    }
    return LEAD_OTHER;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static unsigned char ast_binop_mask(const ast_node_t *n)
{
    unsigned char p, pl, pr, lr;
    unsigned char m = 0;
    binop_t op = n->d.binop.op;
    const ast_node_t *l = n->d.binop.left;

    p = ast_disp_prec(n);
    if (p == AP_LOW) return AP_OUTER;           /* unknown op: keep canonical */
    pl = ast_disp_prec(l);
    pr = ast_disp_prec(n->d.binop.right);
    lr = ast_disp_lead(n->d.binop.right);
    if (pl < p || (op == OP_POW && pl == p)) m |= AP_L;
    if (pr <= p || lr == LEAD_MINUS) m |= AP_R;
    /* -2 parsed from text is NEG(2), whose text still ends in a digit. */
    if (l && l->type == NODE_UNOP && l->d.unop.op == UOP_NEG &&
        l->d.unop.operand && l->d.unop.operand->type == NODE_NUMBER &&
        !ast_number_is_neg(l->d.unop.operand->d.number))
        l = l->d.unop.operand;
    /* A numeric factor has precedence above multiplication, so AP_L is clear. */
    if (op == OP_MUL && l && l->type == NODE_NUMBER &&
        ce_isfinite(l->d.number) &&
        ((m & AP_R) || lr == LEAD_WORD || lr == LEAD_PAREN))
        m |= AP_IMPL;
    return m;
}

/* Unary minus / factorial operand: wrap it?  Canonical mode never wraps a
 * factorial operand and always writes "(-x)" around a negation. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int ast_unop_wrap(const ast_node_t *n)
{
    const ast_node_t *o = n->d.unop.operand;
    if (n->d.unop.op == UOP_NEG)                /* -(X+1), -(2X), -(-3) */
        return ast_disp_prec(o) <= AP_MUL || ast_disp_lead(o) == LEAD_MINUS;
    return ast_disp_prec(o) < AP_POST;          /* (X+1)!, (X^2)! */
}

/* Display-mode punctuation mask (s_ast_disp points here).  A binop gets
 * ast_binop_mask(); a unop gets AP_L when its operand is wrapped, else 0.
 * Canonical mode uses AP_OUTER for both, which never occurs here. */
static unsigned char ast_disp_mask(const ast_node_t *n)
{
    if (n->type == NODE_UNOP) return ast_unop_wrap(n) ? AP_L : 0;
    return ast_binop_mask(n);
}

#endif /* CE_AST_DISPLAY_PRINT */

/* Punctuation around a binop (phase 0: before the left operand, 1: between
 * the operands, 2: after the right one) or a unop (phase 0: before the
 * operand, 2: after it). Keep punctuation independent of traversal state.
 * Recompute the decision for each phase. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int ast_print_punct(const ast_node_t *n, unsigned char phase,
                           char *buf, int pos, int size)
{
    /* Canonical mask: AP_OUTER for every binop and unop.  Display mask:
     * ast_disp_mask().  Text written per phase:
     *
     *   binop  0: "(" if AP_OUTER or AP_L
     *          1: ")" if AP_L, the operator unless AP_IMPL, "(" if AP_R
     *          2: ")" if AP_OUTER or AP_R
     *   neg    0: "(" if AP_OUTER, "-", "(" if AP_L     ("(-", "-(", "-")
     *          2: ")" if AP_OUTER or AP_L
     *   fact   0: "(" if AP_L
     *          2: ")" if AP_L, "!"                       canonical never wraps
     *
     * A numeric left factor (AP_IMPL) never needs AP_L.  The text is
     * collected first and appended once, so a cut leaves exactly what one
     * append_str() of the same string would. */
    char t[4];
    char *p = t;
    unsigned char un = n->type == NODE_UNOP;
    unsigned char neg = un && n->d.unop.op == UOP_NEG;
    unsigned char outer;
#if CE_AST_DISPLAY_PRINT
    unsigned char m = s_ast_disp ? s_ast_disp(n) : AP_OUTER;
#else
    const unsigned char m = AP_OUTER;
#endif

    outer = (m & AP_OUTER) && (!un || neg);
    if (phase == 0) {
        if (outer) *p++ = '(';
        if (neg) *p++ = '-';
        if (m & AP_L) *p++ = '(';
    } else if (phase == 1) {
        if (m & AP_L) *p++ = ')';
        if (!(m & AP_IMPL)) *p++ = binop_str(n->d.binop.op)[0];
        if (m & AP_R) *p++ = '(';
    } else {
        if (outer || (m & (un ? AP_L : AP_R))) *p++ = ')';
        if (un && !neg) *p++ = '!';
    }
    *p = '\0';
    return append_str(buf, pos, size, t);
}

/* Keep the 24 B number buffer separate from traversal state. ast_print_dims
 * uses the same out-of-line pattern for its dimension buffer. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int ast_print_number(ce_float_t value, char *buf, int pos, int size)
{
    char tmp[24];

    /* Format: strip trailing zeroes */
    sprintf(tmp, "%.6g", (double)value);
    return append_str(buf, pos, size, tmp);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int ast_print_dims(int rows, int cols, char *buf, int pos, int size)
{
    char tmp[24];

    sprintf(tmp, "%dx%d", rows, cols);
    return append_str(buf, pos, size, tmp);
}

/* ── Call-style nodes ──────────────────────────────────────
 *
 * Every node printed as name(child0, ...) goes through one path in
 * ast_print: punctuation phase 0, child 0, phase 1, child 1, phase 2,
 * child 2, phase 3, child 3, phase 4.  An absent child is NULL, which
 * ast_print skips.  The text is the historical one:
 *
 *   func/stat/matrix_op/list_op/list_elem   name(arg)
 *   expand/factor                           name(expr)
 *   deriv                                   d/dV(expr)
 *   list literal                            {elems}
 *   solve                                   solve(expr,V)
 *   fourier/tangent                         name(expr,V,a)
 *   lim                                     lim(expr,V,a[,+|-])
 *   taylor                                  taylor(expr,V,a,b)
 *   int/sum/prod                            name(expr,V[,a,b])     a,b when a
 *   polar                                   name(e1[,e2][,V][,lo,hi])
 *
 * The non-polar ones with a variable share the layout
 * {char var; ast_node_t *expr; ast_node_t *a; ast_node_t *b} (a common
 * initial sequence of the union members, checked below) and are read
 * through d.integral.  Keeping the per-type decisions in these
 * helpers keeps type-specific state out of the traversal and replaces
 * 18 nearly identical case bodies. */
typedef char ast_call_layout_check[
    (offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.deriv.var)   &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.deriv.expr)  &&
     offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.limit.var)   &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.limit.expr)  &&
     offsetof(ast_node_t, d.integral.lo)   == offsetof(ast_node_t, d.limit.point) &&
     offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.sum.var)     &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.sum.expr)    &&
     offsetof(ast_node_t, d.integral.lo)   == offsetof(ast_node_t, d.sum.lo)      &&
     offsetof(ast_node_t, d.integral.hi)   == offsetof(ast_node_t, d.sum.hi)      &&
     offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.taylor.var)  &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.taylor.expr) &&
     offsetof(ast_node_t, d.integral.lo)   == offsetof(ast_node_t, d.taylor.center) &&
     offsetof(ast_node_t, d.integral.hi)   == offsetof(ast_node_t, d.taylor.degree) &&
     offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.fourier.var) &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.fourier.expr) &&
     offsetof(ast_node_t, d.integral.lo)   == offsetof(ast_node_t, d.fourier.nterms) &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.expand.expr) &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.factor.expr) &&
     offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.solve.var)   &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.solve.expr)  &&
     offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.tangent.var) &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.tangent.expr) &&
     offsetof(ast_node_t, d.integral.lo)   == offsetof(ast_node_t, d.tangent.point) &&
     offsetof(ast_node_t, d.integral.var)  == offsetof(ast_node_t, d.product.var) &&
     offsetof(ast_node_t, d.integral.expr) == offsetof(ast_node_t, d.product.expr) &&
     offsetof(ast_node_t, d.integral.lo)   == offsetof(ast_node_t, d.product.lo)  &&
     offsetof(ast_node_t, d.integral.hi)   == offsetof(ast_node_t, d.product.hi)) ? 1 : -1];

#define AC_VAR  0x01   /* ",V" after child 0 */
#define AC_A    0x02   /* ",a" always (a may print as nothing) */
#define AC_OPT  0x04   /* ",a,b" only when a is present */
#define AC_B    0x08   /* ",b" always */

/* Indexed by node type; 0 for nodes with no variable or extra children
 * (and for nodes ast_print handles itself). */
static const unsigned char s_ast_call_shape[NODE_PRODUCT + 1] = {
    [NODE_INTEGRAL] = AC_VAR | AC_OPT,
    [NODE_LIMIT]    = AC_VAR | AC_A,
    [NODE_SUM]      = AC_VAR | AC_OPT,
    [NODE_TAYLOR]   = AC_VAR | AC_A | AC_B,
    [NODE_FOURIER]  = AC_VAR | AC_A,
    [NODE_SOLVE]    = AC_VAR,
    [NODE_TANGENT]  = AC_VAR | AC_A,
    [NODE_POLAR]    = AC_OPT,          /* its variable is optional too */
    [NODE_PRODUCT]  = AC_VAR | AC_OPT,
};

static const char *ast_polar_name(polar_op_t op)
{
    switch (op) {
    case POLOP_TO_RECT:      return "polar";
    case POLOP_FROM_RECT:    return "rect";
    case POLOP_ARCLEN:       return "arclen";
    case POLOP_ARCLEN_PARAM: return "arclen";
    case POLOP_SURFREV:      return "surfarea";
    case POLOP_DISK:         return "disk";
    case POLOP_WASHER:       return "washer";
    case POLOP_SHELL:        return "shell";
    case POLOP_GRAPH:        return "graph";
    case POLOP_GRAPHEQ:      return "grapheq";
    case POLOP_ODE_SEP:      return "ode_sep";
    case POLOP_ODE_LINEAR:   return "ode_linear";
    case POLOP_DIFFEQ:       return "diffeq";
    case POLOP_EULER:        return "euler";
    case POLOP_SLOPEFIELD:   return "slopefield";
    case POLOP_INTPARTS:     return "intparts";
    case POLOP_TRIGSUB:      return "trigsub";
    case POLOP_USUB:         return "usub";
    case POLOP_INTSTRATEGY:  return "intstrategy";
    case POLOP_AREABETWEEN:  return "areabetween";
    case POLOP_IMPROPER:     return "improper";
    /* ODE solver */
    case POLOP_DSOLVE:       return "dsolve";
    /* Polar calculus */
    case POLOP_POLARAREA:    return "polararea";
    case POLOP_POLARARCLEN:  return "polararclen";
    /* Vector calculus */
    case POLOP_VEC_DOT:      return "dot";
    case POLOP_VEC_CROSS:    return "cross";
    case POLOP_VEC_GRAD:     return "grad";
    case POLOP_VEC_DIV:      return "div";
    case POLOP_VEC_CURL:     return "curl";
    /* Laplace transforms */
    case POLOP_LAPLACE:      return "laplace";
    case POLOP_ILAPLACE:     return "ilaplace";
    /* Unit conversion */
    case POLOP_CONVERT:      return "convert";
    /* Second-order ODE */
    case POLOP_DSOLVE2:      return "dsolve2";
    /* Eigenvalues / eigenvectors */
    case POLOP_EIGENVAL:     return "eigenval";
    case POLOP_EIGENVEC:     return "eigenvec";
    /* Implicit graphing */
    case POLOP_GRAPHIMPL:    return "graphimpl";
    default:                 return "polar?";
    }
}

/* Child k (0..3) of a call-style node, or NULL when it is not printed. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static const ast_node_t *ast_call_child(const ast_node_t *n, unsigned char k)
{
    node_type_t t = n->type;
    unsigned char shape = s_ast_call_shape[t];
    const ast_node_t *a;

    switch (k) {
    case 0:
        switch (t) {
        case NODE_FUNC:         return n->d.func.arg;
        case NODE_STAT:         return n->d.stat.args;
        case NODE_MATRIX_OP:    return n->d.matrix_op.arg;
        case NODE_POLAR:        return n->d.polar.expr1;
        case NODE_LIST_LITERAL: return n->d.list_literal.elems;
        case NODE_LIST_OP:      return n->d.list_op.arg;
        case NODE_LIST_ELEM:    return n->d.list_elem.index;
        default:                return n->d.integral.expr;
        }
    case 1:
        return t == NODE_POLAR ? n->d.polar.expr2 : NULL;
    default:
        if (!(shape & (AC_A | AC_OPT))) return NULL;
        a = t == NODE_POLAR ? n->d.polar.lo : n->d.integral.lo;
        if (k == 2) return a;
        /* b: always for AC_B (taylor), with a for AC_OPT, never for AC_A. */
        if (!(shape & AC_B) && !((shape & AC_OPT) && a)) return NULL;
        return t == NODE_POLAR ? n->d.polar.hi : n->d.integral.hi;
    }
}

/* Name of a call-style node; tmp (>= 3 B) holds an L1..L6 name. */
static const char *ast_call_name(const ast_node_t *n, char *tmp)
{
    static const char *const list_fn_names[] = {
        "sum","prod","min","max","mean","median","stdev",
        "dim","cumsum","deltalist","sorta","sortd","seq"
    };
    switch (n->type) {
    case NODE_FUNC:      return func_name(n->d.func.id);
    case NODE_DERIV:     return "d/d";
    case NODE_INTEGRAL:  return "int";
    case NODE_LIMIT:     return "lim";
    case NODE_SUM:       return "sum";
    case NODE_TAYLOR:    return "taylor";
    case NODE_FOURIER:   return "fourier";
    case NODE_EXPAND:    return "expand";
    case NODE_FACTOR:    return "factor";
    case NODE_SOLVE:     return "solve";
    case NODE_TANGENT:   return "tangent";
    case NODE_PRODUCT:   return "prod";
    case NODE_STAT:
#if CE_AST_ENABLE_STAT_NAMES
        return stat_fn_name(n->d.stat.fn);
#else
        return "stat";
#endif
    case NODE_MATRIX_OP: return matrix_op_name(n->d.matrix_op.op);
    case NODE_POLAR:     return ast_polar_name(n->d.polar.op);
    case NODE_LIST_OP:
        return n->d.list_op.fn < 13 ? list_fn_names[n->d.list_op.fn] : "list?";
    default:             /* NODE_LIST_ELEM */
        tmp[0] = 'L'; tmp[1] = (char)('1' + n->d.list_elem.list_idx);
        tmp[2] = '\0';
        return tmp;
    }
}

/* Punctuation of a call-style node before child 0 (phase 0), after
 * child k-1 (phase k), and the closing text (phase 4). */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int ast_call_punct(const ast_node_t *n, unsigned char phase,
                          char *buf, int pos, int size)
{
    char t[4];
    char *p = t;
    node_type_t ty = n->type;
    unsigned char shape = s_ast_call_shape[ty];
    unsigned char polar = ty == NODE_POLAR;
    const ast_node_t *a = NULL;

    /* The optional ",a,b" depends on a; only nodes with that shape own it. */
    if (shape & AC_OPT) a = polar ? n->d.polar.lo : n->d.integral.lo;
    switch (phase) {
    case 0:
        if (ty == NODE_LIST_LITERAL) {
            *p++ = '{';
            break;
        }
        pos = append_str(buf, pos, size, ast_call_name(n, t));
        /* The variable goes through append_char, as it always did. */
        if (ty == NODE_DERIV) pos = append_char(buf, pos, size, n->d.deriv.var);
        *p++ = '(';
        break;
    case 1:
        if (polar && n->d.polar.expr2) *p++ = ',';
        break;
    case 2:
        if ((shape & AC_VAR) || (polar && n->d.polar.var)) {
            pos = append_char(buf, pos, size, ',');
            pos = append_char(buf, pos, size,
                              polar ? n->d.polar.var : n->d.integral.var);
        }
        if ((shape & AC_A) || a) *p++ = ',';
        break;
    case 3:
        if ((shape & AC_B) || a) *p++ = ',';
        break;
    default:
        if (ty == NODE_LIMIT && n->d.limit.direction != 0) {
            *p++ = ',';
            *p++ = n->d.limit.direction > 0 ? '+' : '-';
        }
        *p++ = ty == NODE_LIST_LITERAL ? '}' : ')';
        break;
    }
    *p = '\0';
    return append_str(buf, pos, size, t);
}

/* A module has one active printer, like s_ast_disp and s_ast_cut. Keep
 * continuations in static storage instead of multiplying the OS stack frame.
 * A valid tree has at most NODE_POOL_SIZE nodes, including its root. */
static const ast_node_t *s_ast_print_nodes[NODE_POOL_SIZE];
static unsigned char s_ast_print_phase[NODE_POOL_SIZE];

static int ast_print(const ast_node_t *n, char *buf, int pos, int size)
{
    unsigned depth = 0;
    unsigned char phase;
    const ast_node_t *child;
    s_ast_print_nodes[0] = n;
    s_ast_print_phase[0] = 0;
    for (;;) {
        n = s_ast_print_nodes[depth];
        phase = s_ast_print_phase[depth];
        child = NULL;
        if (!n) goto finished;
        /* Piecewise continuations hold the current pair, not the root. */
        if (phase >= 6) {
            if (phase == 6 && n->type == NODE_PAIR) {
                child = n->d.pair.head;
                s_ast_print_phase[depth] = 7;
                goto descend;
            }
            if (phase == 7) {
                n = n->d.pair.tail;
                if (n) pos = append_str(buf, pos, size, ", ");
            }
            if (n && n->type == NODE_PAIR) {
                s_ast_print_nodes[depth] = n;
                s_ast_print_phase[depth] = 6;
                continue;
            }
            pos = append_char(buf, pos, size, ')');
            goto finished;
        }
        switch (n->type) {
        case NODE_NUMBER:
            pos = ast_print_number(n->d.number, buf, pos, size);
            break;
        case NODE_VARIABLE:
            pos = append_char(buf, pos, size, n->d.variable);
            break;
        case NODE_CONSTANT:
            pos = append_str(buf, pos, size,
                             n->d.constant == CONST_PI ? "pi" :
                             n->d.constant == CONST_E ? "e" :
                             n->d.constant == CONST_I ? "i" : "inf");
            break;
        case NODE_BINOP:
            pos = ast_print_punct(n, phase, buf, pos, size);
            if (phase == 2) break;
            child = phase ? n->d.binop.right : n->d.binop.left;
            s_ast_print_phase[depth] = phase + 1;
            goto descend;
        case NODE_UNOP:
            pos = ast_print_punct(n, phase ? 2 : 0, buf, pos, size);
            if (phase) break;
            child = n->d.unop.operand;
            s_ast_print_phase[depth] = 1;
            goto descend;
        case NODE_FUNC: case NODE_DERIV: case NODE_INTEGRAL:
        case NODE_LIMIT: case NODE_SUM: case NODE_TAYLOR:
        case NODE_FOURIER: case NODE_EXPAND: case NODE_FACTOR:
        case NODE_SOLVE: case NODE_STAT: case NODE_TANGENT:
        case NODE_MATRIX_OP: case NODE_POLAR: case NODE_LIST_LITERAL:
        case NODE_LIST_OP: case NODE_LIST_ELEM: case NODE_PRODUCT:
            pos = ast_call_punct(n, phase, buf, pos, size);
            if (phase == 4) break;
            child = ast_call_child(n, phase);
            s_ast_print_phase[depth] = phase + 1;
            goto descend;
        case NODE_PAIR:
            if (!phase) {
                child = n->d.pair.head;
                s_ast_print_phase[depth] = 1;
                goto descend;
            }
            n = n->d.pair.tail;
            if (n) pos = append_char(buf, pos, size, ',');
            if (n && n->type == NODE_PAIR) {
                s_ast_print_nodes[depth] = n;
                s_ast_print_phase[depth] = 0;
                continue;
            }
            break;
        case NODE_MATRIX:
            pos = append_str(buf, pos, size, "mat[");
            pos = ast_print_dims(n->d.matrix.rows, n->d.matrix.cols, buf, pos, size);
            pos = append_char(buf, pos, size, ']');
            break;
        case NODE_LIST_REF: {
            char lname[3];
            lname[0] = 'L';
            lname[1] = (char)('1' + n->d.list_ref.idx);
            lname[2] = '\0';
            pos = append_str(buf, pos, size, lname);
            break;
        }
        case NODE_STORE:
            if (!phase) {
                child = n->d.store.expr;
                s_ast_print_phase[depth] = 1;
                goto descend;
            }
            pos = append_str(buf, pos, size, "->");
#if CE_EVAL_USER_VARS || defined(DESKTOP_BUILD)
            /* A variable store (`5->X`) keeps its letter in list_idx - 128
             * (parser.c). Only modules that store variables print it. */
            if (n->d.store.list_idx >= 128) {
                pos = append_char(buf, pos, size, (char)(n->d.store.list_idx - 128));
                break;
            }
#endif
            {
                char lname[3];
                lname[0] = 'L';
                lname[1] = (char)('1' + n->d.store.list_idx);
                lname[2] = '\0';
                pos = append_str(buf, pos, size, lname);
            }
            break;
        case NODE_PIECEWISE:
            pos = append_str(buf, pos, size, "piecewise(");
            n = n->d.piecewise.pieces;
            if (n && n->type == NODE_PAIR) {
                s_ast_print_nodes[depth] = n;
                s_ast_print_phase[depth] = 6;
                continue;
            }
            pos = append_char(buf, pos, size, ')');
            break;
        default:
            pos = append_str(buf, pos, size, "<?>");
            break;
        }
finished:
        if (!depth) return pos;
        --depth;
        continue;
descend:
        if (!child) continue;
        if (depth + 1 >= NODE_POOL_SIZE) {
            /* Invalid over-pool trees cannot overrun the continuation arrays. */
            return append_str(buf, pos, size, "...");
        }
        ++depth;
        s_ast_print_nodes[depth] = child;
        s_ast_print_phase[depth] = 0;
    }
}

int ast_to_str(const ast_node_t *node, char *buf, int size)
{
    if (size <= 0) return 0;
    buf[0] = '\0';
#if CE_AST_DISPLAY_PRINT
    s_ast_disp = NULL;
#endif
    s_ast_cut = 0;
    return ast_print(node, buf, 0, size);
}

/* A cut display string ends in "..." (when there is room for it). */
static int ast_mark_cut(char *buf, int pos, int size)
{
    if (s_ast_cut && size >= 4 && pos >= 3) {
        buf[pos - 3] = '.'; buf[pos - 2] = '.'; buf[pos - 1] = '.';
        buf[pos] = '\0';
    }
    return pos;
}

#if CE_AST_DISPLAY_PRINT
int ast_to_display_str(const ast_node_t *node, char *buf, int size)
{
    int pos;
    if (size <= 0) return 0;
    buf[0] = '\0';
    s_ast_disp = ast_disp_mask;
    s_ast_cut = 0;
    pos = ast_print(node, buf, 0, size);
    s_ast_disp = NULL;
    return ast_mark_cut(buf, pos, size);
}

int ast_to_display_term(const ast_node_t *node, char *buf, int size)
{
    int pos = 0, wrap;
    if (size <= 0) return 0;
    buf[0] = '\0';
    s_ast_disp = ast_disp_mask;
    s_ast_cut = 0;
    wrap = ast_disp_prec(node) < AP_MUL || ast_disp_lead(node) == LEAD_MINUS;
    if (wrap) pos = append_char(buf, pos, size, '(');
    pos = ast_print(node, buf, pos, size);
    if (wrap) pos = append_char(buf, pos, size, ')');
    s_ast_disp = NULL;
    return ast_mark_cut(buf, pos, size);
}
#else
/* Canonical text wraps every binop and negation, so it only needs the cut
 * marker, and parentheses around a leading '-' for the term form. */
int ast_to_display_str(const ast_node_t *node, char *buf, int size)
{
    int pos;
    if (size <= 0) return 0;
    s_ast_cut = 0;
    pos = ast_to_str(node, buf, size);
    return ast_mark_cut(buf, pos, size);
}

int ast_to_display_term(const ast_node_t *node, char *buf, int size)
{
    int pos = ast_to_display_str(node, buf, size);
    if (pos > 0 && buf[0] == '-' && pos + 2 < size) {
        memmove(buf + 1, buf, (size_t)pos + 1);
        buf[0] = '(';
        buf[pos + 1] = ')';
        buf[pos + 2] = '\0';
        pos += 2;
    }
    return pos;
}
#endif

/* ── Structural equality ─────────────────────────────────── */

/* Exact structural equality: same node types, operators, function ids,
 * variables and constants, and bit-equal numbers (no tolerance, unlike
 * ast_equal). Node types it does not know compare unequal, so callers that
 * act only on "equal" stay on the safe side. Used to tell an integral whose
 * bounds were typed alike (int(f,x,2,2), always 0) from bounds that differ
 * but round to the same float32 (int(f,x,1,1+1/100000000), PR #99). */
bool ast_same_exact(const ast_node_t *a, const ast_node_t *b)
{
    while (a && b && a->type == b->type) {
        switch (a->type) {
        case NODE_NUMBER:   return a->d.number == b->d.number;
        case NODE_VARIABLE: return a->d.variable == b->d.variable;
        case NODE_CONSTANT: return a->d.constant == b->d.constant;
        case NODE_UNOP:
            if (a->d.unop.op != b->d.unop.op) return false;
            a = a->d.unop.operand; b = b->d.unop.operand;
            break;
        case NODE_FUNC:
            if (a->d.func.id != b->d.func.id) return false;
            a = a->d.func.arg; b = b->d.func.arg;
            break;
        case NODE_BINOP:
            if (a->d.binop.op != b->d.binop.op ||
                !ast_same_exact(a->d.binop.left, b->d.binop.left)) return false;
            a = a->d.binop.right; b = b->d.binop.right;
            break;
        default:
            return false;
        }
    }
    return false;
}

/* Two integral bounds whose equal float32 values mean a true zero width:
 * identical trees, or two leaves (number, variable, constant). A leaf's value
 * is one float32 with no rounding inside an expression, so equal values are
 * equal bounds; any difference left is literal rounding (1.00000001 vs 1).
 * Nested integrals rely on the leaf case: int(int(1,t,0,x),x,0,1) samples
 * x = 0, where the inner bounds 0 and x are equal (review round 22). */
bool ast_same_bound(const ast_node_t *a, const ast_node_t *b)
{
    if (ast_same_exact(a, b)) return true;
    return a && b &&
           (a->type == NODE_NUMBER || a->type == NODE_VARIABLE || a->type == NODE_CONSTANT) &&
           (b->type == NODE_NUMBER || b->type == NODE_VARIABLE || b->type == NODE_CONSTANT);
}

/* Structural equality (tolerance-based for numbers). */
bool ast_equal(const ast_node_t *a, const ast_node_t *b)
{
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;
    switch (a->type) {
    case NODE_NUMBER:
        return fabsf(a->d.number - b->d.number) < 1e-10f;
    case NODE_VARIABLE: return a->d.variable == b->d.variable;
    case NODE_CONSTANT: return a->d.constant == b->d.constant;
    case NODE_BINOP:
        return a->d.binop.op == b->d.binop.op &&
               ast_equal(a->d.binop.left,  b->d.binop.left) &&
               ast_equal(a->d.binop.right, b->d.binop.right);
    case NODE_UNOP:
        return a->d.unop.op == b->d.unop.op &&
               ast_equal(a->d.unop.operand, b->d.unop.operand);
    case NODE_FUNC:
        return a->d.func.id == b->d.func.id &&
               ast_equal(a->d.func.arg, b->d.func.arg);
    case NODE_DERIV:
        return a->d.deriv.var == b->d.deriv.var &&
               ast_equal(a->d.deriv.expr, b->d.deriv.expr);
    case NODE_INTEGRAL:
        return a->d.integral.var == b->d.integral.var &&
               ast_equal(a->d.integral.expr, b->d.integral.expr) &&
               ast_equal(a->d.integral.lo,   b->d.integral.lo) &&
               ast_equal(a->d.integral.hi,   b->d.integral.hi);
    case NODE_LIMIT:
        return a->d.limit.var == b->d.limit.var &&
               a->d.limit.direction == b->d.limit.direction &&
               ast_equal(a->d.limit.expr,  b->d.limit.expr) &&
               ast_equal(a->d.limit.point, b->d.limit.point);
    case NODE_SUM:
        return a->d.sum.var == b->d.sum.var &&
               ast_equal(a->d.sum.expr, b->d.sum.expr) &&
               ast_equal(a->d.sum.lo,   b->d.sum.lo) &&
               ast_equal(a->d.sum.hi,   b->d.sum.hi);
    case NODE_PRODUCT:
        return a->d.product.var == b->d.product.var &&
               ast_equal(a->d.product.expr, b->d.product.expr) &&
               ast_equal(a->d.product.lo,   b->d.product.lo) &&
               ast_equal(a->d.product.hi,   b->d.product.hi);
    case NODE_TAYLOR:
        return a->d.taylor.var == b->d.taylor.var &&
               ast_equal(a->d.taylor.expr,   b->d.taylor.expr) &&
               ast_equal(a->d.taylor.center, b->d.taylor.center) &&
               ast_equal(a->d.taylor.degree, b->d.taylor.degree);
    case NODE_FOURIER:
        return a->d.fourier.var == b->d.fourier.var &&
               ast_equal(a->d.fourier.expr,   b->d.fourier.expr) &&
               ast_equal(a->d.fourier.nterms, b->d.fourier.nterms);
    case NODE_EXPAND:
        return a->d.expand.var == b->d.expand.var &&
               ast_equal(a->d.expand.expr, b->d.expand.expr);
    case NODE_FACTOR:
        return a->d.factor.var == b->d.factor.var &&
               ast_equal(a->d.factor.expr, b->d.factor.expr);
    case NODE_SOLVE:
        return a->d.solve.var == b->d.solve.var &&
               ast_equal(a->d.solve.expr, b->d.solve.expr);
    case NODE_TANGENT:
        return a->d.tangent.var == b->d.tangent.var &&
               ast_equal(a->d.tangent.expr,  b->d.tangent.expr) &&
               ast_equal(a->d.tangent.point, b->d.tangent.point);
    case NODE_PAIR:
        return ast_equal(a->d.pair.head, b->d.pair.head) &&
               ast_equal(a->d.pair.tail, b->d.pair.tail);
    case NODE_STAT:
        return a->d.stat.fn == b->d.stat.fn &&
               ast_equal(a->d.stat.args, b->d.stat.args);
    case NODE_MATRIX:
        return a->d.matrix.store_idx == b->d.matrix.store_idx &&
               a->d.matrix.rows == b->d.matrix.rows &&
               a->d.matrix.cols == b->d.matrix.cols;
    case NODE_MATRIX_OP:
        return a->d.matrix_op.op == b->d.matrix_op.op &&
               a->d.matrix_op.rows == b->d.matrix_op.rows &&
               a->d.matrix_op.cols == b->d.matrix_op.cols &&
               ast_equal(a->d.matrix_op.arg, b->d.matrix_op.arg);
    case NODE_POLAR:
        return a->d.polar.op == b->d.polar.op &&
               a->d.polar.var == b->d.polar.var &&
               ast_equal(a->d.polar.expr1, b->d.polar.expr1) &&
               ast_equal(a->d.polar.expr2, b->d.polar.expr2) &&
               ast_equal(a->d.polar.lo,    b->d.polar.lo) &&
               ast_equal(a->d.polar.hi,    b->d.polar.hi);
    case NODE_LIST_LITERAL:
        return ast_equal(a->d.list_literal.elems, b->d.list_literal.elems);
    case NODE_LIST_REF:
        return a->d.list_ref.idx == b->d.list_ref.idx;
    case NODE_LIST_OP:
        return a->d.list_op.fn == b->d.list_op.fn &&
               ast_equal(a->d.list_op.arg, b->d.list_op.arg);
    case NODE_LIST_ELEM:
        return a->d.list_elem.list_idx == b->d.list_elem.list_idx &&
               ast_equal(a->d.list_elem.index, b->d.list_elem.index);
    case NODE_STORE:
        return a->d.store.list_idx == b->d.store.list_idx &&
               ast_equal(a->d.store.expr, b->d.store.expr);
    case NODE_PIECEWISE:
        return ast_equal(a->d.piecewise.pieces, b->d.piecewise.pieces);
    default:
        return false;
    }
}

/* ── Pool-exhaustion check ───────────────────────────────── */

/* ast_binop/ast_unop/ast_func keep a NULL operand when the 512-node pool
 * runs out part-way through building a tree.  Such a tree prints as "cos()"
 * or "(0+)", and simplify/fold rules dereference operands, so derivative and
 * integral results are checked with this before they are simplified or
 * shown.  Returns false if any operator node is missing an operand (and for
 * a NULL tree). */
bool ast_tree_complete(const ast_node_t *n)
{
    while (n) {
        switch (n->type) {
        case NODE_BINOP:
            if (!n->d.binop.left || !n->d.binop.right) return false;
            if (!ast_tree_complete(n->d.binop.left)) return false;
            n = n->d.binop.right;
            break;
        case NODE_UNOP:
            n = n->d.unop.operand;
            break;
        case NODE_FUNC:
            n = n->d.func.arg;
            break;
        default:
            return true;
        }
    }
    return false;
}

/* ── Check if node is a number with a specific value ─────── */

bool ast_is_num(const ast_node_t *n, ce_float_t v)
{
    return n && n->type == NODE_NUMBER &&
           fabsf(n->d.number - v) < 1e-10f;
}
