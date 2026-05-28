project('mbedtls', 'c',
  default_options: [
    'c_std=c99',
    'warning_level=0',
    'optimization=2',
  ],
)

# Mbed TLS v3.6 LTS. Built as one combined static library covering
# crypto, x509, and TLS. We use the upstream default config (no
# mbedtls_config.h override): no PSA P256-M, no Everest, no AES-NI/CE
# acceleration via separate target — the per-file CPU-feature gating
# inside the upstream sources handles fallback to software paths.
#
# Source lists mirror upstream/library/CMakeLists.txt at the pinned revision.

cc = meson.get_compiler('c')

src_crypto = files(
  'upstream/library/aes.c',                     'upstream/library/aesce.c',
  'upstream/library/aesni.c',                   'upstream/library/aria.c',
  'upstream/library/asn1parse.c',               'upstream/library/asn1write.c',
  'upstream/library/base64.c',                  'upstream/library/bignum.c',
  'upstream/library/bignum_core.c',             'upstream/library/bignum_mod.c',
  'upstream/library/bignum_mod_raw.c',          'upstream/library/block_cipher.c',
  'upstream/library/camellia.c',                'upstream/library/ccm.c',
  'upstream/library/chacha20.c',                'upstream/library/chachapoly.c',
  'upstream/library/cipher.c',                  'upstream/library/cipher_wrap.c',
  'upstream/library/constant_time.c',           'upstream/library/cmac.c',
  'upstream/library/ctr_drbg.c',                'upstream/library/des.c',
  'upstream/library/dhm.c',                     'upstream/library/ecdh.c',
  'upstream/library/ecdsa.c',                   'upstream/library/ecjpake.c',
  'upstream/library/ecp.c',                     'upstream/library/ecp_curves.c',
  'upstream/library/ecp_curves_new.c',          'upstream/library/entropy.c',
  'upstream/library/entropy_poll.c',            'upstream/library/error.c',
  'upstream/library/gcm.c',                     'upstream/library/hkdf.c',
  'upstream/library/hmac_drbg.c',               'upstream/library/lmots.c',
  'upstream/library/lms.c',                     'upstream/library/md.c',
  'upstream/library/md5.c',                     'upstream/library/memory_buffer_alloc.c',
  'upstream/library/nist_kw.c',                 'upstream/library/oid.c',
  'upstream/library/padlock.c',                 'upstream/library/pem.c',
  'upstream/library/pk.c',                      'upstream/library/pk_ecc.c',
  'upstream/library/pk_wrap.c',                 'upstream/library/pkcs12.c',
  'upstream/library/pkcs5.c',                   'upstream/library/pkparse.c',
  'upstream/library/pkwrite.c',                 'upstream/library/platform.c',
  'upstream/library/platform_util.c',           'upstream/library/poly1305.c',
  'upstream/library/psa_crypto.c',              'upstream/library/psa_crypto_aead.c',
  'upstream/library/psa_crypto_cipher.c',       'upstream/library/psa_crypto_client.c',
  'upstream/library/psa_crypto_driver_wrappers_no_static.c',
  'upstream/library/psa_crypto_ecp.c',          'upstream/library/psa_crypto_ffdh.c',
  'upstream/library/psa_crypto_hash.c',         'upstream/library/psa_crypto_mac.c',
  'upstream/library/psa_crypto_pake.c',         'upstream/library/psa_crypto_rsa.c',
  'upstream/library/psa_crypto_se.c',           'upstream/library/psa_crypto_slot_management.c',
  'upstream/library/psa_crypto_storage.c',      'upstream/library/psa_its_file.c',
  'upstream/library/psa_util.c',                'upstream/library/ripemd160.c',
  'upstream/library/rsa.c',                     'upstream/library/rsa_alt_helpers.c',
  'upstream/library/sha1.c',                    'upstream/library/sha256.c',
  'upstream/library/sha512.c',                  'upstream/library/sha3.c',
  'upstream/library/threading.c',               'upstream/library/timing.c',
  'upstream/library/version.c',                 'upstream/library/version_features.c',
)

src_x509 = files(
  'upstream/library/pkcs7.c',          'upstream/library/x509.c',
  'upstream/library/x509_create.c',    'upstream/library/x509_crl.c',
  'upstream/library/x509_crt.c',       'upstream/library/x509_csr.c',
  'upstream/library/x509write.c',      'upstream/library/x509write_crt.c',
  'upstream/library/x509write_csr.c',
)

src_tls = files(
  'upstream/library/debug.c',                'upstream/library/mps_reader.c',
  'upstream/library/mps_trace.c',            'upstream/library/net_sockets.c',
  'upstream/library/ssl_cache.c',            'upstream/library/ssl_ciphersuites.c',
  'upstream/library/ssl_client.c',           'upstream/library/ssl_cookie.c',
  'upstream/library/ssl_debug_helpers_generated.c',
  'upstream/library/ssl_msg.c',              'upstream/library/ssl_ticket.c',
  'upstream/library/ssl_tls.c',              'upstream/library/ssl_tls12_client.c',
  'upstream/library/ssl_tls12_server.c',     'upstream/library/ssl_tls13_keys.c',
  'upstream/library/ssl_tls13_server.c',     'upstream/library/ssl_tls13_client.c',
  'upstream/library/ssl_tls13_generic.c',
)

mbedtls_inc = include_directories('upstream/include')

extra_deps = []
if host_machine.system() == 'linux'
  thread_dep = dependency('threads')
  extra_deps += thread_dep
endif

mbedtls_lib = static_library('mbedtls',
  src_crypto + src_x509 + src_tls,
  include_directories: [mbedtls_inc, include_directories('upstream/library')],
  dependencies: extra_deps,
  c_args: ['-w'],
  install: false,
)

mbedtls_dep = declare_dependency(
  link_with: mbedtls_lib,
  include_directories: mbedtls_inc,
  dependencies: extra_deps,
)

meson.override_dependency('mbedtls', mbedtls_dep)
