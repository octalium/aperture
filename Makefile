# Thin Makefile wrapping meson. Meson stays the source of truth; this
# is a developer-UX convenience for the standard `make` / `make
# install` flow. For non-default flags (cross-compile, custom prefix,
# sanitizers), drive meson directly per the README's "Manual meson"
# section.
#
# Three parallel build dirs so the common workflows don't trip over
# each other:
#   build/         debug   (default `make`)
#   build-release/ release (system install)
#   build-user/    release + $HOME/.local prefix (user install)

.PHONY: all release run install install-user reconfigure clean

# Default: incremental debug build.
all: build/build.ninja
	@meson compile -C build

build/build.ninja:
	@meson setup build --buildtype=debug

# Release build, parallel dir so debug + release coexist.
release: build-release/build.ninja
	@meson compile -C build-release

build-release/build.ninja:
	@meson setup build-release --buildtype=release

# Build + run the debug binary.
run: all
	@./build/aperture

# System install. Always a release build.
install: release
	@sudo meson install -C build-release

# Per-user install ($HOME/.local), release build, separate dir for
# the different prefix so it doesn't fight build-release/.
install-user: build-user/build.ninja
	@meson compile -C build-user
	@meson install -C build-user

build-user/build.ninja:
	@meson setup build-user --buildtype=release --prefix="$$HOME/.local"

# Re-run configure on the default debug dir (e.g. after editing meson.build).
reconfigure:
	@meson setup --reconfigure build

# Nuke all build dirs.
clean:
	@rm -rf build build-release build-user
