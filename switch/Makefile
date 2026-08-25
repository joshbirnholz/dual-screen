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
# 256x256 JPEG -- a Switch icon is not a PNG, whatever the 3DS and Wii U
# templates do.
ICON        := icon.jpg

# Directories the sources live in. Whole directories are not listed, because
# most of them hold files this program has no business compiling -- an emulator
# core loader, a game browser, a GL compositor.
THIRD_PARTY := ../third_party
# Where the console artwork lands once it has been turned into source. Defined
# before SOURCE_DIRS, which expands it immediately.
GENERATED   := generated

SOURCE_DIRS := source \
               $(GENERATED) \
               $(SHARED)/audio \
               $(SHARED)/core \
               $(SHARED)/input \
               $(SHARED)/layout \
               $(SHARED)/net \
               $(SHARED)/render \
               $(SHARED)/ui \
               $(SHARED)/util

CPPFILES    := main.cpp \
               audio_output.cpp \
               system.cpp \
               controller_mapping.cpp cursor_visibility.cpp ds_input.cpp input_manager.cpp \
               layout.cpp layout_registry.cpp \
               companion_session.cpp discovery.cpp protocol.cpp transport.cpp \
               bezel_art.cpp compositor.cpp font.cpp gl_loader.cpp gl_window.cpp \
               png_image.cpp quad_renderer.cpp text_renderer.cpp texture.cpp \
               connecting_screen.cpp launcher_screen.cpp layout_picker.cpp \
               log.cpp \
               kDsBezelPng.cpp kThreeDsBezelPng.cpp

INCLUDES    := source $(SHARED) $(THIRD_PARTY)

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

# Which compiler driver does the link. devkitPro's base_rules sets CC, CXX, AS
# and the rest but deliberately leaves this one to the project, because only
# the project knows whether there is any C++ to link a runtime for -- and make
# has a built-in default of a bare `ld`, which is not a cross-linker and not
# even on the PATH, so leaving it unset fails at the last step with "ld: No
# such file or directory" after everything has compiled.
ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

# A missing portlib otherwise shows up as several hundred undefined SDL
# symbols at the link, which says nothing about what to install.
ifeq ($(wildcard $(SDL2_CONFIG)),)
$(error Cannot find $(SDL2_CONFIG) -- install it with: dkp-pacman -S switch-sdl2)
endif

# $(TOPDIR), not $(CURDIR): the sub-make re-reads this file from $(BUILD),
# where $(CURDIR) is that directory rather than the project's.
ifeq ($(wildcard $(TOPDIR)/$(ICON)),)
$(error Cannot find $(ICON), which the .nro needs an asset section built from)
endif

# The console artwork the Standard layout draws, turned into source so it
# travels inside the binary. The same script the desktop build uses, run the
# same way, so the two cannot end up with different pictures in them.
CMAKE ?= cmake
ifeq ($(shell command -v $(CMAKE) 2>/dev/null),)
$(error Cannot find cmake, which turns the console artwork into source. \
        dkp-pacman -S cmake, or set CMAKE to it)
endif

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

# What goes in the .nro's asset section: the icon, and the name and version a
# menu shows. Without them elf2nro is still handed a good ELF and still writes
# a file, but one with no asset header on the end at all -- which is what a
# loader means when it says it cannot parse the nro, and why launching it drops
# straight back to the menu with nothing having run. Set here rather than at
# the top of the file because $(CURDIR) is the project directory only out here:
# the sub-make runs in $(BUILD) and inherits these through the environment, the
# same way it inherits OUTPUT and VPATH.
export APP_ICON := $(CURDIR)/$(ICON)
export NROFLAGS := --icon=$(APP_ICON) --nacp=$(OUTPUT).nacp

.PHONY: all clean

all: $(BUILD) assets
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

.PHONY: assets
assets: $(GENERATED)/kDsBezelPng.cpp $(GENERATED)/kThreeDsBezelPng.cpp

$(GENERATED)/kDsBezelPng.cpp: $(CURDIR)/../assets/ds_bezel.png $(CURDIR)/../tools/embed_asset.cmake
	@mkdir -p $(GENERATED)
	@echo embedding ds_bezel.png
	@$(CMAKE) -DINPUT=$< -DOUTPUT=$@ -DSYMBOL=kDsBezelPng -P $(CURDIR)/../tools/embed_asset.cmake

$(GENERATED)/kThreeDsBezelPng.cpp: $(CURDIR)/../assets/three_ds_bezel.png $(CURDIR)/../tools/embed_asset.cmake
	@mkdir -p $(GENERATED)
	@echo embedding three_ds_bezel.png
	@$(CMAKE) -DINPUT=$< -DOUTPUT=$@ -DSYMBOL=kThreeDsBezelPng -P $(CURDIR)/../tools/embed_asset.cmake

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(GENERATED) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

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
