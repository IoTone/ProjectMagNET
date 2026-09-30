/*
 * magnet_pkg_keys.h — the OTA release-key store (OTA-PACKAGE §6): up to two
 * keys, current (KEY0) and next (KEY1), each tagged with its algorithm.
 *
 * Default: the bench DEV store (magnet_pkg_keys_dev.h). A deployment build
 * sets MN_PKG_RELEASE_KEYS=1 and gets magnet_pkg_keys_release.h, generated
 * from PUBLIC keys only (tools/keys/README.md):
 *     tools/mnpkg.py c-keys --release release.pub.pem [next.pub.pem] \
 *         > firmware-idf/components/magnet/include/magnet_pkg_keys_release.h
 */
#ifndef MAGNET_PKG_KEYS_H
#define MAGNET_PKG_KEYS_H

#if MN_PKG_RELEASE_KEYS
#  if __has_include("magnet_pkg_keys_release.h")
#    include "magnet_pkg_keys_release.h"
#  else
#    error "MN_PKG_RELEASE_KEYS=1 but magnet_pkg_keys_release.h is missing — see tools/keys/README.md"
#  endif
#  if MN_PKG_KEYS_ARE_DEV
#    error "magnet_pkg_keys_release.h holds DEV keys — regenerate it with c-keys --release"
#  endif
#else
#  include "magnet_pkg_keys_dev.h"
#endif

#endif
