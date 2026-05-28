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
	core/v810_cpu.c \
	core/v810_ins.c \
	core/v810_mem.c \
	core/interpreter.c \
	core/video_common.c \
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
