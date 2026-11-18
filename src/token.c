/*
 * token.c — Tokenizer implementation
 *
 * Scans an input string and produces a flat array of tokens.
 * Recognises numbers, single-letter variables, operators,
 * function keywords (sin, cos, …), constants (pi, e),
 * and the d/dx derivative shorthand.
 */
#include "token.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ── Internal helpers ────────────────────────────────────── */

/* Portable case-insensitive string compare */
static int ci_strcmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

static char peek(const tokenizer_t *t)
{
    return (t->pos < t->len) ? t->src[t->pos] : '\0';
}

static char next_char(tokenizer_t *t)
{
    return (t->pos < t->len) ? t->src[t->pos++] : '\0';
}

static char peek_at(const tokenizer_t *t, uint16_t offset)
{
    uint16_t i = t->pos + offset;
    return (i < t->len) ? t->src[i] : '\0';
}

/* Append a token; returns false on overflow. */
static bool emit(tokenizer_t *t, token_type_t type, const char *text,
                 ce_float_t num, char var)
{
    token_t *tok;
    if (t->count >= MAX_TOKENS) {
        t->error = CE_ERR_OVERFLOW;
        return false;
    }
    tok = &t->tokens[t->count++];
    tok->type     = type;
    tok->num_val  = num;
    tok->var_name = var;
    strncpy(tok->text, text, MAX_TOKEN_LEN - 1);
    tok->text[MAX_TOKEN_LEN - 1] = '\0';
    return true;
}

/* ── Number scanner ──────────────────────────────────────── */

/*
 * Value of a scanned number literal (digits, optional '.', optional
 * exponent). On the CE, CEdev's atof() wraps out-of-range literals instead
 * of giving inf: on the OS 5.8 ROM 1E39 read as 2.35e-38, 1E99 as 2.35e22
 * and 3.5E38 as NaN (internal review). So the range is decided from the
 * digits first: a literal at or above FLT_MAX + 1/2 ulp is +inf (what an
 * IEEE host's float conversion gives), one below 1e-46 is 0, and atof()
 * only sees literals it converts correctly.
 */
static ce_float_t number_value(const char *buf)
{
    /* Significant digits of FLT_MAX + 1/2 ulp = 3.4028235677973366e38. */
    static const char limit[] = "34028235677973366";
    char sig[sizeof(limit)];
    const char *p = buf;
    long dexp = 0, e = 0;
    int nsig = 0, seen = 0, frac = 0, eneg = 0;

    for (; *p && *p != 'e' && *p != 'E'; p++) {
        if (*p == '.') { frac = 1; continue; }
        if (!seen && *p == '0') {
            if (frac) dexp--;          /* 0.00x: one decade lower per zero */
            continue;
        }
        if (!seen) { seen = 1; if (frac) dexp--; }
        else if (!frac) dexp++;        /* each further integer digit */
        if (nsig < (int)sizeof(sig) - 1) sig[nsig++] = *p;
    }
    if (!seen) return 0.0f;
    if (*p) {                          /* exponent */
        p++;
        if (*p == '+' || *p == '-') eneg = (*p++ == '-');
        for (; *p; p++)
            if (e < 10000) e = e * 10 + (*p - '0');
    }
    dexp += eneg ? -e : e;             /* decimal exponent of the first digit */
    if (dexp > 38) return CE_INF;
    if (dexp < -46) return 0.0f;
    if (dexp == 38) {
        while (nsig < (int)sizeof(sig) - 1) sig[nsig++] = '0';
        sig[nsig] = '\0';
        if (strcmp(sig, limit) >= 0) return CE_INF;
        {
            /* In [1e38, FLT_MAX + 1/2 ulp): CE atof() reads a few ulp high
             * (3.4E38 by 5), so close to the limit it can wrap. A result
             * outside [5e37, FLT_MAX] (NaN compares above FLT_MAX on the
             * CE; a wrap is NaN, tiny or negative) is such a wrap of a value
             * that rounds to FLT_MAX. */
            ce_float_t v = (ce_float_t)atof(buf);
            if (!(v >= 5e37f && v <= 3.40282347e38f)) v = 3.40282347e38f;
            return v;
        }
    }
    return (ce_float_t)atof(buf);
}

/* True when the literal's significand is not 0: a digit 1-9 in buf
 * before its exponent, or one cut off past the buffer (cut_nz). */
static bool number_nonzero(const char *buf, bool cut_nz)
{
    if (cut_nz) return true;          /* a nonzero digit past the buffer */
    for (; *buf && *buf != 'e' && *buf != 'E'; buf++)
        if (*buf >= '1' && *buf <= '9') return true;
    return false;
}

/*
 * True when a literal with a nonzero digit reads as +-0 or as a value
 * whose exponent field is 0. CE atof() returns 0 for 1.3E-38, although
 * that is a normal float (internal review), and a literal below FLT_MIN
 * would be a tiny value the CE's arithmetic cannot use (ce_float_ops.h).
 * Such a literal is refused (CE_ERR_PRECISION), never read as 0. True
 * zero is told by its digits, not by a float compare.
 */
static bool number_lost(const char *buf, ce_float_t v, bool cut_nz)
{
    volatile unsigned char *p = (volatile unsigned char *)&v;
    if ((p[3] & 0x7Fu) || (p[2] & 0x80u)) return false;   /* exponent != 0 */
    return number_nonzero(buf, cut_nz);
}

static bool scan_number(tokenizer_t *t)
{
    char buf[MAX_TOKEN_LEN];
    int  n = 0;
    uint16_t start = t->pos;
    ce_float_t v;
    bool cut_nz = false;      /* a significand digit 1-9 did not fit in buf */
    bool cut_e = false;       /* an exponent character did not fit in buf */

    while (t->pos < t->len && isdigit((unsigned char)peek(t)))
        if (n < MAX_TOKEN_LEN - 1) buf[n++] = next_char(t);
        else if (next_char(t) != '0') cut_nz = true;

    if (peek(t) == '.') {
        if (n < MAX_TOKEN_LEN - 1) buf[n++] = next_char(t); else next_char(t);
        while (t->pos < t->len && isdigit((unsigned char)peek(t)))
            if (n < MAX_TOKEN_LEN - 1) buf[n++] = next_char(t);
            else if (next_char(t) != '0') cut_nz = true;
    }

    /* Scientific notation: 1.5e3, 2E-4, 1e+10 */
    if (t->pos < t->len && (peek(t) == 'e' || peek(t) == 'E')) {
        char next = peek_at(t, 1);
        if (isdigit((unsigned char)next) || next == '+' || next == '-') {
            if (n < MAX_TOKEN_LEN - 1) buf[n++] = next_char(t);
            else { next_char(t); cut_e = true; }
            if (peek(t) == '+' || peek(t) == '-') {
                if (n < MAX_TOKEN_LEN - 1) {
                    buf[n++] = next_char(t);
                } else {
                    next_char(t);
                    cut_e = true;
                }
            }
            while (t->pos < t->len && isdigit((unsigned char)peek(t)))
                if (n < MAX_TOKEN_LEN - 1) buf[n++] = next_char(t);
                else { next_char(t); cut_e = true; }
        }
    }

    buf[n] = '\0';
    /* A literal too long to read is refused ("Number too long"), never
     * misread, when:
     *  - an exponent character was dropped: that changes the magnitude
     *    (the first 15 characters of 1.0000000000E-99 read as 1e-9, which
     *    would pass the tiny check below; review round 9), unless the
     *    literal is a true zero, which is 0 whatever its exponent;
     *  - every nonzero digit was dropped: 0.00000000000001 is 1e-14, not
     *    0 and not tiny (round 9 nit).
     * Dropped fraction digits otherwise only round. Dropped integer digits
     * shrink the value by powers of ten (100000000000000000000 reads as
     * 1e14), a misread main has too; they cannot let a tiny literal
     * through, so this slice leaves them (follow-up). */
    if ((cut_e && number_nonzero(buf, cut_nz)) ||
        (cut_nz && !number_nonzero(buf, false))) {
        t->error   = CE_ERR_OVERFLOW;
        t->err_pos = start;
        return false;
    }
    v = number_value(buf);
    if (number_lost(buf, v, cut_nz)) {
        t->error   = CE_ERR_PRECISION;
        t->err_pos = start;
        return false;
    }
    return emit(t, TOK_NUMBER, buf, v, 0);
}

/* ── Keyword table ───────────────────────────────────────── */

typedef struct { const char *name; token_type_t type; } kw_t;

static const kw_t kw_table[] = {
    { "sin",    TOK_SIN  },  { "cos",    TOK_COS  },
    { "tan",    TOK_TAN  },  { "sec",    TOK_SEC  },
    { "csc",    TOK_CSC  },  { "cot",    TOK_COT  },
    { "asin",   TOK_ASIN },  { "acos",   TOK_ACOS },
    { "atan",   TOK_ATAN },  { "arcsin", TOK_ASIN },
    { "arccos", TOK_ACOS },  { "arctan", TOK_ATAN },
    { "asec",   TOK_ASEC },  { "acsc",   TOK_ACSC },
    { "acot",   TOK_ACOT },
    { "ln",     TOK_LN   },
    { "log",    TOK_LOG  },  { "sqrt",   TOK_SQRT },
    { "abs",    TOK_ABS  },  { "exp",    TOK_EXP  },
    { "pi",     TOK_PI   },  { "inf",    TOK_INF  },
    { "lim",    TOK_LIMIT},  { "seqlim", TOK_SEQLIM },
    { "nderiv", TOK_NDERIV},  { "deriv", TOK_DERIV },
    { "sum",    TOK_SIGMA},  { "int",    TOK_INTEGRAL },
    { "taylor", TOK_TAYLOR}, { "fourier",TOK_FOURIER },
    { "expand", TOK_EXPAND}, { "factor", TOK_FACTOR },
    { "solve",  TOK_SOLVE },
    { "mean",   TOK_MEAN },   { "median",   TOK_MEDIAN },
    { "mode",   TOK_MODE },   { "stdev",    TOK_STDEV },
    { "variance",TOK_VARIANCE},{ "var",     TOK_VARIANCE },
    { "linreg", TOK_LINREG },
    { "ncr",    TOK_NCR },    { "npr",    TOK_NPR },
    { "mat",    TOK_MAT  },   { "mat2",     TOK_MAT2 },
    { "det",      TOK_DET },
    { "inv",    TOK_INV  },   { "rref",     TOK_RREF },
    { "transpose",TOK_TRANSPOSE },
    { "tangent", TOK_TANGENT },
    { "polar",   TOK_POLAR },    { "rect",    TOK_RECT },
    { "arclen",  TOK_ARCLEN },   { "surfarea",TOK_SURFAREA },
    { "disk",    TOK_DISK },     { "washer",  TOK_WASHER },
    { "shell",   TOK_SHELL },   { "areabetween", TOK_AREABETWEEN },
    { "relrate", TOK_RELRATE },
    { "sinh",    TOK_SINH },      { "cosh",    TOK_COSH },
    { "tanh",    TOK_TANH },      { "asinh",   TOK_ASINH },
    { "acosh",   TOK_ACOSH },     { "atanh",   TOK_ATANH },
    { "sech",    TOK_SECH },      { "csch",    TOK_CSCH },
    { "coth",    TOK_COTH },
    { "piecewise", TOK_PIECEWISE },
    { "floor",   TOK_FLOOR },     { "ceil",    TOK_CEIL },
    { "round",   TOK_ROUND },     { "ipart",   TOK_IPART },
    { "fpart",   TOK_FPART },
    { "gcd",     TOK_GCD },       { "lcm",     TOK_LCM },
    { "mod",     TOK_MOD },       { "randint", TOK_RANDINT },
    { "rand",    TOK_RAND },
    { "normalcdf",  TOK_NORMALCDF },  { "invnorm",    TOK_INVNORM },
    { "binompdf",   TOK_BINOMPDF },   { "binomcdf",   TOK_BINOMCDF },
    { "poissonpdf", TOK_POISSONPDF }, { "poissoncdf", TOK_POISSONCDF },
    { "geometpdf",  TOK_GEOMETPDF },  { "geometcdf",  TOK_GEOMETCDF },
    { "tcdf",       TOK_TCDF },       { "chi2cdf",    TOK_CHI2CDF },
    { "normalpdf",  TOK_NORMALPDF },  { "invt",       TOK_INVT },
    { "quadreg", TOK_QUADREG },   { "expreg",  TOK_EXPREG },
    { "pwrreg",  TOK_PWRREG },    { "lnreg",   TOK_LNREG },
    { "dec",     TOK_DEC },       { "hex",     TOK_HEX },
    { "bin",     TOK_BIN },       { "oct",     TOK_OCT },
    { "pv",      TOK_FIN_PV },    { "fv",      TOK_FIN_FV },
    { "pmt",     TOK_FIN_PMT },   { "nper",    TOK_FIN_NPER },
    { "rate",    TOK_FIN_RATE },
    { "nom",     TOK_FIN_NOM },     { "eff",     TOK_FIN_EFF },
    { "bal",     TOK_FIN_BAL },     { "sumprn",  TOK_FIN_SUMPRN },
    { "sumint",  TOK_FIN_SUMINT },
    /* 2-Var Stats */
    { "covar",   TOK_COVAR },
    { "2varstats", TOK_2VARSTATS },
    /* List references */
    { "L1", TOK_L1 }, { "L2", TOK_L2 }, { "L3", TOK_L3 },
    { "L4", TOK_L4 }, { "L5", TOK_L5 }, { "L6", TOK_L6 },
    /* List operations */
    { "cumsum",    TOK_CUMSUM },   { "deltalist", TOK_DELTALIST },
    { "sorta",     TOK_SORTA },    { "sortd",     TOK_SORTD },
    { "seq",       TOK_SEQ },      { "dim",       TOK_DIM },
    /* Complex number support */
    { "real",    TOK_REAL },       { "imag",    TOK_IMAG },
    { "conj",    TOK_CONJ },      { "angle",   TOK_ANGLE },
    /* Algebra/calculus wrappers */
    { "csquare",   TOK_CSQUARE },     { "pfrac",     TOK_PFRAC },
    { "descartes", TOK_DESCARTES },   { "line",      TOK_LINE2PT },
    { "linept",    TOK_LINEPT },      { "parabola",  TOK_PARABOLA },
    { "continuity",TOK_CONTINUITY },  { "tofrac",    TOK_TOFRAC },
    /* Previous answer */
    { "ans",       TOK_ANS },
    /* Statistical tests */
    { "ztest",       TOK_ZTEST },        { "ttest",       TOK_TTEST },
    { "2sampztest",  TOK_2SAMPZTEST },   { "2sampttest",  TOK_2SAMPTTEST },
    { "1propztest",  TOK_1PROPZTEST },   { "2propztest",  TOK_2PROPZTEST },
    { "chi2test",    TOK_CHI2TEST },
    { "zinterval",   TOK_ZINTERVAL },    { "tinterval",   TOK_TINTERVAL },
    { "1propzint",   TOK_1PROPZINT },    { "2propzint",   TOK_2PROPZINT },
    { "2sampzint",   TOK_2SAMPZINT },    { "2samptint",   TOK_2SAMPTINT },
    /* Additional stat tests */
    { "linregttest", TOK_LINREGTTEST },  { "anova",       TOK_ANOVA },
    { "resid",       TOK_RESID },
    /* Recursive sequences */
    { "recseq",      TOK_RECSEQ },
    /* F-distribution */
    { "fpdf",        TOK_FPDF },        { "fcdf",        TOK_FCDF },
    /* Unit circle */
    { "unitcirc",    TOK_UNITCIRC },
    /* Inverse chi-squared */
    { "invchi2",     TOK_INVCHI2 },
    /* Nonlinear system solving */
    { "nlsolve",     TOK_NLSOLVE },
    /* Inverse F-distribution */
    { "invf",        TOK_INVF },
    /* Arithmetic/geometric series sums */
    { "arithsum",    TOK_ARITHSUM },   { "geomsum",     TOK_GEOMSUM },
    /* Coordinate geometry */
    { "midpoint",    TOK_MIDPOINT },   { "distance",    TOK_DISTANCE },
    { "slope",       TOK_SLOPE },
    /* Numerical methods */
    { "newton",      TOK_NEWTON },     { "bisect",      TOK_BISECT },
    { "secant",      TOK_SECANT },
    /* Product notation */
    { "prod",        TOK_PRODUCT },
    /* Sign / Heaviside */
    { "sign",        TOK_SIGN },       { "sgn",         TOK_SIGN },
    { "heaviside",   TOK_HEAVISIDE },
    /* Multivariable calculus */
    { "pdiff",       TOK_PDIFF },      { "gradient",    TOK_GRADIENT },
    { "dblint",      TOK_DBLINT },     { "critical",    TOK_CRITICAL },
    /* Polynomial long division */
    { "polydiv",     TOK_POLYDIV },
    /* Graphing */
    { "graph",       TOK_GRAPH },
    { "grapheq",     TOK_GRAPHEQ },
    /* Differential equations / integration strategy */
    { "ode_sep",     TOK_ODE_SEP },    { "ode_linear",  TOK_ODE_LINEAR },
    { "odelinear",   TOK_ODE_LINEAR }, { "diffeq",      TOK_DIFFEQ },
    { "euler",       TOK_EULER },      { "slopefield",  TOK_SLOPEFIELD },
    { "dirfield",    TOK_SLOPEFIELD },
    { "intparts",    TOK_INTPARTS },   { "trigsub",     TOK_TRIGSUB },
    { "usub",        TOK_USUB },       { "intstrategy", TOK_INTSTRATEGY },
    { "improper",    TOK_IMPROPER },
    /* ODE solver */
    { "dsolve",      TOK_DSOLVE },     { "dsolve2",     TOK_DSOLVE2 },
    /* Polar calculus */
    { "polararea",   TOK_POLARAREA },    { "polararclen", TOK_POLARARCLEN },
    /* Vector calculus */
    { "dot",         TOK_VEC_DOT },      { "cross",       TOK_VEC_CROSS },
    { "grad",        TOK_VEC_GRAD },     { "div",         TOK_DIVERGENCE },
    { "curl",        TOK_VEC_CURL },
    /* Laplace transforms */
    { "laplace",     TOK_LAPLACE },      { "ilaplace",    TOK_ILAPLACE },
    /* Unit conversion */
    { "convert",     TOK_CONVERT },
    /* Eigenvalues / eigenvectors */
    { "eigenval",    TOK_EIGENVAL },   { "eigenvec",    TOK_EIGENVEC },
    /* Implicit graphing */
    { "graphimpl",   TOK_GRAPHIMPL },
    /* Custom functions */
    { "define",      TOK_DEFINE },       { "undef",       TOK_UNDEF_FN },
    { "clearfuncs",  TOK_CLEARFUNCS },   { "ws",          TOK_WS_CMD },
    { "funcs",       TOK_FUNCS_CMD },
    /* Bookmarks */
    { "bookmark",    TOK_BOOKMARK_CMD }, { "delbm",       TOK_DELBM },
    { "clearbm",     TOK_CLEARBM },
    { NULL, TOK_ERROR }
};

/* ── Word scanner (functions / variables / constants) ────── */

static bool scan_word(tokenizer_t *t)
{
    char buf[MAX_TOKEN_LEN];
    int  n = 0, i;
    uint16_t start = t->pos;

    /* Read identifier chars, then try extended forms like chi2cdf. */
    while (t->pos < t->len &&
           (isalpha((unsigned char)peek(t)) || peek(t) == '_'))
        if (n < MAX_TOKEN_LEN - 1) buf[n++] = next_char(t); else next_char(t);

    /* Check if extending with digits+alpha matches a keyword (e.g. chi2cdf) */
    if (t->pos < t->len && isdigit((unsigned char)peek(t))) {
        int saved_n = n;
        uint16_t saved_pos = t->pos;
        char extended[MAX_TOKEN_LEN];
        int en = n;
        memcpy(extended, buf, n);
        while (t->pos < t->len &&
               (isalpha((unsigned char)peek(t)) ||
                isdigit((unsigned char)peek(t)) ||
                peek(t) == '_'))
            if (en < MAX_TOKEN_LEN - 1) extended[en++] = next_char(t); else next_char(t);
        extended[en] = '\0';
        /* Check if extended form matches a keyword (case-insensitive) */
        for (i = 0; kw_table[i].name; i++) {
            if (ci_strcmp(extended, kw_table[i].name) == 0)
                return emit(t, kw_table[i].type, extended, 0, 0);
        }
        /* No match — revert position and use alpha-only buffer */
        t->pos = saved_pos;
        n = saved_n;
    }

    buf[n] = '\0';

    /* Match keyword (case-insensitive) */
    for (i = 0; kw_table[i].name; i++) {
        if (ci_strcmp(buf, kw_table[i].name) == 0)
            return emit(t, kw_table[i].type, buf, 0, 0);
    }

    /* Single-letter 'e'/'E' → Euler's number */
    if (n == 1 && tolower((unsigned char)buf[0]) == 'e')
        return emit(t, TOK_E, "e", 0, 0);

    /* Single-letter 'i'/'I' → imaginary unit when complex mode active */
    if (n == 1 && tolower((unsigned char)buf[0]) == 'i' && g_settings.complex_mode != COMPLEX_OFF)
        return emit(t, TOK_CPLX_I, "i", 0, 0);

    /* Single letter → variable */
    if (n == 1)
        return emit(t, TOK_VARIABLE, buf, 0, buf[0]);

    /* Differential shorthand: dx, dy, dt, ... */
    if (n == 2 &&
        tolower((unsigned char)buf[0]) == 'd' &&
        isalpha((unsigned char)buf[1])) {
        char dvar[3];
        dvar[0] = 'd';
        /* Keep the variable's case: the CE types DX for dX (internal review). */
        dvar[1] = buf[1];
        dvar[2] = '\0';
        return emit(t, TOK_DIFFVAR, dvar, 0, dvar[1]);
    }

    /* Unknown multi-letter word */
    t->error   = CE_ERR_SYNTAX;
    t->err_pos = start;
    return false;
}

/* ── Public API ──────────────────────────────────────────── */

void tok_init(tokenizer_t *t, const char *input)
{
    memset(t, 0, sizeof(*t));
    t->src = input;
    t->len = (uint16_t)strlen(input);
}

ce_error_t tok_tokenize(tokenizer_t *t)
{
    char c, s[2] = {0, 0};

    while (t->pos < t->len) {
        c = peek(t);

        /* whitespace */
        if (c == ' ' || c == '\t') { next_char(t); continue; }

        /* Digit-leading keywords: 1propztest, 2sampztest, 2varstats, etc. */
        if (isdigit((unsigned char)c) && isalpha((unsigned char)peek_at(t, 1))) {
            char kbuf[MAX_TOKEN_LEN];
            int kn = 0, ki;
            uint16_t saved_pos = t->pos;
            bool kw_found = false;
            while (t->pos < t->len && (isalpha((unsigned char)t->src[t->pos]) ||
                                        isdigit((unsigned char)t->src[t->pos]))) {
                if (kn < MAX_TOKEN_LEN - 1) kbuf[kn++] = t->src[t->pos];
                t->pos++;
            }
            kbuf[kn] = '\0';
            for (ki = 0; kw_table[ki].name; ki++) {
                if (ci_strcmp(kbuf, kw_table[ki].name) == 0) {
                    if (!emit(t, kw_table[ki].type, kbuf, 0, 0)) return t->error;
                    kw_found = true;
                    break;
                }
            }
            if (kw_found) continue;
            t->pos = saved_pos;  /* revert — parse as number */
        }

        /* numbers (also handles leading-dot decimals like .5) */
        if (isdigit((unsigned char)c) ||
            (c == '.' && isdigit((unsigned char)peek_at(t, 1)))) {
            if (!scan_number(t)) return t->error;
            continue;
        }

        /* words / d/dx derivative notation */
        if (isalpha((unsigned char)c)) {
            /* LI/ or LI\x83 shorthand for lim.  Before 2026-11 the CE
               launcher's ALPHA table was shifted from M onward, so ALPHA+÷
               could not produce 'M' and users ended up with "LI÷(".  The
               launcher now types M from ALPHA+÷ like the keycap says; this
               shorthand stays as a backward-compatible fallback (it also
               accepts the raw TI OS ÷ token 0x83). */
            if (tolower((unsigned char)c) == 'l' &&
                tolower((unsigned char)peek_at(t, 1)) == 'i' &&
                (peek_at(t, 2) == '/' || (unsigned char)peek_at(t, 2) == 0x83u) &&
                peek_at(t, 3) == '(') {
                t->pos += 3; /* consume 'I', '/', leaving '(' as next token */
                if (!emit(t, TOK_LIMIT, "lim", 0, 0)) return t->error;
                continue;
            }
            /* d/d<var> derivative shorthand — accept ASCII '/' or TI OS_TOK_DIVIDE (0x83) */
            if (tolower((unsigned char)c) == 'd' &&
                (peek_at(t, 1) == '/' || (unsigned char)peek_at(t, 1) == 0x83u) &&
                tolower((unsigned char)peek_at(t, 2)) == 'd' &&
                isalpha((unsigned char)peek_at(t, 3))) {
                char var = (char)peek_at(t, 3);
                t->pos += 4;
                if (!emit(t, TOK_DERIV, "d/d", 0, var)) return t->error;
                continue;
            }
            if (!scan_word(t)) return t->error;
            continue;
        }

        /* single-character tokens */
        next_char(t);
        s[0] = c;
        switch (c) {
        case '+': if (!emit(t, TOK_PLUS,      s, 0, 0)) return t->error; break;
        case '-':
            /* Check for -> (store) */
            if (peek(t) == '>') {
                next_char(t);
                if (!emit(t, TOK_STORE, "->", 0, 0)) return t->error;
            } else {
                if (!emit(t, TOK_MINUS, s, 0, 0)) return t->error;
            }
            break;
        case '*': if (!emit(t, TOK_MULTIPLY,   s, 0, 0)) return t->error; break;
        case '/': if (!emit(t, TOK_DIVIDE,     s, 0, 0)) return t->error; break;
        case '^': if (!emit(t, TOK_POWER,      s, 0, 0)) return t->error; break;
        case '!': if (!emit(t, TOK_FACTORIAL,  s, 0, 0)) return t->error; break;
        case '(': if (!emit(t, TOK_LPAREN,     s, 0, 0)) return t->error; break;
        case ')': if (!emit(t, TOK_RPAREN,     s, 0, 0)) return t->error; break;
        case ',': if (!emit(t, TOK_COMMA,      s, 0, 0)) return t->error; break;
        case '=': if (!emit(t, TOK_EQUALS,     s, 0, 0)) return t->error; break;
        case ';': if (!emit(t, TOK_SEMICOLON,  s, 0, 0)) return t->error; break;
        case '{': if (!emit(t, TOK_LBRACE,     s, 0, 0)) return t->error; break;
        case '}': if (!emit(t, TOK_RBRACE,     s, 0, 0)) return t->error; break;
        case '%': if (!emit(t, TOK_PERCENT,    s, 0, 0)) return t->error; break;
        case '\x83': /* TI OS_TOK_DIVIDE (÷ key without ALPHA in os_GetStringInput) */
            s[0] = '/';
            if (!emit(t, TOK_DIVIDE, s, 0, 0)) return t->error;
            break;
        case '\xC2': /* TI OS_TOK_SIN (sin key without ALPHA; auto-opens paren on CE) */
            if (!emit(t, TOK_SIN, "sin", 0, 0)) return t->error;
            s[0] = '(';
            if (!emit(t, TOK_LPAREN, s, 0, 0)) return t->error;
            break;
        default:
            t->error   = CE_ERR_SYNTAX;
            t->err_pos = (uint16_t)(t->pos - 1);   /* next_char() took it */
            return t->error;
        }
    }

    /* sentinel */
    emit(t, TOK_EOF, "", 0, 0);
    return CE_OK;
}

const token_t *tok_get(const tokenizer_t *t, uint16_t index)
{
    return (index < t->count) ? &t->tokens[index] : NULL;
}

bool tok_is_function(token_type_t type)
{
    /* Explicit switch instead of fragile range-based checks.
     * Enum reordering no longer silently breaks this logic. */
    switch (type) {
    /* Trig */
    case TOK_SIN: case TOK_COS: case TOK_TAN:
    case TOK_ASIN: case TOK_ACOS: case TOK_ATAN:
    case TOK_SEC: case TOK_CSC: case TOK_COT:
    case TOK_ASEC: case TOK_ACSC: case TOK_ACOT:
    /* Hyperbolic */
    case TOK_SINH: case TOK_COSH: case TOK_TANH:
    case TOK_ASINH: case TOK_ACOSH: case TOK_ATANH:
    case TOK_SECH: case TOK_CSCH: case TOK_COTH:
    /* Log / exp / root / abs */
    case TOK_LN: case TOK_LOG: case TOK_SQRT:
    case TOK_ABS: case TOK_EXP:
    /* Rounding / parts */
    case TOK_FLOOR: case TOK_CEIL: case TOK_ROUND:
    case TOK_IPART: case TOK_FPART:
    /* Base conversions */
    case TOK_DEC: case TOK_HEX: case TOK_BIN: case TOK_OCT:
    /* Complex accessors */
    case TOK_REAL: case TOK_IMAG: case TOK_CONJ: case TOK_ANGLE:
    /* Sign / Heaviside */
    case TOK_SIGN: case TOK_HEAVISIDE:
    /* ODE solver */
    case TOK_DSOLVE:
    /* Polar calculus */
    case TOK_POLARAREA: case TOK_POLARARCLEN:
    /* Vector calculus */
    case TOK_VEC_DOT: case TOK_VEC_CROSS:
    case TOK_VEC_GRAD: case TOK_DIVERGENCE: case TOK_VEC_CURL:
    /* Laplace transforms */
    case TOK_LAPLACE: case TOK_ILAPLACE:
    /* Unit conversion */
    case TOK_CONVERT:
        return true;
    default:
        return false;
    }
}

const char *tok_type_name(token_type_t type)
{
    switch (type) {
    case TOK_NUMBER:   return "NUM";    case TOK_VARIABLE: return "VAR";
    case TOK_PLUS:     return "+";      case TOK_MINUS:    return "-";
    case TOK_MULTIPLY: return "*";      case TOK_DIVIDE:   return "/";
    case TOK_POWER:    return "^";      case TOK_FACTORIAL: return "!";
    case TOK_LPAREN:   return "(";      case TOK_RPAREN:   return ")";
    case TOK_COMMA:    return ",";      case TOK_EQUALS:   return "=";
    case TOK_SIN:      return "sin";    case TOK_COS:      return "cos";
    case TOK_TAN:      return "tan";    case TOK_SEC:      return "sec";
    case TOK_CSC:      return "csc";    case TOK_COT:      return "cot";
    case TOK_ASIN:     return "asin";
    case TOK_ACOS:     return "acos";   case TOK_ATAN:     return "atan";
    case TOK_ASEC:     return "asec";   case TOK_ACSC:     return "acsc";
    case TOK_ACOT:     return "acot";
    case TOK_LN:       return "ln";     case TOK_LOG:      return "log";
    case TOK_SQRT:     return "sqrt";   case TOK_ABS:      return "abs";
    case TOK_EXP:      return "exp";    case TOK_PI:       return "pi";
    case TOK_E:        return "e";      case TOK_INF:      return "inf";
    case TOK_DERIV:    return "d/d";    case TOK_NDERIV:   return "nderiv";
    case TOK_INTEGRAL: return "int";    case TOK_DIFFVAR:  return "d?";
    case TOK_LIMIT:    return "lim";
    case TOK_SIGMA:    return "sum";    case TOK_TAYLOR:   return "taylor";
    case TOK_FOURIER:  return "fourier"; case TOK_SEQLIM:  return "seqlim";
    case TOK_EXPAND:   return "expand";  case TOK_FACTOR:   return "factor";
    case TOK_SOLVE:    return "solve";
    case TOK_MEAN:     return "mean";    case TOK_MEDIAN:   return "median";
    case TOK_MODE:     return "mode";    case TOK_STDEV:    return "stdev";
    case TOK_VARIANCE: return "var";     case TOK_LINREG:   return "linreg";
    case TOK_NCR:      return "ncr";     case TOK_NPR:      return "npr";
    case TOK_MAT:      return "mat";     case TOK_MAT2:     return "mat2";
    case TOK_DET:      return "det";
    case TOK_INV:      return "inv";     case TOK_RREF:     return "rref";
    case TOK_TRANSPOSE:return "transpose"; case TOK_SEMICOLON:return ";";
    case TOK_TANGENT:  return "tangent";
    case TOK_POLAR:    return "polar";   case TOK_RECT:     return "rect";
    case TOK_ARCLEN:   return "arclen";  case TOK_SURFAREA: return "surfarea";
    case TOK_DISK:     return "disk";    case TOK_WASHER:   return "washer";
    case TOK_SHELL:    return "shell";   case TOK_AREABETWEEN: return "areabetween";
    case TOK_RELRATE:  return "relrate";
    case TOK_SINH:     return "sinh";    case TOK_COSH:     return "cosh";
    case TOK_TANH:     return "tanh";    case TOK_ASINH:    return "asinh";
    case TOK_ACOSH:    return "acosh";   case TOK_ATANH:    return "atanh";
    case TOK_SECH:     return "sech";    case TOK_CSCH:     return "csch";
    case TOK_COTH:     return "coth";
    case TOK_PIECEWISE:return "piecewise";
    case TOK_FLOOR:    return "floor";   case TOK_CEIL:     return "ceil";
    case TOK_ROUND:    return "round";   case TOK_IPART:    return "ipart";
    case TOK_FPART:    return "fpart";
    case TOK_GCD:      return "gcd";     case TOK_LCM:      return "lcm";
    case TOK_MOD:      return "mod";     case TOK_RANDINT:  return "randint";
    case TOK_RAND:     return "rand";
    case TOK_NORMALCDF: return "normalcdf"; case TOK_INVNORM:   return "invnorm";
    case TOK_BINOMPDF:  return "binompdf";  case TOK_BINOMCDF:  return "binomcdf";
    case TOK_POISSONPDF:return "poissonpdf";case TOK_POISSONCDF:return "poissoncdf";
    case TOK_GEOMETPDF: return "geometpdf"; case TOK_GEOMETCDF: return "geometcdf";
    case TOK_TCDF:      return "tcdf";      case TOK_CHI2CDF:   return "chi2cdf";
    case TOK_NORMALPDF: return "normalpdf"; case TOK_INVT:      return "invt";
    case TOK_QUADREG:  return "quadreg";  case TOK_EXPREG:   return "expreg";
    case TOK_PWRREG:   return "pwrreg";   case TOK_LNREG:    return "lnreg";
    case TOK_DEC:      return "dec";      case TOK_HEX:      return "hex";
    case TOK_BIN:      return "bin";      case TOK_OCT:      return "oct";
    case TOK_FIN_PV:   return "pv";       case TOK_FIN_FV:   return "fv";
    case TOK_FIN_PMT:  return "pmt";      case TOK_FIN_NPER: return "nper";
    case TOK_FIN_RATE: return "rate";
    case TOK_FIN_NOM:  return "nom";       case TOK_FIN_EFF:   return "eff";
    case TOK_FIN_BAL:  return "bal";       case TOK_FIN_SUMPRN: return "sumprn";
    case TOK_FIN_SUMINT: return "sumint";
    /* 2-Var Stats */
    case TOK_COVAR:    return "covar";    case TOK_2VARSTATS: return "2varstats";
    /* List operations */
    case TOK_LBRACE:   return "{";        case TOK_RBRACE:    return "}";
    case TOK_L1:       return "L1";       case TOK_L2:        return "L2";
    case TOK_L3:       return "L3";       case TOK_L4:        return "L4";
    case TOK_L5:       return "L5";       case TOK_L6:        return "L6";
    case TOK_CUMSUM:   return "cumsum";   case TOK_DELTALIST: return "deltalist";
    case TOK_SORTA:    return "sorta";    case TOK_SORTD:     return "sortd";
    case TOK_SEQ:      return "seq";      case TOK_DIM:       return "dim";
    case TOK_STORE:    return "->";
    /* Complex */
    case TOK_CPLX_I:   return "i";       case TOK_REAL:      return "real";
    case TOK_IMAG:     return "imag";     case TOK_CONJ:      return "conj";
    case TOK_ANGLE:    return "angle";
    /* Statistical tests */
    case TOK_ZTEST:      return "ztest";       case TOK_TTEST:      return "ttest";
    case TOK_2SAMPZTEST: return "2sampztest";  case TOK_2SAMPTTEST: return "2sampttest";
    case TOK_1PROPZTEST: return "1propztest";  case TOK_2PROPZTEST: return "2propztest";
    case TOK_CHI2TEST:   return "chi2test";
    case TOK_ZINTERVAL:  return "zinterval";   case TOK_TINTERVAL:  return "tinterval";
    case TOK_1PROPZINT:  return "1propzint";   case TOK_2PROPZINT:  return "2propzint";
    case TOK_2SAMPZINT:  return "2sampzint";   case TOK_2SAMPTINT:  return "2samptint";
    /* Additional stat tests */
    case TOK_LINREGTTEST: return "linregttest";
    case TOK_ANOVA:      return "anova";       case TOK_RESID:      return "resid";
    /* Algebra/calculus wrappers */
    case TOK_CSQUARE:  return "csquare"; case TOK_PFRAC:     return "pfrac";
    case TOK_DESCARTES:return "descartes";case TOK_LINE2PT:  return "line";
    case TOK_LINEPT:   return "linept";  case TOK_PARABOLA:  return "parabola";
    case TOK_CONTINUITY:return "continuity"; case TOK_TOFRAC: return "tofrac";
    case TOK_ANS:      return "ans";
    case TOK_RECSEQ:   return "recseq"; case TOK_FPDF:    return "fpdf";
    case TOK_FCDF:     return "fcdf";   case TOK_UNITCIRC: return "unitcirc";
    case TOK_INVCHI2:  return "invchi2";
    case TOK_NLSOLVE:  return "nlsolve";
    case TOK_INVF:     return "invf";
    case TOK_ARITHSUM: return "arithsum"; case TOK_GEOMSUM:  return "geomsum";
    case TOK_MIDPOINT: return "midpoint"; case TOK_DISTANCE: return "distance";
    case TOK_SLOPE:    return "slope";    case TOK_PERCENT:  return "%";
    case TOK_NEWTON:   return "newton";  case TOK_BISECT:   return "bisect";
    case TOK_SECANT:   return "secant";  case TOK_PRODUCT:  return "prod";
    case TOK_SIGN:     return "sign";    case TOK_HEAVISIDE:return "heaviside";
    case TOK_PDIFF:    return "pdiff";  case TOK_GRADIENT: return "gradient";
    case TOK_DBLINT:   return "dblint"; case TOK_CRITICAL: return "critical";
    case TOK_POLYDIV:  return "polydiv";
    case TOK_GRAPH:    return "graph";
    case TOK_GRAPHEQ:  return "grapheq";
    case TOK_ODE_SEP:  return "ode_sep";
    case TOK_ODE_LINEAR:return "ode_linear";
    case TOK_DIFFEQ:   return "diffeq";
    case TOK_EULER:    return "euler";
    case TOK_SLOPEFIELD:return "slopefield";
    case TOK_INTPARTS: return "intparts";
    case TOK_TRIGSUB:  return "trigsub";
    case TOK_USUB:     return "usub";
    case TOK_INTSTRATEGY:return "intstrategy";
    case TOK_IMPROPER: return "improper";
    /* ODE solver */
    case TOK_DSOLVE:   return "dsolve";
    case TOK_DSOLVE2:  return "dsolve2";
    /* Polar calculus */
    case TOK_POLARAREA:    return "polararea";
    case TOK_POLARARCLEN:  return "polararclen";
    /* Vector calculus */
    case TOK_VEC_DOT:  return "dot";
    case TOK_VEC_CROSS:return "cross";
    case TOK_VEC_GRAD: return "grad";
    case TOK_DIVERGENCE:return "div";
    case TOK_VEC_CURL: return "curl";
    /* Laplace transforms */
    case TOK_LAPLACE:  return "laplace";
    case TOK_ILAPLACE: return "ilaplace";
    /* Unit conversion */
    case TOK_CONVERT:  return "convert";
    /* Eigenvalues / eigenvectors */
    case TOK_EIGENVAL: return "eigenval";
    case TOK_EIGENVEC: return "eigenvec";
    /* Implicit graphing */
    case TOK_GRAPHIMPL:return "graphimpl";
    /* Custom functions */
    case TOK_DEFINE:   return "define";
    case TOK_UNDEF_FN: return "undef";
    case TOK_CLEARFUNCS:return "clearfuncs";
    case TOK_WS_CMD:   return "ws";
    case TOK_FUNCS_CMD:return "funcs";
    /* Bookmarks */
    case TOK_BOOKMARK_CMD: return "bookmark";
    case TOK_DELBM:    return "delbm";
    case TOK_CLEARBM:  return "clearbm";
    case TOK_EOF:      return "EOF";
    case TOK_ERROR:    return "ERR";
    default:           return "?";
    }
}
