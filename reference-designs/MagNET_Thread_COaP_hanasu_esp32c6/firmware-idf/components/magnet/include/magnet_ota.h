/*
 * magnet_ota.h — admin-signed remote apply + post-reboot report
 * (design proposal §13.3–13.5) and admin revocation (§13.8 item 2).
 *
 * The mesh side lives in magnet_core.c (shared, both targets). A platform
 * that can stage packages (magnet_ota_idf.c, firmware-zephyr/src/ota.c)
 * provides the four mn_ota_platform_* hooks; builds without OTA get weak
 * defaults in magnet_core.c that answer E_UNSUPPORTED.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "magnet_pkg.h"

/* system/ota_apply status (Type 2 reply byte 0): the number of the §13.3
 * check that failed, as package refusals carry theirs (OTA-PACKAGE §7.1). */
enum {
    MN_OTA_ACCEPTED           = 0,
    MN_OTA_E_NOT_TARGET       = 2,
    MN_OTA_E_NOT_STAGED       = 3,
    MN_OTA_E_SHA_MISMATCH     = 4,
    MN_OTA_E_VERSION_MISMATCH = 5,
    MN_OTA_E_DOWNGRADE        = 6,
    MN_OTA_E_UNSUPPORTED      = 7,
};
const char *mn_ota_status_name(uint8_t st);

enum { MN_OTA_MODE_TEST = 0, MN_OTA_MODE_PERMANENT = 1 };
#define MN_OTA_F_ALLOW_DOWNGRADE 0x01

/* system/ota_report outcome and bundle status (§13.4) */
enum { MN_OTA_CONFIRMED = 1, MN_OTA_ROLLED_BACK = 2 };
enum { MN_BUNDLES_NONE = 0, MN_BUNDLES_OK = 1, MN_BUNDLES_FAILED = 2, MN_BUNDLES_CLEARED = 3 };

/* ---- platform hooks ---- */
bool                mn_ota_platform_supported(void);
const mn_pkg_hdr_t *mn_ota_platform_staged(void);        /* NULL: none, or still receiving */
void                mn_ota_platform_running(mn_pkg_ver_t *v);
int                 mn_ota_platform_arm(uint8_t mode);   /* make the staged slot boot next; 0 = ok */

/* ---- core, called by the platform ---- */
/* The running image is settled: confirmed after a trial, or never on trial.
 * If an apply is on record, this queues the §13.4 report for when READY. */
void mn_ota_settled(void);
/* At boot: the apply that installed this image asked for PERMANENT. */
bool mn_ota_pending_permanent(void);
/* Health (OTA-PACKAGE §7.4) = READY and no persisted bundle failed to re-apply. */
bool mn_ota_node_healthy(void);

/* Boot-time bundle re-apply result, set by main after craw_role_bundle_apply_saved. */
void    mn_boot_bundles_set(int found, int failed);
uint8_t mn_boot_bundles(void);

/* ---- operator side (HCP OTA / ADMIN verbs) ---- */
int mn_ota_apply_send(const char *peer, const uint8_t sha128[16], const mn_pkg_ver_t *v,
                      uint8_t mode, uint8_t flags);     /* -3 unknown peer */
void mn_admin_fp(const uint8_t pub65[65], uint8_t fp[8]); /* SHA-256(pub)[0:8] */
int  mn_admin_remove(const uint8_t fp[8]);               /* 0 removed, -1 not held */
int  mn_admin_remove_index(int n);                       /* 1-based, as ADMIN LIST shows */
int  mn_admin_revoke_send(const uint8_t fp[8]);          /* signed mesh revoke + local remove */
