# openfpgaOS SDK Makefile
#
# Quick start:
#   make setup          Install RISC-V toolchain
#   make core           Create your app

# ── Paths ────────────────────────────────────────────────────────────
RUNTIME = runtime

# ── Detect user app (src/<name>/ excluding apps/, sdk/, tools/) ──────
APP_NAME := $(shell for d in src/*/; do \
	[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
	[ -f "$$d/Makefile" ] && basename "$$d"; \
done)

# ── Colors (auto-detect terminal) ────────────────────────────────────
ifneq ($(shell tput colors 2>/dev/null),)
C_LOGO  := \033[96m
C_HEAD  := \033[1m
C_CMD   := \033[93m
C_DESC  := \033[0m
C_RESET := \033[0m
else
C_LOGO  :=
C_HEAD  :=
C_CMD   :=
C_DESC  :=
C_RESET :=
endif

# ── Default target ───────────────────────────────────────────────────
all: help

# ── Help ─────────────────────────────────────────────────────────────
help:
	@printf "$(C_LOGO)"
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
	@printf "$(C_RESET)\n"
	@printf "  $(C_HEAD)Getting started:$(C_RESET)\n"
	@printf "    $(C_CMD)make setup$(C_RESET)                    Install RISC-V toolchain\n"
	@printf "    $(C_CMD)make core$(C_RESET)                     Create your app\n"
	@echo ""
	@printf "  $(C_HEAD)Then work from your app directory:$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/<app>$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                          Build\n"
	@printf "    $(C_CMD)make exec$(C_RESET)                     Build, push via UART, stream console\n"
	@printf "    $(C_CMD)make deploy$(C_RESET)                   Deploy to Pocket SD card\n"
	@printf "    $(C_CMD)make package$(C_RESET)                  Package core into a ZIP\n"
	@printf "    $(C_CMD)make pc$(C_RESET)                       Test on desktop (SDL2)\n"
	@printf "    $(C_CMD)make clean$(C_RESET)                    Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)To work with the demo apps:$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/apps$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                          Build all demos\n"
	@printf "    $(C_CMD)make new APP=demo$(C_RESET)             Create a new demo app\n"
	@printf "    $(C_CMD)make package$(C_RESET)                  Package SDK core into a ZIP\n"
	@printf "    $(C_CMD)make deploy$(C_RESET)                   Deploy SDK + demos to SD card\n"
	@printf "    $(C_CMD)make clean$(C_RESET)                    Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)From the root:$(C_RESET)\n"
	@printf "    $(C_CMD)make build$(C_RESET)                    Build everything\n"
	@printf "    $(C_CMD)make build  APP=<app>$(C_RESET)         Build sdk or <app>\n"
	@printf "    $(C_CMD)make exec   APP=<app>$(C_RESET)         Build, push via UART, stream console\n"
	@printf "    $(C_CMD)make deploy$(C_RESET)                   Deploy everything to SD card\n"
	@printf "    $(C_CMD)make deploy APP=<app>$(C_RESET)         Deploy sdk or <app> to SD card\n"
	@printf "    $(C_CMD)make tools$(C_RESET)                    Build PHDP host tools\n"
	@printf "    $(C_CMD)make package$(C_RESET)                  Package all cores into ZIPs\n"
	@printf "    $(C_CMD)make clean$(C_RESET)                    Remove all build artifacts\n"

# ── Setup ────────────────────────────────────────────────────────────
setup:
	@./scripts/setup.sh

# ── Create your app ──────────────────────────────────────────────────
core:
	@./scripts/customize.sh

# ── Build ────────────────────────────────────────────────────────────
build:
ifdef APP
ifeq ($(APP),sdk)
	$(MAKE) -C src/apps
else
	$(MAKE) -C src/$(APP)
endif
else
	$(MAKE) -C src/apps
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] || continue; \
		name=$$(basename "$$d"); \
		echo "Building $$name..."; \
		$(MAKE) -C "$$d" || exit 1; \
	done
endif

# ── Exec (UART push + console) ──────────────────────────────────────
exec:
ifndef APP
ifneq ($(APP_NAME),)
	$(MAKE) -C src/$(APP_NAME) exec
else
	@echo "Usage: make exec APP=<app>"
	@exit 1
endif
else
	$(MAKE) -C src/$(APP) exec
endif

# ── Deploy ───────────────────────────────────────────────────────────
deploy:
ifdef APP
ifeq ($(APP),sdk)
	$(MAKE) -C src/apps deploy
else
	$(MAKE) -C src/$(APP) deploy
endif
else
	$(MAKE) -C src/apps deploy
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] || continue; \
		$(MAKE) -C "$$d" deploy || true; \
	done
endif

# ── Package ──────────────────────────────────────────────────────────
package:
ifdef APP
ifeq ($(APP),sdk)
	$(MAKE) -C src/apps package
else
	$(MAKE) -C src/$(APP) package
endif
else
	./scripts/package.sh
endif

# ── Build host tools ────────────────────────────────────────────────
tools:
	$(MAKE) -C src/tools/phdp

# ── Clean ────────────────────────────────────────────────────────────
clean:
ifdef APP
ifeq ($(APP),sdk)
	$(MAKE) -C src/apps clean
else
	$(MAKE) -C src/$(APP) clean
endif
else
	$(MAKE) -C src/apps clean
	$(MAKE) -C src/tools/phdp clean
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] && $(MAKE) -C "$$d" clean; \
	done
	rm -rf build .obj releases
endif

.PHONY: all help setup core build exec deploy package tools clean
