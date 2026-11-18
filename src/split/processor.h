#ifndef SPLIT_PROCESSOR_H
#define SPLIT_PROCESSOR_H

#include "split/ipc.h"
#include "ast.h"

#ifndef CE_PROC_ALG
#define CE_PROC_ALG 0
#endif

#ifndef CE_PROC_ALG_BASIC
#define CE_PROC_ALG_BASIC CE_PROC_ALG
#endif

#ifndef CE_PROC_ALG_EXPFAC
#define CE_PROC_ALG_EXPFAC CE_PROC_ALG_BASIC
#endif

#ifndef CE_PROC_ALG_SOLVE
#define CE_PROC_ALG_SOLVE CE_PROC_ALG_BASIC
#endif

#ifndef CE_PROC_ALG_POLYDIV
#define CE_PROC_ALG_POLYDIV CE_PROC_ALG_SOLVE
#endif

#ifndef CE_PROC_ALG_ADV
#define CE_PROC_ALG_ADV CE_PROC_ALG
#endif

#ifndef CE_PROC_ALG_ADV_SYM
#define CE_PROC_ALG_ADV_SYM CE_PROC_ALG_ADV
#endif

#ifndef CE_PROC_ALG_ADV_GEO
#define CE_PROC_ALG_ADV_GEO CE_PROC_ALG_ADV
#endif

#ifndef CE_PROC_CALC
#define CE_PROC_CALC 0
#endif

#ifndef CE_PROC_CALC_BASE
#define CE_PROC_CALC_BASE CE_PROC_CALC
#endif

#ifndef CE_PROC_CALC_DERIV
#define CE_PROC_CALC_DERIV CE_PROC_CALC_BASE
#endif

#ifndef CE_PROC_CALC_TANGENT
#define CE_PROC_CALC_TANGENT CE_PROC_CALC_DERIV
#endif

#ifndef CE_PROC_CALC_LIMIT
#define CE_PROC_CALC_LIMIT CE_PROC_CALC_BASE
#endif

#ifndef CE_PROC_CALC_LIMIT_CONTINUITY
#define CE_PROC_CALC_LIMIT_CONTINUITY CE_PROC_CALC_LIMIT
#endif

#ifndef CE_PROC_CALC_LIMIT_RELRATE
#define CE_PROC_CALC_LIMIT_RELRATE CE_PROC_CALC_LIMIT
#endif

#ifndef CE_PROC_CALC_INT
#define CE_PROC_CALC_INT CE_PROC_CALC
#endif

#ifndef CE_PROC_CALC_INT_SYMBOLIC
#define CE_PROC_CALC_INT_SYMBOLIC CE_PROC_CALC_INT
#endif

#ifndef CE_PROC_CALC_INT_DEFINITE
#define CE_PROC_CALC_INT_DEFINITE CE_PROC_CALC_INT_SYMBOLIC
#endif

/* The full areabetween()/improper() handlers (desktop). They need
 * area_between_curves() and the limits-based improper path, which do not
 * fit the definite-only worker CECIDF (51,200 B), so CECIDF sets this to 0
 * and CE_PROC_CALC_INT_POLAR_LOWER to 1 instead. */
#ifndef CE_PROC_CALC_INT_DEFINITE_POLAR
#define CE_PROC_CALC_INT_DEFINITE_POLAR CE_PROC_CALC_INT_DEFINITE
#endif

/* areabetween()/improper() answered through handle_integral() (internal review).
 * The launcher routes both ops to CECINT, which forwards them to CECIDF.
 * Before, they fell through to CEBASE, which printed "Polar/advanced op".
 * improper(f,x,a,b) is int(f,x,a,b), with the same checks and refusals.
 * areabetween(f,g,x,a,b) sums |int(f-g)| over the pieces between the sign
 * changes of f-g, found by sampling and bisection. */
#ifndef CE_PROC_CALC_INT_POLAR_LOWER
#define CE_PROC_CALC_INT_POLAR_LOWER 0
#endif

#ifndef CE_PROC_CALC_INT_POLAR
#define CE_PROC_CALC_INT_POLAR CE_PROC_CALC_INT
#endif

#ifndef CE_PROC_CALC_INT_STRATEGY
#define CE_PROC_CALC_INT_STRATEGY CE_PROC_CALC_INT_POLAR
#endif

#ifndef CE_PROC_CALC_POLAR_NUM
#define CE_PROC_CALC_POLAR_NUM CE_PROC_CALC_INT_POLAR
#endif

#ifndef CE_PROC_CALC_ODE
#define CE_PROC_CALC_ODE CE_PROC_CALC_INT_POLAR
#endif

#ifndef CE_PROC_CALC_EULER
#define CE_PROC_CALC_EULER CE_PROC_CALC_INT_POLAR
#endif

#ifndef CE_PROC_CALC_SERIES
#define CE_PROC_CALC_SERIES CE_PROC_CALC
#endif

#ifndef CE_PROC_CALC_SERIES_TF
#define CE_PROC_CALC_SERIES_TF CE_PROC_CALC_SERIES
#endif

#ifndef CE_PROC_CALC_SERIES_SEQ
#define CE_PROC_CALC_SERIES_SEQ CE_PROC_CALC_SERIES
#endif

#ifndef CE_PROC_STAT
#define CE_PROC_STAT 0
#endif

#ifndef CE_PROC_FIN
#define CE_PROC_FIN 0
#endif

#ifndef CE_PROC_MATRIX
#define CE_PROC_MATRIX 0
#endif

#ifndef CE_PROC_CMPLX
#define CE_PROC_CMPLX 0
#endif

#ifndef CE_PROC_GRAPH
#define CE_PROC_GRAPH 0
#endif

#ifndef CE_PROC_GENERIC
#define CE_PROC_GENERIC 1
#endif

#ifndef CE_PROC_USE_EVAL
#define CE_PROC_USE_EVAL 1
#endif

/* The generic processor can answer an expression with a free variable
 * (x^2*sin(x)+x) with an ok=2 "choose an action" menu. Only the desktop CLI
 * (tools/desktop/calcengine_cli.c, which prints it) and the unshipped legacy
 * split core (src/split/core_main.c) read ok=2; the desktop UI has its own
 * split/disambig.c dialog. The CE launcher (src/core/launcher/main.c) has
 * no menu for it, so the split modules answer with a "has no value" error
 * instead (internal review). */
#ifndef CE_PROC_DISAMBIG_MENU
#ifdef HW_BUILD_SPLIT
#define CE_PROC_DISAMBIG_MENU 0
#else
#define CE_PROC_DISAMBIG_MENU 1
#endif
#endif

#ifndef CE_PROC_USE_CLASSIFY
#define CE_PROC_USE_CLASSIFY 1
#endif

#ifndef CE_PROC_PARSE_INPUT
#define CE_PROC_PARSE_INPUT 1
#endif

#ifndef CE_PROC_ENABLE_LINREG
#define CE_PROC_ENABLE_LINREG 1
#endif

#ifndef CE_PROC_STAT_NAMES
#define CE_PROC_STAT_NAMES 1
#endif

#ifndef CE_PROC_CALC_INT_ROUTE_ADV
#define CE_PROC_CALC_INT_ROUTE_ADV 0
#endif

#ifndef CE_PROC_CALC_LIMIT_ROUTE_ADV
#define CE_PROC_CALC_LIMIT_ROUTE_ADV 0
#endif

bool ce_processor_run(const char *input, ce_response_t *resp);
bool ce_processor_run_preparsed(const char *input,
                                ast_node_t *root,
                                bool input_had_equals,
                                ce_response_t *resp);

#endif /* SPLIT_PROCESSOR_H */
