# Thin façade over meson + packaging scripts. Meson stays the source of
# truth for the build itself; these targets just wrap common workflows
# so contributors don't have to remember multi-arg invocations.

BUILD_DIR    ?= build
PREFIX       ?= /usr/local
BUILDTYPE    ?= release

.PHONY: help build setup compile install clean appimage flatpak release-linux app macos

help:
	@echo "common targets:"
	@echo "  make build            configure (if needed) + compile aperture"
	@echo "  make install          install into \$$PREFIX (default $(PREFIX))"
	@echo "  make appimage         build a single-file AppImage (needs linuxdeploy)"
	@echo "  make flatpak          build a .flatpak bundle (needs flatpak-builder + flathub remote)"
	@echo "  make release-linux    build both AppImage and Flatpak"
	@echo "  make app              build Aperture.app (macOS host only; needs dylibbundler)"
	@echo "  make macos            build Aperture.app + .dmg (macOS host only; needs create-dmg)"
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

appimage:
	@if [ ! -d $(BUILD_DIR) ]; then \
		meson setup $(BUILD_DIR) --buildtype=release --prefix=/usr; \
	fi
	meson compile -C $(BUILD_DIR)
	BUILD_DIR=$(BUILD_DIR) packaging/appimage/build-appimage.sh

flatpak:
	flatpak-builder --user --install-deps-from=flathub --force-clean \
		--repo=$(BUILD_DIR)/flatpak-repo \
		$(BUILD_DIR)/flatpak packaging/flatpak/io.github.octalium.aperture.yml
	flatpak build-bundle $(BUILD_DIR)/flatpak-repo \
		$(BUILD_DIR)/aperture.flatpak io.github.octalium.aperture

release-linux: appimage flatpak

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

clean:
	rm -rf $(BUILD_DIR)
