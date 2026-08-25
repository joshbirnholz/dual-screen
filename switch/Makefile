#---------------------------------------------------------------------------------
# dualscreen companion for the Nintendo Switch.
#
# Built with devkitPro's devkitA64 toolchain and libnx. It needs switch-sdl2 and
# switch-zlib from the portlibs:
#
#   sudo dkp-pacman -S switch-dev switch-sdl2 switch-zlib
#   make            # -> dualscreen-companion.nro
#
# The sources under ../src are the desktop app's own, compiled unchanged: the
# wire protocol, discovery, the session, the layout arithmetic and the
# controller mapping are one implementation used by both ends rather than two
# that have to be kept in step. The list below is the same one CMakeLists.txt
# builds for this machine, which is where the client is actually tested.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO in your environment, e.g. export DEVKITPRO=/opt/devkitpro)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET      := dualscreen-companion
BUILD       := build
SHARED      := ../src

APP_TITLE   := dualscreen companion
APP_AUTHOR  := dualscreen
APP_VERSION := 0.1.0

# Directories the sources live in. Whole directories are not listed, because
# most of them hold files this program has no business compiling -- an emulator
# core loader, a game browser, a GL compositor.
SOURCE_DIRS := source \
               $(SHARED)/audio \
               $(SHARED)/core \
               $(SHARED)/input \
               $(SHARED)/layout \
               $(SHARED)/net \
               $(SHARED)/render \
               $(SHARED)/util

CPPFILES    := main.cpp host_list.cpp screen_quad.cpp text_painter.cpp \
               audio_output.cpp \
               system.cpp \
               controller_mapping.cpp cursor_visibility.cpp ds_input.cpp input_manager.cpp \
               layout.cpp layout_registry.cpp \
               companion_session.cpp discovery.cpp protocol.cpp transport.cpp \
               font.cpp \
               gl_loader.cpp texture.cpp \
               log.cpp

# CompanionSession keeps each received screen as a GPU texture as well as
# pixels, for the desktop compositor that draws it that way. This client draws
# through SDL's renderer and never asks for those, but gl_loader.cpp and
# texture.cpp still have to be compiled for it to link. Nothing in them runs.

INCLUDES    := source $(SHARED)

#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

PORTLIBS   := $(PORTLIBS_PATH)/switch
SDL2_CONFIG := $(PORTLIBS)/bin/sdl2-config

CFLAGS  := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           $(shell $(SDL2_CONFIG) --cflags) \
           -D__SWITCH__ $(INCLUDE)
# The template's -fno-rtti -fno-exceptions are deliberately not here. The
# shared code throws nothing and asks nothing its type, so neither flag would
# change what the program does -- only what happens if the standard library
# ever needs to raise something, which on a device with no console is better
# as an error than as an abort.
CXXFLAGS := $(CFLAGS) -std=gnu++17

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# zlib is the frame codec's; everything else SDL2 needs on this platform comes
# out of sdl2-config, including the GL stack SDL's own renderer sits on. This
# program links no GL of its own: gl_loader.cpp reaches every entry point
# through SDL_GL_GetProcAddress, so it names no GL symbol at link time.
LIBS    := $(shell $(SDL2_CONFIG) --libs) -lz -lnx

LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------
export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCE_DIRS),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

export OFILES   := $(CPPFILES:.cpp=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
