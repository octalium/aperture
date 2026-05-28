# Thin façade over meson + packaging scripts. Meson stays the source of
# truth for the build itself; these targets just wrap common workflows
# so contributors don't have to remember multi-arg invocations.

BUILD_DIR    ?= build
PREFIX       ?= /usr/local
BUILDTYPE    ?= release

.PHONY: help build setup compile install test clean flatpak app macos windows

help:
	@echo "common targets:"
	@echo "  make build            configure (if needed) + compile aperture"
	@echo "  make install          install into \$$PREFIX (default $(PREFIX))"
	@echo "  make test             build + run the test suite (meson test)"
	@echo "  make flatpak          build a .flatpak bundle (needs flatpak-builder + flathub remote)"
	@echo "  make app              build Aperture.app (macOS host only; needs dylibbundler)"
	@echo "  make macos            build Aperture.app + .dmg (macOS host only; needs create-dmg)"
	@echo "  make windows          build portable .zip (Windows host only; needs MSVC + vcpkg)"
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

test: setup
	meson test -C $(BUILD_DIR)

flatpak:
	flatpak-builder --user --install-deps-from=flathub --force-clean \
		--repo=$(BUILD_DIR)/flatpak-repo \
		$(BUILD_DIR)/flatpak packaging/flatpak/io.github.octalium.aperture.yml
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
	BUILD_DIR=$(BUILD_DIR) packaging/macos/build-app.sh

macos: app
	packaging/macos/build-dmg.sh

# windows host detection covers both MSYS/MINGW (uname reports MINGW*/
# MSYS*) and POSIX-on-Windows toolchains. cross-builds from Linux are
# out of scope (#434); native MSVC runner only.
windows:
	@case "$$(uname -s)" in \
		MINGW*|MSYS*|CYGWIN*|Windows_NT) ;; \
		*) echo "make windows requires a Windows host (got $$(uname -s))" >&2; exit 1;; \
	esac
	@if ! command -v pwsh >/dev/null 2>&1 && ! command -v powershell >/dev/null 2>&1; then \
		echo "make windows needs pwsh or powershell on PATH" >&2; exit 1; \
	fi
	@PS=$$(command -v pwsh || command -v powershell); \
		"$$PS" -NoProfile -ExecutionPolicy Bypass -File packaging/windows/setup-deps.ps1 && \
		meson setup $(BUILD_DIR) --buildtype=$(BUILDTYPE) \
			--pkg-config-path "$${VCPKG_ROOT:-dep/vcpkg}/installed/x64-windows/lib/pkgconfig" && \
		meson compile -C $(BUILD_DIR) && \
		"$$PS" -NoProfile -ExecutionPolicy Bypass -File packaging/windows/build-zip.ps1 -BuildDir $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
