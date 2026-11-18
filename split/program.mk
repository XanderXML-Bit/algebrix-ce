NAME ?= DEMO
DESCRIPTION ?= "CalcEngine Module"
COMPRESSED ?= NO
ARCHIVED = YES

V_MAJOR ?= 2
V_MINOR ?= 0
V_PATCH ?= 1

SRCDIR = src/split/empty
# Active module NAMEs are unique. Keep LTO and stack reports per program.
OBJDIR ?= obj/$(NAME)
BINDIR ?= bin

CFLAGS = -Wall -Wextra -Wno-unused-command-line-argument -Oz -I src \
         -DCE_VERSION_MAJOR=$(V_MAJOR) \
         -DCE_VERSION_MINOR=$(V_MINOR) \
         -DCE_VERSION_PATCH=$(V_PATCH) \
         -fstack-usage \
         -include src/ce_fmt.h \
         $(PROGRAM_CFLAGS)

# CEdev's libc printf cannot format floats (%g/%e/%f print literally).
# ce_fmt.h (force-included above) routes snprintf/sprintf/vsnprintf/vsprintf
# through ce_fmt.c, which renders float conversions itself.
EXTRA_C_SOURCES = $(PROGRAM_SOURCES) src/split/module_runtime.c src/ce_fmt.c
EXTRA_C_SOURCES += $(PROGRAM_RUNTIME_SOURCES)

include $(shell cedev-config --makefile)
