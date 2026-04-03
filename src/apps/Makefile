# openfpgaOS SDK — Build All Bundled Apps
#
# Each subdirectory is an app. Builds app.elf in each.
#

# Apps with standard SDK build (single main.c, no SDL2 dependency)
APPS = $(shell find . -maxdepth 1 -mindepth 1 -type d \
         | sort | sed 's|^\./||')

all:
	@for app in $(APPS); do \
		echo "Building $$app..."; \
		$(MAKE) -C $$app -f ../app.mk || exit 1; \
	done

clean:
	@for app in $(APPS); do \
		$(MAKE) -C $$app -f ../app.mk clean; \
	done

.PHONY: all clean
