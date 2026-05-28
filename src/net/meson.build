if host_machine.system() == 'windows'
  aperture_sources += files('http_winhttp.c')
elif host_machine.system() == 'darwin'
  aperture_sources += files('http_nsurlsession.m')
else
  aperture_sources += files('http_mbedtls.c')
endif
