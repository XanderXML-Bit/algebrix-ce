/*
 * token.h — Token types and tokenizer
 * Converts an input string into a stream of tokens.
 */
#ifndef TOKEN_H
#define TOKEN_H

#include "calcengine.h"

/* ── Token types ─────────────────────────────────────────── */
typedef enum {
    /* Literals */
    TOK_NUMBER,           /* 3, 3.14, .5          */
    TOK_VARIABLE,         /* x, y, z, t, n        */

    /* Operators */
    TOK_PLUS,             /* +                    */
    TOK_MINUS,            /* -                    */
    TOK_MULTIPLY,         /* *                    */
    TOK_DIVIDE,           /* /                    */
    TOK_POWER,            /* ^                    */
    TOK_FACTORIAL,        /* !                    */

    /* Grouping / punctuation */
    TOK_LPAREN,           /* (                    */
    TOK_RPAREN,           /* )                    */
    TOK_COMMA,            /* ,                    */
    TOK_EQUALS,           /* =                    */

    /* Built-in functions */
    TOK_SIN,
    TOK_COS,
    TOK_TAN,
    TOK_ASIN,
    TOK_ACOS,
    TOK_ATAN,
    TOK_SEC,
    TOK_CSC,
    TOK_COT,
    TOK_LN,
    TOK_LOG,
    TOK_SQRT,
    TOK_ABS,
    TOK_EXP,

    /* Constants */
    TOK_PI,               /* pi                   */
    TOK_E,                /* e (Euler's number)   */
    TOK_INF,              /* inf (infinity)       */

    /* Calculus operators */
    TOK_DERIV,            /* d/dx                 */
    TOK_NDERIV,           /* nderiv(expr,var,n)   */
    TOK_INTEGRAL,         /* int(                 */
    TOK_DIFFVAR,          /* differential: dx     */
    TOK_LIMIT,            /* lim(                 */
    TOK_SIGMA,            /* sum(                 */
    TOK_TAYLOR,           /* taylor(              */
    TOK_FOURIER,          /* fourier(             */
    TOK_SEQLIM,           /* seqlim(expr,var)     */

    /* Algebra commands */
    TOK_EXPAND,           /* expand(              */
    TOK_FACTOR,           /* factor(              */
    TOK_SOLVE,            /* solve(               */

    /* Statistics functions */
    TOK_MEAN,             /* mean(                */
    TOK_MEDIAN,           /* median(              */
    TOK_MODE,             /* mode(                */
    TOK_STDEV,            /* stdev(               */
    TOK_VARIANCE,         /* var(                 */
    TOK_LINREG,           /* linreg(              */
    TOK_NCR,              /* ncr(                 */
    TOK_NPR,              /* npr(                 */

    /* Matrix operations */
    TOK_MAT,              /* mat(                 */
    TOK_MAT2,             /* mat2(a,b,c,d) 2×2    */
    TOK_DET,              /* det(                 */
    TOK_INV,              /* inv(                 */
    TOK_RREF,             /* rref(                */
    TOK_TRANSPOSE,        /* transpose(           */
    TOK_SEMICOLON,        /* ;  (matrix row sep)  */

    /* Inverse trig (reciprocal) */
    TOK_ASEC,             /* asec(                */
    TOK_ACSC,             /* acsc(                */
    TOK_ACOT,             /* acot(                */

    /* Hyperbolic functions */
    TOK_SINH,             /* sinh(                */
    TOK_COSH,             /* cosh(                */
    TOK_TANH,             /* tanh(                */
    TOK_ASINH,            /* asinh(               */
    TOK_ACOSH,            /* acosh(               */
    TOK_ATANH,            /* atanh(               */
    TOK_SECH,             /* sech(                */
    TOK_CSCH,             /* csch(                */
    TOK_COTH,             /* coth(                */

    /* Extended math */
    TOK_FLOOR,            /* floor(               */
    TOK_CEIL,             /* ceil(                */
    TOK_ROUND,            /* round(               */
    TOK_IPART,            /* ipart(               */
    TOK_FPART,            /* fpart(               */

    /* Multi-arg math */
    TOK_GCD,              /* gcd(                 */
    TOK_LCM,              /* lcm(                 */
    TOK_MOD,              /* mod(                 */
    TOK_RANDINT,          /* randint(             */
    TOK_RAND,             /* rand                 */

    /* Tangent line */
    TOK_TANGENT,          /* tangent(             */

    /* Polar / parametric / advanced */
    TOK_POLAR,            /* polar(               */
    TOK_RECT,             /* rect(                */
    TOK_ARCLEN,           /* arclen(              */
    TOK_SURFAREA,         /* surfarea(            */
    TOK_DISK,             /* disk(                */
    TOK_WASHER,           /* washer(              */
    TOK_SHELL,            /* shell(               */
    TOK_AREABETWEEN,      /* areabetween(         */

    /* Related rates */
    TOK_RELRATE,          /* relrate(             */

    /* Probability distributions */
    TOK_NORMALCDF,        /* normalcdf(           */
    TOK_INVNORM,          /* invnorm(             */
    TOK_BINOMPDF,         /* binompdf(            */
    TOK_BINOMCDF,         /* binomcdf(            */
    TOK_POISSONPDF,       /* poissonpdf(          */
    TOK_POISSONCDF,       /* poissoncdf(          */
    TOK_GEOMETPDF,        /* geometpdf(           */
    TOK_GEOMETCDF,        /* geometcdf(           */
    TOK_TCDF,             /* tcdf(                */
    TOK_CHI2CDF,          /* chi2cdf(             */
    TOK_NORMALPDF,        /* normalpdf(           */
    TOK_INVT,             /* invt(                */

    /* Additional regressions */
    TOK_QUADREG,          /* quadreg(             */
    TOK_EXPREG,           /* expreg(              */
    TOK_PWRREG,           /* pwrreg(              */
    TOK_LNREG,            /* lnreg(               */

    /* Number base conversions */
    TOK_DEC,              /* dec(                 */
    TOK_HEX,              /* hex(                 */
    TOK_BIN,              /* bin(                 */
    TOK_OCT,              /* oct(                 */

    /* Finance TVM */
    TOK_FIN_PV,           /* pv(                  */
    TOK_FIN_FV,           /* fv(                  */
    TOK_FIN_PMT,          /* pmt(                 */
    TOK_FIN_NPER,         /* nper(                */
    TOK_FIN_RATE,         /* rate(                */
    TOK_FIN_NOM,          /* nom(                 */
    TOK_FIN_EFF,          /* eff(                 */
    TOK_FIN_BAL,          /* bal(                 */
    TOK_FIN_SUMPRN,       /* sumprn(              */
    TOK_FIN_SUMINT,       /* sumint(              */

    /* 2-Var Stats */
    TOK_COVAR,            /* covar(               */
    TOK_2VARSTATS,        /* 2varstats(           */

    /* List operations */
    TOK_LBRACE,           /* {                    */
    TOK_RBRACE,           /* }                    */
    TOK_L1, TOK_L2, TOK_L3, TOK_L4, TOK_L5, TOK_L6,
    TOK_CUMSUM,           /* cumsum(              */
    TOK_DELTALIST,        /* deltalist(           */
    TOK_SORTA,            /* sorta(               */
    TOK_SORTD,            /* sortd(               */
    TOK_SEQ,              /* seq(                 */
    TOK_DIM,              /* dim(                 */
    TOK_STORE,            /* ->                   */

    /* Complex number support */
    TOK_CPLX_I,           /* i (imaginary unit)   */
    TOK_REAL,             /* real(                */
    TOK_IMAG,             /* imag(                */
    TOK_CONJ,             /* conj(                */
    TOK_ANGLE,            /* angle(               */

    /* Statistical tests */
    TOK_ZTEST,            /* ztest(               */
    TOK_TTEST,            /* ttest(               */
    TOK_2SAMPZTEST,       /* 2sampztest(          */
    TOK_2SAMPTTEST,       /* 2sampttest(          */
    TOK_1PROPZTEST,       /* 1propztest(          */
    TOK_2PROPZTEST,       /* 2propztest(          */
    TOK_CHI2TEST,         /* chi2test(            */
    TOK_ZINTERVAL,        /* zinterval(           */
    TOK_TINTERVAL,        /* tinterval(           */
    TOK_1PROPZINT,        /* 1propzint(           */
    TOK_2PROPZINT,        /* 2propzint(           */
    TOK_2SAMPZINT,        /* 2sampzint(           */
    TOK_2SAMPTINT,        /* 2samptint(           */

    /* Piecewise */
    TOK_PIECEWISE,        /* piecewise(           */

    /* Algebra/calculus wrappers */
    TOK_CSQUARE,          /* csquare(             */
    TOK_PFRAC,            /* pfrac(               */
    TOK_DESCARTES,        /* descartes(           */
    TOK_LINE2PT,          /* line(                */
    TOK_LINEPT,           /* linept(              */
    TOK_PARABOLA,         /* parabola(            */
    TOK_CONTINUITY,       /* continuity(          */
    TOK_TOFRAC,           /* tofrac(              */

    /* Previous answer */
    TOK_ANS,              /* ans                  */

    /* Recursive sequences */
    TOK_RECSEQ,           /* recseq(              */

    /* F-distribution */
    TOK_FPDF,             /* fpdf(                */
    TOK_FCDF,             /* fcdf(                */

    /* Unit circle */
    TOK_UNITCIRC,         /* unitcirc(            */

    /* Inverse chi-squared */
    TOK_INVCHI2,          /* invchi2(             */

    /* Nonlinear system solving */
    TOK_NLSOLVE,          /* nlsolve(             */

    /* Inverse F-distribution */
    TOK_INVF,             /* invf(                */

    /* Arithmetic/geometric series sums */
    TOK_ARITHSUM,         /* arithsum(            */
    TOK_GEOMSUM,          /* geomsum(             */

    /* Coordinate geometry */
    TOK_MIDPOINT,         /* midpoint(            */
    TOK_DISTANCE,         /* distance(            */
    TOK_SLOPE,            /* slope(               */

    /* Percentage */
    TOK_PERCENT,          /* %                    */

    /* Numerical methods */
    TOK_NEWTON,           /* newton(              */
    TOK_BISECT,           /* bisect(              */
    TOK_SECANT,           /* secant(              */

    /* Product notation */
    TOK_PRODUCT,          /* prod(                */

    /* Sign / Heaviside */
    TOK_SIGN,             /* sign(                */
    TOK_HEAVISIDE,        /* heaviside(           */

    /* Multivariable calculus */
    TOK_PDIFF,            /* pdiff(               */
    TOK_GRADIENT,         /* gradient(            */
    TOK_DBLINT,           /* dblint(              */
    TOK_CRITICAL,         /* critical(            */

    /* Polynomial long division */
    TOK_POLYDIV,          /* polydiv(             */

    /* Graphing */
    TOK_GRAPH,            /* graph(               */
    TOK_GRAPHEQ,          /* grapheq(             */

    /* Differential equations / integration strategy */
    TOK_ODE_SEP,          /* ode_sep(             */
    TOK_ODE_LINEAR,       /* ode_linear(          */
    TOK_DIFFEQ,           /* diffeq(              */
    TOK_EULER,            /* euler(               */
    TOK_SLOPEFIELD,       /* slopefield(          */
    TOK_INTPARTS,         /* intparts(            */
    TOK_TRIGSUB,          /* trigsub(             */
    TOK_USUB,             /* usub(                */
    TOK_INTSTRATEGY,      /* intstrategy(         */
    TOK_IMPROPER,         /* improper(            */

    /* Additional statistical tests */
    TOK_LINREGTTEST,      /* linregttest(         */
    TOK_ANOVA,            /* anova(               */
    TOK_RESID,            /* resid(               */

    /* ODE solver */
    TOK_DSOLVE,           /* dsolve(              */
    TOK_DSOLVE2,          /* dsolve2(             */

    /* Polar calculus */
    TOK_POLARAREA,        /* polararea(           */
    TOK_POLARARCLEN,      /* polararclen(         */

    /* Vector calculus */
    TOK_VEC_DOT,          /* dot(                 */
    TOK_VEC_CROSS,        /* cross(               */
    TOK_VEC_GRAD,         /* grad(                */
    TOK_DIVERGENCE,       /* div(                 */
    TOK_VEC_CURL,         /* curl(                */

    /* Laplace transforms */
    TOK_LAPLACE,          /* laplace(             */
    TOK_ILAPLACE,         /* ilaplace(            */

    /* Unit conversion */
    TOK_CONVERT,          /* convert(             */

    /* Eigenvalues / eigenvectors */
    TOK_EIGENVAL,         /* eigenval(            */
    TOK_EIGENVEC,         /* eigenvec(            */

    /* Implicit graphing */
    TOK_GRAPHIMPL,        /* graphimpl(           */

    /* Custom functions */
    TOK_DEFINE,           /* define(              */
    TOK_UNDEF_FN,         /* undef(               */
    TOK_CLEARFUNCS,       /* clearfuncs(          */
    TOK_WS_CMD,           /* ws(                  */
    TOK_FUNCS_CMD,        /* funcs(               */

    /* Bookmarks */
    TOK_BOOKMARK_CMD,     /* bookmark(            */
    TOK_DELBM,            /* delbm(               */
    TOK_CLEARBM,          /* clearbm(             */

    /* Sentinel */
    TOK_EOF,
    TOK_ERROR
} token_type_t;

/* ── Token ───────────────────────────────────────────────── */
typedef struct {
    token_type_t type;
    ce_float_t   num_val;              /* valid when type == TOK_NUMBER   */
    char         var_name;             /* valid for variable/deriv/diffvar */
    char         text[MAX_TOKEN_LEN];  /* raw text (debug / display)      */
} token_t;

/* ── Tokenizer state ─────────────────────────────────────── */
typedef struct {
    const char  *src;
    uint16_t     pos;
    uint16_t     len;
    token_t      tokens[MAX_TOKENS];
    uint16_t     count;
    ce_error_t   error;
    uint16_t     err_pos;   /* on CE_ERR_SYNTAX: source offset of the unknown
                               character or the start of the unknown word;
                               on CE_ERR_PRECISION: the start of a nonzero
                               literal that would read as 0 or a tiny
                               value (token.c number_lost);
                               on CE_ERR_OVERFLOW: the start of a literal
                               too long to read (token.c scan_number); a
                               MAX_TOKENS overflow is also CE_ERR_OVERFLOW,
                               at 0, but the launcher's 127-character input
                               cannot reach it */
} tokenizer_t;

/* Initialize tokenizer with an input string */
void tok_init(tokenizer_t *t, const char *input);

/* Tokenize the entire input.  Returns CE_OK or an error code. */
ce_error_t tok_tokenize(tokenizer_t *t);

/* Retrieve token at index (NULL if out of bounds) */
const token_t *tok_get(const tokenizer_t *t, uint16_t index);

/* Debug helper: token type → human-readable name */
const char *tok_type_name(token_type_t type);

/* Is this token type a built-in function? */
bool tok_is_function(token_type_t type);

#endif /* TOKEN_H */
