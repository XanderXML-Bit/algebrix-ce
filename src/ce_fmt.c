/*
 * ce_fmt.c - CE-safe printf front end (see ce_fmt.h).
 *
 * A self-contained snprintf: the engine no longer links the CEdev libc
 * printf (nanoprintf), whose float conversions print literally. Dropping it
 * makes each program 1.8-3.7 KB smaller than before.
 *
 * Size matters more than generality: every split program links this file,
 * several sit close to the 51,200 B module limit, and on the eZ80 each
 * float or 32-bit operation is a library call. The supported syntax is the
 * subset the engine uses; tests/cemu/test_ce_numeric_formatting.py enforces
 * it over all of src/:
 *   flags '-' '+' ' ' '0', width (N or *), precision (.N or .*), 'l'/'h'
 *   %d %i %u %x %X %c %s %%, and floats %e %g.
 *   Not supported (printed literally): %f %E %G %p %o %n, 'll' 'z' 'j' 'L'.
 *   '#' is accepted and ignored; the guard rejects it in src/.
 *   Integer precision is capped at 15 digits.
 *   '0' is ignored for floats (the guard rejects it there too).
 * Rounding is half-up on the 32-bit value, as TI-OS rounds; glibc rounds
 * the exact binary value half-to-even, so a last digit can differ on a tie.
 * At most 9 significant digits are produced (precision is capped at 9).
 * -0 prints as "0". No static state.
 */
#define CE_FMT_IMPL 1
#include "ce_fmt.h"

#include <string.h>

/* program.mk force-includes ce_fmt.h before this file defines CE_FMT_IMPL;
 * drop the redirect so this file defines only the ce_* names. */
#undef snprintf
#undef sprintf
#undef vsnprintf
#undef vsprintf

/* Internal arithmetic type. On the CE, double is already a 32-bit float;
 * desktop tests build with -DCE_FMT_REAL=float to reproduce that precision. */
#ifndef CE_FMT_REAL
#define CE_FMT_REAL double
#endif
typedef CE_FMT_REAL ce_real_t;

#define CE_FMT_MAXDIG 9          /* digits that fit an unsigned long */

/* round(v * 10^k), half-up. Steps of exact powers of ten (1e8, then 10)
 * keep the error to a few float ulps, far below the 6 digits the engine
 * normally prints.
 * Saturates instead of overflowing the conversion. */
static unsigned long scaled(ce_real_t v, int k)
{
    for (; k >= 8; k -= 8) v *= 1e8;    /* 1e8 is exact in a float too */
    for (; k <= -8; k += 8) v /= 1e8;
    for (; k > 0; k--) v *= 10.0;
    for (; k < 0; k++) v /= 10.0;
    if (v >= 4294967295.0) return 0xFFFFFFFFUL;   /* estimate was off: retry */
    /* A 32-bit float at or above 2^23 is an integer, and v + 0.5 rounds
     * odd values up (16777215 -> 16777216 under %.8g). Whole values need
     * no rounding. */
    if (v >= 8388608.0 && (ce_real_t)(unsigned long)v == v)
        return (unsigned long)v;
    return (unsigned long)(v + 0.5);
}

/* Render v for conv 'e' or 'g' (prec < 0 means 6) at s; returns the end.
 * Writes at most 20 bytes. */
static char *fmt_float(char *s, ce_real_t v, char conv, int prec)
{
    char dig[CE_FMT_MAXDIG];
    unsigned long d, lim = 1UL;
    ce_real_t t;
    int p, x = 0, j, k, sci;

    if (v < 0) { *s++ = '-'; v = -v; }
    if (v != v || v > 3.4028235e38) {
        memcpy(s, v != v ? "nan" : "inf", 3);
        return s + 3;
    }
    if (prec < 0) prec = 6;
    p = conv == 'e' ? prec + 1 : prec;                 /* significant digits */
    if (p < 1) p = 1;
    if (p > CE_FMT_MAXDIG) p = CE_FMT_MAXDIG;
    for (j = 0; j < p; j++) lim *= 10UL;

    if (v > 0) {                                       /* exponent estimate */
        for (t = v; t >= 10.0 && x < 400; t /= 10.0) x++;
        for (; t < 1.0 && x > -400; t *= 10.0) x--;  /* capped: denormals */
    }
    for (j = 0; ; j++) {                               /* p digits in d */
        d = scaled(v, p - 1 - x);
        if (j > 2 || v == 0) break;
        if (d >= lim) x++;
        else if (d < lim / 10UL) x--;
        else break;
    }
    if (v != 0 && d < lim / 10UL) {                  /* never a leading 0 */
        x--;
        d = scaled(v, p - 1 - x);
    }
    if (d >= lim) d = lim - 1UL;
    for (j = p; j-- > 0; d /= 10UL) dig[j] = (char)('0' + (int)(d % 10UL));

    /* %g picks plain or scientific notation the way C does. */
    sci = conv == 'e' || x < -4 || x >= p;
    k = sci ? 0 : x;                  /* index of the digit before the '.' */
    if (k < 0) {
        *s++ = '0';
        *s++ = '.';
        for (j = k; ++j < 0; ) *s++ = '0';
    }
    for (j = 0; j < p; j++) {
        *s++ = dig[j];
        if (j == k && j < p - 1) *s++ = '.';
    }
    if (conv == 'g' && (k < 0 || k < p - 1)) {         /* strip zeros */
        while (s[-1] == '0') s--;
        if (s[-1] == '.') s--;
    }
    if (sci) {
        *s++ = 'e';
        *s++ = x < 0 ? '-' : '+';
        if (x < 0) x = -x;
        if (x >= 100) { *s++ = (char)('0' + x / 100); x %= 100; }  /* desktop doubles */
        *s++ = (char)('0' + x / 10);
        *s++ = (char)('0' + x % 10);
    }
    return s;
}

/* No side effects in ch: it is only evaluated while there is room. */
#define CE_PUT(ch) do { if (pos + 1 < size) buf[pos] = (ch); pos++; } while (0)

int ce_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char num[40];   /* sign + 20 digits (64-bit host long); floats <= 21 */
    size_t pos = 0;

    if (!buf) size = 0;
    while (*fmt) {
        const char *st = fmt, *str;
        char c = *fmt++;
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, len, r;
        char sign = 0, *e;

        if (c != '%' || *fmt == '%') {
            if (c == '%') fmt++;
            CE_PUT(c);
            continue;
        }
        for (; (c = *fmt) != 0; fmt++) {
            if (c == '+' || c == ' ') { if (sign != '+') sign = c; }
            else if (c == '-') left = 1;
            else if (c == '0' && prec < 0 && width == 0) zero = 1;
            else if (c == '.') prec = 0;
            else if (c >= '0' && c <= '9') {
                if (prec >= 0) prec = prec * 10 + (c - '0');
                else width = width * 10 + (c - '0');
            } else if (c == '*') {
                r = va_arg(ap, int);
                if (prec >= 0) prec = r < 0 ? -1 : r;
                else if (r < 0) { left = 1; width = -r; }
                else width = r;
            } else if (c == 'l') lng++;
            else if (c != '#' && c != 'h') break;
        }
        if (c) fmt++;

        e = num;
        str = num;
        if (c == 'e' || c == 'g') {
            ce_real_t v = (ce_real_t)va_arg(ap, double);
            if (sign && !(v < 0)) *e++ = sign;
            e = fmt_float(e, v, c, prec);
            zero = 0;
        } else if (lng < 2 && (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X')) {
            unsigned long u;
            unsigned base = (c == 'x' || c == 'X') ? 16U : 10U;
            char tmp[22];   /* 20 digits of a 64-bit host long */
            int n = 0;
            if (c == 'd' || c == 'i') {
                long sv = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (sv < 0) { *e++ = '-'; u = 0UL - (unsigned long)sv; }
                else { if (sign) *e++ = sign; u = (unsigned long)sv; }
            } else {
                u = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            }
            do {
                unsigned dg = (unsigned)(u % base);
                tmp[n++] = (char)(dg < 10U ? '0' + dg : (c == 'X' ? 'A' : 'a') + dg - 10U);
                u /= base;
            } while (u);
            if (prec > 15) prec = 15;                  /* keeps num bounded */
            for (; prec > n; prec--) *e++ = '0';
            while (n) *e++ = tmp[--n];
            if (prec >= 0) zero = 0;
        } else if (c == 'c') {
            *e++ = (char)va_arg(ap, int);
            zero = 0;
        } else if (c == 's') {
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            for (len = 0; (prec < 0 || len < prec) && str[len]; len++) { }
            e = (char *)str + len;
            zero = 0;
        } else {
            /* Unsupported (%f, %E, %p, %ll...): print the spec literally,
             * as the CE libc does for floats, and step over the argument
             * so later conversions stay aligned. */
            if (c && strchr("fFEGaA", c)) (void)va_arg(ap, double);
            else if (c == 'p' || c == 'n') (void)va_arg(ap, void *);
            else if (c && strchr("diouxX", c)) {        /* %o, %ll*: skip it */
                if (lng >= 2) (void)va_arg(ap, long long);
                else if (lng) (void)va_arg(ap, long);
                else (void)va_arg(ap, int);
            }
            str = st;
            e = (char *)fmt;
            zero = 0;
            width = 0;
        }

        len = (int)(e - str);
        r = width - len;
        if (zero && r > 0 && (*str == '-' || *str == '+' || *str == ' ')) {
            CE_PUT(*str);
            str++;
        }
        if (!left) for (; r > 0; r--) CE_PUT(zero ? '0' : ' ');
        for (; str < e; str++) CE_PUT(*str);
        for (; r > 0; r--) CE_PUT(' ');
    }
    if (size > 0) buf[pos < size ? pos : size - 1] = '\0';
    return (int)pos;
}

int ce_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ce_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int ce_vsprintf(char *buf, const char *fmt, va_list ap)
{
    return ce_vsnprintf(buf, ((size_t)-1) >> 1, fmt, ap);
}

int ce_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = ce_vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int ce_fmt_num(char *buf, size_t size, double value, int sig)
{
    return ce_snprintf(buf, size, "%.*g", sig > 0 ? sig : 6, value);
}
