aperture_sources += files(
  'fs.c',
  'log.c',
  'random.c',
  'version.c',
  'worker.c',
)

# platform-split OS surfaces, same mechanism as src/net. exported as
# variables so test executables that compile library.c / sidecar.c
# directly can pull in the matching backend without re-deriving the
# host selection.
if host_machine.system() == 'windows'
  ap_dir_src    = files('dir_windows.c')
  ap_thread_src = files('thread_windows.c')
else
  ap_dir_src    = files('dir_posix.c')
  ap_thread_src = files('thread_posix.c')
endif

aperture_sources += ap_dir_src
aperture_sources += ap_thread_src
