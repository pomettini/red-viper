HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = RedViper.pdx

SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif

ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

# Playdate frontend + Red Viper V810 core (interpreter path only). See
# playdate/NOTES.md for what's stubbed vs. linked vs. rewritten.
VPATH += src
VPATH += core

# The "core/" directory holds symlinks into ../source/common/. Going through
# symlinks (instead of ../source/common/ paths directly in SRC) keeps both
# the device build (make pattern rules) and the simulator build (raw clang
# invocation) consistent, and keeps object files inside playdate/build/
# instead of leaking up to source/common/ (which collides with the bundle's
# Source/ directory on macOS case-insensitive filesystems).
SRC = \
	src/main.c \
	src/pd_core.c \
	src/pd_stubs.c \
	src/pd_video.c \
	src/pd_itcm.c \
	src/pd_jit.c \
	src/pd_jit_run.c \
	src/pd_jit_test.c \
	core/v810_cpu.c \
	core/v810_ins.c \
	core/v810_mem.c \
	core/interpreter.c \
	core/video_common.c \
	core/video_soft.cpp \
	core/patches.c \
	core/rom_db.c

UINCDIR = src ../include

UASRC =
# RV_PLAYDATE is set for both device and simulator builds (UDEFS applies to
# both), so we can guard out 3DS-frontend-only code regardless of which
# Playdate target is being compiled.
# RV_JIT: register-allocating basic-block dynarec. CORRECT (runs Wario Land
# cleanly) but ~3x slower than the interpreter without block chaining (Stage 5),
# so it's OFF by default. Toggling this flag requires `make clean` (make doesn't
# track flag changes). See NOTES "BASIC-BLOCK BACKEND".
UDEFS = -DRV_PLAYDATE=1
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

# Use our forked linker script (adds the .itcm.rv section). LDFLAGS expands
# LDSCRIPT lazily, so overriding it here applies to the device link. The
# simulator build is a clang dylib and ignores the linker script entirely.
LDSCRIPT := $(CURDIR)/link_map.ld

# The relocated .itcm.rv functions must be position-independent under a raw
# memcpy: no absolute jump tables (switch -> if/else), and they only call out
# through function pointers (indirect), so no -mlong-calls needed.
$(OBJDIR)/src/pd_itcm.o: CPFLAGS += -fno-jump-tables

# ---------------------------------------------------------------------------
# C++ support (Red Viper's video_soft.cpp uses two templates).
#
# common.mk does `_OBJS = $(SRC:.c=.o)` and `$(OBJDIR)/pdex.elf: $(OBJS)`,
# but the prereq list of pdex.elf is expanded at parse time — before any
# override here can run — so .cpp files leak into the link prereqs as
# .cpp paths. Re-define pdex.elf below with a corrected OBJS list.
#
# Linking is still done by gcc (not g++). video_soft.cpp deliberately
# avoids RTTI, exceptions, STL, and anything that would pull in
# libstdc++ symbols, so plain gcc-driven linking works.
RV_OBJS := $(addprefix $(OBJDIR)/, $(patsubst %.cpp,%.o,$(patsubst %.c,%.o,$(SRC))))

CXXFLAGS = $(CPFLAGS) -fno-exceptions -fno-rtti

$(OBJDIR)/%.o : %.cpp | MKOBJDIR MKDEPDIR
	@mkdir -p `dirname $@`
	$(GCC)$(TRGT)g++ -c $(CXXFLAGS) -I . $(INCDIR) $< -o $@

# Re-define the device link rule. The recipe defined here wins over the
# one in common.mk (make warns but uses the latest).
$(OBJDIR)/pdex.elf: $(RV_OBJS) $(LDSCRIPT)
	$(CC) $(RV_OBJS) $(LDFLAGS) $(LIBS) -o $@

# common.mk's pdex.elf rule has a stale prereq list ($(OBJS) was expanded
# at parse-time, with .cpp untranslated), which leaves "build/core/
# video_soft.cpp" hanging in the merged prereq set. Satisfy it with a
# noop copy so make's dependency check passes; the actual link uses
# RV_OBJS, not OBJS, so this copy isn't really referenced.
$(OBJDIR)/%.cpp: %.cpp | MKOBJDIR
	@mkdir -p `dirname $@`
	@cp $< $@
