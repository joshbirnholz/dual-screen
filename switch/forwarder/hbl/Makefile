#---------------------------------------------------------------------------------
# hbloader, as a forwarder carries it: build/hbl.nso (the forwarder's
# exefs/main) and build/hbl.npdm (its main.npdm), which switch/Makefile
# builds into Dualscreen and forwarder_install.cpp puts in each forwarder.
#
# nx-hbloader (ISC, see LICENSE.md) as Sphaira modified it -- source/ and
# hbl.json are Sphaira's hbl/ unchanged. Standalone rather than devkitPro's
# template, which reads what switch/Makefile exports to its own sub-make.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error Set DEVKITPRO in your environment, e.g. export DEVKITPRO=/opt/devkitpro)
endif

TOOLS    := $(DEVKITPRO)/devkitA64/bin
CC       := $(TOOLS)/aarch64-none-elf-gcc
ELF2NSO  ?= $(DEVKITPRO)/tools/bin/elf2nso
NPDMTOOL ?= $(DEVKITPRO)/tools/bin/npdmtool
LIBNX    := $(DEVKITPRO)/libnx
BUILD    := build
VERSION  := 3.0.0

ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -Wall -O2 -ffunction-sections -std=gnu11 $(ARCH) -D__SWITCH__ \
           '-DVERSION="$(VERSION)"' -I$(LIBNX)/include -MMD -MP
ASFLAGS := -g $(ARCH)
# exit() is never called; main.c stubs it, as nx-hbloader does.
LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-wrap,exit -L$(LIBNX)/lib

.PHONY: all clean
all: $(BUILD)/hbl.nso $(BUILD)/hbl.npdm

$(BUILD):
	@mkdir -p $@

$(BUILD)/main.o: source/main.c | $(BUILD)
	@echo hbloader: $(notdir $<)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/trampoline.o: source/trampoline.s | $(BUILD)
	@echo hbloader: $(notdir $<)
	@$(CC) -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

$(BUILD)/hbl.elf: $(BUILD)/main.o $(BUILD)/trampoline.o
	@$(CC) $(LDFLAGS) $^ -lnx -o $@

$(BUILD)/hbl.nso: $(BUILD)/hbl.elf
	@$(ELF2NSO) $< $@
	@echo hbloader: built $(notdir $@)

$(BUILD)/hbl.npdm: hbl.json | $(BUILD)
	@$(NPDMTOOL) $< $@
	@echo hbloader: built $(notdir $@)

clean:
	@rm -rf $(BUILD)

-include $(BUILD)/main.d
