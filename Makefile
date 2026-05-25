# Thin Makefile wrapping meson. Meson stays the source of truth;
# this is a developer-UX convenience for the standard `make` / `make
# install` flow. Set BUILD=somedir to use a different build directory.

BUILD ?= build
TYPE  ?= debug

# Default: configure (if needed) + incremental build (debug).
.PHONY: all
all: $(BUILD)/build.ninja
	@meson compile -C $(BUILD)

# Release build. Uses a separate dir so debug + release can coexist.
.PHONY: release
release: BUILD := build-release
release: TYPE  := release
release: $(BUILD)/build.ninja
	@meson compile -C $(BUILD)

# Auto-configure on first build. Trips on the ninja manifest, which
# meson regenerates after a meson.build edit so this is cheap.
$(BUILD)/build.ninja:
	@meson setup $(BUILD) --buildtype=$(TYPE)

# Build + run the binary out of the build dir.
.PHONY: run
run: all
	@$(BUILD)/aperture

# System-wide install (typically /usr/local). Uses sudo.
.PHONY: install
install: all
	@sudo meson install -C $(BUILD)

# Per-user install under $HOME/.local. No sudo. Reconfigures with the
# user prefix on first run if the build dir was set up with the
# default prefix.
.PHONY: install-user
install-user:
	@meson setup $(BUILD) --buildtype=$(TYPE) --prefix="$$HOME/.local" --reconfigure 2>/dev/null || \
	 meson setup $(BUILD) --buildtype=$(TYPE) --prefix="$$HOME/.local"
	@meson compile -C $(BUILD)
	@meson install -C $(BUILD)

# Re-run configure (e.g. after editing meson.build).
.PHONY: reconfigure
reconfigure:
	@meson setup --reconfigure $(BUILD)

# Nuke the build dir.
.PHONY: clean
clean:
	@rm -rf $(BUILD) build-release
