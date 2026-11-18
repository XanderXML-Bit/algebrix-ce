/*
 * parser.c — Pratt (operator-precedence) recursive-descent parser
 *
 * Converts a token stream into an AST.  Handles:
 *   - Standard arithmetic (+, -, *, /, ^)
 *   - Unary minus and factorial
 *   - Function calls: sin(x), ln(x+1), …
 *   - Implicit multiplication: 2x, 3sin(x), (a)(b)
 *   - Parenthesised sub-expressions
 *   - Constants: pi, e
 *   - Calculus stubs: d/dx(expr)
 */
#include "parser.h"
#include "memory.h"
#include "list.h"
#include <string.h>
#ifndef CE_LAUNCHER_TINY_ERRORS
#include <stdio.h>
#endif
#include <stdlib.h>
#include <ctype.h>

/* ── Precedence levels ───────────────────────────────────── */
enum {
    PREC_NONE   = 0,
    PREC_ASSIGN = 1,   /* =                          */
    PREC_ADD    = 2,   /* + -                        */
    PREC_MUL    = 3,   /* * /   (and implicit mul)   */
    PREC_UNARY  = 4,   /* unary -                    */
    PREC_POW    = 5,   /* ^  (right-associative)     */
    PREC_POST   = 6,   /* !                          */
    PREC_ATOM   = 7
};

/* ── Helpers ─────────────────────────────────────────────── */

static const token_t *cur(const parser_t *p)
{
    return tok_get(p->tokens, p->pos);
}

static token_type_t cur_type(const parser_t *p)
{
    const token_t *t = cur(p);
    return t ? t->type : TOK_EOF;
}

static void advance(parser_t *p) { p->pos++; }

static bool at_end(const parser_t *p)
{
    return cur_type(p) == TOK_EOF;
}

static bool match(parser_t *p, token_type_t type)
{
    if (cur_type(p) == type) { advance(p); return true; }
    return false;
}

static bool expect(parser_t *p, token_type_t type)
{
    if (match(p, type)) return true;
    p->error = CE_ERR_SYNTAX;
#ifdef CE_LAUNCHER_TINY_ERRORS
    (void)type;
    strncpy(p->error_msg, "Expected token", sizeof(p->error_msg) - 1);
    p->error_msg[sizeof(p->error_msg) - 1] = '\0';
#else
    snprintf(p->error_msg, sizeof(p->error_msg),
             "Expected '%s'", tok_type_name(type));
#endif
    return false;
}

static void set_err(parser_t *p, const char *msg)
{
    p->error = CE_ERR_SYNTAX;
    strncpy(p->error_msg, msg, sizeof(p->error_msg) - 1);
    p->error_msg[sizeof(p->error_msg) - 1] = '\0';
}

/* ── Forward declarations ────────────────────────────────── */
static ast_node_t *parse_expr(parser_t *p, int min_prec);
static ast_node_t *parse_prefix(parser_t *p);

/* Variable for an operator typed without one, such as int(x^2), deriv(f),
 * lim(f) or sum(f) (internal review).  The CE keypad types upper-case letters,
 * so the fixed lower-case default made int(X^2) integrate X^2 as a
 * constant with respect to x: "((X^2)*x) + C".  Variable names are
 * case-sensitive everywhere else, so take the default letter in the case
 * the input uses: dflt, unless a and b contain no dflt but do contain dflt
 * in the other case.  An explicit ", var" argument still overrides it.
 *
 * Only the case is inferred, never a different letter, so an upper-case
 * input parses like its lower-case form does: the CE answer for int(X^2)
 * is the desktop answer for int(x^2), and a lower-case input parses as
 * before (review, PR #124: inferring "the only variable" also moved letters
 * bound by nested operators and changed lower-case answers).  The one
 * exception is a second default variable that would equal an explicit
 * first one (nlsolve, graphimpl): it stays y. */
static char default_var(const ast_node_t *a, const ast_node_t *b, char dflt)
{
    char other = (char)(dflt ^ 0x20);   /* dflt is always a letter */
    if (ast_contains_var(a, dflt) || ast_contains_var(b, dflt)) return dflt;
    if (ast_contains_var(a, other) || ast_contains_var(b, other)) return other;
    return dflt;
}

/* Parse optional differential suffix after an integral call.
 * Supports:
 *   int(expr)dx      -> TOK_DIFFVAR
 *   int(expr) d x    -> TOK_VARIABLE('d') TOK_VARIABLE('x')
 */
static bool parse_integral_diffvar(parser_t *p, char *out_var)
{
    const token_t *t = cur(p);
    if (!t || !out_var) return false;

    if (t->type == TOK_DIFFVAR) {
        *out_var = t->var_name;
        advance(p);
        return true;
    }

    if (t->type == TOK_VARIABLE &&
        tolower((unsigned char)t->var_name) == 'd') {
        const token_t *next = tok_get(p->tokens, p->pos + 1);
        if (next && next->type == TOK_VARIABLE) {
            *out_var = next->var_name;
            advance(p);
            advance(p);
            return true;
        }
    }

    return false;
}

/* ── Map token → function id ─────────────────────────────── */

static func_id_t tok_to_func(token_type_t t)
{
    switch (t) {
    case TOK_SIN:  return FN_SIN;   case TOK_COS:  return FN_COS;
    case TOK_TAN:  return FN_TAN;   case TOK_SEC:  return FN_SEC;
    case TOK_CSC:  return FN_CSC;   case TOK_COT:  return FN_COT;
    case TOK_ASIN: return FN_ASIN;
    case TOK_ACOS: return FN_ACOS;  case TOK_ATAN: return FN_ATAN;
    case TOK_ASEC: return FN_ASEC;  case TOK_ACSC: return FN_ACSC;
    case TOK_ACOT: return FN_ACOT;
    case TOK_LN:   return FN_LN;    case TOK_LOG:  return FN_LOG;
    case TOK_SQRT: return FN_SQRT;  case TOK_ABS:  return FN_ABS;
    case TOK_EXP:  return FN_EXP;
    case TOK_SINH: return FN_SINH;  case TOK_COSH:  return FN_COSH;
    case TOK_TANH: return FN_TANH;  case TOK_ASINH: return FN_ASINH;
    case TOK_ACOSH: return FN_ACOSH; case TOK_ATANH: return FN_ATANH;
    case TOK_SECH: return FN_SECH;  case TOK_CSCH: return FN_CSCH;
    case TOK_COTH: return FN_COTH;
    case TOK_FLOOR: return FN_FLOOR; case TOK_CEIL:  return FN_CEIL;
    case TOK_ROUND: return FN_ROUND; case TOK_IPART: return FN_IPART;
    case TOK_FPART: return FN_FPART;
    case TOK_SIGN: return FN_SIGN;   case TOK_HEAVISIDE: return FN_HEAVISIDE;
    case TOK_DEC:  return FN_DEC;   case TOK_HEX:  return FN_HEX;
    case TOK_BIN:  return FN_BIN;   case TOK_OCT:  return FN_OCT;
    case TOK_REAL: return FN_REAL;  case TOK_IMAG: return FN_IMAG;
    case TOK_CONJ: return FN_CONJ;  case TOK_ANGLE: return FN_ANGLE;
    default:       return FN_SIN;   /* unreachable */
    }
}

/* ── Map token → binary-op tag ───────────────────────────── */

static binop_t tok_to_binop(token_type_t t)
{
    switch (t) {
    case TOK_PLUS:     return OP_ADD;
    case TOK_MINUS:    return OP_SUB;
    case TOK_MULTIPLY: return OP_MUL;
    case TOK_DIVIDE:   return OP_DIV;
    case TOK_POWER:    return OP_POW;
    default:           return OP_ADD;
    }
}

/* ── Infix precedence of a token ─────────────────────────── */

static int infix_prec(token_type_t t)
{
    switch (t) {
    case TOK_EQUALS:   return PREC_ASSIGN;
    case TOK_PLUS:
    case TOK_MINUS:    return PREC_ADD;
    case TOK_MULTIPLY:
    case TOK_DIVIDE:   return PREC_MUL;
    case TOK_POWER:    return PREC_POW;
    case TOK_FACTORIAL:return PREC_POST;
    case TOK_PERCENT:  return PREC_POST;
    default:           return PREC_NONE;
    }
}

/* ── Can the previous "value" implicitly multiply the next token? ── */

static bool starts_value(token_type_t t)
{
    return t == TOK_NUMBER   || t == TOK_VARIABLE ||
           t == TOK_PI       || t == TOK_E        ||
           t == TOK_INF      || t == TOK_CPLX_I   ||
           t == TOK_LPAREN   || t == TOK_LBRACE   ||
           t == TOK_L1 || t == TOK_L2 || t == TOK_L3 ||
           t == TOK_L4 || t == TOK_L5 || t == TOK_L6 ||
           tok_is_function(t) ||
           t == TOK_DERIV;
}

/* ── parse_prefix domain-grouped helpers ─────────────────── */

static ast_node_t *parse_basic_literal(parser_t *p)
{
    const token_t *t = cur(p);

    switch (t->type) {
    case TOK_NUMBER:
        advance(p);
        return ast_number(t->num_val);
    case TOK_VARIABLE:
        advance(p);
        return ast_variable(t->var_name);
    case TOK_PI:  advance(p); return ast_constant(CONST_PI);
    case TOK_E:   advance(p); return ast_constant(CONST_E);
    case TOK_INF: advance(p); return ast_constant(CONST_INF);
    case TOK_MINUS: {
        ast_node_t *operand;
        advance(p);
        operand = parse_expr(p, PREC_UNARY);
        if (!operand) return NULL;
        return ast_unop(UOP_NEG, operand);
    }
    case TOK_LPAREN: {
        ast_node_t *inner;
        advance(p);
        inner = parse_expr(p, PREC_NONE);
        if (!inner) return NULL;
        if (!expect(p, TOK_RPAREN)) { ast_free(inner); return NULL; }
        return inner;
    }
    default: return NULL; /* unreachable */
    }
}

static ast_node_t *parse_single_arg_func(parser_t *p)
{
    const token_t *t = cur(p);
    func_id_t fn = tok_to_func(t->type);
    ast_node_t *arg;
    advance(p);
    if (!expect(p, TOK_LPAREN)) return NULL;
    arg = parse_expr(p, PREC_NONE);
    if (!arg) return NULL;
    if (!expect(p, TOK_RPAREN)) { ast_free(arg); return NULL; }
    return ast_func(fn, arg);
}

static ast_node_t *parse_calculus_op(parser_t *p)
{
    const token_t *t = cur(p);

    switch (t->type) {
    case TOK_DERIV: {
        char var = t->var_name;
        ast_node_t *expr, *node;
        advance(p);
        if (var == 0) {
            /* "deriv(expr, var)" keyword form */
            if (!expect(p, TOK_LPAREN)) return NULL;
            expr = parse_expr(p, PREC_NONE);
            if (!expr) return NULL;
            if (match(p, TOK_COMMA)) {
                const token_t *vt = cur(p);
                if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
            }
            if (var == 0) var = default_var(expr, NULL, 'x');
            if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        } else if (match(p, TOK_LPAREN)) {
            expr = parse_expr(p, PREC_NONE);
            if (!expr) return NULL;
            if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        } else {
            /* Natural shorthand: d/dx expr */
            expr = parse_expr(p, PREC_UNARY);
            if (!expr) {
                set_err(p, "Expected expression after derivative");
                return NULL;
            }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type         = NODE_DERIV;
        node->d.deriv.var  = var;
        node->d.deriv.expr = expr;
        return node;
    }
    case TOK_NDERIV: {
        ast_node_t *expr;
        char var = 'x';
        int order = 2;
        int i;
        ast_node_t *outermost, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        if (match(p, TOK_COMMA)) {
            ast_node_t *n_expr = parse_expr(p, PREC_NONE);
            if (n_expr && n_expr->type == NODE_NUMBER) {
                order = (int)n_expr->d.number;
                if (order < 1) order = 1;
                if (order > 10) order = 10;
            }
            if (n_expr) ast_free(n_expr);
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        outermost = expr;
        for (i = 0; i < order; i++) {
            node = mem_alloc_node();
            if (!node) { ast_free(outermost); set_err(p, "Out of memory"); return NULL; }
            node->type         = NODE_DERIV;
            node->d.deriv.var  = var;
            node->d.deriv.expr = outermost;
            outermost = node;
        }
        return outermost;
    }
    case TOK_SEQLIM: {
        ast_node_t *expr, *node;
        char var = 'n';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'n');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type              = NODE_LIMIT;
        node->d.limit.var       = var;
        node->d.limit.expr      = expr;
        node->d.limit.point     = ast_constant(CONST_INF);
        node->d.limit.direction = 0;
        return node;
    }
    case TOK_INTEGRAL: {
        ast_node_t *expr, *node;
        char var = 'x';
        bool has_explicit_var = false;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) {
                var = vt->var_name;
                has_explicit_var = true;
                advance(p);
            }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type              = NODE_INTEGRAL;
        node->d.integral.var    = var;
        node->d.integral.expr   = expr;
        node->d.integral.lo     = NULL;
        node->d.integral.hi     = NULL;
        if (match(p, TOK_COMMA)) {
            node->d.integral.lo = parse_expr(p, PREC_NONE);
            if (match(p, TOK_COMMA))
                node->d.integral.hi = parse_expr(p, PREC_NONE);
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }

        {
            char dvar;
            if (parse_integral_diffvar(p, &dvar)) {
                if (has_explicit_var &&
                    tolower((unsigned char)dvar) != tolower((unsigned char)var)) {
                    ast_free(node);
                    set_err(p, "Integral variable and differential mismatch");
                    return NULL;
                }
                /* An explicit variable matched case-insensitively, so it
                 * wins; otherwise use the differential in the case the
                 * integrand uses (int(X^2)dx integrates in X). */
                if (!has_explicit_var)
                    node->d.integral.var = default_var(expr, NULL, dvar);
            } else if (cur_type(p) == TOK_VARIABLE &&
                       tolower((unsigned char)cur(p)->var_name) == 'd') {
                ast_free(node);
                set_err(p, "Expected variable after differential 'd'");
                return NULL;
            }
        }

        return node;
    }
    case TOK_LIMIT: {
        ast_node_t *expr, *point, *node;
        char var = 'x';
        int8_t dir = 0;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        point = NULL;
        if (match(p, TOK_COMMA))
            point = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA)) {
            token_type_t dt = cur_type(p);
            const token_t *after = tok_get(p->tokens, p->pos + 1);
            if ((dt == TOK_PLUS || dt == TOK_MINUS) &&
                after && after->type == TOK_RPAREN) {
                dir = (dt == TOK_PLUS) ? 1 : -1;
                advance(p);
            } else {
                ast_node_t *d_expr = parse_expr(p, PREC_NONE);
                if (d_expr) {
                    if (d_expr->type == NODE_NUMBER) {
                        if (d_expr->d.number > 0) dir = 1;
                        else if (d_expr->d.number < 0) dir = -1;
                    }
                    ast_free(d_expr);
                }
            }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(point); set_err(p, "Out of memory"); return NULL; }
        node->type              = NODE_LIMIT;
        node->d.limit.var       = var;
        node->d.limit.expr      = expr;
        node->d.limit.point     = point;
        node->d.limit.direction = dir;
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }
    default: return NULL; /* unreachable */
    }
}

/* ── Series, algebra, and special form parsers ───────────── */

static ast_node_t *parse_series_algebra(parser_t *p)
{
    const token_t *t = cur(p);

    switch (t->type) {
    case TOK_SIGMA: {
        ast_node_t *expr, *node;
        char var = 'n';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'n');
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_SUM;
        node->d.sum.var     = var;
        node->d.sum.expr    = expr;
        node->d.sum.lo      = NULL;
        node->d.sum.hi      = NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); node->d.sum.var = var; }
        }
        if (match(p, TOK_COMMA))
            node->d.sum.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.sum.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── product notation: prod( expr, var, lo, hi ) ──── */
    case TOK_PRODUCT: {
        ast_node_t *expr, *node;
        char var = 'n';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'n');
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type            = NODE_PRODUCT;
        node->d.product.var   = var;
        node->d.product.expr  = expr;
        node->d.product.lo    = NULL;
        node->d.product.hi    = NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); node->d.product.var = var; }
        }
        if (match(p, TOK_COMMA))
            node->d.product.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.product.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── taylor( expr, var, center, degree ) ──────────── */
    case TOK_TAYLOR: {
        ast_node_t *expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type             = NODE_TAYLOR;
        node->d.taylor.var     = var;
        node->d.taylor.expr    = expr;
        node->d.taylor.center  = ast_number(0);
        node->d.taylor.degree  = ast_number(5);
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) {
                var = vt->var_name; advance(p);
                node->d.taylor.var = var;
            }
        }
        if (match(p, TOK_COMMA)) {
            ast_free(node->d.taylor.center);
            node->d.taylor.center = parse_expr(p, PREC_NONE);
        }
        if (match(p, TOK_COMMA)) {
            ast_free(node->d.taylor.degree);
            node->d.taylor.degree = parse_expr(p, PREC_NONE);
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── expand( expr ) or expand( expr, var ) ────────── */
    case TOK_EXPAND: {
        ast_node_t *expr, *node;
        char var = 0;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type           = NODE_EXPAND;
        node->d.expand.var   = var;
        node->d.expand.expr  = expr;
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── factor( expr ) or factor( expr, var ) ─────────── */
    case TOK_FACTOR: {
        ast_node_t *expr, *node;
        char var = 0;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type           = NODE_FACTOR;
        node->d.factor.var   = var;
        node->d.factor.expr  = expr;
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── solve( expr [= expr], var ) ──────────────────── */
    case TOK_SOLVE: {
        ast_node_t *lhs, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        lhs = parse_expr(p, PREC_ASSIGN);  /* stop before = */
        if (!lhs) return NULL;
        /* Handle equation: lhs = rhs → lhs - rhs = 0 */
        if (match(p, TOK_EQUALS)) {
            ast_node_t *rhs = parse_expr(p, PREC_ASSIGN);
            if (!rhs) { ast_free(lhs); return NULL; }
            lhs = ast_binop(OP_SUB, lhs, rhs);
        }
        var = default_var(lhs, NULL, 'x');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(lhs); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_SOLVE;
        node->d.solve.var   = var;
        node->d.solve.expr  = lhs;
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── nlsolve(expr1, expr2, var1, var2) ──────────── */
    case TOK_NLSOLVE: {
        ast_node_t *e1, *e2, *node;
        char v1 = 'x', v2 = 'y';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        e1 = parse_expr(p, PREC_ASSIGN);
        if (!e1) return NULL;
        /* Handle equation: lhs = rhs -> lhs - rhs */
        if (match(p, TOK_EQUALS)) {
            ast_node_t *rhs = parse_expr(p, PREC_ASSIGN);
            if (!rhs) { ast_free(e1); return NULL; }
            e1 = ast_binop(OP_SUB, e1, rhs);
        }
        if (!expect(p, TOK_COMMA)) { ast_free(e1); return NULL; }
        e2 = parse_expr(p, PREC_ASSIGN);
        if (!e2) { ast_free(e1); return NULL; }
        if (match(p, TOK_EQUALS)) {
            ast_node_t *rhs = parse_expr(p, PREC_ASSIGN);
            if (!rhs) { ast_free(e1); ast_free(e2); return NULL; }
            e2 = ast_binop(OP_SUB, e2, rhs);
        }
        v1 = default_var(e1, e2, 'x');
        v2 = 0;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { v1 = vt->var_name; advance(p); }
            if (match(p, TOK_COMMA)) {
                vt = cur(p);
                if (vt && vt->type == TOK_VARIABLE) { v2 = vt->var_name; advance(p); }
            }
        }
        if (!v2) {
            /* Never the explicit first variable: nlsolve(Y-1,2Y-2,Y) keeps
             * y, as before (review, PR #124). */
            v2 = default_var(e1, e2, 'y');
            if (v2 == v1) v2 = 'y';
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(e1); ast_free(e2); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(e1); ast_free(e2); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_NLSOLVE;
        node->d.polar.var   = v1;
        node->d.polar.expr1 = e1;
        node->d.polar.expr2 = e2;
        /* Store v2 in lo as a number node encoding the variable name */
        node->d.polar.lo    = ast_number((ce_float_t)v2);
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── tangent( expr, var, point ) ──────────────────── */
    case TOK_TANGENT: {
        ast_node_t *expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type              = NODE_TANGENT;
        node->d.tangent.var     = var;
        node->d.tangent.expr    = expr;
        node->d.tangent.point   = ast_number(0);
        if (match(p, TOK_COMMA)) {
            ast_free(node->d.tangent.point);
            node->d.tangent.point = parse_expr(p, PREC_NONE);
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── fourier( expr, var, nterms ) ─────────────────── */
    case TOK_FOURIER: {
        ast_node_t *expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type              = NODE_FOURIER;
        node->d.fourier.var     = var;
        node->d.fourier.expr    = expr;
        node->d.fourier.nterms  = ast_number(5);
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) {
                var = vt->var_name; advance(p);
                node->d.fourier.var = var;
            }
        }
        if (match(p, TOK_COMMA)) {
            ast_free(node->d.fourier.nterms);
            node->d.fourier.nterms = parse_expr(p, PREC_NONE);
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── piecewise( expr1, cond1, expr2, cond2, ..., default ) ── */
    case TOK_PIECEWISE: {
        ast_node_t *node, *tail, *arg;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;

        /* Build PAIR chain of all arguments */
        arg = parse_expr(p, PREC_NONE);
        if (!arg) return NULL;
        tail = mem_alloc_node();
        if (!tail) { ast_free(arg); set_err(p, "Out of memory"); return NULL; }
        tail->type = NODE_PAIR;
        tail->d.pair.head = arg;
        tail->d.pair.tail = NULL;

        node = mem_alloc_node();
        if (!node) { ast_free(tail); set_err(p, "Out of memory"); return NULL; }
        node->type = NODE_PIECEWISE;
        node->d.piecewise.pieces = tail;

        while (match(p, TOK_COMMA)) {
            ast_node_t *next_arg = parse_expr(p, PREC_NONE);
            ast_node_t *next_pair;
            if (!next_arg) { ast_free(node); return NULL; }
            next_pair = mem_alloc_node();
            if (!next_pair) { ast_free(next_arg); ast_free(node); set_err(p, "Out of memory"); return NULL; }
            next_pair->type = NODE_PAIR;
            next_pair->d.pair.head = next_arg;
            next_pair->d.pair.tail = NULL;
            tail->d.pair.tail = next_pair;
            tail = next_pair;
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    case TOK_RAND: {
        static bool seeded = false;
        if (!seeded) { srand(42); seeded = true; }
        advance(p);
        return ast_number((ce_float_t)rand() / (ce_float_t)RAND_MAX);
    }
    default: return NULL; /* unreachable */
    }
}

/* ── Statistics and multi-arg function parser ────────────── */

static ast_node_t *parse_stat_func(parser_t *p)
{
    const token_t *t = cur(p);
    stat_fn_t fn;
    ast_node_t *node, *tail, *arg;

    switch (t->type) {
        case TOK_MEAN:     fn = STAT_MEAN;   break;
        case TOK_MEDIAN:   fn = STAT_MEDIAN; break;
        case TOK_MODE:     fn = STAT_MODE;   break;
        case TOK_STDEV:    fn = STAT_STDEV;  break;
        case TOK_VARIANCE: fn = STAT_VAR;    break;
        case TOK_LINREG:   fn = STAT_LINREG; break;
        case TOK_NCR:      fn = STAT_NCR;    break;
        case TOK_NPR:      fn = STAT_NPR;    break;
        case TOK_GCD:      fn = STAT_GCD;    break;
        case TOK_LCM:      fn = STAT_LCM;    break;
        case TOK_MOD:      fn = STAT_MOD;    break;
        case TOK_RANDINT:  fn = STAT_RANDINT; break;
        case TOK_NORMALCDF:  fn = STAT_NORMALCDF;  break;
        case TOK_INVNORM:    fn = STAT_INVNORM;    break;
        case TOK_BINOMPDF:   fn = STAT_BINOMPDF;   break;
        case TOK_BINOMCDF:   fn = STAT_BINOMCDF;   break;
        case TOK_POISSONPDF: fn = STAT_POISSONPDF; break;
        case TOK_POISSONCDF: fn = STAT_POISSONCDF; break;
        case TOK_GEOMETPDF:  fn = STAT_GEOMETPDF;  break;
        case TOK_GEOMETCDF:  fn = STAT_GEOMETCDF;  break;
        case TOK_TCDF:       fn = STAT_TCDF;       break;
        case TOK_CHI2CDF:    fn = STAT_CHI2CDF;    break;
        case TOK_NORMALPDF:  fn = STAT_NORMALPDF;  break;
        case TOK_INVT:       fn = STAT_INVT;       break;
        case TOK_FPDF:       fn = STAT_FPDF;       break;
        case TOK_FCDF:       fn = STAT_FCDF;       break;
        case TOK_INVCHI2:    fn = STAT_INVCHI2;    break;
        case TOK_INVF:       fn = STAT_INVF;       break;
        case TOK_QUADREG:  fn = STAT_QUADREG; break;
        case TOK_EXPREG:   fn = STAT_EXPREG;  break;
        case TOK_PWRREG:   fn = STAT_PWRREG;  break;
        case TOK_LNREG:    fn = STAT_LNREG;   break;
        case TOK_FIN_PV:   fn = STAT_FIN_PV;  break;
        case TOK_FIN_FV:   fn = STAT_FIN_FV;  break;
        case TOK_FIN_PMT:  fn = STAT_FIN_PMT; break;
        case TOK_FIN_NPER: fn = STAT_FIN_NPER; break;
        case TOK_FIN_RATE: fn = STAT_FIN_RATE; break;
        case TOK_FIN_NOM:  fn = STAT_FIN_NOM;  break;
        case TOK_FIN_EFF:  fn = STAT_FIN_EFF;  break;
        case TOK_FIN_BAL:    fn = STAT_FIN_BAL;    break;
        case TOK_FIN_SUMPRN: fn = STAT_FIN_SUMPRN; break;
        case TOK_FIN_SUMINT: fn = STAT_FIN_SUMINT; break;
        case TOK_COVAR:      fn = STAT_COVAR;     break;
        case TOK_2VARSTATS:  fn = STAT_2VARSTATS;  break;
        case TOK_ZTEST:      fn = STAT_ZTEST;      break;
        case TOK_TTEST:      fn = STAT_TTEST;      break;
        case TOK_2SAMPZTEST: fn = STAT_2SAMPZTEST; break;
        case TOK_2SAMPTTEST: fn = STAT_2SAMPTTEST; break;
        case TOK_1PROPZTEST: fn = STAT_1PROPZTEST; break;
        case TOK_2PROPZTEST: fn = STAT_2PROPZTEST; break;
        case TOK_CHI2TEST:   fn = STAT_CHI2TEST;   break;
        case TOK_ZINTERVAL:  fn = STAT_ZINTERVAL;  break;
        case TOK_TINTERVAL:  fn = STAT_TINTERVAL;  break;
        case TOK_1PROPZINT:  fn = STAT_1PROPZINT;  break;
        case TOK_2PROPZINT:  fn = STAT_2PROPZINT;  break;
        case TOK_2SAMPZINT:  fn = STAT_2SAMPZINT;  break;
        case TOK_2SAMPTINT:  fn = STAT_2SAMPTINT;  break;
        case TOK_LINREGTTEST: fn = STAT_LINREGTTEST; break;
        case TOK_ANOVA:      fn = STAT_ANOVA;      break;
        case TOK_RESID:      fn = STAT_RESID;      break;
    default:           fn = STAT_MEAN;   break;
    }
    advance(p);
    if (!expect(p, TOK_LPAREN)) return NULL;

    /* Build a right-linked NODE_PAIR chain: PAIR(a, PAIR(b, PAIR(c, NULL))) */
    arg = parse_expr(p, PREC_NONE);
    if (!arg) return NULL;

    /* First pair node */
    tail = mem_alloc_node();
    if (!tail) { ast_free(arg); set_err(p, "Out of memory"); return NULL; }
    tail->type         = NODE_PAIR;
    tail->d.pair.head  = arg;
    tail->d.pair.tail  = NULL;

    {
        ast_node_t *head_pair = tail;  /* remember list head */
        /* Parse remaining comma-separated arguments */
        while (match(p, TOK_COMMA)) {
            ast_node_t *next_arg = parse_expr(p, PREC_NONE);
            ast_node_t *next_pair;
            if (!next_arg) { ast_free(head_pair); return NULL; }
            next_pair = mem_alloc_node();
            if (!next_pair) { ast_free(next_arg); ast_free(head_pair);
                              set_err(p, "Out of memory"); return NULL; }
            next_pair->type        = NODE_PAIR;
            next_pair->d.pair.head = next_arg;
            next_pair->d.pair.tail = NULL;
            tail->d.pair.tail = next_pair;
            tail = next_pair;
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(head_pair); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(head_pair); set_err(p, "Out of memory"); return NULL; }
        node->type        = NODE_STAT;
        node->d.stat.fn   = fn;
        node->d.stat.args = head_pair;
    }
    return node;
}

/* ── Matrix literal parser ───────────────────────────────── */

/* mat2(a,b,c,d) — 2×2 matrix constructor that requires no semicolons.
 * Enables deterministic matrix-literal entry on the real-ROM autotest path
 * where the semicolon (APPS-key) injection is unstable (see matrix-gap-analysis.md).
 * Produces an identical NODE_MATRIX_OP(MAT_LITERAL, rows=2, cols=2) to mat(a,b;c,d). */
static ast_node_t *parse_mat2_literal(parser_t *p)
{
    ast_node_t *elems[4];
    ast_node_t *head_pair, *tail_p;
    ast_node_t *node;
    int i;

    advance(p); /* consume TOK_MAT2 */
    if (!expect(p, TOK_LPAREN)) return NULL;

    for (i = 0; i < 4; i++) {
        elems[i] = parse_expr(p, PREC_NONE);
        if (!elems[i]) {
            int j;
            for (j = 0; j < i; j++) ast_free(elems[j]);
            return NULL;
        }
        if (i < 3) {
            if (!expect(p, TOK_COMMA)) {
                int j;
                for (j = 0; j <= i; j++) ast_free(elems[j]);
                return NULL;
            }
        }
    }
    if (!expect(p, TOK_RPAREN)) {
        for (i = 0; i < 4; i++) ast_free(elems[i]);
        return NULL;
    }

    /* Build forward pair chain: elems[0] → elems[1] → elems[2] → elems[3] → NULL */
    head_pair = mem_alloc_node();
    if (!head_pair) {
        for (i = 0; i < 4; i++) ast_free(elems[i]);
        set_err(p, "Out of memory");
        return NULL;
    }
    head_pair->type        = NODE_PAIR;
    head_pair->d.pair.head = elems[0];
    head_pair->d.pair.tail = NULL;
    tail_p = head_pair;

    for (i = 1; i < 4; i++) {
        ast_node_t *next_pair = mem_alloc_node();
        if (!next_pair) {
            int j;
            for (j = i; j < 4; j++) ast_free(elems[j]);
            /* elems[0..i-1] now owned by the chain; ast_free chain frees them */
            ast_free(head_pair);
            set_err(p, "Out of memory");
            return NULL;
        }
        next_pair->type        = NODE_PAIR;
        next_pair->d.pair.head = elems[i];
        next_pair->d.pair.tail = NULL;
        tail_p->d.pair.tail    = next_pair;
        tail_p                 = next_pair;
    }

    node = mem_alloc_node();
    if (!node) {
        ast_free(head_pair);
        set_err(p, "Out of memory");
        return NULL;
    }
    node->type              = NODE_MATRIX_OP;
    node->d.matrix_op.op   = MAT_LITERAL;
    node->d.matrix_op.arg  = head_pair;
    node->d.matrix_op.rows = 2;
    node->d.matrix_op.cols = 2;
    return node;
}

static ast_node_t *parse_matrix_literal(parser_t *p)
{
    ast_node_t *tail_p, *head_pair, *arg, *node;
    uint8_t rows = 1, cols = 0, cur_cols = 0;

    advance(p);
    if (!expect(p, TOK_LPAREN)) return NULL;

    /* Parse first element */
    arg = parse_expr(p, PREC_NONE);
    if (!arg) return NULL;
    cur_cols = 1;

    tail_p = mem_alloc_node();
    if (!tail_p) { ast_free(arg); set_err(p, "Out of memory"); return NULL; }
    tail_p->type        = NODE_PAIR;
    tail_p->d.pair.head = arg;
    tail_p->d.pair.tail = NULL;
    head_pair = tail_p;

    /* Parse remaining values separated by commas (same row) or semicolons (new row) */
    while (cur_type(p) == TOK_COMMA || cur_type(p) == TOK_SEMICOLON) {
        ast_node_t *next_arg, *next_pair;
        if (cur_type(p) == TOK_SEMICOLON) {
            advance(p);
            if (cols == 0) cols = cur_cols;
            cur_cols = 0;
            rows++;
        } else {
            advance(p);
        }
        next_arg = parse_expr(p, PREC_NONE);
        if (!next_arg) { ast_free(head_pair); return NULL; }
        cur_cols++;
        next_pair = mem_alloc_node();
        if (!next_pair) { ast_free(next_arg); ast_free(head_pair);
                          set_err(p, "Out of memory"); return NULL; }
        next_pair->type        = NODE_PAIR;
        next_pair->d.pair.head = next_arg;
        next_pair->d.pair.tail = NULL;
        tail_p->d.pair.tail = next_pair;
        tail_p = next_pair;
    }
    if (cols == 0) cols = cur_cols;
    if (!expect(p, TOK_RPAREN)) { ast_free(head_pair); return NULL; }

    node = mem_alloc_node();
    if (!node) { ast_free(head_pair); set_err(p, "Out of memory"); return NULL; }
    node->type              = NODE_MATRIX_OP;
    node->d.matrix_op.op   = MAT_LITERAL;
    node->d.matrix_op.arg  = head_pair;
    node->d.matrix_op.rows = rows;
    node->d.matrix_op.cols = cols;
    return node;
}

/* ── Polar, volume, and related-rate parsers ─────────────── */

static ast_node_t *parse_polar_volume(parser_t *p)
{
    const token_t *t = cur(p);

    switch (t->type) {
    case TOK_POLAR: {
        ast_node_t *arg1, *arg2, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        arg1 = parse_expr(p, PREC_NONE);
        if (!arg1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(arg1); return NULL; }
        arg2 = parse_expr(p, PREC_NONE);
        if (!arg2) { ast_free(arg1); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(arg1); ast_free(arg2); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(arg1); ast_free(arg2); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_TO_RECT;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = arg1;
        node->d.polar.expr2 = arg2;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── rect(x, y) — rectangular to polar ─────────────── */
    case TOK_RECT: {
        ast_node_t *arg1, *arg2, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        arg1 = parse_expr(p, PREC_NONE);
        if (!arg1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(arg1); return NULL; }
        arg2 = parse_expr(p, PREC_NONE);
        if (!arg2) { ast_free(arg1); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(arg1); ast_free(arg2); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(arg1); ast_free(arg2); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_FROM_RECT;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = arg1;
        node->d.polar.expr2 = arg2;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── arclen(f, var, a, b) or arclen(xt, yt, var, a, b) ─ */
    case TOK_ARCLEN: {
        ast_node_t *expr1, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr1 = parse_expr(p, PREC_NONE);
        if (!expr1) return NULL;
        var = default_var(expr1, NULL, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr1); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_ARCLEN;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr1;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;

        if (match(p, TOK_COMMA)) {
            /* Check: is next token a variable followed by comma?
             * If so: Cartesian form arclen(f, var, a, b)
             * If expression: parametric form arclen(yt, ...) */
            const token_t *vt = cur(p);
            const token_t *after = tok_get(p->tokens, p->pos + 1);
            if (vt && vt->type == TOK_VARIABLE && after &&
                after->type == TOK_COMMA) {
                /* Cartesian: arclen(f, var, a, b) */
                var = vt->var_name; advance(p);
                node->d.polar.var = var;
            } else {
                /* Parametric: arclen(xt, yt, var, a, b) */
                ast_node_t *expr2 = parse_expr(p, PREC_NONE);
                if (!expr2) { ast_free(node); return NULL; }
                node->d.polar.expr2 = expr2;
                node->d.polar.op = POLOP_ARCLEN_PARAM;
                /* Both coordinates decide the default: arclen(1,X^2)
                 * is in X (review, PR #124). */
                var = default_var(expr1, expr2, 'x');
                node->d.polar.var = var;
                if (match(p, TOK_COMMA)) {
                    vt = cur(p);
                    if (vt && vt->type == TOK_VARIABLE) {
                        var = vt->var_name; advance(p);
                        node->d.polar.var = var;
                    }
                }
            }
        }
        if (match(p, TOK_COMMA))
            node->d.polar.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.polar.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── surfarea(f, var, a, b) — surface area of revolution ── */
    case TOK_SURFAREA: {
        ast_node_t *expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_SURFREV;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); node->d.polar.var = var; }
        }
        if (match(p, TOK_COMMA))
            node->d.polar.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.polar.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── disk(f, var, a, b) — disk method volume ──────────── */
    case TOK_DISK: {
        ast_node_t *expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_DISK;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); node->d.polar.var = var; }
        }
        if (match(p, TOK_COMMA))
            node->d.polar.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.polar.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── washer(R, r, var, a, b) — washer method volume ───── */
    case TOK_WASHER: {
        ast_node_t *expr1, *expr2, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr1 = parse_expr(p, PREC_NONE);
        if (!expr1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr1); return NULL; }
        expr2 = parse_expr(p, PREC_NONE);
        if (!expr2) { ast_free(expr1); return NULL; }
        var = default_var(expr1, expr2, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr1); ast_free(expr2); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_WASHER;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr1;
        node->d.polar.expr2 = expr2;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); node->d.polar.var = var; }
        }
        if (match(p, TOK_COMMA))
            node->d.polar.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.polar.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── shell(f, var, a, b) — shell method volume ────────── */
    case TOK_SHELL: {
        ast_node_t *expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_SHELL;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); node->d.polar.var = var; }
        }
        if (match(p, TOK_COMMA))
            node->d.polar.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.polar.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── areabetween(f, g, var, a, b) — area between two curves ─ */
    case TOK_AREABETWEEN: {
        ast_node_t *expr1, *expr2, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr1 = parse_expr(p, PREC_NONE);
        if (!expr1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr1); return NULL; }
        expr2 = parse_expr(p, PREC_NONE);
        if (!expr2) { ast_free(expr1); return NULL; }
        var = default_var(expr1, expr2, 'x');
        node = mem_alloc_node();
        if (!node) { ast_free(expr1); ast_free(expr2); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_AREABETWEEN;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr1;
        node->d.polar.expr2 = expr2;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) {
                var = vt->var_name;
                advance(p);
                node->d.polar.var = var;
            }
        }
        if (match(p, TOK_COMMA))
            node->d.polar.lo = parse_expr(p, PREC_NONE);
        if (match(p, TOK_COMMA))
            node->d.polar.hi = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(node); return NULL; }
        return node;
    }

    /* ── relrate(expr) — related rates (chain rule w.r.t. time) ─ */
    case TOK_RELRATE: {
        ast_node_t *expr, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        /* Handle equation form: lhs = rhs → lhs - rhs */
        if (match(p, TOK_EQUALS)) {
            ast_node_t *rhs = parse_expr(p, PREC_NONE);
            if (!rhs) { ast_free(expr); return NULL; }
            expr = ast_binop(OP_SUB, expr, rhs);
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_RELRATE;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    case TOK_ANS: {
        advance(p);
        return ast_number(g_last_answer);
    }
    default: return NULL; /* unreachable */
    }
}

/* ── Special operation parsers ───────────────────────────── */

static ast_node_t *parse_special_ops(parser_t *p)
{
    const token_t *t = cur(p);

    switch (t->type) {
    case TOK_CSQUARE:
    /* ── pfrac(expr) or pfrac(expr, var) ──────────────── */
    case TOK_PFRAC:
    /* ── descartes(expr) or descartes(expr, var) ──────── */
    case TOK_DESCARTES: {
        polar_op_t pop;
        ast_node_t *expr, *node;
        char var = 'x';
        switch (t->type) {
        case TOK_CSQUARE:  pop = POLOP_CSQUARE;  break;
        case TOK_PFRAC:    pop = POLOP_PFRAC;     break;
        default:           pop = POLOP_DESCARTES; break;
        }
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── line(x1,y1,x2,y2) — line from two points ────── */
    case TOK_LINE2PT: {
        ast_node_t *a1, *a2, *a3, *a4, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        a1 = parse_expr(p, PREC_NONE);
        if (!a1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(a1); return NULL; }
        a2 = parse_expr(p, PREC_NONE);
        if (!a2) { ast_free(a1); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); return NULL; }
        a3 = parse_expr(p, PREC_NONE);
        if (!a3) { ast_free(a1); ast_free(a2); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        a4 = parse_expr(p, PREC_NONE);
        if (!a4) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(a1); ast_free(a2); ast_free(a3); ast_free(a4); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(a1); ast_free(a2); ast_free(a3); ast_free(a4); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_LINE2PT;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = a1;
        node->d.polar.expr2 = a2;
        node->d.polar.lo    = a3;
        node->d.polar.hi    = a4;
        return node;
    }

    /* ── linept(m,x1,y1) — line from slope+point ─────── */
    case TOK_LINEPT: {
        ast_node_t *a1, *a2, *a3, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        a1 = parse_expr(p, PREC_NONE);
        if (!a1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(a1); return NULL; }
        a2 = parse_expr(p, PREC_NONE);
        if (!a2) { ast_free(a1); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); return NULL; }
        a3 = parse_expr(p, PREC_NONE);
        if (!a3) { ast_free(a1); ast_free(a2); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(a1); ast_free(a2); ast_free(a3); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_LINEPT;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = a1;
        node->d.polar.expr2 = a2;
        node->d.polar.lo    = a3;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── parabola(vx,vy,px,py) — parabola from vertex+point */
    case TOK_PARABOLA: {
        ast_node_t *a1, *a2, *a3, *a4, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        a1 = parse_expr(p, PREC_NONE);
        if (!a1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(a1); return NULL; }
        a2 = parse_expr(p, PREC_NONE);
        if (!a2) { ast_free(a1); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); return NULL; }
        a3 = parse_expr(p, PREC_NONE);
        if (!a3) { ast_free(a1); ast_free(a2); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        a4 = parse_expr(p, PREC_NONE);
        if (!a4) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(a1); ast_free(a2); ast_free(a3); ast_free(a4); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(a1); ast_free(a2); ast_free(a3); ast_free(a4); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_PARABOLA;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = a1;
        node->d.polar.expr2 = a2;
        node->d.polar.lo    = a3;
        node->d.polar.hi    = a4;
        return node;
    }

    /* ── continuity(expr, var, point) ─────────────────── */
    case TOK_CONTINUITY: {
        ast_node_t *expr, *pt_expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
        }
        pt_expr = NULL;
        if (match(p, TOK_COMMA))
            pt_expr = parse_expr(p, PREC_NONE);
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(pt_expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(pt_expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_CONTINUITY;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = pt_expr;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── tofrac(expr) — decimal to fraction ──────────────── */
    case TOK_TOFRAC: {
        ast_node_t *expr, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_TOFRAC;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── recseq(expr, var, init, k) — recursive sequence ── */
    case TOK_RECSEQ: {
        ast_node_t *expr, *init_node, *k_node, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        /* var */
        {const token_t *vt = cur(p);
        if (vt && vt->type == TOK_VARIABLE) {
            var = vt->var_name;
            advance(p);
        } else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }}
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        init_node = parse_expr(p, PREC_NONE);
        if (!init_node) { ast_free(expr); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(init_node); return NULL; }
        k_node = parse_expr(p, PREC_NONE);
        if (!k_node) { ast_free(expr); ast_free(init_node); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(init_node); ast_free(k_node); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(init_node); ast_free(k_node);
                     set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_RECSEQ;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = init_node;
        node->d.polar.hi    = k_node;
        return node;
    }

    /* ── unitcirc(angle_deg) — unit circle lookup ───────── */
    case TOK_UNITCIRC: {
        ast_node_t *angle, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        angle = parse_expr(p, PREC_NONE);
        if (!angle) return NULL;
        if (!expect(p, TOK_RPAREN)) { ast_free(angle); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(angle); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_UNITCIRC;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = angle;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── arithsum(a1,d,n) / geomsum(a1,r,n) — series sums ── */
    case TOK_ARITHSUM:
    case TOK_GEOMSUM: {
        polar_op_t pop = (t->type == TOK_ARITHSUM) ? POLOP_ARITHSUM : POLOP_GEOMSUM;
        ast_node_t *a1, *a2, *a3, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        a1 = parse_expr(p, PREC_NONE);
        if (!a1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(a1); return NULL; }
        a2 = parse_expr(p, PREC_NONE);
        if (!a2) { ast_free(a1); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); return NULL; }
        a3 = parse_expr(p, PREC_NONE);
        if (!a3) { ast_free(a1); ast_free(a2); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(a1); ast_free(a2); ast_free(a3); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = a1;
        node->d.polar.expr2 = a2;
        node->d.polar.lo    = a3;
        node->d.polar.hi    = NULL;
        return node;
    }
    default: return NULL; /* unreachable */
    }
}

/* ── Numerical method and multivariable parsers ──────────── */

static ast_node_t *parse_numerical_multivariable(parser_t *p)
{
    const token_t *t = cur(p);

    switch (t->type) {
    /* ── ode_sep(rhs, x, y), ode_linear(rhs, x, y), diffeq(rhs, x, y) ── */
    case TOK_ODE_SEP:
    case TOK_ODE_LINEAR:
    case TOK_DIFFEQ: {
        ast_node_t *rhs, *node;
        polar_op_t pop;
        char xvar, yvar;
        if (t->type == TOK_ODE_SEP) pop = POLOP_ODE_SEP;
        else if (t->type == TOK_ODE_LINEAR) pop = POLOP_ODE_LINEAR;
        else pop = POLOP_DIFFEQ;

        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        rhs = parse_expr(p, PREC_NONE);
        if (!rhs) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); return NULL; }
        { const token_t *vx = cur(p);
          if (vx && vx->type == TOK_VARIABLE) { xvar = vx->var_name; advance(p); }
          else { ast_free(rhs); set_err(p, "Expected x-variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); return NULL; }
        { const token_t *vy = cur(p);
          if (vy && vy->type == TOK_VARIABLE) { yvar = vy->var_name; advance(p); }
          else { ast_free(rhs); set_err(p, "Expected y-variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(rhs); return NULL; }

        node = mem_alloc_node();
        if (!node) { ast_free(rhs); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = xvar;
        node->d.polar.expr1 = rhs;
        node->d.polar.expr2 = ast_variable(yvar);
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (!node->d.polar.expr2) {
            ast_free(rhs);
            mem_free_node(node);
            set_err(p, "Out of memory");
            return NULL;
        }
        return node;
    }

    /* ── euler(rhs, x, y, x0, y0, xf, h) ── */
    case TOK_EULER: {
        ast_node_t *rhs, *x0, *y0, *xf, *hstep, *xpair, *ypair, *node;
        char xvar, yvar;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        rhs = parse_expr(p, PREC_NONE);
        if (!rhs) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); return NULL; }
        { const token_t *vx = cur(p);
          if (vx && vx->type == TOK_VARIABLE) { xvar = vx->var_name; advance(p); }
          else { ast_free(rhs); set_err(p, "Expected x-variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); return NULL; }
        { const token_t *vy = cur(p);
          if (vy && vy->type == TOK_VARIABLE) { yvar = vy->var_name; advance(p); }
          else { ast_free(rhs); set_err(p, "Expected y-variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); return NULL; }
        x0 = parse_expr(p, PREC_NONE);
        if (!x0) { ast_free(rhs); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); ast_free(x0); return NULL; }
        y0 = parse_expr(p, PREC_NONE);
        if (!y0) { ast_free(rhs); ast_free(x0); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); ast_free(x0); ast_free(y0); return NULL; }
        xf = parse_expr(p, PREC_NONE);
        if (!xf) { ast_free(rhs); ast_free(x0); ast_free(y0); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); ast_free(x0); ast_free(y0); ast_free(xf); return NULL; }
        hstep = parse_expr(p, PREC_NONE);
        if (!hstep) { ast_free(rhs); ast_free(x0); ast_free(y0); ast_free(xf); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(rhs); ast_free(x0); ast_free(y0); ast_free(xf); ast_free(hstep); return NULL; }

        xpair = mem_alloc_node();
        ypair = mem_alloc_node();
        node  = mem_alloc_node();
        if (!xpair || !ypair || !node) {
            if (xpair) mem_free_node(xpair);
            if (ypair) mem_free_node(ypair);
            if (node) mem_free_node(node);
            ast_free(rhs); ast_free(x0); ast_free(y0); ast_free(xf); ast_free(hstep);
            set_err(p, "Out of memory");
            return NULL;
        }

        xpair->type = NODE_PAIR;
        xpair->d.pair.head = x0;
        xpair->d.pair.tail = y0;
        ypair->type = NODE_PAIR;
        ypair->d.pair.head = xf;
        ypair->d.pair.tail = hstep;

        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_EULER;
        node->d.polar.var   = xvar;
        node->d.polar.expr1 = rhs;
        node->d.polar.expr2 = ast_variable(yvar);
        node->d.polar.lo    = xpair;
        node->d.polar.hi    = ypair;
        if (!node->d.polar.expr2) {
            ast_free(rhs);
            ast_free(xpair);
            ast_free(ypair);
            mem_free_node(node);
            set_err(p, "Out of memory");
            return NULL;
        }
        return node;
    }

    /* ── slopefield(rhs, x, y [, xmin, xmax, ymin, ymax]) ── */
    case TOK_SLOPEFIELD: {
        ast_node_t *rhs, *xmin = NULL, *xmax = NULL;
        ast_node_t *ymin = NULL, *ymax = NULL;
        ast_node_t *xpair = NULL, *ypair = NULL, *node;
        char xvar, yvar;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        rhs = parse_expr(p, PREC_NONE);
        if (!rhs) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); return NULL; }
        { const token_t *vx = cur(p);
          if (vx && vx->type == TOK_VARIABLE) { xvar = vx->var_name; advance(p); }
          else { ast_free(rhs); set_err(p, "Expected x-variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(rhs); return NULL; }
        { const token_t *vy = cur(p);
          if (vy && vy->type == TOK_VARIABLE) { yvar = vy->var_name; advance(p); }
          else { ast_free(rhs); set_err(p, "Expected y-variable"); return NULL; }
        }

        if (match(p, TOK_COMMA)) {
            xmin = parse_expr(p, PREC_NONE);
            if (!xmin) { ast_free(rhs); return NULL; }
            if (!expect(p, TOK_COMMA)) { ast_free(rhs); ast_free(xmin); return NULL; }
            xmax = parse_expr(p, PREC_NONE);
            if (!xmax) { ast_free(rhs); ast_free(xmin); return NULL; }
            if (!expect(p, TOK_COMMA)) { ast_free(rhs); ast_free(xmin); ast_free(xmax); return NULL; }
            ymin = parse_expr(p, PREC_NONE);
            if (!ymin) { ast_free(rhs); ast_free(xmin); ast_free(xmax); return NULL; }
            if (!expect(p, TOK_COMMA)) { ast_free(rhs); ast_free(xmin); ast_free(xmax); ast_free(ymin); return NULL; }
            ymax = parse_expr(p, PREC_NONE);
            if (!ymax) { ast_free(rhs); ast_free(xmin); ast_free(xmax); ast_free(ymin); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) {
            ast_free(rhs); ast_free(xmin); ast_free(xmax); ast_free(ymin); ast_free(ymax);
            return NULL;
        }

        node = mem_alloc_node();
        if (!node) {
            ast_free(rhs); ast_free(xmin); ast_free(xmax); ast_free(ymin); ast_free(ymax);
            set_err(p, "Out of memory");
            return NULL;
        }

        if (xmin && xmax && ymin && ymax) {
            xpair = mem_alloc_node();
            ypair = mem_alloc_node();
            if (!xpair || !ypair) {
                if (xpair) mem_free_node(xpair);
                if (ypair) mem_free_node(ypair);
                ast_free(rhs); ast_free(xmin); ast_free(xmax); ast_free(ymin); ast_free(ymax);
                mem_free_node(node);
                set_err(p, "Out of memory");
                return NULL;
            }
            xpair->type = NODE_PAIR;
            xpair->d.pair.head = xmin;
            xpair->d.pair.tail = xmax;
            ypair->type = NODE_PAIR;
            ypair->d.pair.head = ymin;
            ypair->d.pair.tail = ymax;
        }

        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_SLOPEFIELD;
        node->d.polar.var   = xvar;
        node->d.polar.expr1 = rhs;
        node->d.polar.expr2 = ast_variable(yvar);
        node->d.polar.lo    = xpair;
        node->d.polar.hi    = ypair;
        if (!node->d.polar.expr2) {
            ast_free(rhs);
            if (xpair) ast_free(xpair);
            if (ypair) ast_free(ypair);
            mem_free_node(node);
            set_err(p, "Out of memory");
            return NULL;
        }
        return node;
    }

    /* ── intparts(expr, var), trigsub(expr, var), intstrategy(expr, var) ── */
    case TOK_INTPARTS:
    case TOK_TRIGSUB:
    case TOK_INTSTRATEGY: {
        ast_node_t *expr, *node;
        polar_op_t pop;
        char var = 'x';
        if (t->type == TOK_INTPARTS) pop = POLOP_INTPARTS;
        else if (t->type == TOK_TRIGSUB) pop = POLOP_TRIGSUB;
        else pop = POLOP_INTSTRATEGY;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── usub(expr, var, u_expr) ── */
    case TOK_USUB: {
        ast_node_t *expr, *u_expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        u_expr = parse_expr(p, PREC_NONE);
        if (!u_expr) { ast_free(expr); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(u_expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(u_expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_USUB;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = u_expr;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── improper(expr, var, lo, hi) — explicit improper integral ── */
    case TOK_IMPROPER: {
        ast_node_t *expr, *lo_node, *hi_node, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        lo_node = parse_expr(p, PREC_NONE);
        if (!lo_node) { ast_free(expr); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(lo_node); return NULL; }
        hi_node = parse_expr(p, PREC_NONE);
        if (!hi_node) { ast_free(expr); ast_free(lo_node); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(lo_node); ast_free(hi_node); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(lo_node); ast_free(hi_node); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_IMPROPER;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = lo_node;
        node->d.polar.hi    = hi_node;
        return node;
    }

    case TOK_NEWTON: {
        ast_node_t *expr, *x0_node, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        x0_node = parse_expr(p, PREC_NONE);
        if (!x0_node) { ast_free(expr); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(x0_node); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(x0_node); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_NEWTON;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = x0_node;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── bisect(expr, var, a, b) — Bisection method ── */
    case TOK_BISECT: {
        ast_node_t *expr, *a_node, *b_node, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        a_node = parse_expr(p, PREC_NONE);
        if (!a_node) { ast_free(expr); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(a_node); return NULL; }
        b_node = parse_expr(p, PREC_NONE);
        if (!b_node) { ast_free(expr); ast_free(a_node); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(a_node); ast_free(b_node); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(a_node); ast_free(b_node); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_BISECT;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = a_node;
        node->d.polar.hi    = b_node;
        return node;
    }

    /* ── secant(expr, var, x0, x1) — Secant method ── */
    case TOK_SECANT: {
        ast_node_t *expr, *x0_node, *x1_node, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        x0_node = parse_expr(p, PREC_NONE);
        if (!x0_node) { ast_free(expr); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(x0_node); return NULL; }
        x1_node = parse_expr(p, PREC_NONE);
        if (!x1_node) { ast_free(expr); ast_free(x0_node); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(x0_node); ast_free(x1_node); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(x0_node); ast_free(x1_node); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_SECANT;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = x0_node;
        node->d.polar.hi    = x1_node;
        return node;
    }

    /* ── pdiff(expr, var1, var2, ...) — mixed partial derivatives ── */
    case TOK_PDIFF: {
        ast_node_t *expr, *node;
        char var1, var2 = 0;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var1 = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        /* Optional second variable for mixed partials */
        if (match(p, TOK_COMMA)) {
            const token_t *vt2 = cur(p);
            if (vt2 && vt2->type == TOK_VARIABLE) { var2 = vt2->var_name; advance(p); }
            else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_PDIFF;
        node->d.polar.var   = var1;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = var2 ? ast_variable(var2) : NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── gradient(expr, var1, var2) — gradient vector ── */
    case TOK_GRADIENT: {
        ast_node_t *expr, *node;
        char var1, var2;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var1 = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt2 = cur(p);
          if (vt2 && vt2->type == TOK_VARIABLE) { var2 = vt2->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_GRADIENT;
        node->d.polar.var   = var1;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = ast_variable(var2);
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── dblint(expr, x, xlo, xhi, y, ylo, yhi) — double integral ── */
    case TOK_DBLINT: {
        ast_node_t *expr, *xlo, *xhi, *ylo, *yhi, *node;
        char xvar, yvar;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { xvar = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        xlo = parse_expr(p, PREC_NONE);
        if (!xlo) { ast_free(expr); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(xlo); return NULL; }
        xhi = parse_expr(p, PREC_NONE);
        if (!xhi) { ast_free(expr); ast_free(xlo); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(xlo); ast_free(xhi); return NULL; }
        { const token_t *vt2 = cur(p);
          if (vt2 && vt2->type == TOK_VARIABLE) { yvar = vt2->var_name; advance(p); }
          else { ast_free(expr); ast_free(xlo); ast_free(xhi); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(xlo); ast_free(xhi); return NULL; }
        ylo = parse_expr(p, PREC_NONE);
        if (!ylo) { ast_free(expr); ast_free(xlo); ast_free(xhi); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(xlo); ast_free(xhi); ast_free(ylo); return NULL; }
        yhi = parse_expr(p, PREC_NONE);
        if (!yhi) { ast_free(expr); ast_free(xlo); ast_free(xhi); ast_free(ylo); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(xlo); ast_free(xhi); ast_free(ylo); ast_free(yhi); return NULL; }
        /* Pack: var=xvar, expr1=expr, expr2=yvar node, lo=PAIR(xlo,xhi), hi=PAIR(ylo,yhi) */
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(xlo); ast_free(xhi); ast_free(ylo); ast_free(yhi); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_DBLINT;
        node->d.polar.var   = xvar;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = ast_variable(yvar);
        {
            ast_node_t *xpair = mem_alloc_node();
            ast_node_t *ypair = mem_alloc_node();
            if (!xpair || !ypair) {
                if (xpair) mem_free_node(xpair);
                if (ypair) mem_free_node(ypair);
                ast_free(node->d.polar.expr2);
                ast_free(expr); ast_free(xlo); ast_free(xhi); ast_free(ylo); ast_free(yhi);
                mem_free_node(node);
                set_err(p, "Out of memory"); return NULL;
            }
            xpair->type = NODE_PAIR;
            xpair->d.pair.head = xlo;
            xpair->d.pair.tail = xhi;
            ypair->type = NODE_PAIR;
            ypair->d.pair.head = ylo;
            ypair->d.pair.tail = yhi;
            node->d.polar.lo = xpair;
            node->d.polar.hi = ypair;
        }
        return node;
    }

    /* ── critical(expr, var) — critical point finder ── */
    case TOK_CRITICAL: {
        ast_node_t *expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_CRITICAL;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── polydiv(p, q, var) — polynomial long division ── */
    case TOK_POLYDIV: {
        ast_node_t *p_expr, *q_expr, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        p_expr = parse_expr(p, PREC_NONE);
        if (!p_expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(p_expr); return NULL; }
        q_expr = parse_expr(p, PREC_NONE);
        if (!q_expr) { ast_free(p_expr); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(p_expr); ast_free(q_expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { var = vt->var_name; advance(p); }
          else { ast_free(p_expr); ast_free(q_expr); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(p_expr); ast_free(q_expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(p_expr); ast_free(q_expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_POLYDIV;
        node->d.polar.var   = var;
        node->d.polar.expr1 = p_expr;
        node->d.polar.expr2 = q_expr;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── graph(expr, var [, xmin, xmax]) — function graphing ── */
    case TOK_GRAPH: {
        ast_node_t *expr, *lo_node = NULL, *hi_node = NULL, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        expr = parse_expr(p, PREC_NONE);
        if (!expr) return NULL;
        var = default_var(expr, NULL, 'x');
        /* Optional: variable name */
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) {
                var = vt->var_name; advance(p);
            } else {
                /* Not a variable — might be xmin; keep the default variable */
                lo_node = parse_expr(p, PREC_NONE);
                if (!lo_node) { ast_free(expr); return NULL; }
                if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(lo_node); return NULL; }
                hi_node = parse_expr(p, PREC_NONE);
                if (!hi_node) { ast_free(expr); ast_free(lo_node); return NULL; }
                if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(lo_node); ast_free(hi_node); return NULL; }
                node = mem_alloc_node();
                if (!node) { ast_free(expr); ast_free(lo_node); ast_free(hi_node); set_err(p, "Out of memory"); return NULL; }
                node->type          = NODE_POLAR;
                node->d.polar.op    = POLOP_GRAPH;
                node->d.polar.var   = var;
                node->d.polar.expr1 = expr;
                node->d.polar.expr2 = NULL;
                node->d.polar.lo    = lo_node;
                node->d.polar.hi    = hi_node;
                return node;
            }
            /* Check for optional xmin, xmax after variable */
            if (match(p, TOK_COMMA)) {
                lo_node = parse_expr(p, PREC_NONE);
                if (!lo_node) { ast_free(expr); return NULL; }
                if (!expect(p, TOK_COMMA)) { ast_free(expr); ast_free(lo_node); return NULL; }
                hi_node = parse_expr(p, PREC_NONE);
                if (!hi_node) { ast_free(expr); ast_free(lo_node); return NULL; }
            }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(expr); ast_free(lo_node); ast_free(hi_node); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(expr); ast_free(lo_node); ast_free(hi_node); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_GRAPH;
        node->d.polar.var   = var;
        node->d.polar.expr1 = expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = lo_node;
        node->d.polar.hi    = hi_node;
        return node;
    }

    /* ── grapheq(lhs, rhs, var [, xmin, xmax]) — equation graphing ── */
    case TOK_GRAPHEQ: {
        ast_node_t *lhs, *rhs, *lo_node = NULL, *hi_node = NULL, *node;
        char var = 'x';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        lhs = parse_expr(p, PREC_NONE);
        if (!lhs) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(lhs); return NULL; }
        rhs = parse_expr(p, PREC_NONE);
        if (!rhs) { ast_free(lhs); return NULL; }
        var = default_var(lhs, rhs, 'x');
        /* Optional: variable name */
        if (match(p, TOK_COMMA)) {
            const token_t *vt = cur(p);
            if (vt && vt->type == TOK_VARIABLE) {
                var = vt->var_name; advance(p);
            } else {
                /* Not a variable — might be xmin */
                lo_node = parse_expr(p, PREC_NONE);
                if (!lo_node) { ast_free(lhs); ast_free(rhs); return NULL; }
                if (!expect(p, TOK_COMMA)) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); return NULL; }
                hi_node = parse_expr(p, PREC_NONE);
                if (!hi_node) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); return NULL; }
                if (!expect(p, TOK_RPAREN)) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); ast_free(hi_node); return NULL; }
                node = mem_alloc_node();
                if (!node) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); ast_free(hi_node); set_err(p, "Out of memory"); return NULL; }
                node->type          = NODE_POLAR;
                node->d.polar.op    = POLOP_GRAPHEQ;
                node->d.polar.var   = var;
                node->d.polar.expr1 = lhs;
                node->d.polar.expr2 = rhs;
                node->d.polar.lo    = lo_node;
                node->d.polar.hi    = hi_node;
                return node;
            }
            /* Check for optional xmin, xmax after variable */
            if (match(p, TOK_COMMA)) {
                lo_node = parse_expr(p, PREC_NONE);
                if (!lo_node) { ast_free(lhs); ast_free(rhs); return NULL; }
                if (!expect(p, TOK_COMMA)) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); return NULL; }
                hi_node = parse_expr(p, PREC_NONE);
                if (!hi_node) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); return NULL; }
            }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); ast_free(hi_node); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(lhs); ast_free(rhs); ast_free(lo_node); ast_free(hi_node); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_GRAPHEQ;
        node->d.polar.var   = var;
        node->d.polar.expr1 = lhs;
        node->d.polar.expr2 = rhs;
        node->d.polar.lo    = lo_node;
        node->d.polar.hi    = hi_node;
        return node;
    }

    /* ── dsolve(eq, y, x) — auto ODE solver ── */
    case TOK_DSOLVE: {
        ast_node_t *eq, *node;
        char yvar, xvar;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        eq = parse_expr(p, PREC_NONE);
        if (!eq) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(eq); return NULL; }
        { const token_t *vy = cur(p);
          if (vy && vy->type == TOK_VARIABLE) { yvar = vy->var_name; advance(p); }
          else { ast_free(eq); set_err(p, "Expected y-variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(eq); return NULL; }
        { const token_t *vx = cur(p);
          if (vx && vx->type == TOK_VARIABLE) { xvar = vx->var_name; advance(p); }
          else { ast_free(eq); set_err(p, "Expected x-variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(eq); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(eq); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_DSOLVE;
        node->d.polar.var   = xvar;
        node->d.polar.expr1 = eq;
        node->d.polar.expr2 = ast_variable(yvar);
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (!node->d.polar.expr2) {
            ast_free(eq); mem_free_node(node);
            set_err(p, "Out of memory"); return NULL;
        }
        return node;
    }

    /* ── polararea(r, theta, a, b) — polar area ½∫r²dθ ── */
    /* ── polararclen(r, theta, a, b) — polar arc length ── */
    case TOK_POLARAREA:
    case TOK_POLARARCLEN: {
        ast_node_t *r_expr, *a_expr, *b_expr, *node;
        polar_op_t pop = (t->type == TOK_POLARAREA) ? POLOP_POLARAREA : POLOP_POLARARCLEN;
        char tvar = 't';
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        r_expr = parse_expr(p, PREC_NONE);
        if (!r_expr) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(r_expr); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { tvar = vt->var_name; advance(p); }
          else { ast_free(r_expr); set_err(p, "Expected theta variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(r_expr); return NULL; }
        a_expr = parse_expr(p, PREC_NONE);
        if (!a_expr) { ast_free(r_expr); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(r_expr); ast_free(a_expr); return NULL; }
        b_expr = parse_expr(p, PREC_NONE);
        if (!b_expr) { ast_free(r_expr); ast_free(a_expr); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(r_expr); ast_free(a_expr); ast_free(b_expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(r_expr); ast_free(a_expr); ast_free(b_expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = tvar;
        node->d.polar.expr1 = r_expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = a_expr;
        node->d.polar.hi    = b_expr;
        return node;
    }

    /* ── dot(a1,...,an, b1,...,bn) — dot product (flat PAIR chain) ── */
    /* ── cross(a1,a2,a3, b1,b2,b3) — cross product ── */
    case TOK_VEC_DOT:
    case TOK_VEC_CROSS: {
        polar_op_t pop = (t->type == TOK_VEC_DOT) ? POLOP_VEC_DOT : POLOP_VEC_CROSS;
        ast_node_t *args[8];
        int nargs = 0;
        ast_node_t *chain, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        /* Parse all args into flat array (max 8) */
        do {
            if (nargs >= 8) { int k; for (k=0;k<nargs;k++) ast_free(args[k]); set_err(p,"Too many args"); return NULL; }
            args[nargs] = parse_expr(p, PREC_NONE);
            if (!args[nargs]) { int k; for (k=0;k<nargs;k++) ast_free(args[k]); return NULL; }
            nargs++;
        } while (match(p, TOK_COMMA));
        if (!expect(p, TOK_RPAREN)) { int k; for (k=0;k<nargs;k++) ast_free(args[k]); return NULL; }
        /* Build right-linked PAIR chain */
        if (nargs == 1) {
            chain = args[0];
        } else {
            int k;
            chain = args[nargs - 1];
            for (k = nargs - 2; k >= 0; k--) {
                ast_node_t *pair = mem_alloc_node();
                if (!pair) {
                    ast_free(chain);
                    for (; k >= 0; k--) ast_free(args[k]);
                    set_err(p, "Out of memory"); return NULL;
                }
                pair->type = NODE_PAIR;
                pair->d.pair.head = args[k];
                pair->d.pair.tail = chain;
                chain = pair;
            }
        }
        node = mem_alloc_node();
        if (!node) { ast_free(chain); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = chain;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── grad(f, x, y) — gradient vector (2D/3D) ── */
    case TOK_VEC_GRAD: {
        ast_node_t *f, *node;
        char v1, v2;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        f = parse_expr(p, PREC_NONE);
        if (!f) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(f); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { v1 = vt->var_name; advance(p); }
          else { ast_free(f); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(f); return NULL; }
        { const token_t *vt2 = cur(p);
          if (vt2 && vt2->type == TOK_VARIABLE) { v2 = vt2->var_name; advance(p); }
          else { ast_free(f); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(f); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(f); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_VEC_GRAD;
        node->d.polar.var   = v1;
        node->d.polar.expr1 = f;
        node->d.polar.expr2 = ast_variable(v2);
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (!node->d.polar.expr2) {
            ast_free(f); mem_free_node(node);
            set_err(p, "Out of memory"); return NULL;
        }
        return node;
    }

    /* ── div(Fx, Fy, x, y) — divergence (2D) ── */
    case TOK_DIVERGENCE: {
        ast_node_t *Fx, *Fy, *node;
        char v1, v2;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Fx = parse_expr(p, PREC_NONE);
        if (!Fx) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); return NULL; }
        Fy = parse_expr(p, PREC_NONE);
        if (!Fy) { ast_free(Fx); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); ast_free(Fy); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { v1 = vt->var_name; advance(p); }
          else { ast_free(Fx); ast_free(Fy); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); ast_free(Fy); return NULL; }
        { const token_t *vt2 = cur(p);
          if (vt2 && vt2->type == TOK_VARIABLE) { v2 = vt2->var_name; advance(p); }
          else { ast_free(Fx); ast_free(Fy); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(Fx); ast_free(Fy); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(Fx); ast_free(Fy); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_VEC_DIV;
        node->d.polar.var   = v1;
        node->d.polar.expr1 = Fx;
        node->d.polar.expr2 = Fy;
        node->d.polar.lo    = ast_variable(v2);
        node->d.polar.hi    = NULL;
        if (!node->d.polar.lo) {
            ast_free(Fx); ast_free(Fy); mem_free_node(node);
            set_err(p, "Out of memory"); return NULL;
        }
        return node;
    }

    /* ── curl(Fx, Fy, Fz, x, y, z) — 3D curl ── */
    case TOK_VEC_CURL: {
        ast_node_t *Fx, *Fy, *Fz, *node;
        char vx, vy, vz;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        Fx = parse_expr(p, PREC_NONE);
        if (!Fx) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); return NULL; }
        Fy = parse_expr(p, PREC_NONE);
        if (!Fy) { ast_free(Fx); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); ast_free(Fy); return NULL; }
        Fz = parse_expr(p, PREC_NONE);
        if (!Fz) { ast_free(Fx); ast_free(Fy); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); ast_free(Fy); ast_free(Fz); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { vx = vt->var_name; advance(p); }
          else { ast_free(Fx); ast_free(Fy); ast_free(Fz); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); ast_free(Fy); ast_free(Fz); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { vy = vt->var_name; advance(p); }
          else { ast_free(Fx); ast_free(Fy); ast_free(Fz); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(Fx); ast_free(Fy); ast_free(Fz); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { vz = vt->var_name; advance(p); }
          else { ast_free(Fx); ast_free(Fy); ast_free(Fz); set_err(p, "Expected variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(Fx); ast_free(Fy); ast_free(Fz); return NULL; }
        {
            /* Pack: expr1=Fx, expr2=PAIR(Fy,Fz), var=vx, lo=PAIR(vy_node,vz_node), hi=NULL */
            ast_node_t *yz_pair = mem_alloc_node();
            ast_node_t *fyz_pair = mem_alloc_node();
            if (!yz_pair || !fyz_pair) {
                if (yz_pair) mem_free_node(yz_pair);
                if (fyz_pair) mem_free_node(fyz_pair);
                ast_free(Fx); ast_free(Fy); ast_free(Fz);
                set_err(p, "Out of memory"); return NULL;
            }
            ast_node_t *vy_node = ast_variable(vy);
            ast_node_t *vz_node = ast_variable(vz);
            if (!vy_node || !vz_node) {
                if (vy_node) ast_free(vy_node);
                if (vz_node) ast_free(vz_node);
                mem_free_node(yz_pair); mem_free_node(fyz_pair);
                ast_free(Fx); ast_free(Fy); ast_free(Fz);
                set_err(p, "Out of memory"); return NULL;
            }
            fyz_pair->type = NODE_PAIR;
            fyz_pair->d.pair.head = Fy;
            fyz_pair->d.pair.tail = Fz;
            yz_pair->type = NODE_PAIR;
            yz_pair->d.pair.head = vy_node;
            yz_pair->d.pair.tail = vz_node;
            node = mem_alloc_node();
            if (!node) {
                ast_free(Fx); ast_free(fyz_pair); ast_free(yz_pair);
                set_err(p, "Out of memory"); return NULL;
            }
            node->type          = NODE_POLAR;
            node->d.polar.op    = POLOP_VEC_CURL;
            node->d.polar.var   = vx;
            node->d.polar.expr1 = Fx;
            node->d.polar.expr2 = fyz_pair;
            node->d.polar.lo    = yz_pair;
            node->d.polar.hi    = NULL;
            return node;
        }
    }

    /* ── laplace(f, t, s) — forward Laplace transform ── */
    /* ── ilaplace(F, s, t) — inverse Laplace transform ── */
    case TOK_LAPLACE:
    case TOK_ILAPLACE: {
        ast_node_t *f, *node;
        polar_op_t pop = (t->type == TOK_LAPLACE) ? POLOP_LAPLACE : POLOP_ILAPLACE;
        char v1, v2;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        f = parse_expr(p, PREC_NONE);
        if (!f) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(f); return NULL; }
        { const token_t *vt = cur(p);
          if (vt && vt->type == TOK_VARIABLE) { v1 = vt->var_name; advance(p); }
          else { ast_free(f); set_err(p, "Expected time/s variable"); return NULL; }
        }
        if (!expect(p, TOK_COMMA)) { ast_free(f); return NULL; }
        { const token_t *vt2 = cur(p);
          if (vt2 && vt2->type == TOK_VARIABLE) { v2 = vt2->var_name; advance(p); }
          else { ast_free(f); set_err(p, "Expected target variable"); return NULL; }
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(f); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(f); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = v1;
        node->d.polar.expr1 = f;
        node->d.polar.expr2 = ast_variable(v2);
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        if (!node->d.polar.expr2) {
            ast_free(f); mem_free_node(node);
            set_err(p, "Out of memory"); return NULL;
        }
        return node;
    }

    /* ── dsolve2(a, b, c, f, x) — 2nd-order linear ODE a*y''+b*y'+c*y=f(x) ── */
    case TOK_DSOLVE2: {
        ast_node_t *a_node, *b_node, *c_node, *f_node, *node;
        char xvar;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        a_node = parse_expr(p, PREC_NONE); if (!a_node) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(a_node); return NULL; }
        b_node = parse_expr(p, PREC_NONE);
        if (!b_node) { ast_free(a_node); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a_node); ast_free(b_node); return NULL; }
        c_node = parse_expr(p, PREC_NONE);
        if (!c_node) { ast_free(a_node); ast_free(b_node); return NULL; }
        if (!expect(p, TOK_COMMA)) {
            ast_free(a_node); ast_free(b_node); ast_free(c_node); return NULL;
        }
        f_node = parse_expr(p, PREC_NONE);
        if (!f_node) { ast_free(a_node); ast_free(b_node); ast_free(c_node); return NULL; }
        if (cur(p) && cur(p)->type == TOK_COMMA) {
            advance(p);
            const token_t *vx = cur(p);
            if (vx && vx->type == TOK_VARIABLE) { xvar = vx->var_name; advance(p); }
            else {
                ast_free(a_node); ast_free(b_node); ast_free(c_node); ast_free(f_node);
                set_err(p, "Expected x variable"); return NULL;
            }
        } else {
            xvar = default_var(f_node, NULL, 'x');
        }
        if (!expect(p, TOK_RPAREN)) {
            ast_free(a_node); ast_free(b_node); ast_free(c_node); ast_free(f_node);
            return NULL;
        }
        /* Pack: expr1=a_node, expr2=b_node, lo=c_node, hi=f_node, var=xvar */
        node = mem_alloc_node();
        if (!node) {
            ast_free(a_node); ast_free(b_node); ast_free(c_node); ast_free(f_node);
            set_err(p, "Out of memory"); return NULL;
        }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_DSOLVE2;
        node->d.polar.var   = xvar;
        node->d.polar.expr1 = a_node;
        node->d.polar.expr2 = b_node;
        node->d.polar.lo    = c_node;
        node->d.polar.hi    = f_node;
        return node;
    }

    /* ── eigenval(matrix_expr) — eigenvalues ── */
    /* ── eigenvec(matrix_expr) — eigenvectors ── */
    case TOK_EIGENVAL:
    case TOK_EIGENVEC: {
        ast_node_t *mat_expr, *node;
        polar_op_t pop = (t->type == TOK_EIGENVAL) ? POLOP_EIGENVAL : POLOP_EIGENVEC;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        mat_expr = parse_expr(p, PREC_NONE);
        if (!mat_expr) return NULL;
        if (!expect(p, TOK_RPAREN)) { ast_free(mat_expr); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(mat_expr); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = mat_expr;
        node->d.polar.expr2 = NULL;
        node->d.polar.lo    = NULL;
        node->d.polar.hi    = NULL;
        return node;
    }

    /* ── graphimpl(eq, x, y[, xmin, xmax, ymin, ymax]) — implicit curve ── */
    case TOK_GRAPHIMPL: {
        ast_node_t *eq, *xmin_n, *xmax_n, *ymin_n, *ymax_n, *node;
        char xvar, yvar;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        eq = parse_expr(p, PREC_NONE);
        if (!eq) return NULL;
        xvar = default_var(eq, NULL, 'x');
        yvar = 0;
        /* optional: ,x variable */
        if (cur(p) && cur(p)->type == TOK_COMMA) {
            advance(p);
            const token_t *vx = cur(p);
            if (vx && vx->type == TOK_VARIABLE) { xvar = vx->var_name; advance(p); }
            else { ast_free(eq); set_err(p, "Expected x variable"); return NULL; }
        }
        /* optional: ,y variable */
        if (cur(p) && cur(p)->type == TOK_COMMA) {
            advance(p);
            const token_t *vy = cur(p);
            if (vy && vy->type == TOK_VARIABLE) { yvar = vy->var_name; advance(p); }
            else { ast_free(eq); set_err(p, "Expected y variable"); return NULL; }
        }
        if (!yvar) {
            /* Never the explicit x variable: graphimpl(Y^2-1,Y) keeps y,
             * as before (review, PR #124). */
            yvar = default_var(eq, NULL, 'y');
            if (yvar == xvar) yvar = 'y';
        }
        /* optional bounds: ,xmin,xmax,ymin,ymax */
        xmin_n = xmax_n = ymin_n = ymax_n = NULL;
        if (cur(p) && cur(p)->type == TOK_COMMA) {
            advance(p);
            xmin_n = parse_expr(p, PREC_NONE);
            if (!xmin_n) { ast_free(eq); return NULL; }
            if (!expect(p, TOK_COMMA)) { ast_free(eq); ast_free(xmin_n); return NULL; }
            xmax_n = parse_expr(p, PREC_NONE);
            if (!xmax_n) { ast_free(eq); ast_free(xmin_n); return NULL; }
            if (!expect(p, TOK_COMMA)) {
                ast_free(eq); ast_free(xmin_n); ast_free(xmax_n); return NULL;
            }
            ymin_n = parse_expr(p, PREC_NONE);
            if (!ymin_n) { ast_free(eq); ast_free(xmin_n); ast_free(xmax_n); return NULL; }
            if (!expect(p, TOK_COMMA)) {
                ast_free(eq); ast_free(xmin_n); ast_free(xmax_n); ast_free(ymin_n);
                return NULL;
            }
            ymax_n = parse_expr(p, PREC_NONE);
            if (!ymax_n) {
                ast_free(eq); ast_free(xmin_n); ast_free(xmax_n); ast_free(ymin_n);
                return NULL;
            }
        }
        if (!expect(p, TOK_RPAREN)) {
            ast_free(eq); ast_free(xmin_n); ast_free(xmax_n);
            ast_free(ymin_n); ast_free(ymax_n); return NULL;
        }
        /*
         * Pack: expr1=eq(rearranged as lhs-rhs=0), expr2=xmin_n,
         *       lo=xmax_n, hi=ymin_n; store ymax via a secondary node attached as expr2 chain.
         * Simpler: encode xvar in var field, yvar encode as secondary NODE_VARIABLE in expr2.
         * For bounds, pack xmin/xmax as lo/hi, ymin/ymax are lost but defaults suffice.
         * We store: var=xvar, expr1=eq, expr2=ast_variable(yvar),
         *           lo=xmin_n (or NULL), hi=xmax_n (or NULL).
         * ymin/ymax are freed (defaults ±10 used at render time).
         */
        if (ymin_n) { ast_free(ymin_n); ymin_n = NULL; }
        if (ymax_n) { ast_free(ymax_n); ymax_n = NULL; }
        node = mem_alloc_node();
        if (!node) {
            ast_free(eq); ast_free(xmin_n); ast_free(xmax_n);
            set_err(p, "Out of memory"); return NULL;
        }
        node->type          = NODE_POLAR;
        node->d.polar.op    = POLOP_GRAPHIMPL;
        node->d.polar.var   = xvar;
        node->d.polar.expr1 = eq;
        /* We need yvar — attach as a secondary variable node in expr2 slot */
        node->d.polar.expr2 = ast_variable(yvar);
        node->d.polar.lo    = xmin_n;
        node->d.polar.hi    = xmax_n;
        if (!node->d.polar.expr2) {
            ast_free(eq); ast_free(xmin_n); ast_free(xmax_n);
            mem_free_node(node); set_err(p, "Out of memory"); return NULL;
        }
        return node;
    }

    default: return NULL; /* unreachable */
    }
}

/* ── Geometry, list, and matrix operation parsers ────────── */

static ast_node_t *parse_geometry_list_matrix(parser_t *p)
{
    const token_t *t = cur(p);

    switch (t->type) {
    case TOK_MIDPOINT:
    case TOK_DISTANCE:
    case TOK_SLOPE: {
        polar_op_t pop;
        ast_node_t *a1, *a2, *a3, *a4, *node;
        switch (t->type) {
        case TOK_MIDPOINT: pop = POLOP_MIDPOINT; break;
        case TOK_DISTANCE: pop = POLOP_DISTANCE; break;
        default:           pop = POLOP_SLOPE;    break;
        }
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        a1 = parse_expr(p, PREC_NONE);
        if (!a1) return NULL;
        if (!expect(p, TOK_COMMA)) { ast_free(a1); return NULL; }
        a2 = parse_expr(p, PREC_NONE);
        if (!a2) { ast_free(a1); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); return NULL; }
        a3 = parse_expr(p, PREC_NONE);
        if (!a3) { ast_free(a1); ast_free(a2); return NULL; }
        if (!expect(p, TOK_COMMA)) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        a4 = parse_expr(p, PREC_NONE);
        if (!a4) { ast_free(a1); ast_free(a2); ast_free(a3); return NULL; }
        if (!expect(p, TOK_RPAREN)) { ast_free(a1); ast_free(a2); ast_free(a3); ast_free(a4); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(a1); ast_free(a2); ast_free(a3); ast_free(a4); set_err(p, "Out of memory"); return NULL; }
        node->type          = NODE_POLAR;
        node->d.polar.op    = pop;
        node->d.polar.var   = 0;
        node->d.polar.expr1 = a1;
        node->d.polar.expr2 = a2;
        node->d.polar.lo    = a3;
        node->d.polar.hi    = a4;
        return node;
    }

    /* ── imaginary unit: i ──────────────────────────────── */
    case TOK_CPLX_I:
        advance(p);
        return ast_constant(CONST_I);

    /* ── list literal: {1,2,3} ──────────────────────────── */
    case TOK_LBRACE: {
        ast_node_t *arg, *tail_p, *head_pair, *node;
        advance(p);
        arg = parse_expr(p, PREC_NONE);
        if (!arg) return NULL;
        tail_p = mem_alloc_node();
        if (!tail_p) { ast_free(arg); set_err(p, "Out of memory"); return NULL; }
        tail_p->type = NODE_PAIR;
        tail_p->d.pair.head = arg;
        tail_p->d.pair.tail = NULL;
        head_pair = tail_p;
        while (match(p, TOK_COMMA)) {
            ast_node_t *next_arg = parse_expr(p, PREC_NONE);
            ast_node_t *next_pair;
            if (!next_arg) { ast_free(head_pair); return NULL; }
            next_pair = mem_alloc_node();
            if (!next_pair) { ast_free(next_arg); ast_free(head_pair);
                              set_err(p, "Out of memory"); return NULL; }
            next_pair->type = NODE_PAIR;
            next_pair->d.pair.head = next_arg;
            next_pair->d.pair.tail = NULL;
            tail_p->d.pair.tail = next_pair;
            tail_p = next_pair;
        }
        if (!expect(p, TOK_RBRACE)) { ast_free(head_pair); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(head_pair); set_err(p, "Out of memory"); return NULL; }
        node->type = NODE_LIST_LITERAL;
        node->d.list_literal.elems = head_pair;
        return node;
    }

    /* ── list references: L1-L6 and L1(i) element access ── */
    case TOK_L1: case TOK_L2: case TOK_L3:
    case TOK_L4: case TOK_L5: case TOK_L6: {
        uint8_t idx = (uint8_t)(t->type - TOK_L1);
        ast_node_t *node;
        advance(p);
        /* Check for element access: L1(expr) */
        if (cur_type(p) == TOK_LPAREN) {
            ast_node_t *index;
            advance(p);
            index = parse_expr(p, PREC_NONE);
            if (!index) return NULL;
            if (!expect(p, TOK_RPAREN)) { ast_free(index); return NULL; }
            node = mem_alloc_node();
            if (!node) { ast_free(index); set_err(p, "Out of memory"); return NULL; }
            node->type = NODE_LIST_ELEM;
            node->d.list_elem.list_idx = idx;
            node->d.list_elem.index = index;
            return node;
        }
        node = mem_alloc_node();
        if (!node) { set_err(p, "Out of memory"); return NULL; }
        node->type = NODE_LIST_REF;
        node->d.list_ref.idx = idx;
        return node;
    }

    /* ── list operations: cumsum(L1), dim(L1), etc. ─────── */
    case TOK_CUMSUM: case TOK_DELTALIST:
    case TOK_SORTA: case TOK_SORTD:
    case TOK_DIM: {
        uint8_t fn;
        ast_node_t *arg, *node;
        switch (t->type) {
        case TOK_CUMSUM:    fn = LIST_CUMSUM;    break;
        case TOK_DELTALIST: fn = LIST_DELTALIST; break;
        case TOK_SORTA:     fn = LIST_SORTA;     break;
        case TOK_SORTD:     fn = LIST_SORTD;     break;
        case TOK_DIM:       fn = LIST_DIM;       break;
        default:            fn = LIST_DIM;       break;
        }
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        arg = parse_expr(p, PREC_NONE);
        if (!arg) return NULL;
        if (!expect(p, TOK_RPAREN)) { ast_free(arg); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(arg); set_err(p, "Out of memory"); return NULL; }
        node->type = NODE_LIST_OP;
        node->d.list_op.fn = fn;
        node->d.list_op.arg = arg;
        return node;
    }

    /* ── seq(expr, var, start, end, step) ─────────────── */
    case TOK_SEQ: {
        ast_node_t *head_pair, *tail_p, *arg, *node;
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        /* Parse all comma-separated args as PAIR chain */
        arg = parse_expr(p, PREC_NONE);
        if (!arg) return NULL;
        tail_p = mem_alloc_node();
        if (!tail_p) { ast_free(arg); set_err(p, "Out of memory"); return NULL; }
        tail_p->type = NODE_PAIR;
        tail_p->d.pair.head = arg;
        tail_p->d.pair.tail = NULL;
        head_pair = tail_p;
        while (match(p, TOK_COMMA)) {
            ast_node_t *next_arg = parse_expr(p, PREC_NONE);
            ast_node_t *next_pair;
            if (!next_arg) { ast_free(head_pair); return NULL; }
            next_pair = mem_alloc_node();
            if (!next_pair) { ast_free(next_arg); ast_free(head_pair);
                              set_err(p, "Out of memory"); return NULL; }
            next_pair->type = NODE_PAIR;
            next_pair->d.pair.head = next_arg;
            next_pair->d.pair.tail = NULL;
            tail_p->d.pair.tail = next_pair;
            tail_p = next_pair;
        }
        if (!expect(p, TOK_RPAREN)) { ast_free(head_pair); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(head_pair); set_err(p, "Out of memory"); return NULL; }
        node->type = NODE_LIST_OP;
        node->d.list_op.fn = LIST_SEQ;
        node->d.list_op.arg = head_pair;
        return node;
    }

    case TOK_DET: case TOK_INV: case TOK_RREF: case TOK_TRANSPOSE: {
        matrix_op_t mop;
        ast_node_t *arg, *node;
        switch (t->type) {
        case TOK_DET:       mop = MAT_DET;       break;
        case TOK_INV:       mop = MAT_INV;       break;
        case TOK_RREF:      mop = MAT_RREF;      break;
        case TOK_TRANSPOSE: mop = MAT_TRANSPOSE;  break;
        default:            mop = MAT_DET;        break;
        }
        advance(p);
        if (!expect(p, TOK_LPAREN)) return NULL;
        arg = parse_expr(p, 0);  /* allow full expressions, e.g. det(A+B) */
        if (!arg) return NULL;
        if (!expect(p, TOK_RPAREN)) { ast_free(arg); return NULL; }
        node = mem_alloc_node();
        if (!node) { ast_free(arg); set_err(p, "Out of memory"); return NULL; }
        node->type             = NODE_MATRIX_OP;
        node->d.matrix_op.op  = mop;
        node->d.matrix_op.arg = arg;
        return node;
    }

    default: return NULL; /* unreachable */
    }
}

/* ── Prefix dispatcher ───────────────────────────────────── */

static ast_node_t *parse_prefix(parser_t *p)
{
    const token_t *t = cur(p);
    if (!t) { set_err(p, "Unexpected end"); return NULL; }

    switch (t->type) {

    /* ── basic literals ────────────────────────────────── */
    case TOK_NUMBER: case TOK_VARIABLE:
    case TOK_PI: case TOK_E: case TOK_INF:
    case TOK_MINUS: case TOK_LPAREN:
        return parse_basic_literal(p);

    /* ── single-argument functions ─────────────────────── */
    case TOK_SIN: case TOK_COS: case TOK_TAN:
    case TOK_SEC: case TOK_CSC: case TOK_COT:
    case TOK_ASIN: case TOK_ACOS: case TOK_ATAN:
    case TOK_ASEC: case TOK_ACSC: case TOK_ACOT:
    case TOK_LN: case TOK_LOG: case TOK_SQRT:
    case TOK_ABS: case TOK_EXP:
    case TOK_SINH: case TOK_COSH: case TOK_TANH:
    case TOK_ASINH: case TOK_ACOSH: case TOK_ATANH:
    case TOK_SECH: case TOK_CSCH: case TOK_COTH:
    case TOK_FLOOR: case TOK_CEIL: case TOK_ROUND:
    case TOK_IPART: case TOK_FPART:
    case TOK_DEC: case TOK_HEX: case TOK_BIN: case TOK_OCT:
    case TOK_REAL: case TOK_IMAG: case TOK_CONJ: case TOK_ANGLE:
    case TOK_SIGN: case TOK_HEAVISIDE:
        return parse_single_arg_func(p);

    /* ── calculus operations ───────────────────────────── */
    case TOK_DERIV: case TOK_NDERIV: case TOK_SEQLIM:
    case TOK_INTEGRAL: case TOK_LIMIT:
        return parse_calculus_op(p);

    /* ── series, algebra, and special forms ─────────────── */
    case TOK_SIGMA: case TOK_PRODUCT:
    case TOK_TAYLOR: case TOK_EXPAND: case TOK_FACTOR:
    case TOK_SOLVE: case TOK_NLSOLVE: case TOK_TANGENT:
    case TOK_FOURIER: case TOK_PIECEWISE: case TOK_RAND:
        return parse_series_algebra(p);

    /* ── statistics / multi-arg functions ───────────────── */
    case TOK_MEAN: case TOK_MEDIAN: case TOK_MODE:
    case TOK_STDEV: case TOK_VARIANCE: case TOK_LINREG:
    case TOK_NCR: case TOK_NPR:
    case TOK_GCD: case TOK_LCM: case TOK_MOD: case TOK_RANDINT:
    case TOK_NORMALCDF: case TOK_INVNORM:
    case TOK_BINOMPDF: case TOK_BINOMCDF:
    case TOK_POISSONPDF: case TOK_POISSONCDF:
    case TOK_GEOMETPDF: case TOK_GEOMETCDF:
    case TOK_TCDF: case TOK_CHI2CDF:
    case TOK_NORMALPDF: case TOK_INVT:
    case TOK_FPDF: case TOK_FCDF:
    case TOK_INVCHI2: case TOK_INVF:
    case TOK_QUADREG: case TOK_EXPREG: case TOK_PWRREG: case TOK_LNREG:
    case TOK_FIN_PV: case TOK_FIN_FV: case TOK_FIN_PMT:
    case TOK_FIN_NPER: case TOK_FIN_RATE:
    case TOK_FIN_NOM: case TOK_FIN_EFF:
    case TOK_FIN_BAL: case TOK_FIN_SUMPRN: case TOK_FIN_SUMINT:
    case TOK_COVAR: case TOK_2VARSTATS:
    case TOK_ZTEST: case TOK_TTEST:
    case TOK_2SAMPZTEST: case TOK_2SAMPTTEST:
    case TOK_1PROPZTEST: case TOK_2PROPZTEST:
    case TOK_CHI2TEST:
    case TOK_ZINTERVAL: case TOK_TINTERVAL:
    case TOK_1PROPZINT: case TOK_2PROPZINT:
    case TOK_2SAMPZINT: case TOK_2SAMPTINT:
    case TOK_LINREGTTEST: case TOK_ANOVA: case TOK_RESID:
        return parse_stat_func(p);

    /* ── matrix literal ────────────────────────────────── */
    case TOK_MAT:
        return parse_matrix_literal(p);
    case TOK_MAT2:
        return parse_mat2_literal(p);

    /* ── polar, volume, related rates ──────────────────── */
    case TOK_POLAR: case TOK_RECT:
    case TOK_ARCLEN: case TOK_SURFAREA:
    case TOK_DISK: case TOK_WASHER: case TOK_SHELL:
    case TOK_AREABETWEEN:
    case TOK_RELRATE: case TOK_ANS:
        return parse_polar_volume(p);

    /* ── special operations ────────────────────────────── */
    case TOK_CSQUARE: case TOK_PFRAC: case TOK_DESCARTES:
    case TOK_LINE2PT: case TOK_LINEPT: case TOK_PARABOLA:
    case TOK_CONTINUITY: case TOK_TOFRAC: case TOK_RECSEQ:
    case TOK_UNITCIRC: case TOK_ARITHSUM: case TOK_GEOMSUM:
        return parse_special_ops(p);

    /* ── numerical methods / multivariable / graphing ───── */
    case TOK_ODE_SEP: case TOK_ODE_LINEAR: case TOK_DIFFEQ:
    case TOK_EULER: case TOK_SLOPEFIELD:
    case TOK_INTPARTS: case TOK_TRIGSUB: case TOK_USUB:
    case TOK_INTSTRATEGY: case TOK_IMPROPER:
    case TOK_NEWTON: case TOK_BISECT: case TOK_SECANT:
    case TOK_PDIFF: case TOK_GRADIENT: case TOK_DBLINT:
    case TOK_CRITICAL: case TOK_POLYDIV: case TOK_GRAPH:
    case TOK_GRAPHEQ:
    /* ── new Pass 74/75 operations ─────────────────────────── */
    case TOK_DSOLVE:
    case TOK_DSOLVE2:
    case TOK_POLARAREA: case TOK_POLARARCLEN:
    case TOK_VEC_DOT: case TOK_VEC_CROSS:
    case TOK_VEC_GRAD: case TOK_DIVERGENCE: case TOK_VEC_CURL:
    case TOK_LAPLACE: case TOK_ILAPLACE:
    case TOK_EIGENVAL: case TOK_EIGENVEC:
    case TOK_GRAPHIMPL:
        return parse_numerical_multivariable(p);

    /* ── geometry, lists, matrix ops ───────────────────── */
    case TOK_MIDPOINT: case TOK_DISTANCE: case TOK_SLOPE:
    case TOK_CPLX_I:
    case TOK_LBRACE:
    case TOK_L1: case TOK_L2: case TOK_L3:
    case TOK_L4: case TOK_L5: case TOK_L6:
    case TOK_CUMSUM: case TOK_DELTALIST:
    case TOK_SORTA: case TOK_SORTD: case TOK_DIM:
    case TOK_SEQ:
    case TOK_DET: case TOK_INV: case TOK_RREF: case TOK_TRANSPOSE:
        return parse_geometry_list_matrix(p);

    default:
        set_err(p, "Unexpected token");
        return NULL;
    }
}

/* ── Expression parser (Pratt core) ──────────────────────── */

static ast_node_t *parse_expr(parser_t *p, int min_prec)
{
    ast_node_t *left;
    if (p->depth >= PARSER_MAX_DEPTH) {
        set_err(p, "Expression too deeply nested");
        return NULL;
    }
    p->depth++;
    left = parse_prefix(p);
    if (!left) { p->depth--; return NULL; }

    for (;;) {
        token_type_t tt = cur_type(p);
        int          prec;

        if (at_end(p)) break;

        /* ── Postfix factorial ────────────────────────── */
        if (tt == TOK_FACTORIAL) {
            advance(p);
            left = ast_unop(UOP_FACT, left);
            if (!left) return NULL;
            continue;
        }

        /* ── Postfix percent: n% → n/100 ────────────── */
        if (tt == TOK_PERCENT) {
            advance(p);
            left = ast_binop(OP_DIV, left, ast_number(100.0f));
            if (!left) return NULL;
            continue;
        }

        /* ── Implicit multiplication ──────────────────── */
        if (starts_value(tt) && PREC_MUL > min_prec) {
            ast_node_t *right = parse_expr(p, PREC_MUL);
            if (!right) { ast_free(left); return NULL; }
            left = ast_binop(OP_MUL, left, right);
            if (!left) return NULL;
            continue;
        }

        /* ── Normal infix operators ───────────────────── */
        prec = infix_prec(tt);
        if (prec == PREC_NONE || prec <= min_prec) break;

        advance(p);

        if (tt == TOK_POWER) {
            /* right-associative: use (prec - 1) */
            ast_node_t *right = parse_expr(p, prec - 1);
            if (!right) { ast_free(left); return NULL; }
            left = ast_binop(OP_POW, left, right);
        } else if (tt == TOK_EQUALS) {
            /* a = b  →  a - b  (equation: lhs - rhs = 0) */
            ast_node_t *right = parse_expr(p, prec);
            if (!right) { ast_free(left); return NULL; }
            left = ast_binop(OP_SUB, left, right);
        } else {
            ast_node_t *right = parse_expr(p, prec);
            if (!right) { ast_free(left); return NULL; }
            left = ast_binop(tok_to_binop(tt), left, right);
        }
        if (!left) { p->depth--; return NULL; }
    }

    p->depth--;
    return left;
}

/* ── Public API ──────────────────────────────────────────── */

void parser_init(parser_t *p, const tokenizer_t *tokens)
{
    memset(p, 0, sizeof(*p));
    p->tokens = tokens;
}

ast_node_t *parser_parse(parser_t *p)
{
    ast_node_t *root = parse_expr(p, PREC_NONE);
    if (!root) return NULL;

    /* Handle store: expr -> L1 */
    if (cur_type(p) == TOK_STORE) {
        ast_node_t *store;
        token_type_t lt;
        advance(p);
        lt = cur_type(p);
        if (lt >= TOK_L1 && lt <= TOK_L6) {
            uint8_t idx = (uint8_t)(lt - TOK_L1);
            advance(p);
            store = mem_alloc_node();
            if (!store) { ast_free(root); return NULL; }
            store->type = NODE_STORE;
            store->d.store.expr = root;
            store->d.store.list_idx = idx;
            root = store;
        } else if (lt == TOK_VARIABLE) {
            /* Store to variable: expr -> X */
            char vname = cur(p)->var_name;
            advance(p);
            store = mem_alloc_node();
            if (!store) { ast_free(root); return NULL; }
            store->type = NODE_STORE;
            store->d.store.expr = root;
            /* Encode variable: 128 + var_name distinguishes from list idx 0-5 */
            store->d.store.list_idx = (uint8_t)(128 + vname);
            root = store;
        } else {
            set_err(p, "Expected L1-L6 or variable after ->");
            ast_free(root);
            return NULL;
        }
    }

    if (root && !at_end(p)) {
        set_err(p, "Unexpected token after expression");
        ast_free(root);
        return NULL;
    }
    return root;
}

ce_error_t parser_error(const parser_t *p)
{
    return p->error;
}

const char *parser_error_msg(const parser_t *p)
{
    return p->error_msg;
}
