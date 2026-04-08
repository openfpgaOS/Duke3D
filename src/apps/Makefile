# openfpgaOS SDK — Demo Apps
#
# Build, copy, and package the bundled demo apps.
#
# Targets:
#   make                  Build all demos → build/sdk/
#   make new APP=demo     Create a new demo app
#   make copy           Copy build/sdk/ to SD card
#   make package          Package build/sdk/ as ZIP
#   make clean            Remove build artifacts
#

SDK_ROOT  = $(realpath $(CURDIR)/../..)
RUNTIME   = $(SDK_ROOT)/runtime
DIST      = $(SDK_ROOT)/dist/sdk
CORE_ID   = ThinkElastic.openfpgaOS
PLATFORM  = openfpgaos
BUILD     = $(SDK_ROOT)/build/sdk

APPS = $(sort $(patsubst %/,%,$(dir $(wildcard */Makefile))))

all: build-apps release

build-apps:
	@for app in $(APPS); do \
		echo "Building $$app..."; \
		$(MAKE) -C $$app || exit 1; \
	done

# Assemble: dist/ (static) + runtime + ELFs + data files = deployable
release:
	@echo "Assembling build/sdk/..."
	@rm -rf $(BUILD)
	@mkdir -p $(SDK_ROOT)/build
	@cp -r $(DIST)/ $(BUILD)/
	@# Runtime binaries
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $(BUILD)/Cores/$(CORE_ID)/
	@mkdir -p $(BUILD)/Assets/$(PLATFORM)/common
	@cp $(RUNTIME)/os.bin $(BUILD)/Assets/$(PLATFORM)/common/
	@# Demo ELFs + data files
	@for app in $(APPS); do \
		[ -f "$(SDK_ROOT)/.obj/sdk/$$app/app.elf" ] && \
			cp "$(SDK_ROOT)/.obj/sdk/$$app/app.elf" "$(BUILD)/Assets/$(PLATFORM)/common/$$app.elf"; \
		for f in $$app/*.mid $$app/*.mod $$app/*.wav $$app/*.dat $$app/*.png; do \
			[ -f "$$f" ] && cp "$$f" "$(BUILD)/Assets/$(PLATFORM)/common/"; \
		done; \
	done 2>/dev/null; true
	@echo "Ready: build/sdk/"

new:
	@test -n "$(APP)" || { echo "Usage: make new APP=myapp"; exit 1; }
	@$(SDK_ROOT)/scripts/new.sh "$(APP)"
	@if [ -d "$(SDK_ROOT)/src/$(APP)" ] && [ ! -d "$(APP)" ]; then \
		mv "$(SDK_ROOT)/src/$(APP)" "./$(APP)"; \
		echo "Moved to src/apps/$(APP)/"; \
	fi

copy: all
	@$(SDK_ROOT)/scripts/copy.sh

package: all
	@$(SDK_ROOT)/scripts/package.sh

clean:
	@for app in $(APPS); do \
		$(MAKE) -C $$app clean; \
	done
	rm -rf $(BUILD)

.PHONY: all build-apps release new copy package clean
