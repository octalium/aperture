# Thin façade over meson + packaging scripts. Meson stays the source of
# truth for the build itself; these targets just wrap common workflows
# so contributors don't have to remember multi-arg invocations.

BUILD_DIR    ?= build
PREFIX       ?= /usr/local
BUILDTYPE    ?= release

.PHONY: help build setup compile install test clean flatpak app macos dep-overlays

# submodules whose meson build description lives in dep/packagefiles/<name>/
# rather than upstream. linked into the submodule worktree so meson finds
# them at the conventional path; symlinks are untracked (the submodule
# worktree belongs to its own repo) and re-created here on demand.
DEP_OVERLAYS := cimgui tomlc99 lcms2 cJSON nativefiledialog

help:
	@echo "common targets:"
	@echo "  make build            configure (if needed) + compile aperture"
	@echo "  make install          install into \$$PREFIX (default $(PREFIX))"
	@echo "  make test             build + run the test suite (meson test)"
	@echo "  make flatpak          build a .flatpak bundle (needs flatpak-builder + flathub remote)"
	@echo "  make app              build Aperture.app (macOS host only; needs dylibbundler)"
	@echo "  make macos            build Aperture.app + .dmg (macOS host only; needs create-dmg)"
	@echo "  make clean            remove $(BUILD_DIR)"
	@echo ""
	@echo "variables: BUILD_DIR (=$(BUILD_DIR)), PREFIX (=$(PREFIX)), BUILDTYPE (=$(BUILDTYPE))"

dep-overlays:
	@for name in $(DEP_OVERLAYS); do \
		ln -sfn ../packagefiles/$$name/meson.build dep/$$name/meson.build; \
	done

$(BUILD_DIR)/build.ninja: dep-overlays
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

clean:
	rm -rf $(BUILD_DIR)
