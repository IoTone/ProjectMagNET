/*
 * magnet_ota_idf.c — OTA package receiver + health policy for ESP-IDF (C6).
 *
 * The ESP-IDF counterpart of firmware-zephyr/src/ota.c. Receives an OTA
 * package (docs/OTA-PACKAGE.md, wire format 1) carried by a Type 6 transfer
 * with meta "mnpkg:1" and stages it in the inactive app partition:
 *   chunk 0     -> the 256-byte header is checked at once (§4 checks 1-6) and
 *                  a bad package is refused before its payload crosses the air
 *   every chunk -> payload bytes land in the slot at package offset - 256,
 *                  4 KB sectors erased lazily on first touch (esp_ota_begin
 *                  would erase the whole slot up front — seconds of a blocked
 *                  pump — and esp_ota_write cannot take out-of-order chunks)
 *   completion  -> check 7 (SHA-256 read back from the slot), 8 (esp_image_
 *                  verify: the bootloader's own image check), 9 (min_running)
 * Apply (Forth `ota-apply`, privileged via the FORTH verb) = esp_ota_set_boot_
 * partition + reboot. The new image boots PENDING_VERIFY (CONFIG_BOOTLOADER_
 * APP_ROLLBACK_ENABLE) and marks itself valid only once healthy; otherwise a
 * reboot makes the bootloader roll back — the analogue of MCUboot TEST mode.
 *
 * Compiled only with MN_ENABLE_OTA=1 (env esp32c6_xiao_ble_led_ota), which
 * also needs partitions_ota.csv and sdkconfig.defaults.ota.
 */
#include "magnet.h"

#if MN_ENABLE_OTA

#include <string.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "forth_core.h"
#include "magnet_pkg.h"
#include "magnet_ota.h"
#include "magnet_xfer.h"

#ifndef MN_PKG_BOARD
#error "MN_ENABLE_OTA needs MN_PKG_BOARD (the OTA-PACKAGE §3.1 board ID)"
#endif
#if !defined(MN_FW_VER_MAJOR) || !defined(MN_FW_VER_MINOR) || \
    !defined(MN_FW_VER_REV) || !defined(MN_FW_VER_BUILD)
#error "MN_ENABLE_OTA needs MN_FW_VER_MAJOR/MINOR/REV/BUILD"
#endif

/* MN_OTA_FORCE_UNHEALTHY (test builds only): never healthy, 20 s deadline —
 * exercises the rollback path end to end. */
#if MN_OTA_FORCE_UNHEALTHY
#define OTA_CONFIRM_DEADLINE_S 20
#define NODE_HEALTHY() false
#else
#define OTA_CONFIRM_DEADLINE_S (10 * 60)
#define NODE_HEALTHY() mn_ota_node_healthy()   /* READY + bundles re-applied (§7.4) */
#endif
#define OTA_POLL_MS   2000
#define OTA_SECTOR    4096u
#define OTA_MAX_SECT  (0x1C0000u / OTA_SECTOR)      /* one 1792 KB slot */

static const mn_pkg_ver_t s_running = {
    MN_FW_VER_MAJOR, MN_FW_VER_MINOR, MN_FW_VER_REV, MN_FW_VER_BUILD
};

/* ============================ health policy =============================== */
static TimerHandle_t s_confirm_timer;
static int64_t       s_deadline_us;

static void confirm_poll(TimerHandle_t t) {
    (void)t;
    if (NODE_HEALTHY()) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        xTimerStop(s_confirm_timer, 0);
        if (err == ESP_OK) mn_emit_event("!OTA confirmed (node healthy)");
        else mn_emit_event("!WARN ota-confirm-failed rc=%d", err);
        mn_ota_settled();                           /* §13.4 report, if an apply is on record */
        return;
    }
    if (esp_timer_get_time() >= s_deadline_us) {
        mn_emit_event("!WARN ota-unhealthy: not READY in %ds, rebooting to roll back",
                      OTA_CONFIRM_DEADLINE_S);
        vTaskDelay(pdMS_TO_TICKS(200));             /* let the line drain */
        esp_restart();
    }
}

/* "# ota: running ..." — at boot and from ota-status. True while in TEST. */
static bool report_running(void) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    bool pending = (st == ESP_OTA_IMG_PENDING_VERIFY);
    mn_emit_event("# ota: running v%u.%u.%u+%u (%s) %s", s_running.major, s_running.minor,
                  s_running.revision, (unsigned)s_running.build, run ? run->label : "?",
                  pending ? "TEST — confirms when READY" : "confirmed");
    return pending;
}

void mn_ota_boot(void) {
    bool pending = report_running();
    if (pending && mn_ota_pending_permanent()) {   /* applied PERMANENT: no trial */
        esp_ota_mark_app_valid_cancel_rollback();
        mn_emit_event("# ota: PERMANENT apply — confirmed without a trial");
        pending = false;
    }
    if (!pending) mn_ota_settled();                 /* e.g. the old image back after a rollback */
    if (pending) {
        s_deadline_us = esp_timer_get_time() + OTA_CONFIRM_DEADLINE_S * 1000000LL;
        s_confirm_timer = xTimerCreate("mn_ota", pdMS_TO_TICKS(OTA_POLL_MS), pdTRUE,
                                       NULL, confirm_poll);
        if (s_confirm_timer) xTimerStart(s_confirm_timer, 0);
    }
}

/* ============================ receive sink ================================ */
static struct {
    const esp_partition_t *part;
    bool          active, have_hdr, staged;
    uint32_t      total;                     /* package bytes announced */
    uint8_t       raw[MN_PKG_HDR_LEN];
    mn_pkg_hdr_t  hdr;
    mn_pkg_self_t self;
    uint8_t       erased[(OTA_MAX_SECT + 7) / 8];
} s_ota;

static int sector_ready(uint32_t sec) {
    if (s_ota.erased[sec >> 3] & (1u << (sec & 7))) return 0;
    esp_err_t err = esp_partition_erase_range(s_ota.part, sec * OTA_SECTOR, OTA_SECTOR);
    if (err == ESP_OK) s_ota.erased[sec >> 3] |= (1u << (sec & 7));
    return err == ESP_OK ? 0 : -1;
}

static bool ota_claim(const uint8_t sender_id[4], const char *meta,
                      uint32_t total_len, uint16_t chunk_len) {
    if (strcmp(meta, "mnpkg:1") != 0) return false;         /* not ours: host path */
    s_ota.active = s_ota.have_hdr = s_ota.staged = false;
    if (chunk_len < MN_PKG_HDR_LEN) {                       /* header must fit chunk 0 */
        mn_post_note("!WARN ota: chunk %u B cannot carry the 256 B header", chunk_len);
        return false;
    }
    s_ota.part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota.part) {
        mn_post_note("!WARN ota: no update partition (not on partitions_ota.csv?)");
        return false;
    }
    memset(&s_ota.self, 0, sizeof(s_ota.self));
    s_ota.self.chip = MN_PKG_CHIP_ESP32C6;
    s_ota.self.board = MN_PKG_BOARD;
    s_ota.self.image_format = MN_PKG_FMT_ESP_IDF;
    s_ota.self.slot_size = s_ota.part->size;
    s_ota.self.running = s_running;
    memset(s_ota.erased, 0, sizeof(s_ota.erased));
    s_ota.total = total_len;
    s_ota.active = true;
    mn_post_note("!OTA receiving package, %u B from %02x%02x%02x%02x into %s",
                 (unsigned)total_len, sender_id[0], sender_id[1], sender_id[2], sender_id[3],
                 s_ota.part->label);
    return true;
}

static int ota_chunk(uint16_t idx, uint32_t off, const uint8_t *data, size_t len) {
    (void)idx;
    if (!s_ota.active) return MN_PKG_E_SIZE;
    if (off < MN_PKG_HDR_LEN) {                             /* chunk 0: the header */
        memcpy(s_ota.raw, data, MN_PKG_HDR_LEN);
        int rc = mn_pkg_check_header(s_ota.raw, s_ota.total, &s_ota.self, &s_ota.hdr);
        if (rc) {
            mn_post_note("!OTA refused %s (check %d) at the header", mn_pkg_rc_name(rc), rc);
            s_ota.active = false;
            return rc;
        }
        s_ota.have_hdr = true;
        data += MN_PKG_HDR_LEN;                             /* rest of chunk 0 = payload */
        len  -= MN_PKG_HDR_LEN;
        off   = MN_PKG_HDR_LEN;
    }
    if (!len) return 0;
    uint32_t so = off - MN_PKG_HDR_LEN;                     /* slot offset */
    if (so + len > s_ota.part->size) return MN_PKG_E_SIZE;
    for (uint32_t s = so / OTA_SECTOR; s <= (so + len - 1) / OTA_SECTOR; s++)
        if (sector_ready(s) != 0) return MN_PKG_E_IMAGE;
    return esp_partition_write(s_ota.part, so, data, len) == ESP_OK ? 0 : MN_PKG_E_IMAGE;
}

static int slot_read(void *ctx, uint32_t off, void *buf, size_t len) {
    (void)ctx;
    return esp_partition_read(s_ota.part, off, buf, len) == ESP_OK ? 0 : -1;
}

static int ota_done(bool complete) {
    if (!s_ota.active) return 0;
    s_ota.active = false;
    if (!complete) { mn_post_note("!WARN ota: transfer ended early, nothing staged"); return 0; }
    if (!s_ota.have_hdr) return MN_PKG_E_FORMAT;

    int rc = mn_pkg_check_payload(&s_ota.hdr, slot_read, NULL);            /* 7 */
    if (rc == MN_PKG_OK) {                                                  /* 8 */
        esp_partition_pos_t pos = { .offset = s_ota.part->address, .size = s_ota.part->size };
        esp_image_metadata_t md;
        if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK ||
            md.image_len > s_ota.hdr.image_len)
            rc = MN_PKG_E_IMAGE;
    }
    if (rc == MN_PKG_OK) rc = mn_pkg_check_min_running(&s_ota.hdr, &s_ota.self);   /* 9 */
    if (rc) {
        mn_post_note("!OTA refused %s (check %d)", mn_pkg_rc_name(rc), rc);
        return rc;
    }
    s_ota.staged = true;
    const uint8_t *id = mn_pkg_sha128(&s_ota.hdr);
    mn_post_note("!OTA staged v%u.%u.%u+%u %02x%02x%02x%02x%02x%02x%02x%02x "
                 "(%u B, key %02x%02x%02x%02x) in %s — FORTH: ota-apply",
                 s_ota.hdr.version.major, s_ota.hdr.version.minor, s_ota.hdr.version.revision,
                 (unsigned)s_ota.hdr.version.build, id[0], id[1], id[2], id[3], id[4], id[5],
                 id[6], id[7], (unsigned)s_ota.hdr.image_len, s_ota.hdr.key_id[0],
                 s_ota.hdr.key_id[1], s_ota.hdr.key_id[2], s_ota.hdr.key_id[3],
                 s_ota.part->label);
    return 0;
}

static const mn_xfer_sink_t s_ota_sink = {
    .claim = ota_claim, .on_chunk = ota_chunk, .on_done = ota_done,
};

/* Forth: ota-status ( -- )  ota-apply ( -- )  — privileged via FORTH verb */
static void w_ota_status(void) {
    (void)report_running();
    if (!s_ota.staged) {
        mn_emit_event("# ota: update slot not staged%s", s_ota.active ? " (receiving)" : "");
        return;
    }
    const uint8_t *id = mn_pkg_sha128(&s_ota.hdr);
    mn_emit_event("# ota: %s STAGED v%u.%u.%u+%u sha128 %02x%02x%02x%02x%02x%02x%02x%02x… "
                  "variant %s fw %s", s_ota.part->label,
                  s_ota.hdr.version.major, s_ota.hdr.version.minor, s_ota.hdr.version.revision,
                  (unsigned)s_ota.hdr.version.build, id[0], id[1], id[2], id[3], id[4], id[5],
                  id[6], id[7], s_ota.hdr.variant, s_ota.hdr.fw_version);
}

static void w_ota_apply(void) {
    if (!s_ota.staged) { mn_emit_event("-ERR E_BAD_STATE nothing staged"); return; }
    mn_ota_note_arm(&s_ota.hdr);
    esp_err_t err = esp_ota_set_boot_partition(s_ota.part);    /* re-verifies the image */
    if (err != ESP_OK) {
        mn_emit_event("-ERR E_INTERNAL esp_ota_set_boot_partition rc=%d", err);
        return;
    }
    mn_emit_event("!OTA applying: rebooting into %s (TEST; confirms when healthy)",
                  s_ota.part->label);
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

/* §13.3 hooks for the core's admin-signed apply */
bool mn_ota_platform_supported(void) { return true; }
const mn_pkg_hdr_t *mn_ota_platform_staged(void) {
    return (s_ota.staged && !s_ota.active) ? &s_ota.hdr : NULL;
}
void mn_ota_platform_running(mn_pkg_ver_t *v) { *v = s_running; }
int mn_ota_platform_arm(uint8_t mode) {
    (void)mode;       /* TEST vs PERMANENT is decided at boot: mn_ota_pending_permanent() */
    esp_err_t err = esp_ota_set_boot_partition(s_ota.part);   /* re-verifies the image */
    if (err != ESP_OK) mn_emit_event("!WARN ota-arm-failed rc=%d", err);
    return err == ESP_OK ? 0 : -1;
}

void mn_ota_sink_init(void) {
    mn_xfer_set_sink(&s_ota_sink);
    forth_register_word("ota-status", w_ota_status);
    forth_register_word("ota-apply", w_ota_apply);
}

#endif /* MN_ENABLE_OTA */
