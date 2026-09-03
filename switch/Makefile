#---------------------------------------------------------------------------------
# Dualscreen for the Nintendo Switch.
#
# Built with devkitPro's devkitA64 toolchain and libnx. It needs switch-sdl2 and
# switch-zlib from the portlibs -- on Windows, from the MSYS2 shell devkitPro's
# own installer sets up, with no `sudo` (there is no such command in that
# shell -- what a `sudo` typed there actually runs is Windows' own unrelated
# sudo.exe, gated behind a Settings toggle that has nothing to do with this --
# and the whole devkitPro tree is already owned by your own account there; on
# Linux, prefix these with `sudo` if devkitPro was installed somewhere only
# root can write). devkitPro's own convention is `(dkp-)pacman`, meaning
# either name should resolve to the same thing on a properly set-up install,
# but only plain `pacman` is guaranteed to be on PATH -- it ships with MSYS2
# itself, where `dkp-pacman` is a devkitPro-added wrapper that is not always
# there to find:
#
#   pacman -S switch-dev switch-sdl2 switch-zlib
#   make            # -> Dualscreen.nro
#
# switch-ffmpeg is optional -- add it and the GamePad screen streams as H.264
# instead of changed tiles, at a fraction of the bandwidth (see
# docs/research-notes.md), through Averne's NVTEGRA hwaccel it is built with:
#
#   pacman -S switch-ffmpeg
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
TARGET      := Dualscreen
BUILD       := build
SHARED      := ../src
CLIENT      := ../companion/source

APP_TITLE   := Dualscreen
APP_AUTHOR  := Dualscreen
APP_VERSION := 0.1.0
# 256x256 JPEG -- a Switch icon is not a PNG, whatever the 3DS and Wii U
# templates do.
ICON        := icon.jpg

# Directories the sources live in. Whole directories are not listed, because
# most of them hold files this program has no business compiling -- an emulator
# core loader, a game browser, a GL compositor.
THIRD_PARTY := ../third_party
# Where an embedded asset lands once it has been turned into source. Defined
# before SOURCE_DIRS, which expands it immediately.
GENERATED   := generated

# The assets that travel inside the binary. One list, so what gets generated
# and what gets compiled cannot drift apart -- a generated file that nothing
# compiles is an undefined symbol at link time and nothing sooner.
EMBEDDED_SOURCES := kDsBezelPng.cpp kThreeDsBezelPng.cpp kRodinOtf.cpp kButtonIconsPng.cpp

SOURCE_DIRS := $(CLIENT) \
               $(GENERATED) \
               $(SHARED)/audio \
               $(SHARED)/core \
               $(SHARED)/input \
               $(SHARED)/layout \
               $(SHARED)/net \
               $(SHARED)/render \
               $(SHARED)/ui \
               $(SHARED)/util \
               $(SHARED)/video

CPPFILES    := main.cpp \
               audio_output.cpp \
               system.cpp \
               controller_mapping.cpp cursor_visibility.cpp ds_input.cpp dsu_client.cpp input_manager.cpp motion.cpp \
               layout.cpp layout_registry.cpp \
               companion_session.cpp discovery.cpp protocol.cpp transport.cpp \
               video_decoder.cpp video_encoder.cpp \
               bezel_art.cpp button_glyphs.cpp compositor.cpp font.cpp gl_loader.cpp gl_window.cpp \
               menu_backdrop.cpp png_image.cpp quad_renderer.cpp text_renderer.cpp texture.cpp \
               connecting_screen.cpp control_guide.cpp launcher_screen.cpp layout_picker.cpp \
               log.cpp \
               $(EMBEDDED_SOURCES)

INCLUDES    := $(CLIENT) $(SHARED) $(THIRD_PARTY)

#---------------------------------------------------------------------------------
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

PORTLIBS   := $(PORTLIBS_PATH)/switch
SDL2_CONFIG := $(PORTLIBS)/bin/sdl2-config

# Optional, the same way CMakeLists.txt treats it for every other build: the
# GamePad screen carries fine as changed tiles without it, just at several
# times the bandwidth -- see docs/research-notes.md. `switch-ffmpeg` is an
# official devkitPro portlib built with --enable-nvtegra (Averne's NVDEC
# work), so its presence here is the same as the SDL2 check just above: found
# by the same $(PORTLIBS)/lib devkitPro already installs everything else
# into, nothing extra to point at.
#
#   pacman -S switch-ffmpeg
#
HAVE_FFMPEG := $(if $(wildcard $(PORTLIBS)/lib/libavcodec.a),1,0)
ifeq ($(HAVE_FFMPEG),1)
$(info video codec: libavcodec found, building with the NVTEGRA decoder)
else
$(info video codec: switch-ffmpeg not installed, building without one)
endif

CFLAGS  := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES) \
           $(shell $(SDL2_CONFIG) --cflags) \
           -D__SWITCH__ -DDS_HAVE_VIDEO_CODEC=$(HAVE_FFMPEG) $(INCLUDE)
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

# switch-ffmpeg's own dependencies (switch-dav1d, switch-libass and what
# *that* needs in turn) are still static archives this binary has to satisfy
# itself, the same as any static link -- ffmpeg only ever gets this far
# because something upstream of it pulled a symbol out of libdav1d.a or
# libass.a, not because this program calls either directly. -lswresample is
# the same story from a different direction: libavcodec's native Opus
# decoder references it internally (used for downmix), and static linking
# pulls in whole object files rather than individual functions, so anything
# else this program needs out of that same file drags Opus's own unused
# references along with it. --start-group / --end-group sidesteps having to
# work out and maintain the right pairwise order among all of them by hand,
# which the version bundled with this one changing would otherwise be a
# standing invitation to get wrong.
#
# mmuRequest*/mmuInitialize/mmuExit, which Averne's NVTEGRA hwaccel calls to
# talk to the Tegra's NVDEC block, are not missing from any archive -- they
# are libnx's own (nx/source/services/mm.c, despite the file being named
# "mm" and not "mmu"), and -lnx is already on this link line above. The
# problem is purely order: a static archive is only scanned once, for
# whatever symbols are still undefined at the point the linker reaches it,
# and -lnx sits before ffmpeg's archives -- so by the time avutil's nvtegra
# code creates the need for mmuRequestInitialize, the linker has already
# passed over libnx.a and never comes back to it. Repeating -lnx inside this
# group makes the linker scan it again alongside ffmpeg's own archives,
# after that need exists.
ifeq ($(HAVE_FFMPEG),1)
LIBS += -Wl,--start-group \
        -lavcodec -lavutil -lswscale -lswresample -ldav1d -lass -lfribidi -lfreetype -lbz2 \
        -lnx \
        -Wl,--end-group
endif

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
$(error Cannot find $(SDL2_CONFIG) -- install it with: pacman -S switch-sdl2)
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
        pacman -S cmake, or set CMAKE to it)
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
assets: $(addprefix $(GENERATED)/,$(EMBEDDED_SOURCES))

$(GENERATED)/kDsBezelPng.cpp: $(CURDIR)/../assets/ds_bezel.png $(CURDIR)/../tools/embed_asset.cmake
	@mkdir -p $(GENERATED)
	@echo embedding ds_bezel.png
	@$(CMAKE) -DINPUT=$< -DOUTPUT=$@ -DSYMBOL=kDsBezelPng -P $(CURDIR)/../tools/embed_asset.cmake

$(GENERATED)/kButtonIconsPng.cpp: $(CURDIR)/../assets/button_icons.png $(CURDIR)/../tools/embed_asset.cmake
	@mkdir -p $(GENERATED)
	@echo embedding button_icons.png
	@$(CMAKE) -DINPUT=$< -DOUTPUT=$@ -DSYMBOL=kButtonIconsPng -P $(CURDIR)/../tools/embed_asset.cmake

$(GENERATED)/kRodinOtf.cpp: $(CURDIR)/../assets/rodin.otf $(CURDIR)/../tools/embed_asset.cmake
	@mkdir -p $(GENERATED)
	@echo Embedding assets/rodin.otf
	@$(CMAKE) -DINPUT=$< -DOUTPUT=$@ -DSYMBOL=kRodinOtf -P $(CURDIR)/../tools/embed_asset.cmake

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
