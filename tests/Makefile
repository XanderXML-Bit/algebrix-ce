# Desktop test build for CalcEngine
# Compiles core engine with mock CE headers using gcc/clang

CC ?= gcc
ifeq ($(OS),Windows_NT)
GCC_EXISTS := $(shell where gcc 2>NUL)
ifeq ($(strip $(GCC_EXISTS)),)
CC := msvc-gcc-compat.cmd
endif
TEST_INCLUDE_FLAGS = -I..\\src -Imock
RUNNER = .\test_runner.exe
FMT_RUNNER = .\fmt_test.exe && .\fmt_test_ce32.exe
CE_FMT_RUNNER = .\test_runner_ce_fmt.exe
FLOAT_CLASS_RUNNER = .\float_class_test.exe
FLOAT_OPS_RUNNER = .\float_ops_test.exe
FLOAT_TINY_REF = .\float_tiny_ref.exe
FLOAT_TINY_TEST = .\float_tiny_test.exe
FLOAT_TINY_FLUSH = .\float_tiny_flush.exe
FLOAT_TINY_BIAS = .\float_tiny_bias.exe
MAKE_BIN_DIR = powershell -NoProfile -Command "New-Item -ItemType Directory -Force '../bin' | Out-Null"
REMOVE_TEST_ARTIFACTS = powershell -NoProfile -Command "Remove-Item -Force -ErrorAction SilentlyContinue 'test_runner','test_runner.exe','fmt_test','fmt_test.exe','fmt_test_ce32','fmt_test_ce32.exe','test_runner_ce_fmt','test_runner_ce_fmt.exe','float_class_test','float_class_test.exe','float_ops_test','float_ops_test.exe','float_tiny_ref','float_tiny_ref.exe','float_tiny_test','float_tiny_test.exe','float_tiny_flush','float_tiny_flush.exe','float_tiny_bias','float_tiny_bias.exe','float_tiny_ref.bin','test_runner_asan','test_runner_asan.exe','test_runner_ce_fmt_asan','test_runner_ce_fmt_asan.exe','../bin/calcengine','../bin/calcengine.exe'; exit 0"
else
TEST_INCLUDE_FLAGS = -I../src -Imock
RUNNER = chmod +x ./test_runner && ./test_runner
FMT_RUNNER = ./fmt_test && ./fmt_test_ce32
CE_FMT_RUNNER = ./test_runner_ce_fmt
FLOAT_CLASS_RUNNER = ./float_class_test
FLOAT_OPS_RUNNER = ./float_ops_test
FLOAT_TINY_REF = ./float_tiny_ref
FLOAT_TINY_TEST = ./float_tiny_test
FLOAT_TINY_FLUSH = ./float_tiny_flush
FLOAT_TINY_BIAS = ./float_tiny_bias
MAKE_BIN_DIR = mkdir -p ../bin
REMOVE_TEST_ARTIFACTS = rm -f test_runner test_runner.exe fmt_test fmt_test.exe fmt_test_ce32 fmt_test_ce32.exe test_runner_ce_fmt test_runner_ce_fmt.exe float_class_test float_class_test.exe float_ops_test float_ops_test.exe float_tiny_ref float_tiny_ref.exe float_tiny_test float_tiny_test.exe float_tiny_flush float_tiny_flush.exe float_tiny_bias float_tiny_bias.exe float_tiny_ref.bin test_runner_asan test_runner_ce_fmt_asan ../bin/calcengine ../bin/calcengine.exe split_cebase_test split_vars_cebase split_vars_celscx split_vars_state.bin
# CEBASE (split/Makefile.base) flags, read from the module makefile so the
# desktop check cannot drift from the .8xp build.
CEBASE_CFLAGS := $(shell sed -n '/^PROGRAM_CFLAGS/,/^$$/p' ../split/Makefile.base | sed 's/^PROGRAM_CFLAGS *= *//; s/\\$$//' | tr '\n' ' ')
CELSCX_CFLAGS := $(shell sed -n '/^PROGRAM_CFLAGS/,/^$$/p' ../split/Makefile.lists_complex | sed 's/^PROGRAM_CFLAGS *= *//; s/\\$$//' | tr '\n' ' ')
SPLIT_RUN = ./split_cebase_test && $(SPLIT_VARS_RUN)
SPLIT_DEPS = split_cebase_test split_vars_cebase split_vars_celscx
endif
ifeq ($(OS),Windows_NT)
SPLIT_RUN = @echo "split_cebase_test: skipped on Windows (needs sed); CI runs it on Ubuntu"
endif

CFLAGS = -Wall -Wextra -Wno-unused-function -g -O0 $(TEST_INCLUDE_FLAGS) -std=c99 \
         -DDESKTOP_BUILD \
         -DCE_PROC_ALG=1 -DCE_PROC_CALC=1 -DCE_PROC_STAT=1 -DCE_PROC_FIN=1 \
         -DCE_PROC_MATRIX=1 -DCE_PROC_CMPLX=1 -DCE_PROC_GRAPH=1
LDFLAGS = -lm

SRC = ../src/memory.c ../src/token.c ../src/ast.c ../src/parser.c \
      ../src/eval.c ../src/deriv.c ../src/simplify.c ../src/steps.c \
      ../src/integrate.c ../src/limits.c ../src/series.c ../src/algebra.c \
      ../src/stats.c ../src/matrix.c ../src/polar.c ../src/ui.c \
      ../src/list.c ../src/complex.c ../src/prob.c ../src/finance.c \
      ../src/stattest.c ../src/macros.c ../src/graph.c \
      ../src/mathprint.c ../src/mathprint_template.c \
      ../src/split/classify.c ../src/split/astwire.c \
      ../src/split/ipc.c ../src/split/module_runtime.c ../src/split/module_forward.c \
      ../src/split/processor.c ../src/split/disambig.c \
      ../src/solver.c ../src/history.c \
      ../src/dsolve.c ../src/vector.c ../src/laplace.c \
      ../src/units.c ../src/userfunc.c ../src/bookmark.c
MOCK_SRC = mock/graphx_mock.c mock/keypadc_mock.c mock/tice_mock.c mock/fileioc_mock.c
TEST_SRC = test_main.c
CLI_SRC = ../tools/desktop/calcengine_cli.c

all: test_runner ../bin/calcengine

test_runner: FORCE $(TEST_SRC) $(SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(SRC) $(MOCK_SRC) $(LDFLAGS)

../bin/calcengine: FORCE $(CLI_SRC) $(SRC) $(MOCK_SRC)
	$(MAKE_BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $(CLI_SRC) $(SRC) $(MOCK_SRC) $(LDFLAGS)

# Desktop run of the CEBASE processor path with the module's own flags.
CEBASE_SRC = ../src/memory.c ../src/token.c ../src/ast.c ../src/parser.c \
             ../src/eval.c ../src/steps.c ../src/split/astwire.c \
             ../src/split/processor.c ../src/split/runtime_globals.c \
             ../src/split/ipc.c
# runtime_globals.c supplies the tice cancel/progress hooks on the module build.
CEBASE_MOCK_SRC = mock/graphx_mock.c mock/keypadc_mock.c mock/fileioc_mock.c

# Like split/program.mk, route formatting through ce_fmt (32-bit float).
split_cebase_test: FORCE split_cebase_test.c $(CEBASE_SRC) ../src/ce_fmt.c
	$(CC) -Wall -Wextra -Wno-unused-function -g -O0 $(TEST_INCLUDE_FLAGS) -std=c99 \
	    -DDESKTOP_BUILD -DCE_FMT_REDIRECT -DCE_FMT_REAL=float -DCE_FLOAT_CLASS_REDIRECT \
	    -include ../src/ce_fmt.h $(CEBASE_CFLAGS) -o $@ split_cebase_test.c $(CEBASE_SRC) ../src/ce_fmt.c \
	    $(CEBASE_MOCK_SRC) $(LDFLAGS)

# Cross-module stored variables: the real CEBASE and CELSCX main()s, each
# with its own module flags.  CELSCX stores 5->X and dumps CE_STATE; CEBASE
# loads that state and must answer X^2 = 25 (and the reverse direction).
SPLIT_FMT_FLAGS = -Wall -Wextra -Wno-unused-function -g -O0 $(TEST_INCLUDE_FLAGS) -std=c99 \
    -DDESKTOP_BUILD -DCE_FMT_REDIRECT -DCE_FMT_REAL=float -DCE_FLOAT_CLASS_REDIRECT \
    -include ../src/ce_fmt.h
VARS_COMMON_SRC = ../src/memory.c ../src/token.c ../src/ast.c ../src/parser.c \
    ../src/eval.c ../src/steps.c ../src/split/astwire.c ../src/split/processor.c \
    ../src/split/runtime_globals.c ../src/split/ipc.c ../src/split/module_runtime.c \
    ../src/ce_fmt.c
split_vars_cebase: FORCE split_vars_test.c $(VARS_COMMON_SRC) ../src/split/module_main.c
	$(CC) $(SPLIT_FMT_FLAGS) $(CEBASE_CFLAGS) -Dmain=module_program_main -o $@ \
	    split_vars_test.c $(VARS_COMMON_SRC) ../src/split/module_main.c $(CEBASE_MOCK_SRC) $(LDFLAGS)
split_vars_celscx: FORCE split_vars_test.c $(VARS_COMMON_SRC) ../src/modules/common/module_entry.c
	$(CC) $(SPLIT_FMT_FLAGS) $(CELSCX_CFLAGS) -Dce_module_entry=module_program_main -o $@ \
	    split_vars_test.c $(VARS_COMMON_SRC) ../src/modules/common/module_entry.c \
	    ../src/list.c ../src/complex.c $(CEBASE_MOCK_SRC) $(LDFLAGS)
SPLIT_VARS_RUN = ./split_vars_celscx --dump split_vars_state.bin && \
    ./split_vars_cebase --load split_vars_state.bin --dump split_vars_state.bin && \
    ./split_vars_celscx --load split_vars_state.bin
split-vars-test: split_vars_cebase split_vars_celscx
	$(SPLIT_VARS_RUN)

# The CEBASE check runs on non-Windows only (CEBASE_CFLAGS needs sed);
# CI desktop runs on Ubuntu.
# CE-safe printf front end (src/ce_fmt.c). The CE build force-includes
# src/ce_fmt.h into every file, so all formatting goes through it there.
# fmt_test checks it against the host libc; fmt_test_ce32 repeats that with
# 32-bit float arithmetic (the CE's double); test_runner_ce_fmt runs the whole
# desktop suite with the engine's formatting routed through it, and with
# NaN/inf tests routed through the CE helpers in src/ce_float_class.h.
FMT_CFLAGS = -Wall -Wextra -g -O0 $(TEST_INCLUDE_FLAGS) -std=c99

fmt_test: FORCE test_ce_fmt.c ../src/ce_fmt.c
	$(CC) $(FMT_CFLAGS) -o $@ test_ce_fmt.c ../src/ce_fmt.c $(LDFLAGS)

fmt_test_ce32: FORCE test_ce_fmt.c ../src/ce_fmt.c
	$(CC) $(FMT_CFLAGS) -DCE_FMT_REAL=float -DCE_FMT_FLOAT_MODE -o $@ test_ce_fmt.c ../src/ce_fmt.c $(LDFLAGS)

fmt-test: fmt_test fmt_test_ce32
	$(FMT_RUNNER)

# CE-safe NaN/inf/finite helpers (src/ce_float_class.h) against the host
# macros over all 2^32 float bit patterns, plus a model of the CEdev
# isinf() bug they replace. -O2: the full sweep takes a few seconds.
float_class_test: FORCE test_ce_float_class.c ../src/ce_float_class.h
	$(CC) -Wall -Wextra -O2 $(TEST_INCLUDE_FLAGS) -std=c99 -o $@ test_ce_float_class.c $(LDFLAGS)

float-class-test: float_class_test
	$(FLOAT_CLASS_RUNNER)

# CE-safe float arithmetic (src/ce_float_ops.h, implemented in eval.c) and
# the literal range check in token.c: bit-for-bit IEEE parity for + - * /,
# no native float op outside the range it is proven safe for, exact integer
# powers, exp/sinh/cosh bounds. The test includes eval.c itself, with the
# eval features CEBASE builds. -O2: about 70 M operations.
FLOAT_OPS_SRC = ../src/memory.c ../src/token.c ../src/ast.c ../src/steps.c \
                ../src/split/runtime_globals.c
float_ops_test: FORCE test_ce_float_ops.c ../src/eval.c ../src/ce_float_ops.h $(FLOAT_OPS_SRC)
	$(CC) -Wall -Wextra -Wno-unused-function -O2 $(TEST_INCLUDE_FLAGS) -std=c99 \
	    -DDESKTOP_BUILD -DCE_SAFE_FLOAT=1 -DCE_FLOAT_CLASS_REDIRECT -DCE_AST_ENABLE_STAT_NAMES=0 \
	    -DCE_EVAL_ENABLE_NODE_STAT=0 -DCE_EVAL_ENABLE_NODE_LIST=0 \
	    -DCE_EVAL_ENABLE_ADV_POLAR=0 -DCE_EVAL_ENABLE_POLYDIV=0 \
	    -o $@ test_ce_float_ops.c $(FLOAT_OPS_SRC) $(CEBASE_MOCK_SRC) $(LDFLAGS)

float-ops-test: float_ops_test
	$(FLOAT_OPS_RUNNER)

# Tiny-arithmetic refusal (CE_SAFE_FLOAT_TINY, CEBASE): the contract in
# test_ce_float_tiny.c, checked on host IEEE arithmetic and again with every
# native operation flushing like the CE; answered pow/exp/sinh results must
# equal the flag-off build's (float_tiny_ref writes them).
FLOAT_TINY_FLAGS = -Wall -Wextra -Wno-unused-function -O2 $(TEST_INCLUDE_FLAGS) -std=c99 \
	    -DDESKTOP_BUILD -DCE_SAFE_FLOAT=1 -DCE_EVAL_SUM_COMPENSATED=1 \
	    -DCE_FLOAT_CLASS_REDIRECT -DCE_AST_ENABLE_STAT_NAMES=0 \
	    -DCE_EVAL_ENABLE_NODE_STAT=0 -DCE_EVAL_ENABLE_NODE_LIST=0 \
	    -DCE_EVAL_ENABLE_ADV_POLAR=0 -DCE_EVAL_ENABLE_POLYDIV=0
float_tiny_ref: FORCE test_ce_float_tiny.c ../src/eval.c ../src/ce_float_ops.h $(FLOAT_OPS_SRC)
	$(CC) $(FLOAT_TINY_FLAGS) -DCE_SAFE_FLOAT_TINY=0 \
	    -o $@ test_ce_float_tiny.c $(FLOAT_OPS_SRC) ../src/parser.c $(CEBASE_MOCK_SRC) $(LDFLAGS)
float_tiny_test: FORCE test_ce_float_tiny.c ../src/eval.c ../src/ce_float_ops.h $(FLOAT_OPS_SRC)
	$(CC) $(FLOAT_TINY_FLAGS) -DCE_SAFE_FLOAT_TINY=1 \
	    -o $@ test_ce_float_tiny.c $(FLOAT_OPS_SRC) ../src/parser.c $(CEBASE_MOCK_SRC) $(LDFLAGS)
float_tiny_flush: FORCE test_ce_float_tiny.c ../src/eval.c ../src/ce_float_ops.h $(FLOAT_OPS_SRC)
	$(CC) $(FLOAT_TINY_FLAGS) -DCE_SAFE_FLOAT_TINY=1 -DTINY_CE_FLUSH \
	    -o $@ test_ce_float_tiny.c $(FLOAT_OPS_SRC) ../src/parser.c $(CEBASE_MOCK_SRC) $(LDFLAGS)

float_tiny_bias: FORCE test_ce_float_tiny.c ../src/eval.c ../src/ce_float_ops.h $(FLOAT_OPS_SRC)
	$(CC) $(FLOAT_TINY_FLAGS) -DCE_SAFE_FLOAT_TINY=1 -DTINY_CE_FLUSH -DTINY_OS_LOW \
	    -o $@ test_ce_float_tiny.c $(FLOAT_OPS_SRC) ../src/parser.c $(CEBASE_MOCK_SRC) $(LDFLAGS)

float-tiny-test: float_tiny_ref float_tiny_test float_tiny_flush float_tiny_bias
	$(FLOAT_TINY_REF) float_tiny_ref.bin
	$(FLOAT_TINY_TEST) float_tiny_ref.bin
	$(FLOAT_TINY_FLUSH) float_tiny_ref.bin
	$(FLOAT_TINY_BIAS)

test_runner_ce_fmt: FORCE $(TEST_SRC) $(SRC) $(MOCK_SRC) ../src/ce_fmt.c
	$(CC) $(CFLAGS) -DCE_FMT_REDIRECT -DCE_FMT_REAL=float -DCE_FLOAT_CLASS_REDIRECT -include ../src/ce_fmt.h -o $@ $(TEST_SRC) $(SRC) $(MOCK_SRC) ../src/ce_fmt.c $(LDFLAGS)

run-ce-fmt: test_runner_ce_fmt
	$(CE_FMT_RUNNER)

# AddressSanitizer + UBSan builds of test_runner and test_runner_ce_fmt
# (gcc/clang on Linux/macOS; not supported on Windows). Any report aborts the
# run (-fno-sanitize-recover=all, ASan halt_on_error=1, LeakSanitizer exit),
# so a memory bug fails CI even when every assertion passes.
# Usage: make run-asan
SAN_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
SAN_ENV = ASAN_OPTIONS=halt_on_error=1:detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1 \
          UBSAN_OPTIONS=print_stacktrace=1

test_runner_asan: FORCE $(TEST_SRC) $(SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $(TEST_SRC) $(SRC) $(MOCK_SRC) $(LDFLAGS)

test_runner_ce_fmt_asan: FORCE $(TEST_SRC) $(SRC) $(MOCK_SRC) ../src/ce_fmt.c
	$(CC) $(CFLAGS) $(SAN_FLAGS) -DCE_FMT_REDIRECT -DCE_FMT_REAL=float -DCE_FLOAT_CLASS_REDIRECT -include ../src/ce_fmt.h -o $@ $(TEST_SRC) $(SRC) $(MOCK_SRC) ../src/ce_fmt.c $(LDFLAGS)

ifeq ($(OS),Windows_NT)
run-asan:
	@echo "run-asan: not supported on Windows (needs gcc/clang sanitizers); CI runs it on Ubuntu" && exit 1
else
run-asan: test_runner_asan test_runner_ce_fmt_asan
	$(SAN_ENV) ./test_runner_asan
	$(SAN_ENV) ./test_runner_ce_fmt_asan
endif

run: test_runner fmt-test float-class-test float-ops-test float-tiny-test $(SPLIT_DEPS)
	$(RUNNER)
	$(SPLIT_RUN)

clean:
	$(REMOVE_TEST_ARTIFACTS)

FORCE:

.PHONY: all run clean fmt-test float-class-test float-ops-test float-tiny-test run-ce-fmt run-asan FORCE
