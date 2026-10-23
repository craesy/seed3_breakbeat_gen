# makebreakbeat for the Daisy Seed3 - firmware (libDaisy v9, 128 KB internal flash).
#   make libdaisy      build libDaisy once (size-optimised, required to fit)
#   make               build build/MakeBreakBeatSeed3.bin
#   make program-dfu   flash over USB (hold BOOT, tap RESET, release BOOT first)
TARGET = MakeBreakBeatSeed3

CPP_SOURCES = src/main.cpp $(wildcard src/engine/*.cpp)

LIBDAISY_DIR = libDaisy
USE_FATFS    = 1
CPP_STANDARD = -std=gnu++17
OPT         ?= -Os -flto # LTO saves ~2.5 KB of flash
C_INCLUDES  += -Isrc/engine

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Generating spends its time in the DSP kernels: -O2 there costs ~2.4 KB of flash.
$(BUILD_DIR)/dsp.o: OPT = -O2 -flto

libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR) OPT=-Os

.PHONY: libdaisy
