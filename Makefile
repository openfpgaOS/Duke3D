# openfpgaOS SDK Makefile
#
# Two ways to ship code with this SDK:
#
#   1. CUSTOM CORE — a standalone openFPGA core wrapping a single
#      application (your game / demo / tool). Lives at src/<name>/,
#      ships its own dist/<name>/ and packages to its own ZIP. Created
#      with `make core`.
#
#   2. SDK APP — an app bundled into the shared "openfpgaOS SDK" demo
#      core alongside the other examples. Lives at src/apps/<name>/,
#      shares dist/sdk/ with every other SDK app, and ships in one ZIP.
#      Created with `cd src/apps && make new APP=<name>`.
#
# Quick start:
#   make setup          Install RISC-V toolchain
#   make core           Scaffold a custom core

# ── Paths ────────────────────────────────────────────────────────────
RUNTIME = runtime

# ── Detect a custom core in src/<name>/ ──────────────────────────────
# Any src/<name>/ directory with a Makefile that isn't apps/, sdk/, or
# tools/ is treated as a custom core. Used to drive the help text and
# the no-argument default for `make debug`.
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

# ── Display name (detected custom core or <core> placeholder) ───────
# Truncate to 10 chars with ... if too long, to keep help aligned
ifneq ($(APP_NAME),)
A := $(shell n="$(APP_NAME)"; [ $${#n} -gt 10 ] && echo "$${n:0:7}..." || echo "$$n")
else
A := <core>
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
	@printf "    $(C_CMD)make $(C_VERB)setup$(C_RESET)                    Install RISC-V toolchain\n"
	@printf "    $(C_CMD)make $(C_VERB)core$(C_RESET)                     Scaffold a custom core (src/<name>/)\n"
	@echo ""
	@printf "  $(C_HEAD)Custom core (work from src/$(A)/):$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/$(A)$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                          Build the custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET)                    Build, push via UART, stream console\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)                     Copy this custom core to Pocket SD\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)                  Package this custom core into a ZIP\n"
	@printf "    $(C_CMD)make $(C_VERB)test$(C_RESET)                     Test on desktop (SDL2)\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)                    Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)SDK apps (bundled into the SDK demo core):$(C_RESET)\n"
	@printf "    $(C_CMD)cd src/apps$(C_RESET)\n"
	@printf "    $(C_CMD)make$(C_RESET)                          Build all SDK apps\n"
	@printf "    $(C_CMD)make $(C_VERB)new$(C_RESET) $(C_ARG)APP=app$(C_RESET)              Scaffold a new SDK app\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET) $(C_ARG)APP=app$(C_RESET)            Build, push via UART, stream console\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)                     Copy SDK core to Pocket SD\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)                  Package SDK core into a ZIP\n"
	@printf "    $(C_CMD)make $(C_VERB)test$(C_RESET) $(C_ARG)APP=app$(C_RESET)             Test on desktop (SDL2)\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)                    Remove build artifacts\n"
	@echo ""
	@printf "  $(C_HEAD)From the root (drives both paths):$(C_RESET)\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET)                    Build SDK core + every custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET) $(C_ARG)CORE=<core|sdk>$(C_RESET)    Build core or sdk only\n"
	@printf "    $(C_CMD)make $(C_VERB)build$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)        Build the <core> custom core only\n"
	@printf "    $(C_CMD)make $(C_VERB)debug$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)        Build + UART push + stream console\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET)                     Copy SDK demo core + custom cores\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET) $(C_ARG)CORE=sdk$(C_RESET)            Copy SDK demo core only\n"
	@printf "    $(C_CMD)make $(C_VERB)copy$(C_RESET) $(C_ARG)CORE=<core>$(C_RESET)         Copy the <core> custom core only\n"
	@printf "    $(C_CMD)make $(C_VERB)package$(C_RESET)                  Package SDK demo core + every custom core\n"
	@printf "    $(C_CMD)make $(C_VERB)tools$(C_RESET)                    Build PHDP host tools\n"
	@printf "    $(C_CMD)make $(C_VERB)clean$(C_RESET)                    Remove all build artifacts\n"

# ── Setup ────────────────────────────────────────────────────────────
setup:
	@./scripts/setup.sh

# ── Scaffold a custom core ───────────────────────────────────────────
core:
	@./scripts/customize.sh

# ── Build ────────────────────────────────────────────────────────────
# `make build`              → SDK demo core + every custom core under src/<name>/
# `make build CORE=sdk`     → SDK demo core only (src/apps/)
# `make build CORE=<name>`  → custom core src/<name>/ only
build:
ifdef CORE
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps
else
	$(MAKE) -C src/$(CORE)
endif
else
	$(MAKE) -C src/apps
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] || continue; \
		name=$$(basename "$$d"); \
		echo "Building custom core: $$name..."; \
		$(MAKE) -C "$$d" || exit 1; \
	done
endif

# ── Debug (UART push + console) ─────────────────────────────────────
# Custom-core path only — `make debug` from the SDK demo core isn't
# meaningful at the root (the demo core is the runtime, not a single
# app to push). To push a single SDK app over UART use
# `cd src/apps && make debug APP=<name>` instead.
# With no CORE, picks the lone custom core if exactly one exists.
debug:
ifndef CORE
ifneq ($(APP_NAME),)
	$(MAKE) -C src/$(APP_NAME) debug
else
	@echo "Usage: make debug CORE=<custom-core>"
	@exit 1
endif
else
	$(MAKE) -C src/$(CORE) debug
endif

# ── Copy to SD card ──────────────────────────────────────────────────
# `make copy`              → SDK demo core + every custom core
# `make copy CORE=sdk`     → SDK demo core only
# `make copy CORE=<name>`  → custom core src/<name>/ only
copy:
ifdef CORE
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps copy
else
	$(MAKE) -C src/$(CORE) copy
endif
else
	$(MAKE) -C src/apps copy
	@for d in src/*/; do \
		[ "$$d" = "src/apps/" ] || [ "$$d" = "src/sdk/" ] || [ "$$d" = "src/tools/" ] && continue; \
		[ -f "$$d/Makefile" ] || continue; \
		$(MAKE) -C "$$d" copy || true; \
	done
endif

# ── Package distributable ZIPs ───────────────────────────────────────
# `make package`              → SDK demo core + every custom core
# `make package CORE=sdk`     → SDK demo core only
# `make package CORE=<name>`  → custom core src/<name>/ only
package:
ifdef CORE
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps package
else
	$(MAKE) -C src/$(CORE) package
endif
else
	./scripts/package.sh
endif

# ── Build host tools ────────────────────────────────────────────────
tools:
	$(MAKE) -C src/tools/phdp

# ── Clean ────────────────────────────────────────────────────────────
clean:
ifdef CORE
ifeq ($(CORE),sdk)
	$(MAKE) -C src/apps clean
else
	$(MAKE) -C src/$(CORE) clean
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
