# SD card write test (see sdtest.cpp).
#   make && make program-dfu   (hold BOOT, tap RESET, release BOOT first)
TARGET = sdtest

CPP_SOURCES = sdtest.cpp

LIBDAISY_DIR = ../../libDaisy
USE_FATFS    = 1
OPT         ?= -Os

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
