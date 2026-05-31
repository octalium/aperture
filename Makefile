# Thin façade over meson + packaging scripts. Meson stays the source of
# truth for the build itself; these targets just wrap common workflows
# so contributors don't have to remember multi-arg invocations.

BUILD_DIR    ?= build
PREFIX       ?= /usr/local
BUILDTYPE    ?= release

# bare `make` builds aperture for the current platform.
.DEFAULT_GOAL := build

.PHONY: help build setup compile install uninstall test clean linux flatpak app macos windows

help:
	@echo "common targets:"
	@echo "  make                  build aperture for the current platform (default)"
	@echo "  make install          install into \$$PREFIX (default $(PREFIX))"
	@echo "  make uninstall        remove a prior 'make install' (system prefix)"
	@echo "  make test             build + run the test suite (meson test)"
	@echo "  make linux            build a .flatpak bundle (Linux distributable)"
	@echo "  make macos            build Aperture.app + .dmg (macOS host only; needs create-dmg)"
	@echo "  make windows          build .msi installer (Windows host only; needs MSVC + vcpkg + WiX v4)"
	@echo "  make clean            remove $(BUILD_DIR)"
	@echo ""
	@echo "variables: BUILD_DIR (=$(BUILD_DIR)), PREFIX (=$(PREFIX)), BUILDTYPE (=$(BUILDTYPE))"

$(BUILD_DIR)/build.ninja:
	meson setup $(BUILD_DIR) --buildtype=$(BUILDTYPE) --prefix=$(PREFIX)

setup: $(BUILD_DIR)/build.ninja

compile: setup
	meson compile -C $(BUILD_DIR)

build: compile

install: compile
	meson install -C $(BUILD_DIR)

# meson's generated uninstall target removes whatever the last install
# logged. it doesn't prune created directories — a meson limitation, not
# ours.
uninstall:
	@if [ ! -d $(BUILD_DIR) ]; then \
		echo "no $(BUILD_DIR)/ — nothing to uninstall (run 'make install' first)" >&2; exit 1; \
	fi
	ninja -C $(BUILD_DIR) uninstall

test: setup
	meson test -C $(BUILD_DIR)

# Linux distributable: a Flatpak bundle. AppImage is intentionally not
# built — Flatpak is aperture's Linux channel (#434).
linux: flatpak

flatpak:
	flatpak-builder --user --install-deps-from=flathub --force-clean \
		--repo=$(BUILD_DIR)/flatpak-repo \
		$(BUILD_DIR)/flatpak pkg/flatpak/io.github.octalium.aperture.yml
	flatpak build-bundle $(BUILD_DIR)/flatpak-repo \
		$(BUILD_DIR)/aperture.flatpak io.github.octalium.aperture

app:
	@if [ "$$(uname -s)" != "Darwin" ]; then \
		echo "make app requires macOS (got $$(uname -s))" >&2; exit 1; \
	fi
	@if [ ! -d $(BUILD_DIR) ]; then \
		meson setup $(BUILD_DIR) --buildtype=release --prefix=/usr/local; \
	fi
	meson compile -C $(BUILD_DIR)
	BUILD_DIR=$(BUILD_DIR) pkg/macos/build-app.sh

macos: app
	pkg/macos/build-dmg.sh

# windows host detection covers both MSYS/MINGW (uname reports MINGW*/
# MSYS*) and POSIX-on-Windows toolchains. cross-builds from Linux are
# out of scope (#434); native MSVC runner only. The whole flow runs
# inside pkg/windows/make-windows.ps1 so env vars set by
# setup-deps (notably VULKAN_SDK) carry through to meson + build-msi.
windows:
	@case "$$(uname -s)" in \
		MINGW*|MSYS*|CYGWIN*|Windows_NT) ;; \
		*) echo "make windows requires a Windows host (got $$(uname -s))" >&2; exit 1;; \
	esac
	@if ! command -v pwsh >/dev/null 2>&1 && ! command -v powershell >/dev/null 2>&1; then \
		echo "make windows needs pwsh or powershell on PATH" >&2; exit 1; \
	fi
	@PS=$$(command -v pwsh || command -v powershell); \
		"$$PS" -NoProfile -ExecutionPolicy Bypass -File pkg/windows/make-windows.ps1 \
			-BuildDir "$(BUILD_DIR)" -BuildType "$(BUILDTYPE)"

clean:
	rm -rf $(BUILD_DIR)
