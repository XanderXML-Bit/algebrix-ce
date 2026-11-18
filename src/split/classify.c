#include "split/classify.h"

static bool is_finance_stat_fn(stat_fn_t fn)
{
    switch (fn) {
    case STAT_FIN_PV:
    case STAT_FIN_FV:
    case STAT_FIN_PMT:
    case STAT_FIN_NPER:
    case STAT_FIN_RATE:
    case STAT_FIN_NOM:
    case STAT_FIN_EFF:
    case STAT_FIN_BAL:
    case STAT_FIN_SUMPRN:
    case STAT_FIN_SUMINT:
        return true;
    default:
        return false;
    }
}

static bool is_probability_stat_fn(stat_fn_t fn)
{
    switch (fn) {
    case STAT_NORMALCDF:
    case STAT_INVNORM:
    case STAT_BINOMPDF:
    case STAT_BINOMCDF:
    case STAT_POISSONPDF:
    case STAT_POISSONCDF:
    case STAT_GEOMETPDF:
    case STAT_GEOMETCDF:
    case STAT_TCDF:
    case STAT_CHI2CDF:
    case STAT_NORMALPDF:
    case STAT_INVT:
    case STAT_FPDF:
    case STAT_FCDF:
    case STAT_INVCHI2:
    case STAT_INVF:
        return true;
    default:
        return false;
    }
}

/* True when node contains a complex part, or (lists) a list part too.
 * lists=false is the classifier's complex test; lists=true is the store
 * rule's "not a plain real scalar" test (ce_classify_expression). */
static bool contains_complex_node_ex(const ast_node_t *node, bool lists)
{
    if (!node) return false;

    switch (node->type) {
    case NODE_LIST_REF:
        return lists;
    case NODE_CONSTANT:
        return node->d.constant == CONST_I;
    case NODE_FUNC:
        if (node->d.func.id == FN_REAL || node->d.func.id == FN_IMAG ||
            node->d.func.id == FN_CONJ || node->d.func.id == FN_ANGLE) {
            return true;
        }
        return contains_complex_node_ex(node->d.func.arg, lists);
    case NODE_BINOP:
        return contains_complex_node_ex(node->d.binop.left, lists) ||
               contains_complex_node_ex(node->d.binop.right, lists);
    case NODE_UNOP:
        return contains_complex_node_ex(node->d.unop.operand, lists);
    case NODE_DERIV:
        return contains_complex_node_ex(node->d.deriv.expr, lists);
    case NODE_INTEGRAL:
        return contains_complex_node_ex(node->d.integral.expr, lists) ||
               contains_complex_node_ex(node->d.integral.lo, lists) ||
               contains_complex_node_ex(node->d.integral.hi, lists);
    case NODE_LIMIT:
        return contains_complex_node_ex(node->d.limit.expr, lists) ||
               contains_complex_node_ex(node->d.limit.point, lists);
    case NODE_SUM:
        return contains_complex_node_ex(node->d.sum.expr, lists) ||
               contains_complex_node_ex(node->d.sum.lo, lists) ||
               contains_complex_node_ex(node->d.sum.hi, lists);
    case NODE_TAYLOR:
        return contains_complex_node_ex(node->d.taylor.expr, lists) ||
               contains_complex_node_ex(node->d.taylor.center, lists) ||
               contains_complex_node_ex(node->d.taylor.degree, lists);
    case NODE_FOURIER:
        return contains_complex_node_ex(node->d.fourier.expr, lists) ||
               contains_complex_node_ex(node->d.fourier.nterms, lists);
    case NODE_EXPAND:
        return contains_complex_node_ex(node->d.expand.expr, lists);
    case NODE_FACTOR:
        return contains_complex_node_ex(node->d.factor.expr, lists);
    case NODE_SOLVE:
        return contains_complex_node_ex(node->d.solve.expr, lists);
    case NODE_TANGENT:
        return contains_complex_node_ex(node->d.tangent.expr, lists) ||
               contains_complex_node_ex(node->d.tangent.point, lists);
    case NODE_PAIR:
        return contains_complex_node_ex(node->d.pair.head, lists) ||
               contains_complex_node_ex(node->d.pair.tail, lists);
    case NODE_STAT:
        return contains_complex_node_ex(node->d.stat.args, lists);
    case NODE_MATRIX_OP:
        return contains_complex_node_ex(node->d.matrix_op.arg, lists);
    case NODE_POLAR:
        return contains_complex_node_ex(node->d.polar.expr1, lists) ||
               contains_complex_node_ex(node->d.polar.expr2, lists) ||
               contains_complex_node_ex(node->d.polar.lo, lists) ||
               contains_complex_node_ex(node->d.polar.hi, lists);
    case NODE_LIST_LITERAL:
        return lists || contains_complex_node_ex(node->d.list_literal.elems, lists);
    case NODE_LIST_OP:
        return lists || contains_complex_node_ex(node->d.list_op.arg, lists);
    case NODE_LIST_ELEM:
        return lists || contains_complex_node_ex(node->d.list_elem.index, lists);
    case NODE_STORE:
        /* A list store (list_idx < 128) inside the value is a list part. */
        return (lists && node->d.store.list_idx < 128) ||
               contains_complex_node_ex(node->d.store.expr, lists);
    case NODE_PIECEWISE:
        return contains_complex_node_ex(node->d.piecewise.pieces, lists);
    case NODE_PRODUCT:
        return contains_complex_node_ex(node->d.product.expr, lists) ||
               contains_complex_node_ex(node->d.product.lo, lists) ||
               contains_complex_node_ex(node->d.product.hi, lists);
    default:
        return false;
    }
}

static bool contains_complex_node(const ast_node_t *node)
{
    return contains_complex_node_ex(node, false);
}

ce_domain_t ce_classify_expression(const ast_node_t *root, bool input_had_equals)
{
    if (!root) return CE_DOMAIN_UNKNOWN;

    switch (root->type) {
    case NODE_DERIV:
        return CE_DOMAIN_CALC;

    case NODE_TANGENT:
        return CE_DOMAIN_CALC_TANGENT;

    case NODE_LIMIT:
        return CE_DOMAIN_CALC_LIMIT;

    case NODE_INTEGRAL:
        return CE_DOMAIN_CALC_INT;

    case NODE_TAYLOR:
    case NODE_FOURIER:
        return CE_DOMAIN_CALC_SERIES;

    case NODE_SUM:
    case NODE_PRODUCT:
        return CE_DOMAIN_CALC_SEQ;

    case NODE_EXPAND:
    case NODE_FACTOR:
        return CE_DOMAIN_ALG_EXPFAC;

    case NODE_SOLVE:
        return CE_DOMAIN_ALG;

    case NODE_STAT:
        if (is_finance_stat_fn(root->d.stat.fn)) return CE_DOMAIN_FIN;
        if (is_probability_stat_fn(root->d.stat.fn)) return CE_DOMAIN_PROB;
        return CE_DOMAIN_STAT;

    case NODE_MATRIX:
    case NODE_MATRIX_OP:
        return CE_DOMAIN_MATRIX;

    case NODE_STORE:
        /* A scalar store (`expr->X`, list_idx >= 128) of a plain real value
         * goes to CEBASE (CE_DOMAIN_CORE), whose checked arithmetic refuses
         * tiny values (#140) before anything is stored. List stores, and a
         * value with a list or complex part, stay in CELSCX, which stores
         * that value with its own (unchecked) arithmetic: a known
         * limitation. */
        if (root->d.store.list_idx >= 128 &&
            ce_classify_expression(root->d.store.expr, input_had_equals) == CE_DOMAIN_CORE &&
            !contains_complex_node_ex(root->d.store.expr, true))
            return CE_DOMAIN_CORE;
        return CE_DOMAIN_CMPLX;

    case NODE_LIST_LITERAL:
    case NODE_LIST_REF:
    case NODE_LIST_OP:
    case NODE_LIST_ELEM:
        return CE_DOMAIN_CMPLX;

    case NODE_POLAR:
        switch (root->d.polar.op) {
        case POLOP_GRAPH:
        case POLOP_GRAPHEQ:
        case POLOP_SLOPEFIELD:
            return CE_DOMAIN_GRAPH;

        case POLOP_TO_RECT:
        case POLOP_FROM_RECT:
            return CE_DOMAIN_CMPLX;

        case POLOP_UNITCIRC:
        case POLOP_LINE2PT:
        case POLOP_LINEPT:
        case POLOP_PARABOLA:
        case POLOP_TOFRAC:
        case POLOP_MIDPOINT:
        case POLOP_DISTANCE:
        case POLOP_SLOPE:
            return CE_DOMAIN_ALG_GEO;

        case POLOP_CSQUARE:
        case POLOP_PFRAC:
        case POLOP_DESCARTES:
        case POLOP_NLSOLVE:
            return CE_DOMAIN_ALG_ADV;

        case POLOP_POLYDIV:
            return CE_DOMAIN_ALG;

        case POLOP_ARCLEN:
        case POLOP_ARCLEN_PARAM:
        case POLOP_SURFREV:
        case POLOP_DISK:
        case POLOP_WASHER:
        case POLOP_SHELL:
        case POLOP_DBLINT:
            return CE_DOMAIN_CALC_POLAR;

        case POLOP_RELRATE:
        case POLOP_CONTINUITY:
            return CE_DOMAIN_CALC_LIMIT;

        case POLOP_NEWTON:
        case POLOP_BISECT:
        case POLOP_SECANT:
        case POLOP_EULER:
        case POLOP_PDIFF:
        case POLOP_GRADIENT:
        case POLOP_CRITICAL:
            return CE_DOMAIN_CALC_TANGENT;

        case POLOP_ODE_SEP:
        case POLOP_ODE_LINEAR:
        case POLOP_DIFFEQ:
            return CE_DOMAIN_CALC;

        case POLOP_INTPARTS:
        case POLOP_TRIGSUB:
        case POLOP_USUB:
        case POLOP_INTSTRATEGY:
            return CE_DOMAIN_CALC_INT;

        case POLOP_RECSEQ:
        case POLOP_ARITHSUM:
        case POLOP_GEOMSUM:
            return CE_DOMAIN_CALC_SEQ;

        /* New Pass 74 operations */
        case POLOP_DSOLVE:
            return CE_DOMAIN_CALC;

        case POLOP_POLARAREA:
        case POLOP_POLARARCLEN:
            return CE_DOMAIN_CALC_POLAR;

        case POLOP_VEC_DOT:
        case POLOP_VEC_CROSS:
        case POLOP_VEC_GRAD:
        case POLOP_VEC_DIV:
        case POLOP_VEC_CURL:
            return CE_DOMAIN_CALC_TANGENT;  /* multivariable/vector bucket */

        case POLOP_LAPLACE:
        case POLOP_ILAPLACE:
            return CE_DOMAIN_CALC;

        case POLOP_CONVERT:
            return CE_DOMAIN_CORE;

        /* New Pass 75 operations */
        case POLOP_DSOLVE2:
            return CE_DOMAIN_CALC;

        case POLOP_EIGENVAL:
        case POLOP_EIGENVEC:
            return CE_DOMAIN_MATRIX;

        case POLOP_GRAPHIMPL:
            return CE_DOMAIN_GRAPH;

        default:
            return CE_DOMAIN_CALC;
        }

    default:
        if (input_had_equals) {
            return CE_DOMAIN_ALG;
        }
        if (contains_complex_node(root)) {
            return CE_DOMAIN_CMPLX;
        }
        return CE_DOMAIN_CORE;
    }
}
