# openfpgaOS SDK — Per-App Build Rules
#
# Invoked as: make -C <appdir> -f ../app.mk
# From src/apps/<name>/:
#   SDK is at ../../sdk/
#   sdk.mk is at ../../../sdk.mk
#

SDK_DIR = ../../sdk
SRCS     = $(wildcard *.c)
SRCS_CXX = $(wildcard *.cpp)

include $(SDK_DIR)/sdk.mk

# Explicit CRT build rule (pattern rule doesn't work across directories)
$(CRT_DIR)/start.o: $(CRT_DIR)/start.S
	$(AS) $(ASFLAGS) -c -o $@ $<

all: app.elf
	$(SIZE) $<

clean: sdk-clean

.PHONY: all clean
