/*
 * magnet_pkg.h — MagNET OTA package (.mnpkg) verification, portable C.
 *
 * Normative spec: docs/OTA-PACKAGE.md (wire format 1, document rev 1.1).
 * Shared by every target (ESP-IDF C6, Zephyr MG24): it does the checks that
 * need only bytes and keys (§4 checks 1-7 and 9). What is platform business
 * stays with the platform — writing the update slot, check 8 (its own image
 * format: esp_ota_end / the MCUboot header), and requesting the swap.
 *
 * Typical receiver flow:
 *   chunk 0 arrives     -> mn_pkg_check_header()   checks 1-6, early abort
 *   all chunks written  -> mn_pkg_check_payload()  check 7, reads the SLOT back
 *                          platform image check    check 8
 *                       -> mn_pkg_check_min_running()   check 9
 *   all OK              -> staged, identified by mn_pkg_sha128()
 */
#ifndef MAGNET_PKG_H
#define MAGNET_PKG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_PKG_HDR_LEN     256
#define MN_PKG_SIGNED_LEN  192
#define MN_PKG_SIG_P256    1
#define MN_PKG_SIG_ED25519 2
#define MN_PKG_FMT_ESP_IDF 1
#define MN_PKG_FMT_MCUBOOT 2
#define MN_PKG_FLAG_REQUIRES_REPARTITION 0x01
#define MN_PKG_FLAG_CLEARS_BUNDLES       0x02

/* §3.1 chip registry (append-only; board IDs carry the chip in the high byte) */
#define MN_PKG_CHIP_ESP32C6   0x0001
#define MN_PKG_CHIP_EFR32MG24 0x0002
#define MN_PKG_CHIP_ESP32     0x0003
#define MN_PKG_CHIP_ESP32S3   0x0004
#define MN_PKG_CHIP_ESP32C3   0x0005
#define MN_PKG_CHIP_NRF52840  0x0006

/* §4 results. Values are the check numbers, so a code names its check. */
typedef enum {
    MN_PKG_OK            = 0,
    MN_PKG_E_FORMAT      = 1,
    MN_PKG_E_KEY         = 2,
    MN_PKG_E_SIG         = 3,
    MN_PKG_E_TARGET      = 4,
    MN_PKG_E_REPARTITION = 5,
    MN_PKG_E_SIZE        = 6,
    MN_PKG_E_SHA         = 7,
    MN_PKG_E_IMAGE       = 8,     /* raised by the platform's own check */
    MN_PKG_E_MIN         = 9,
} mn_pkg_rc_t;

/* "E_PKG_FORMAT" … as the spec names them; "OK" for 0 */
const char *mn_pkg_rc_name(int rc);

typedef struct {
    uint8_t  major, minor;
    uint16_t revision;
    uint32_t build;
} mn_pkg_ver_t;

typedef struct {
    uint8_t      format_version, sig_alg, image_format, flags;
    uint16_t     header_len, chip, board;
    mn_pkg_ver_t version, min_running;
    uint32_t     image_len;
    uint8_t      image_sha256[32];
    int64_t      build_time;
    char         variant[17], fw_version[33];      /* NUL-terminated copies */
    uint8_t      key_id[8];
} mn_pkg_hdr_t;

/* What the receiving node is. `slot_size` = usable bytes for the payload in
 * its update slot (after any bootloader reserve); `running` = its version. */
typedef struct {
    uint16_t     chip, board;
    uint8_t      image_format;
    uint32_t     slot_size;
    mn_pkg_ver_t running;
} mn_pkg_self_t;

/* Checks 1-6 on the 256-byte header. transfer_len = total package bytes the
 * transport announced. Fills *out (also on failure, as far as parsed). */
int mn_pkg_check_header(const uint8_t raw[MN_PKG_HDR_LEN], uint32_t transfer_len,
                        const mn_pkg_self_t *self, mn_pkg_hdr_t *out);

/* Check 7: SHA-256 over the payload AS STORED — read back through `rd`
 * (payload offset 0 .. image_len-1), not as received, so a bad flash write
 * fails here too. rd returns 0 on success. */
typedef int (*mn_pkg_read_fn)(void *ctx, uint32_t off, void *buf, size_t len);
int mn_pkg_check_payload(const mn_pkg_hdr_t *h, mn_pkg_read_fn rd, void *ctx);

/* Check 9: running >= min_running. */
int mn_pkg_check_min_running(const mn_pkg_hdr_t *h, const mn_pkg_self_t *self);

/* <0, 0, >0 like strcmp; compares major, minor, revision, build in order. */
int mn_pkg_ver_cmp(const mn_pkg_ver_t *a, const mn_pkg_ver_t *b);

/* The staged-package identity (§4): image_sha256[0:16]. */
static inline const uint8_t *mn_pkg_sha128(const mn_pkg_hdr_t *h) { return h->image_sha256; }

#ifdef __cplusplus
}
#endif

#endif /* MAGNET_PKG_H */
