aperture_sources += files(
  'check.c',
  'manifest.c',
  'settings.c',
  'updater.c',
)

if ap_dist == 'flatpak'
  aperture_sources += files('updater_flatpak.c')
elif ap_dist == 'macos'
  add_languages('objc', native: false, required: true)
  add_project_arguments('-DAP_DIST_MACOS=1', language: ['objc'])
  aperture_sources += files('updater_macos.m')
  # Sparkle is fetched on demand by packaging/macos/fetch-sparkle.sh,
  # which prints the absolute path to the extracted framework. Run it
  # at configure time so Sparkle.framework is present before the linker
  # invocation. The script is idempotent and uses a content-addressed
  # cache (Sparkle-<version>/), so repeated configures are cheap.
  sparkle_fetch = run_command(
    meson.project_source_root() / 'packaging/macos/fetch-sparkle.sh',
    check: true,
  )
  sparkle_framework_path = sparkle_fetch.stdout().strip()
  sparkle_search_path = run_command(
    'dirname', sparkle_framework_path,
    check: true,
  ).stdout().strip()
  add_project_arguments(
    '-F', sparkle_search_path,
    language: ['objc'],
  )
  add_project_link_arguments(
    '-F', sparkle_search_path,
    '-framework', 'Foundation',
    '-framework', 'AppKit',
    '-framework', 'Sparkle',
    '-Wl,-rpath,@executable_path/../Frameworks',
    language: ['c', 'cpp', 'objc'],
  )
endif
