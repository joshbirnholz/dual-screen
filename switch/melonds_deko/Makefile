#---------------------------------------------------------------------------------
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds RSDuck's Switch port of melonDS -- the one that draws on the GPU with
# deko3d -- as a libretro core the Switch app links in, together with the
# shaders it draws with. From the switch directory:
#
#   git clone -b switch-new https://github.com/RSDuck/melonDS cores/melonds-deko
#   make -C melonds_deko -j8
#   make
#
# It makes cores/melonds_deko_libnx.a, which switch/Makefile prefers over
# libretro's melonDS when it finds it, and cores/melonds_deko_romfs, the
# shaders, which goes into the app's romfs. Needs deko3d and uam from
# devkitPro, and cmake:
#
#   (dkp-)pacman -S deko3d uam
#
# The emulator is compiled exactly as RSDuck's CMake build compiles it for the
# Switch -- the same sources and the same definitions -- except that its own
# Switch frontend is left out and this directory's glue is in its place.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO in your environment, e.g. export DEVKITPRO=/opt/devkitpro)
endif

MELONDS ?= ../cores/melonds-deko
OUT     ?= ../cores
BUILD   ?= build

SRC := $(MELONDS)/src
ifeq ($(wildcard $(SRC)/GPU3D_Deko.cpp),)
$(error No RSDuck melonDS at $(MELONDS) -- clone it there: \
        git clone -b switch-new https://github.com/RSDuck/melonDS cores/melonds-deko \
        (from the switch directory), or set MELONDS to where it is)
endif

# By their whole paths: the make that comes with macOS starts a command with
# no shell in it from its own PATH, which does not have devkitPro's tools on
# it (see switch/README.md).
TOOLS := $(DEVKITPRO)/devkitA64/bin
CC    := $(TOOLS)/aarch64-none-elf-gcc
CXX   := $(TOOLS)/aarch64-none-elf-g++
AR    := $(TOOLS)/aarch64-none-elf-ar
UAM   ?= $(DEVKITPRO)/tools/bin/uam
CMAKE ?= cmake

ifeq ($(wildcard $(DEVKITPRO)/libnx/include/deko3d.hpp),)
$(error deko3d is not installed: (dkp-)pacman -S deko3d)
endif

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections
DEFINES := -D__SWITCH__ -DARCHITECTURE_ARM64=1 -DJIT_ENABLED -DDEKOGPU_ENABLED \
           -DNEONSOFTGPU_ENABLED '-DMELONDS_VERSION="0.9.2"'
INCLUDE := -I$(SRC) -I$(SRC)/teakra/include -I$(DEVKITPRO)/libnx/include \
           -I$(DEVKITPRO)/portlibs/switch/include -I$(CURDIR)/../../third_party/libretro
# gnu++17 rather than c++17, as CMake compiles it: strict C++17 hides the POSIX
# half of devkitPro's C library, which the JIT's borrowed Dolphin code uses.
#
# -fno-strict-aliasing: melonDS reads its byte arrays of video memory through
# u16 and u32 pointers throughout -- the texture decoding the 3D renderer does
# included -- which the language does not allow and a newer GCC at -O3 is free
# to compile into something that reads stale or reordered values. melonDS's
# own Switch release was built years ago with an older GCC that happened not
# to; this build uses today's.
#
# Overridable, for finding out whether today's compiler is what draws the
# wrong textures: make OPTIMISE=-O1 in the switch directory, which passes it
# down to this one. The flags stamp below rebuilds the core when it changes,
# and again when a plain make goes back to the default.
OPTIMISE ?= -O3 -fno-strict-aliasing
CXXFLAGS := $(OPTIMISE) -std=gnu++17 $(ARCH) $(DEFINES) $(INCLUDE) -MMD -MP
CFLAGS   := $(OPTIMISE) -std=gnu11 $(ARCH) $(DEFINES) $(INCLUDE) -MMD -MP

# RSDuck's src/CMakeLists.txt for a Switch build: the core, the deko3d
# renderers, the ARM64 JIT -- and from the frontend, only the pieces the
# renderers are built on and the microphone's recorded blow. Its
# UploadBuffer.cpp is upload_buffer.cpp here: the same, with a hook for
# vramcheck.
CORE_SOURCES := \
    ARCodeFile.cpp AREngine.cpp ARM.cpp ARMInterpreter.cpp ARMInterpreter_ALU.cpp \
    ARMInterpreter_Branch.cpp ARMInterpreter_LoadStore.cpp Config.cpp CP15.cpp CRC32.cpp \
    DMA.cpp DSi.cpp DSi_AES.cpp DSi_Camera.cpp DSi_DSP.cpp DSi_I2C.cpp DSi_NDMA.cpp \
    DSi_NWifi.cpp DSi_SD.cpp DSi_SPI_TSC.cpp GBACart.cpp GPU.cpp GPU2D.cpp GPU2D_Soft.cpp \
    GPU3D.cpp GPU3D_Transform.cpp GPU3D_Soft.cpp NDS.cpp NDSCart.cpp NDSCart_SRAMManager.cpp \
    RTC.cpp Savestate.cpp SPI.cpp SPU.cpp Wifi.cpp WifiAP.cpp \
    tiny-AES-c/aes.c xxhash/xxhash.c \
    GPU2D_Deko.cpp GPU3D_Deko.cpp \
    ARM_InstrInfo.cpp ARMJIT.cpp ARMJIT_Memory.cpp dolphin/CommonFuncs.cpp \
    dolphin/Arm64Emitter.cpp dolphin/MathUtil.cpp \
    ARMJIT_A64/ARMJIT_Compiler.cpp ARMJIT_A64/ARMJIT_ALU.cpp ARMJIT_A64/ARMJIT_LoadStore.cpp \
    ARMJIT_A64/ARMJIT_Branch.cpp ARMJIT_A64/ARMJIT_Linkage.S \
    frontend/Util_Audio.cpp frontend/switch/GpuMemHeap.cpp

# The DSi's DSP, which melonDS links as the teakra library.
TEAKRA_SOURCES := $(addprefix teakra/src/,ahbm.cpp apbp.cpp btdmp.cpp disassembler.cpp dma.cpp \
    timer.cpp memory_interface.cpp mmio.cpp parser.cpp processor.cpp teakra.cpp \
    test_generator.cpp)

ADAPTER_SOURCES := gfx.cpp platform.cpp libretro.cpp upload_buffer.cpp

object_of = $(BUILD)/core/$(basename $(1)).o
CORE_OBJECTS    := $(foreach source,$(CORE_SOURCES) $(TEAKRA_SOURCES),$(call object_of,$(source)))
TEAKRA_OBJECTS  := $(foreach source,$(TEAKRA_SOURCES),$(call object_of,$(source)))
ADAPTER_OBJECTS := $(addprefix $(BUILD)/adapter/,$(ADAPTER_SOURCES:.cpp=.o))

ARCHIVE := $(OUT)/melonds_deko_libnx.a
ROMFS   := $(OUT)/melonds_deko_romfs
SHADERS_STAMP := $(BUILD)/shaders.stamp

.PHONY: all clean standalone
all: $(ARCHIVE) $(SHADERS_STAMP)

# The flags every object was compiled with, rewritten only when they change:
# make notices a changed source file but not a changed flag, and a core built
# before a flag changed would otherwise be linked as it was.
FLAGS_STAMP := $(BUILD)/flags
# Quoted for the shell by hand, as make 3.81 -- macOS's -- has no $(file).
SQ := '
FLAGS_NOW := $(subst $(SQ),$(SQ)"$(SQ)"$(SQ),$(CXX) $(CXXFLAGS) | $(CC) $(CFLAGS))
$(shell mkdir -p $(BUILD); if [ "$$(cat $(FLAGS_STAMP) 2>/dev/null)" != '$(FLAGS_NOW)' ]; then \
    printf '%s' '$(FLAGS_NOW)' > $(FLAGS_STAMP); fi)
$(CORE_OBJECTS) $(ADAPTER_OBJECTS): $(FLAGS_STAMP)

$(ARCHIVE): $(CORE_OBJECTS) $(ADAPTER_OBJECTS)
	@mkdir -p $(dir $@)
	@rm -f $@
	@echo archiving $(notdir $@)
	@$(AR) rcs $@ $^

# teakra's interpreter.h uses std::exchange without including <utility>, which
# newer compilers no longer bring in on their own.
$(TEAKRA_OBJECTS): CXXFLAGS += -include utility -I$(SRC)/teakra/src

$(BUILD)/core/%.o: $(SRC)/%.cpp
	@mkdir -p $(dir $@)
	@echo $*.cpp
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# RSDuck's sources this directory carries a fix for, each in patches/ under
# its name: compiled from a copy with the fix applied, so the checkout stays
# exactly as cloned. patches/GPU3D_Deko-texture-tile-size.patch is the one so
# far -- see it for what it fixes.
PATCHED_SOURCES := $(patsubst patches/%.patch,%,$(wildcard patches/*.patch))
define patched_source
$(BUILD)/patched/$(1).cpp: $(SRC)/$(1).cpp patches/$(2).patch
	@mkdir -p $$(dir $$@)
	@echo patching $(1).cpp with $(2).patch
	@cp $$< $$@
	@patch --quiet $$@ < patches/$(2).patch
$(BUILD)/core/$(1).o: $(BUILD)/patched/$(1).cpp
	@mkdir -p $$(dir $$@)
	@echo $(1).cpp
	@$$(CXX) $$(CXXFLAGS) -c $$< -o $$@
endef
$(foreach patch,$(PATCHED_SOURCES),$(eval $(call patched_source,$(firstword $(subst -, ,$(patch))),$(patch))))

$(BUILD)/core/%.o: $(SRC)/%.c
	@mkdir -p $(dir $@)
	@echo $*.c
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/core/%.o: $(SRC)/%.S
	@mkdir -p $(dir $@)
	@echo $*.S
	@$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

$(BUILD)/adapter/%.o: %.cpp melonds_deko.hpp
	@mkdir -p $(dir $@)
	@echo $<
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(SHADERS_STAMP): compile_shaders.cmake $(wildcard $(SRC)/shaders/*.glsl) \
                  $(SRC)/frontend/switch/CMakeLists.txt
	@mkdir -p $(dir $@)
	@$(CMAKE) -DMELONDS=$(abspath $(MELONDS)) -DOUT=$(abspath $(ROMFS))/shaders \
	    -DWORK=$(abspath $(BUILD))/shaders -DUAM=$(UAM) -P compile_shaders.cmake
	@touch $@

clean:
	@rm -rf $(BUILD) $(ARCHIVE) $(ROMFS)

# melonDS's own Switch app, built from this same checkout with this same
# devkitPro, the way its CMake build makes it:
#
#   make -C melonds_deko standalone
#
# and copy ../cores/melonDS-today.nro to the SD card beside the melonDS.nro
# already there. It is built without the fixes in patches/, so it draws what
# RSDuck's code draws: in Bowser's Inside Story, the same patches of garbage
# Dualscreen drew before GPU3D_Deko-texture-tile-size.patch.
# Link-time optimisation is left off, as this core has it --
# its CMake runs the host's gcc-ar for it, which cannot read these objects --
# and two headers are included up front that newer compilers no longer bring
# in on their own: <utility> for teakra, <unordered_map> for the app's ROM
# database. Its C files are older still: mm_vec.c calls sqrt() and the rest
# without <math.h>, and GCC 14 made that, and the other old C habits below,
# errors rather than warnings.
STANDALONE_BUILD := $(BUILD)/standalone
STANDALONE_C_LENIENCE := -Wno-error=implicit-function-declaration -Wno-error=implicit-int \
    -Wno-error=int-conversion -Wno-error=incompatible-pointer-types
standalone:
	@$(CMAKE) -S $(MELONDS) -B $(STANDALONE_BUILD) \
	    -DCMAKE_TOOLCHAIN_FILE=$(abspath $(MELONDS))/cmake/Toolchain-cross-Switch.cmake \
	    -DCMAKE_BUILD_TYPE=Release -DBUILD_QT_SDL=OFF -DENABLE_OGLRENDERER=OFF -DENABLE_LTO=OFF \
	    '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -include utility -include unordered_map' \
	    '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -include math.h $(STANDALONE_C_LENIENCE)' \
	    -DELF2NRO=$(DEVKITPRO)/tools/bin/elf2nro -DNACPTOOL=$(DEVKITPRO)/tools/bin/nacptool \
	    -DUAM=$(UAM)
	@$(MAKE) -C $(STANDALONE_BUILD)
	@mkdir -p $(OUT)
	@cp $(STANDALONE_BUILD)/melonDS.nro $(OUT)/melonDS-today.nro
	@echo made $(OUT)/melonDS-today.nro

-include $(CORE_OBJECTS:.o=.d) $(ADAPTER_OBJECTS:.o=.d)
