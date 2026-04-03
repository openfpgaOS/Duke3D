# openfpgaOS SDK Makefile
#
# Usage:
#   make              Build all apps, create release/
#   make deploy       Copy release/ to Pocket SD card
#   make clean        Remove all build artifacts
#   make core         Build a standalone game core (interactive)
#   make package      Package game core into a ZIP

# ── Paths ────────────────────────────────────────────────────────
CORE_ID      = ThinkElastic.openfpgaOS
PLATFORM     = openfpgaos
RELEASE      = build/sdk
REL_CORE     = $(RELEASE)/Cores/$(CORE_ID)
REL_ASSETS   = $(RELEASE)/Assets/$(PLATFORM)/common
REL_INSTANCE = $(RELEASE)/Assets/$(PLATFORM)/$(CORE_ID)
REL_PLATFORM = $(RELEASE)/Platforms
RUNTIME      = runtime

# ── Default target ───────────────────────────────────────────────
all: apps release

# ── Build all bundled apps ───────────────────────────────────────
apps:
	$(MAKE) -C src/apps

# ── Create release/ directory ────────────────────────────────────
release: apps
	@echo "Creating release/..."
	@mkdir -p $(REL_CORE) $(REL_ASSETS) $(REL_INSTANCE) $(REL_PLATFORM)/_images
	@# Core: bitstream + loader
	@cp $(RUNTIME)/bitstream.rbf_r $(REL_CORE)/
	@cp $(RUNTIME)/loader.bin $(REL_CORE)/
	@# Core: JSON configs + icon (from dist/sdk/ if present)
	@[ -d dist/sdk/core ] && cp dist/sdk/core/*.json dist/sdk/core/*.bin $(REL_CORE)/ 2>/dev/null || true
	@# Platform
	@[ -d dist/sdk/platform ] && cp dist/sdk/platform/*.json $(REL_PLATFORM)/ 2>/dev/null || true
	@[ -d dist/sdk/platform/_images ] && cp dist/sdk/platform/_images/*.bin $(REL_PLATFORM)/_images/ 2>/dev/null || true
	@# OS binary
	@cp $(RUNTIME)/os.bin $(REL_ASSETS)/
	@# Bundled apps + data files
	@for d in src/apps/*/; do \
		name=$$(basename "$$d"); \
		[ -f "$$d/app.elf" ] && cp "$$d/app.elf" "$(REL_ASSETS)/$$name.elf" || true; \
		find "$$d" -maxdepth 1 \( -name "*.mid" -o -name "*.wav" -o -name "*.dat" -o -name "*.png" \) \
			-exec cp {} "$(REL_ASSETS)/" \; 2>/dev/null || true; \
	done
	@# Instance JSONs
	@[ -d dist/sdk/instances ] && cp dist/sdk/instances/*.json $(REL_INSTANCE)/ 2>/dev/null || true
	@echo "Release ready: $(RELEASE)/"

# ── Deploy to SD card ────────────────────────────────────────────
deploy: release
	@./scripts/deploy.sh

# ── Clean ────────────────────────────────────────────────────────
clean:
	$(MAKE) -C src/apps clean
	rm -rf build releases

# ── Core packaging ───────────────────────────────────────────────
core:
	./scripts/customize.sh

package:
	./scripts/package.sh

.PHONY: all apps release deploy clean core package
