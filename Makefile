# DukeNukem3D — Duke Nukem 3D for openfpgaOS (SDK fork)
#
# Usage:
#   make              Build Duke3D + bundled SDK apps → build/sdk/
#   make deploy       Copy to SD card
#   make clean        Remove all build artifacts
#   make package      Package Duke3D standalone core as ZIP

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
all: duke3d apps release

# ── Build Duke3D ─────────────────────────────────────────────────
duke3d:
	$(MAKE) -C src/duke3d

# ── Build bundled SDK apps ───────────────────────────────────────
apps:
	$(MAKE) -C src/apps

# ── Create build/sdk/ directory ──────────────────────────────────
release: duke3d apps
	@echo "Creating release/..."
	@mkdir -p $(REL_CORE) $(REL_ASSETS) $(REL_INSTANCE) $(REL_PLATFORM)/_images
	@# Core: bitstream + loader
	@cp $(RUNTIME)/bitstream.rbf_r $(REL_CORE)/
	@cp $(RUNTIME)/loader.bin $(REL_CORE)/
	@# Core: JSON configs + icon
	@[ -d dist/sdk/core ] && cp dist/sdk/core/*.json dist/sdk/core/*.bin $(REL_CORE)/ 2>/dev/null || true
	@# Platform
	@[ -d dist/sdk/platform ] && cp dist/sdk/platform/*.json $(REL_PLATFORM)/ 2>/dev/null || true
	@[ -d dist/sdk/platform/_images ] && cp dist/sdk/platform/_images/*.bin $(REL_PLATFORM)/_images/ 2>/dev/null || true
	@# OS binary
	@cp $(RUNTIME)/os.bin $(REL_ASSETS)/
	@# Duke3D
	@[ -f src/duke3d/app.elf ] && cp src/duke3d/app.elf $(REL_ASSETS)/duke3d.elf || true
	@# Bundled apps
	@for d in src/apps/*/; do \
		name=$$(basename "$$d"); \
		[ -f "$$d/app.elf" ] && cp "$$d/app.elf" "$(REL_ASSETS)/$$name.elf" || true; \
		find "$$d" -maxdepth 1 \( -name "*.mid" -o -name "*.wav" -o -name "*.dat" -o -name "*.png" \) \
			-exec cp {} "$(REL_ASSETS)/" \; 2>/dev/null || true; \
	done
	@# Instance JSONs
	@[ -d dist/sdk/instances ] && cp dist/sdk/instances/*.json $(REL_INSTANCE)/ 2>/dev/null || true
	@echo "Release ready: $(RELEASE)/"
	@# Standalone cores: for each dist/<name>/ (not sdk/), copy to build/<name>/
	@for coredir in dist/*/; do \
		name=$$(basename "$$coredir"); \
		[ "$$name" = "sdk" ] && continue; \
		[ ! -d "$$coredir/Cores" ] && continue; \
		echo "Packaging standalone: $$name"; \
		mkdir -p "build/$$name" && cp -r "$$coredir"/* "build/$$name/"; \
		shortlower=$$(echo "$$name" | tr '[:upper:]' '[:lower:]'); \
		if [ -f "src/$$shortlower/app.elf" ]; then \
			find "build/$$name/Assets" -name "*.elf" -delete; \
			assetdir=$$(find "build/$$name/Assets" -name "common" -type d | head -1); \
			[ -n "$$assetdir" ] && cp "src/$$shortlower/app.elf" "$$assetdir/$$shortlower.elf"; \
			sed -i "s/\"filename\": \".*\.elf\"/\"filename\": \"$$shortlower.elf\"/" \
				"build/$$name/Cores/"*"/data.json" 2>/dev/null || true; \
		fi; \
		assetcommon=$$(find "build/$$name/Assets" -name "common" -type d | head -1); \
		[ -n "$$assetcommon" ] && cp $(RUNTIME)/os.bin "$$assetcommon/" 2>/dev/null || true; \
		cp $(RUNTIME)/bitstream.rbf_r "build/$$name/Cores/"*"/bitstream.rbf_r" 2>/dev/null || true; \
		cp $(RUNTIME)/loader.bin "build/$$name/Cores/"*"/loader.bin" 2>/dev/null || true; \
	done

# ── Deploy to SD card ────────────────────────────────────────────
deploy: all
	@./deploy.sh

# ── Clean ────────────────────────────────────────────────────────
clean:
	$(MAKE) -C src/duke3d clean
	$(MAKE) -C src/apps clean
	rm -rf build releases

# ── Package Duke3D standalone core ───────────────────────────────
package:
	./package.sh Duke3D

.PHONY: all duke3d apps release deploy clean package
