/*
 * ast.h — Abstract Syntax Tree node definitions
 * Tagged-union nodes, pool-allocated via memory.h.
 */
#ifndef AST_H
#define AST_H

#include "calcengine.h"
#include "stats.h"    /* for stat_fn_t */

/* ── Node kind ───────────────────────────────────────────── */
typedef enum {
    NODE_NUMBER,          /* literal: 3.14                       */
    NODE_VARIABLE,        /* variable: x, y, t                   */
    NODE_CONSTANT,        /* named constant: pi, e               */
    NODE_BINOP,           /* binary op: a + b, a ^ b             */
    NODE_UNOP,            /* unary op: -x, x!                    */
    NODE_FUNC,            /* function call: sin(x)               */
    NODE_DERIV,           /* derivative: d/dx(expr)              */
    NODE_INTEGRAL,        /* integral: int(expr, var, lo, hi)    */
    NODE_LIMIT,           /* limit: lim(expr, var, pt, dir)      */
    NODE_SUM,             /* summation: sum(expr, var, lo, hi)   */
    NODE_TAYLOR,          /* taylor(expr, var, center, deg)      */
    NODE_FOURIER,         /* fourier(expr, var, nterms)          */
    NODE_EXPAND,          /* expand(expr)                        */
    NODE_FACTOR,          /* factor(expr)                        */
    NODE_SOLVE,           /* solve(expr, var)                    */
    NODE_PAIR,            /* cons cell for argument lists         */
    NODE_STAT,            /* statistics: mean, median, etc.       */
    NODE_TANGENT,         /* tangent(expr, var, point)            */
    NODE_MATRIX,          /* matrix literal (index into store)    */
    NODE_MATRIX_OP,       /* matrix operation: det, inv, etc.     */
    NODE_POLAR,           /* polar/parametric/advanced operations  */
    NODE_LIST_LITERAL,    /* {1,2,3} — list literal (PAIR chain)  */
    NODE_LIST_REF,        /* L1-L6 reference                      */
    NODE_LIST_OP,         /* list operation (cumsum, dim, etc.)    */
    NODE_LIST_ELEM,       /* L1(3) — element access (1-indexed)   */
    NODE_STORE,           /* expr -> L1 (store operation)          */
    NODE_PIECEWISE,       /* piecewise(e1,c1,e2,c2,...,default)    */
    NODE_PRODUCT          /* product(expr, var, lo, hi)             */
} node_type_t;

/* ── Binary / unary operator tags ────────────────────────── */
typedef enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW } binop_t;
typedef enum { UOP_NEG, UOP_FACT } unop_t;

/* ── Built-in function id ────────────────────────────────── */
typedef enum {
    FN_SIN, FN_COS, FN_TAN,
    FN_SEC, FN_CSC, FN_COT,
    FN_ASIN, FN_ACOS, FN_ATAN,
    FN_LN, FN_LOG, FN_SQRT,
    FN_ABS, FN_EXP,
    FN_SINH, FN_COSH, FN_TANH,
    FN_ASINH, FN_ACOSH, FN_ATANH,
    FN_ASEC, FN_ACSC, FN_ACOT,
    FN_SECH, FN_CSCH, FN_COTH,
    FN_FLOOR, FN_CEIL, FN_ROUND, FN_IPART, FN_FPART,
    FN_DEC, FN_HEX, FN_BIN, FN_OCT,
    FN_REAL, FN_IMAG, FN_CONJ, FN_ANGLE,
    FN_SIGN, FN_HEAVISIDE
} func_id_t;

/* ── Named-constant id ───────────────────────────────────── */
typedef enum { CONST_PI, CONST_E, CONST_INF, CONST_I } const_id_t;

/* stat_fn_t comes from stats.h */

/* ── Matrix operation id ────────────────────────────────── */
typedef enum {
    MAT_LITERAL,    /* mat(1,2;3,4) — raw data in arg PAIR chain */
    MAT_DET, MAT_INV, MAT_RREF, MAT_TRANSPOSE
} matrix_op_t;

/* ── Polar/parametric operation id ─────────────────────── */
typedef enum {
    POLOP_TO_RECT,         /* polar(r, theta) -> (x, y)           */
    POLOP_FROM_RECT,       /* rect(x, y) -> (r, theta)            */
    POLOP_ARCLEN,          /* arclen(f, var, a, b)                 */
    POLOP_ARCLEN_PARAM,    /* arclen(xt, yt, var, a, b)            */
    POLOP_SURFREV,         /* surfarea(f, var, a, b)               */
    POLOP_DISK,            /* disk(f, var, a, b)                    */
    POLOP_WASHER,          /* washer(f, g, var, a, b)               */
    POLOP_SHELL,           /* shell(f, var, a, b)                   */
    POLOP_RELRATE,         /* relrate(expr) — related rates          */
    POLOP_CSQUARE,         /* csquare(expr,var) — complete the square */
    POLOP_PFRAC,           /* pfrac(expr,var) — partial fractions     */
    POLOP_DESCARTES,       /* descartes(expr,var) — Descartes' rule   */
    POLOP_LINE2PT,         /* line(x1,y1,x2,y2) — line from 2 points */
    POLOP_LINEPT,          /* linept(m,x1,y1) — line from slope+point */
    POLOP_PARABOLA,        /* parabola(vx,vy,px,py) — parabola vertex */
    POLOP_CONTINUITY,      /* continuity(expr,var,pt)                  */
    POLOP_TOFRAC,          /* tofrac(expr) — decimal to fraction       */
    POLOP_RECSEQ,          /* recseq(expr,var,init,k) — recursive seq  */
    POLOP_UNITCIRC,        /* unitcirc(angle_deg) — unit circle lookup */
    POLOP_NLSOLVE,         /* nlsolve(e1,e2,v1,v2) — nonlinear system  */
    POLOP_ARITHSUM,        /* arithsum(a1,d,n) — arithmetic series sum */
    POLOP_GEOMSUM,         /* geomsum(a1,r,n) — geometric series sum   */
    POLOP_MIDPOINT,        /* midpoint(x1,y1,x2,y2) — midpoint formula */
    POLOP_DISTANCE,        /* distance(x1,y1,x2,y2) — distance formula */
    POLOP_SLOPE,           /* slope(x1,y1,x2,y2) — slope formula       */
    POLOP_NEWTON,          /* newton(expr,var,x0) — Newton's method     */
    POLOP_BISECT,          /* bisect(expr,var,a,b) — Bisection method   */
    POLOP_SECANT,          /* secant(expr,var,x0,x1) — Secant method    */
    POLOP_PDIFF,           /* pdiff(expr,v1,v2,...) — mixed partials    */
    POLOP_GRADIENT,        /* gradient(expr,v1,v2) — gradient vector    */
    POLOP_DBLINT,          /* dblint(expr,x,xlo,xhi,y,ylo,yhi)         */
    POLOP_CRITICAL,        /* critical(expr,var) — critical points       */
    POLOP_POLYDIV,         /* polydiv(p,q,var) — polynomial long div     */
    POLOP_GRAPH,           /* graph(expr,var[,xmin,xmax]) — function plot */
    POLOP_GRAPHEQ,         /* grapheq(lhs,rhs,var[,xmin,xmax]) — equation graph */
    POLOP_ODE_SEP,         /* ode_sep(rhs,x,y) — separable ODE solver     */
    POLOP_ODE_LINEAR,      /* ode_linear(rhs,x,y) — first-order linear ODE */
    POLOP_DIFFEQ,          /* diffeq(rhs,x,y) — auto ODE strategy          */
    POLOP_EULER,           /* euler(rhs,x,y,x0,y0,xf,h)                    */
    POLOP_SLOPEFIELD,      /* slopefield(rhs,x,y[,xmin,xmax,ymin,ymax])    */
    POLOP_INTPARTS,        /* intparts(expr,var) — force integration by parts */
    POLOP_TRIGSUB,         /* trigsub(expr,var) — force trig substitution     */
    POLOP_USUB,            /* usub(expr,var,u) — force u-substitution         */
    POLOP_INTSTRATEGY,     /* intstrategy(expr,var) — rank integration methods */
    POLOP_AREABETWEEN,     /* areabetween(f,g,var,a,b) — area between curves  */
    POLOP_IMPROPER,        /* improper(expr,var,lo,hi) — explicit improper integral */
    /* ODE solver */
    POLOP_DSOLVE,          /* dsolve(eq,y,x) — auto ODE solver (sep/lin/exact) */
    /* Polar calculus */
    POLOP_POLARAREA,       /* polararea(r,t,a,b) — area ½∫r²dθ                 */
    POLOP_POLARARCLEN,     /* polararclen(r,t,a,b) — arc length ∫√(r²+r'²)dθ   */
    /* Vector calculus */
    POLOP_VEC_DOT,         /* dot(u,v) — dot product (PAIR chains)              */
    POLOP_VEC_CROSS,       /* cross(u,v) — 3D cross product (PAIR chains)       */
    POLOP_VEC_GRAD,        /* grad(f,x,y[,z]) — gradient vector                 */
    POLOP_VEC_DIV,         /* div(Fx,Fy[,Fz],x,y[,z]) — divergence             */
    POLOP_VEC_CURL,        /* curl(Fx,Fy,Fz,x,y,z) — curl vector               */
    /* Laplace transforms */
    POLOP_LAPLACE,         /* laplace(f,t,s) — forward Laplace transform        */
    POLOP_ILAPLACE,        /* ilaplace(F,s,t) — inverse Laplace transform       */
    /* Unit conversion */
    POLOP_CONVERT,         /* convert(val,from,to) — unit conversion            */
    /* Second-order ODE solver */
    POLOP_DSOLVE2,         /* dsolve2(a,b,c,f,x) — a*y''+b*y'+c*y=f(x)         */
    /* Eigenvalues / eigenvectors */
    POLOP_EIGENVAL,        /* eigenval(matrix) — eigenvalues of a matrix         */
    POLOP_EIGENVEC,        /* eigenvec(matrix) — eigenvectors of a matrix        */
    /* Implicit curve graphing */
    POLOP_GRAPHIMPL        /* graphimpl(eq,x,y[,xmin,xmax,ymin,ymax])           */
} polar_op_t;

/* ── AST Node (tagged union) ─────────────────────────────── */
struct ast_node {
    node_type_t type;
    union {
        /* NODE_NUMBER */
        ce_float_t number;

        /* NODE_VARIABLE */
        char variable;

        /* NODE_CONSTANT */
        const_id_t constant;

        /* NODE_BINOP */
        struct { binop_t op; ast_node_t *left; ast_node_t *right; } binop;

        /* NODE_UNOP */
        struct { unop_t op; ast_node_t *operand; } unop;

        /* NODE_FUNC */
        struct { func_id_t id; ast_node_t *arg; } func;

        /* NODE_DERIV */
        struct { char var; ast_node_t *expr; } deriv;

        /* NODE_INTEGRAL  (lo/hi NULL → indefinite) */
        struct { char var; ast_node_t *expr;
                 ast_node_t *lo; ast_node_t *hi; } integral;

        /* NODE_LIMIT  (direction: -1 left, 0 both, +1 right) */
        struct { char var; ast_node_t *expr;
                 ast_node_t *point; int8_t direction; } limit;

        /* NODE_SUM */
        struct { char var; ast_node_t *expr;
                 ast_node_t *lo; ast_node_t *hi; } sum;

        /* NODE_TAYLOR */
        struct { char var; ast_node_t *expr;
                 ast_node_t *center; ast_node_t *degree; } taylor;

        /* NODE_FOURIER */
        struct { char var; ast_node_t *expr;
                 ast_node_t *nterms; } fourier;

        /* NODE_EXPAND */
        struct { char var; ast_node_t *expr; } expand;

        /* NODE_FACTOR */
        struct { char var; ast_node_t *expr; } factor;

        /* NODE_SOLVE */
        struct { char var; ast_node_t *expr; } solve;

        /* NODE_TANGENT */
        struct { char var; ast_node_t *expr; ast_node_t *point; } tangent;

        /* NODE_PAIR  (cons cell: head + tail for argument lists) */
        struct { ast_node_t *head; ast_node_t *tail; } pair;

        /* NODE_STAT  (statistics function with argument list) */
        struct { stat_fn_t fn; ast_node_t *args; } stat;

        /* NODE_MATRIX  (index into static matrix store) */
        struct { uint8_t store_idx; uint8_t rows; uint8_t cols; } matrix;

        /* NODE_MATRIX_OP  (operation on a matrix expression) */
        struct { matrix_op_t op; ast_node_t *arg;
                 uint8_t rows; uint8_t cols; } matrix_op;

        /* NODE_POLAR  (polar/parametric/advanced operations) */
        struct { polar_op_t op; char var;
                 ast_node_t *expr1; ast_node_t *expr2;
                 ast_node_t *lo; ast_node_t *hi; } polar;

        /* NODE_LIST_LITERAL  (PAIR chain of elements) */
        struct { ast_node_t *elems; } list_literal;

        /* NODE_LIST_REF  (index 0-5 for L1-L6) */
        struct { uint8_t idx; } list_ref;

        /* NODE_LIST_OP  (list function with args) */
        struct { uint8_t fn; ast_node_t *arg; } list_op;

        /* NODE_LIST_ELEM  (list_ref index + element index expr) */
        struct { uint8_t list_idx; ast_node_t *index; } list_elem;

        /* NODE_STORE  (expr -> list_ref) */
        struct { ast_node_t *expr; uint8_t list_idx; } store;

        /* NODE_PIECEWISE  (PAIR chain: expr,cond,expr,cond,...,default) */
        struct { ast_node_t *pieces; } piecewise;

        /* NODE_PRODUCT  (product notation: prod(expr, var, lo, hi)) */
        struct { char var; ast_node_t *expr;
                 ast_node_t *lo; ast_node_t *hi; } product;
    } d;
};

/* ── Convenience constructors (allocate from pool) ───────── */
ast_node_t *ast_number(ce_float_t val);
ast_node_t *ast_variable(char var);
ast_node_t *ast_constant(const_id_t c);
ast_node_t *ast_binop(binop_t op, ast_node_t *left, ast_node_t *right);
ast_node_t *ast_unop(unop_t op, ast_node_t *operand);
ast_node_t *ast_func(func_id_t fn, ast_node_t *arg);

/* Recursively free an AST subtree back to the pool */
void ast_free(ast_node_t *node);

/* Deep-copy an entire AST subtree (allocates new nodes from pool) */
ast_node_t *ast_clone(const ast_node_t *node);

/* Check whether an AST subtree contains a given variable */
bool ast_contains_var(const ast_node_t *node, char var);

/* Structural equality of two AST subtrees (tolerance-based for numbers) */
bool ast_equal(const ast_node_t *a, const ast_node_t *b);
/* Bit-exact structural equality (unknown node types compare unequal) */
bool ast_same_exact(const ast_node_t *a, const ast_node_t *b);
/* Integral bounds whose equal values mean zero width: identical trees or
 * two leaves */
bool ast_same_bound(const ast_node_t *a, const ast_node_t *b);

/* False if a BINOP/UNOP/FUNC node is missing an operand (pool exhausted
 * while the tree was built), or for a NULL tree. */
bool ast_tree_complete(const ast_node_t *n);

/* Check if node is a number with a specific value (tolerance-based) */
bool ast_is_num(const ast_node_t *n, ce_float_t v);

/* Render AST to a string in the canonical form: every binary operation is
 * wrapped in parentheses and '*' is always written, e.g. ((3*(X^2))+2).
 * Steps, checkpoints and internal text keep this form.  Returns chars
 * written. */
int ast_to_str(const ast_node_t *node, char *buf, int size);

/* Render AST for a result line: minimal parentheses, and '*' is dropped
 * after a numeric literal when the next factor starts with a variable, pi,
 * a function name other than exp, or '(' (3X^2+2, 2(X+1),
 * cos(X)*X^2+sin(X)*(2X)). Operator grouping is preserved when re-parsed
 * by parser.c. Numbers retain the historical six-significant-digit format.
 * Negative NUMBER leaves re-parse as unary minus on a positive NUMBER.
 * A string cut by the buffer size ends in "..." when size >= 4.  Use it only where the
 * whole string stands on its own or is followed by "+ C", "=", "," or ")";
 * next to + or - use ast_to_display_term().  Returns chars written. */
int ast_to_display_str(const ast_node_t *node, char *buf, int size);

/* As ast_to_display_str(), but wrapped in parentheses unless the node binds
 * at least as tightly as '*' and does not start with a unary minus, so the
 * text is safe as either operand of + or - (A - (Y+Z), A + (-X), A - 2X).
 * It is NOT safe after '/' or next to '^' (A/X/2, X^2^3): wrap it yourself
 * there. */
int ast_to_display_term(const ast_node_t *node, char *buf, int size);

#endif /* AST_H */
