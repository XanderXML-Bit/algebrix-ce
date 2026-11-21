/*
 * test_ce_fmt.c - desktop test of the CE-safe printf front end (src/ce_fmt.c).
 *
 * The CE libc printf cannot format floats, so ce_vsnprintf renders them
 * itself. This test checks it against the host libc (glibc/MSVC), which
 * formats floats correctly, for every float conversion the engine uses,
 * over values passed as 32-bit floats (ce_float_t), the way the calculator
 * passes them. It also checks the integer/string passthrough, '*', padding,
 * truncation and the return value. The CE build no longer links the libc
 * printf at all, so integer and string conversions are checked too.
 *
 * Build/run: make -C tests fmt-test
 */
#include "ce_fmt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
static int g_approx = 0;

static void check_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got, want);
    }
}

static void check_int(const char *what, int got, int want)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL %s: got %d want %d\n", what, got, want);
    }
}

/* Value of one unit in the last printed digit of a %e/%g result. In
 * 32-bit arithmetic a product such as 0.35f*10 can land exactly on a tie
 * that the exact binary value does not have, so the last digit may differ
 * by one. */
static double last_digit_unit(const char *s)
{
    const char *dot = strchr(s, '.'), *e = strpbrk(s, "eE");
    int decimals = 0, ex = e ? atoi(e + 1) : 0;
    if (dot) {
        const char *q = dot + 1;
        while (*q >= '0' && *q <= '9') { decimals++; q++; }
    }
    return pow(10.0, (double)(ex - decimals));
}

/* Tolerance applies to digits only: the sign or space flag and %g's zero
 * stripping must always match; notation (plain vs e) must match unless
 * allow_notation is set. */
static int same_shape(const char *fmt, const char *got, const char *want, int allow_notation)
{
    const char *ge = strpbrk(got, "eE"), *we = strpbrk(want, "eE");
    const char *gend = ge ? ge : got + strlen(got);
    int gs = got[0] == '-' || got[0] == '+' || got[0] == ' ';
    int ws = want[0] == '-' || want[0] == '+' || want[0] == ' ';
    if (!allow_notation && (ge == NULL) != (we == NULL)) return 0;
    if (gs != ws || (gs && got[0] != want[0])) return 0;
    if (strchr(fmt, 'g') && !strchr(fmt, '#') && memchr(got, '.', (size_t)(gend - got))) {
        if (gend[-1] == '0' || gend[-1] == '.') return 0;    /* not stripped */
    }
    return 1;
}

/* In 32-bit float mode, %.8g and %.9g ask for more digits than a float
 * holds; rounding there can change the notation (0.0001 vs 9.99e-05), so
 * only the notation check is relaxed for them. */
static int beyond_float_digits(const char *fmt)
{
#ifdef CE_FMT_FLOAT_MODE
    const char *dot = strchr(fmt, '.');
    return dot && atoi(dot + 1) >= 8;
#else
    (void)fmt;
    return 0;
#endif
}

/* Compare ce_snprintf with the host snprintf for one float format. */
static void cmp1(const char *fmt, float fv)
{
    char got[96], want[96], what[64];
    double v = (double)fv;
    int rg = ce_snprintf(got, sizeof(got), fmt, v);
    int rw = snprintf(want, sizeof(want), fmt, v);
    snprintf(what, sizeof(what), "%s of %.9g", fmt, v);
    /* ce_fmt rounds half-up on the 32-bit value; glibc rounds the exact
     * binary value half-to-even. With -DCE_FMT_REAL=float (the CE's
     * precision) a product such as 99.95f*10 also lands on a tie that the
     * exact value does not have. Accept a result within one unit of the
     * finer last printed digit of the two (e.g. "100" vs "99.9"), and in
     * float mode anything within 1e-6 relative (%.7g and wider ask for more
     * digits than a 32-bit float reliably holds). Counted separately. */
    if (strcmp(got, want) != 0 && rg == (int)strlen(got) &&
        same_shape(fmt, got, want, beyond_float_digits(fmt))) {
        double a = strtod(got, NULL), b = strtod(want, NULL);
        double ug = last_digit_unit(got), uw = last_digit_unit(want);
        double tol = (ug < uw ? ug : uw) * 1.01;
#ifdef CE_FMT_FLOAT_MODE
        double rel = 1e-6 * fabs(b);
        if (rel > tol) tol = rel;
#endif
        if (fabs(a - b) <= tol) {
            if (getenv("CE_FMT_VERBOSE")) printf("approx %s: %s vs %s\n", what, got, want);
            g_approx++;
            g_pass += 2;
            return;
        }
    }
    check_str(what, got, want);
    check_int(what, rg, rw);
}

/* Every float spec form the engine uses (see the guard in
 * tests/cemu/test_ce_numeric_formatting.py), plus %e variants. */
static const char *const k_float_formats[] = {
    "%g", "%.6g", "%.4g", "%.3g", "%.2g", "%.0g", "%.5g", "%.1g", "%.7g",
    "%.8g", "%.9g", "%+.4g", "%+.1g", "% g", "% .3g", "%e", "%.3e", "%.0e"
};

static const float k_values[] = {
    0.0f, 1.0f, 2.0f, 3.0f, 5.0f, 6.0f, 10.0f, 100.0f, 0.5f, 0.25f, 0.125f,
    1.5f, 2.5f, 0.1f, 0.2f, 0.3f, 1.0f / 3.0f, 2.0f / 3.0f, 3.14159265f,
    2.71828183f, 1.41421356f, 12345.0f, 123456.0f, 999999.0f, 1000000.0f,
    1234567.0f, 0.0001f, 0.00012345f, 0.00001f, 1e-10f, 1e10f, 3.4e38f,
    1.17549435e-38f, 9.99995f, 9.9999f, 99.95f, 0.99999f, 123.456f,
    42.0f, 7.25f, 1e5f, 1e-4f, 1e-5f, 65536.0f, 4294.0f, 0.000123f,
    6.02214e23f, 1.6e-19f, 299792458.0f, 0.70710678f,
    2.45f, 0.15f, 1.005f, 1234.5f, 0.35f, 12.345f, 8.125f, 16777215.0f, 16777217.0f
};

static void test_float_matches_host(void)
{
    size_t i, j;
    for (i = 0; i < sizeof(k_float_formats) / sizeof(k_float_formats[0]); i++) {
        if (strstr(k_float_formats[i], "x=")) continue;
        for (j = 0; j < sizeof(k_values) / sizeof(k_values[0]); j++) {
            cmp1(k_float_formats[i], k_values[j]);
            if (k_values[j] != 0.0f) cmp1(k_float_formats[i], -k_values[j]); /* -0 prints as 0 */
        }
    }
}

static void test_mixed_and_passthrough(void)
{
    char got[128], want[128];
    int rg, rw;

    rg = ce_snprintf(got, sizeof(got), "x=%.6g, y=%.4g;", 2.5, 1.0 / 3.0);
    rw = snprintf(want, sizeof(want), "x=%.6g, y=%.4g;", 2.5, 1.0 / 3.0);
    check_str("two floats", got, want);
    check_int("two floats ret", rg, rw);

    rg = ce_snprintf(got, sizeof(got), "R_%d(%.4g) <= %.6g * %s / %.9g [%c] %lu %u %x %5d|%-4s|%%",
                     3, 0.5, 12.0, "abc", 479001600.0, 'Z', 4000000000UL, 7u, 255, -42, "q");
    rw = snprintf(want, sizeof(want), "R_%d(%.4g) <= %.6g * %s / %.9g [%c] %lu %u %x %5d|%-4s|%%",
                  3, 0.5, 12.0, "abc", 479001600.0, 'Z', 4000000000UL, 7u, 255, -42, "q");
    check_str("mixed", got, want);
    check_int("mixed ret", rg, rw);

    rg = ce_snprintf(got, sizeof(got), "%.*g|%*d|%-*d|%.*s", 3, 3.14159, 5, 42, 4, 7, 2, "hello");
    rw = snprintf(want, sizeof(want), "%.*g|%*d|%-*d|%.*s", 3, 3.14159, 5, 42, 4, 7, 2, "hello");
    check_str("star args", got, want);
    check_int("star args ret", rg, rw);

    rg = ce_snprintf(got, sizeof(got), "%ld %hd %lu %li %X %x %u", -5L, (short)9, 4294967295UL, -2147483647L, 0xBEEFu, 0u, 65535u);
    rw = snprintf(want, sizeof(want), "%ld %hd %lu %li %X %x %u", -5L, (short)9, 4294967295UL, -2147483647L, 0xBEEFu, 0u, 65535u);
    check_str("length modifiers", got, want);
    check_int("length modifiers ret", rg, rw);

    /* Every non-float spec form used in src/ (launcher, processor). */
    rg = ce_snprintf(got, sizeof(got), "%06lu|%-26s|%3d|%c%-12s[%s]|%-3s|%+d|% d|%05d|%-5d|%.3d|%.2s|%*s",
                     42UL, "msg", 7, '>', "Precision", "Float", "ab", 5, 6, -42, 3, 9, "xyz", 4, "r");
    rw = snprintf(want, sizeof(want), "%06lu|%-26s|%3d|%c%-12s[%s]|%-3s|%+d|% d|%05d|%-5d|%.3d|%.2s|%*s",
                  42UL, "msg", 7, '>', "Precision", "Float", "ab", 5, 6, -42, 3, 9, "xyz", 4, "r");
    check_str("int/string forms", got, want);
    check_int("int/string forms ret", rg, rw);

    rg = ce_snprintf(got, sizeof(got), "%d %d %d", 0, -1, 2147483647);
    rw = snprintf(want, sizeof(want), "%d %d %d", 0, -1, 2147483647);
    check_str("int edge", got, want);
}

static void test_truncation(void)
{
    char small[8];
    int r;

    memset(small, 'Z', sizeof(small));
    r = ce_snprintf(small, sizeof(small), "d/dx=%.6g", 123.456);
    check_str("truncated", small, "d/dx=12");
    check_int("truncated ret", r, (int)strlen("d/dx=123.456"));

    memset(small, 'Z', sizeof(small));
    r = ce_snprintf(small, sizeof(small), "%d%d%d", 1234, 5678, 9);
    check_str("truncated passthrough", small, "1234567");
    check_int("truncated passthrough ret", r, 9);

    r = ce_snprintf(NULL, 0, "%.4g+%d", 1.5, 22);
    check_int("null buffer length", r, (int)strlen("1.5+22"));

    small[0] = 'Q';
    r = ce_snprintf(small, 1, "%g", 5.0);
    check_str("size 1", small, "");
    check_int("size 1 ret", r, 1);
}

/* Not a literal NULL, so -Wformat does not flag the deliberate %s of NULL. */
static const char *null_str(void)
{
    static const char *volatile p = NULL;
    return p;
}

static void test_special_values(void)
{
    char got[64], want[64];
    int rg, rw;
    (void)rg;
    ce_snprintf(got, sizeof(got), "%g", (double)INFINITY);
    check_str("inf", got, "inf");
    ce_snprintf(got, sizeof(got), "%g", -(double)INFINITY);
    check_str("-inf", got, "-inf");
    ce_snprintf(got, sizeof(got), "%g", (double)NAN);
    check_str("nan", got, "nan");
    ce_snprintf(got, sizeof(got), "%g", -0.0);
    check_str("-0 prints as 0", got, "0");
    ce_snprintf(got, sizeof(got), "%.2g|%.1g|%.3g", 0.125, 2.5, 99.95);
    check_str("half-up ties", got, "0.13|3|100");
    ce_snprintf(got, sizeof(got), "[%f|%.2E|%d|%s]", 1.5, 2.5, 7, null_str());
    check_str("%f/%E unsupported: literal, args stay aligned", got, "[%f|%.2E|7|(null)]");
    {
        char abc[3] = {'a', 'b', 'c'};          /* not NUL-terminated */
        ce_snprintf(got, sizeof(got), "[%.3s]", abc);
        check_str("%.Ns stops at the precision", got, "[abc]");
    }
    rg = ce_snprintf(got, sizeof(got), "[%lu|%.30lu|%lx]", (unsigned long)-1, 7UL, (unsigned long)-1);
    rw = snprintf(want, sizeof(want), "[%lu|%.15lu|%lx]", (unsigned long)-1, 7UL, (unsigned long)-1);
    check_str("host-width longs fit; %d precision capped at 15", got, want);
    check_int("host-width longs ret", rg, rw);
    ce_snprintf(got, sizeof(got), "[%p|%d]", (void *)got, 7);
    check_str("%p unsupported: literal, args stay aligned", got, "[%p|7]");
    ce_snprintf(got, sizeof(got), "[%o|%lo|%lld|%d]", 8, 9L, 10LL, 11);
    check_str("%o/%ll unsupported: literal, args stay aligned", got, "[%o|%lo|%lld|11]");
    check_int("shape: missing strip is not tolerated", same_shape("%g", "1.50", "1.5", 1), 0);
    check_int("shape: notation must match", same_shape("%g", "1e+02", "100", 0), 0);
    check_int("shape: tie digit may differ", same_shape("%.3g", "100", "99.9", 0), 1);
    check_int("shape: sign must match even with relaxed notation", same_shape("%+.8g", "1", "+1", 1), 0);
    check_int("shape: strip must match even with relaxed notation", same_shape("%.8g", "1.0", "1", 1), 0);
    ce_snprintf(got, sizeof(got), "[%.12d]", 5);
    check_str("%d precision", got, "[000000000005]");
    ce_snprintf(got, sizeof(got), "[%.40d]", 5);
    check_str("%d precision is capped at 15", got, "[000000000000005]");
#ifndef CE_FMT_FLOAT_MODE
    ce_snprintf(got, sizeof(got), "[%g|%g]", 1e-200, 1e200);
    check_str("desktop double exponents", got, "[1e-200|inf]");
#endif
    ce_snprintf(got, sizeof(got), "[%e|%+g|% g]", 0.0, 0.0, 1e-30);
    check_str("zero and tiny", got, "[0.000000e+00|+0| 1e-30]");
    /* Odd whole numbers at >= 2^23 must not round up: in a
     * 32-bit float, 16777215 + 0.5 rounds to 16777216. */
    ce_snprintf(got, sizeof(got), "[%.8g|%.7g|%.8g|%.8g]",
                (double)16777215.0f, (double)8388609.0f, (double)-16777215.0f, (double)16777213.0f);
    check_str("odd whole numbers >= 2^23 keep their last digit", got,
              "[16777215|8388609|-16777215|16777213]");
}

static void test_ce_fmt_num(void)
{
    char got[32];
    int r = ce_fmt_num(got, sizeof(got), 3.0, 6);
    check_str("ce_fmt_num 3", got, "3");
    check_int("ce_fmt_num ret", r, 1);
    ce_fmt_num(got, sizeof(got), -2.0, 6);
    check_str("ce_fmt_num -2", got, "-2");
    ce_fmt_num(got, sizeof(got), 1.0 / 3.0, 4);
    check_str("ce_fmt_num 1/3 sig4", got, "0.3333");
    ce_fmt_num(got, sizeof(got), 1234567.0, 0);
    check_str("ce_fmt_num default sig", got, "1.23457e+06");
    ce_sprintf(got, "((X-%.6g)*(X-%.6g))", 2.0, 3.0);
    check_str("factor print", got, "((X-2)*(X-3))");
    ce_sprintf(got, "(%.6g*(X^(%.6g-%.6g)))", 3.0, 3.0, 1.0);
    check_str("deriv print", got, "(3*(X^(3-1)))");
}

int main(void)
{
    test_float_matches_host();
    test_mixed_and_passthrough();
    test_truncation();
    test_special_values();
    test_ce_fmt_num();
    printf("ce_fmt: %d passed (%d within float tolerance), %d failed\n",
           g_pass, g_approx, g_fail);
    return g_fail == 0 ? 0 : 1;
}
