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
UDEFS = -DRV_PLAYDATE=1
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

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
