# Duke Nukem 3D for openfpgaOS
#
# Quick start:
#   make build            Build Duke3D + SDK demos
#   make build APP=duke3d Build just Duke3D
#   make build APP=sdk    Build just SDK demos
#   make deploy           Deploy to Pocket SD card

# ── Paths ────────────────────────────────────────────────────────────
RUNTIME = runtime

# ── Default target ───────────────────────────────────────────────────
all: help

# ── Help ─────────────────────────────────────────────────────────────
help:
	@echo "         ___  ___  ___ ___"
	@echo "        / _ \\/ _ \\/ -_) _ \\"
	@echo "        \\___/ .__/\\__/_//_/"
	@echo "       ____/_/  ________"
	@echo "      / __/ _ \\/ ___/ _ |"
	@echo "     / _// ___/ (_ / __ |"
	@echo "    /_/_/_/___\\___/_/ |_|"
	@echo "   / __ \\/ __/"
	@echo "  / /_/ /\\ \\"
	@echo "  \\____/___/  Duke Nukem 3D"
	@echo ""
	@echo "  Build:"
	@echo "    make build            Build everything"
	@echo "    make build APP=duke3d Build just Duke3D  → build/duke3d/"
	@echo "    make build APP=sdk    Build SDK demos    → build/sdk/"
	@echo ""
	@echo "  Deploy:"
	@echo "    make deploy           Deploy everything to SD card"
	@echo "    make deploy APP=duke3d Deploy just Duke3D"
	@echo "    make deploy APP=sdk   Deploy just SDK + demos"
	@echo ""
	@echo "  Dev loop (UART):"
	@echo "    make exec             Build Duke3D, push via UART, stream console"
	@echo ""
	@echo "  Other:"
	@echo "    make package          Package Duke3D as distributable ZIP"
	@echo "    make tools            Build PHDP host tools"
	@echo "    make clean            Remove all build artifacts"

# ── Build ────────────────────────────────────────────────────────────
build:
ifdef APP
ifeq ($(APP),sdk)
	$(MAKE) -C src/apps
else ifeq ($(APP),duke3d)
	$(MAKE) -C src/duke3d
	@$(MAKE) --no-print-directory release-duke3d
else
	$(MAKE) -C src/$(APP)
endif
else
	$(MAKE) -C src/duke3d
	$(MAKE) -C src/apps
	@$(MAKE) --no-print-directory release-duke3d
endif

# ── Assemble build/duke3d/ from dist/ + runtime + ELF ────────────────
release-duke3d:
	@echo "Assembling build/duke3d/..."
	@rm -rf build/duke3d
	@mkdir -p build
	@cp -r dist/Duke3D build/duke3d
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $$(ls -d build/duke3d/Cores/*/)/
	@mkdir -p $$(ls -d build/duke3d/Assets/*/)/common
	@cp $(RUNTIME)/os.bin $$(ls -d build/duke3d/Assets/*/)/common/
	@[ -f .obj/duke3d/app.elf ] && cp .obj/duke3d/app.elf $$(ls -d build/duke3d/Assets/*/)/common/duke3d.elf || true
	@echo "Ready: build/duke3d/"

# ── Exec (UART) ─────────────────────────────────────────────────────
exec:
	$(MAKE) -C src/duke3d
	@$(MAKE) --no-print-directory release-duke3d
	@./scripts/exec.sh $$(find build/duke3d/Assets -name "duke3d.elf" | head -1)

# ── Deploy ───────────────────────────────────────────────────────────
deploy:
ifdef APP
ifeq ($(APP),sdk)
	$(MAKE) -C src/apps deploy
else ifeq ($(APP),duke3d)
	@$(MAKE) --no-print-directory build APP=duke3d
	@src/sdk/platforms/pocket/deploy.sh "duke3d" "$$(find build/duke3d/Assets -name 'duke3d.elf' | head -1)"
else
	$(MAKE) -C src/$(APP) deploy
endif
else
	@$(MAKE) --no-print-directory build
	@src/sdk/platforms/pocket/deploy.sh "duke3d" "$$(find build/duke3d/Assets -name 'duke3d.elf' | head -1)"
endif

# ── Package ──────────────────────────────────────────────────────────
package:
	@$(MAKE) --no-print-directory build APP=duke3d
	./scripts/package.sh Duke3D

# ── Tools ────────────────────────────────────────────────────────────
tools:
	$(MAKE) -C src/tools/phdp

# ── Clean ────────────────────────────────────────────────────────────
clean:
	$(MAKE) -C src/duke3d clean
	$(MAKE) -C src/apps clean
	$(MAKE) -C src/tools/phdp clean 2>/dev/null || true
	rm -rf build .obj releases

.PHONY: all help build release-duke3d exec deploy package tools clean
