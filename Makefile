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
C_VERB  := \033[1;93m
C_ARG   := \033[3;93m
C_RESET := \033[0m
else
C_LOGO  :=
C_HEAD  :=
C_CMD   :=
C_VERB  :=
C_ARG   :=
C_RESET :=
endif

# ── Display name (use detected app or <app> placeholder) ────────────
# Truncate to 10 chars with ... if too long, to keep help aligned
ifneq ($(APP_NAME),)
A := $(shell n="$(APP_NAME)"; [ $${#n} -gt 10 ] && echo "$${n:0:7}..." || echo "$$n")
else
A := <app>
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
	@echo "  \\____/___/  SDK"
	@printf "$(C_RESET)\n"
	@printf "  $(C_HEAD)Getting started:$(C_RESET)\n"
	@printf "    $(C_CMD)make $(C_VERB)setup$(C_RESET)              Install RISC-V toolchain\n"
	@printf "    $(C_CMD)make $(C_VERB)core$(C_RESET)               Create your app\n"
	@echo ""
	@printf "  $(C_HEAD)Then work from your app directory:$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/$(A)$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                    Build\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET)              Build, push via UART, stream console\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)               Copy to Pocket SD card\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)            Package core into a ZIP\n"
	@printf "    $(C_CMD)make $(C_VERB)test$(C_RESET)               Test on desktop (SDL2)\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)              Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)To work with the demo apps:$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/apps$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                    Build all demos\n"
	@printf "    $(C_CMD)make $(C_VERB)new$(C_RESET) $(C_ARG)APP=demo$(C_RESET)       Create a new demo app\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)            Package SDK core into a ZIP\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)               Copy SDK + demos to SD card\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)              Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)From the root:$(C_RESET)\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET)              Build everything\n"
	@A="$(A)"; printf "    $(C_CMD)make $(C_VERB)build$(C_RESET) $(C_ARG)APP=%-8s$(C_RESET) Build sdk or $$A\n" "$$A"
	@A="$(A)"; printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET) $(C_ARG)APP=%-8s$(C_RESET) Build, push via UART, stream console\n" "$$A"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)               Copy everything to SD card\n"
	@A="$(A)"; printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET) $(C_ARG)APP=%-8s$(C_RESET)  Copy sdk or $$A to SD card\n" "$$A"
	@printf "    $(C_CMD)make $(C_VERB)tools$(C_RESET)              Build PHDP host tools\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)            Package all cores into ZIPs\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)              Remove all build artifacts\n"

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
debug:
ifndef APP
ifneq ($(APP_NAME),)
	$(MAKE) -C src/$(APP_NAME) debug
else
	@echo "Usage: make debug APP=$(A)"
	@exit 1
endif
else
	$(MAKE) -C src/$(APP) debug
endif

# ── Copy ───────────────────────────────────────────────────────────
copy:
ifdef APP
ifeq ($(APP),sdk)
	$(MAKE) -C src/apps copy
else
	$(MAKE) -C src/$(APP) copy
endif
else
	$(MAKE) -C src/apps copy
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] || continue; \
		$(MAKE) -C "$$d" copy || true; \
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

.PHONY: all help setup core build debug copy package tools clean
