# openfpgaOS SDK — Per-App Build Rules
#
# Included by each demo app's Makefile.
# Intermediates: .obj/sdk/<name>/
# Final ELF copied to build/sdk/ by the parent Makefile's release step.
#

SDK_DIR   = ../../sdk
ROOT      = $(realpath $(CURDIR)/../../..)
APP_NAME  = $(notdir $(CURDIR))
OBJ_DIR   = $(ROOT)/.obj/sdk/$(APP_NAME)
BUILD_DIR = $(OBJ_DIR)

SRCS     = $(wildcard *.c)
SRCS_CXX = $(wildcard *.cpp)

.DEFAULT_GOAL := all

include $(SDK_DIR)/sdk.mk

all: $(OBJ_DIR)/app.elf
	@$(SIZE) $<

clean: sdk-clean

.PHONY: all clean
