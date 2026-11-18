# program_diag.mk — Variant of split/program.mk for diagnostic builds.
# Key difference: does NOT auto-add src/split/module_runtime.c to EXTRA_C_SOURCES.
# The diagnostic CEBASE build provides module_runtime_diag.c via PROGRAM_RUNTIME_SOURCES.
#
# Usage: include tools/hardware-test/diag-calceng/program_diag.mk
#        (from a Makefile.*.diag file, after setting NAME, PROGRAM_SOURCES, etc.)

NAME        ?= DEMO
DESCRIPTION ?= "CalcEngine Diag Module"
COMPRESSED  ?= NO
ARCHIVED     = YES

V_MAJOR ?= 1
V_MINOR ?= 0
V_PATCH ?= 0

SRCDIR  = src/split/empty
OBJDIR ?= obj
BINDIR ?= bin

CFLAGS = -Wall -Wextra -Wno-unused-command-line-argument -Oz \
         -I src \
         -I tools/hardware-test/diag-calceng \
         -DDIAG_BUILD \
         -DCE_VERSION_MAJOR=$(V_MAJOR) \
         -DCE_VERSION_MINOR=$(V_MINOR) \
         -DCE_VERSION_PATCH=$(V_PATCH) \
         $(PROGRAM_CFLAGS)

# PROGRAM_RUNTIME_SOURCES must be set by the caller to include
# module_runtime_diag.c (and diag_helpers.c if not already in PROGRAM_SOURCES).
EXTRA_C_SOURCES  = $(PROGRAM_SOURCES)
EXTRA_C_SOURCES += $(PROGRAM_RUNTIME_SOURCES)

include $(shell cedev-config --makefile)
