/*
 * ce_fmt.h - CE-safe snprintf family.
 *
 * The CEdev v11.2 libc printf (nanoprintf) does not implement the
 * floating-point conversions: "%g", "%.6g", "%e" and "%f" are copied to the
 * output literally, so on the calculator every formatted number became the
 * text "%.6g". Desktop builds use glibc/MSVC and never showed the problem.
 *
 * ce_vsnprintf() is a small self-contained formatter for the subset the
 * engine uses (see the header comment of ce_fmt.c): %d %i %u %x %X %c %s %%
 * with flags/width/precision/'l', and the floats %e and %g.
 *
 * On eZ80 (CEdev) builds, split/program.mk force-includes this header into
 * every translation unit (-include src/ce_fmt.h), so the macros below route
 * every snprintf/sprintf/vsnprintf/vsprintf call through it, including
 * steps_addf() and proc_okf(), which call vsnprintf. The libc printf is then
 * no longer linked. Desktop builds keep the host libc unless CE_FMT_REDIRECT
 * is defined (make -C tests run-ce-fmt).
 */
#ifndef CE_FMT_H
#define CE_FMT_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CE_FMT_PRINTF(f, a) __attribute__((format(__printf__, f, a)))
#else
#define CE_FMT_PRINTF(f, a)
#endif

/* The format attributes keep -Wformat checking once the macros below
 * rename snprintf to ce_snprintf. */
int ce_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) CE_FMT_PRINTF(3, 0);
int ce_snprintf(char *buf, size_t size, const char *fmt, ...) CE_FMT_PRINTF(3, 4);
int ce_vsprintf(char *buf, const char *fmt, va_list ap) CE_FMT_PRINTF(2, 0);
int ce_sprintf(char *buf, const char *fmt, ...) CE_FMT_PRINTF(2, 3);

/* Format value like "%.<sig>g" (sig <= 0 means 6; at most 9). Returns the
 * length the full result needs, like snprintf; buf is NUL-terminated if
 * size > 0. */
int ce_fmt_num(char *buf, size_t size, double value, int sig);

#ifdef __cplusplus
}
#endif

#if (defined(__ez80__) || defined(_EZ80) || defined(CE_FMT_REDIRECT)) && !defined(CE_FMT_IMPL)
#undef snprintf
#undef sprintf
#undef vsnprintf
#undef vsprintf
#define snprintf ce_snprintf
#define sprintf ce_sprintf
#define vsnprintf ce_vsnprintf
#define vsprintf ce_vsprintf
#endif

#endif /* CE_FMT_H */
