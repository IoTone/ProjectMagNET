/*
 * ota.c — MCUboot image health policy (Z-F).
 *
 * After MCUboot swaps in a new image it runs in "test" state: unless the app
 * confirms it, the NEXT reset swaps the old image back. We confirm only once
 * the node is demonstrably healthy — Thread attached, state READY (a lone
 * node still gets there by becoming leader) — and if that has not happened
 * within MN_OTA_CONFIRM_DEADLINE, we reboot so MCUboot rolls back rather
 * than leave a broken image running on a node nobody can reach.
 */
#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "magnet.h"

/* MN_OTA_FORCE_UNHEALTHY (test builds only): never counts as healthy and
 * gives up after 20 s — exercises the automatic rollback path end to end. */
#if MN_OTA_FORCE_UNHEALTHY
#define MN_OTA_CONFIRM_DEADLINE_S 20
#define MN_NODE_HEALTHY() false
#else
#define MN_OTA_CONFIRM_DEADLINE_S (10 * 60)
#define MN_NODE_HEALTHY() (mn_get_state() == MN_READY)
#endif
#define MN_OTA_POLL_MS            2000

static int64_t s_deadline_ms;

static void confirm_poll(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(s_confirm_work, confirm_poll);

static void confirm_poll(struct k_work *w) {
    ARG_UNUSED(w);
    if (MN_NODE_HEALTHY()) {
        int rc = boot_write_img_confirmed();
        mn_emit_event(rc == 0 ? "!OTA confirmed (node READY)"
                              : "!WARN ota-confirm-failed rc=%d", rc);
        return;
    }
    if (k_uptime_get() >= s_deadline_ms) {
        mn_emit_event("!WARN ota-unhealthy: not READY in %ds, rebooting to roll back",
                      MN_OTA_CONFIRM_DEADLINE_S);
        k_msleep(200);                       /* let the line drain */
        sys_reboot(SYS_REBOOT_COLD);
    }
    k_work_reschedule(&s_confirm_work, K_MSEC(MN_OTA_POLL_MS));
}

/* Called once from main after the link is up (events can be emitted). */
void mn_ota_boot(void) {
    struct mcuboot_img_header hdr;
    int rc = boot_read_bank_header(PARTITION_ID(slot0_partition), &hdr, sizeof(hdr));
    bool confirmed = boot_is_img_confirmed();
    if (rc == 0) {
        mn_emit_event("# ota: running v%u.%u.%u+%u (slot0) %s",
                      hdr.h.v1.sem_ver.major, hdr.h.v1.sem_ver.minor,
                      hdr.h.v1.sem_ver.revision, hdr.h.v1.sem_ver.build_num,
                      confirmed ? "confirmed" : "TEST — confirms when READY");
    } else {
        mn_emit_event("# ota: no MCUboot header (rc=%d) — not booted via MCUboot?", rc);
    }
    if (!confirmed) {
        s_deadline_ms = k_uptime_get() + MN_OTA_CONFIRM_DEADLINE_S * 1000LL;
        k_work_reschedule(&s_confirm_work, K_MSEC(MN_OTA_POLL_MS));
    }
}

/* ======================= mesh OTA receive sink =============================
 *
 * Receives an OTA package (docs/OTA-PACKAGE.md, wire format 1) carried by a
 * Type 6 transfer with meta "mnpkg:1" and stages it in MCUboot slot1.
 * The node has no host to stream to, so the xfer sink hands us each verified,
 * de-duplicated chunk:
 *   chunk 0     -> the 256-byte header is checked at once (§4 checks 1-6) and
 *                  a bad package is refused before its payload crosses the air
 *   every chunk -> payload bytes land in slot1 at package offset - 256
 *   completion  -> check 7 (SHA-256 read back from slot1), 8 (MCUboot header
 *                  consistent), 9 (min_running); any failure answers the sender
 *                  "refused:<check>" instead of COMPLETE (§7.1)
 * Applying stays a separate privileged step (Forth `ota-apply`; the
 * admin-signed mesh apply is next). Authenticity: the package signature here,
 * and MCUboot re-checks its own at boot (§5.2).
 */
#include <zephyr/drivers/flash.h>
#include <string.h>
#include "magnet_xfer.h"
#include "magnet_pkg.h"
#include "forth_core.h"

#define OTA_PAGE         8192u
#define OTA_MAX_PAGES    96u                      /* 712 KB / 8 KB = 89 */
#define OTA_TAIL_RESERVE (2u * OTA_PAGE)          /* move-mode sector + trailer */
#define OTA_BOARD_XIAO_MG24 0x0201                /* registry §3.1 */

static struct {
    const struct flash_area *fa;
    bool          active, have_hdr, staged;
    uint32_t      total;                          /* package bytes announced */
    uint8_t       raw[MN_PKG_HDR_LEN];
    mn_pkg_hdr_t  hdr;
    mn_pkg_self_t self;
    uint8_t       erased[OTA_MAX_PAGES / 8];
} s_ota;

static int page_ready(uint32_t page) {
    if (s_ota.erased[page >> 3] & (1u << (page & 7))) return 0;
    int rc = flash_area_erase(s_ota.fa, page * OTA_PAGE, OTA_PAGE);
    if (rc == 0) s_ota.erased[page >> 3] |= (1u << (page & 7));
    return rc;
}

/* what this node is, for §4 checks 4, 6 and 9 */
static void self_fill(void) {
    struct mcuboot_img_header h;
    memset(&s_ota.self, 0, sizeof(s_ota.self));
    s_ota.self.chip = MN_PKG_CHIP_EFR32MG24;
    s_ota.self.board = OTA_BOARD_XIAO_MG24;
    s_ota.self.image_format = MN_PKG_FMT_MCUBOOT;
    s_ota.self.slot_size = s_ota.fa->fa_size - OTA_TAIL_RESERVE;
    if (boot_read_bank_header(PARTITION_ID(slot0_partition), &h, sizeof(h)) == 0) {
        s_ota.self.running = (mn_pkg_ver_t){ h.h.v1.sem_ver.major, h.h.v1.sem_ver.minor,
                                             h.h.v1.sem_ver.revision, h.h.v1.sem_ver.build_num };
    }
}

static bool ota_claim(const uint8_t sender_id[4], const char *meta,
                      uint32_t total_len, uint16_t chunk_len) {
    if (strcmp(meta, "mnpkg:1") != 0) return false;         /* not ours: host path */
    s_ota.active = s_ota.have_hdr = s_ota.staged = false;
    if (chunk_len < MN_PKG_HDR_LEN) {                       /* header must fit chunk 0 */
        mn_post_note("!WARN ota: chunk %u B cannot carry the 256 B header", chunk_len);
        return false;
    }
    if (!s_ota.fa && flash_area_open(PARTITION_ID(slot1_partition), &s_ota.fa) != 0) {
        mn_post_note("!WARN ota: cannot open slot1");
        return false;
    }
    self_fill();
    memset(s_ota.erased, 0, sizeof(s_ota.erased));
    /* A stale trailer (an earlier staged/tested image) must not survive:
     * MCUboot reads it to decide whether a swap is pending. */
    if (page_ready(s_ota.fa->fa_size / OTA_PAGE - 1) != 0) return false;
    s_ota.total = total_len;
    s_ota.active = true;
    mn_post_note("!OTA receiving package, %u B from %02x%02x%02x%02x",
                 (unsigned)total_len, sender_id[0], sender_id[1], sender_id[2], sender_id[3]);
    return true;
}

static int ota_chunk(uint16_t idx, uint32_t off, const uint8_t *data, size_t len) {
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
    uint32_t so = off - MN_PKG_HDR_LEN;                     /* slot offset, 16-aligned */
    if (so + len > s_ota.fa->fa_size - OTA_TAIL_RESERVE) return MN_PKG_E_SIZE;
    for (uint32_t p = so / OTA_PAGE; p <= (so + len - 1) / OTA_PAGE; p++)
        if (page_ready(p) != 0) return MN_PKG_E_IMAGE;
    uint8_t buf[MN_XFER_CHUNK + 4];                         /* flash writes are 4-aligned */
    size_t n = (len + 3) & ~(size_t)3;
    memcpy(buf, data, len);
    memset(buf + len, 0xff, n - len);
    (void)idx;
    return flash_area_write(s_ota.fa, so, buf, n) == 0 ? 0 : MN_PKG_E_IMAGE;
}

static int slot_read(void *ctx, uint32_t off, void *buf, size_t len) {
    ARG_UNUSED(ctx);
    return flash_area_read(s_ota.fa, off, buf, len);
}

static int ota_done(bool complete) {
    if (!s_ota.active) return 0;
    s_ota.active = false;
    if (!complete) { mn_post_note("!WARN ota: transfer ended early, nothing staged"); return 0; }
    if (!s_ota.have_hdr) return MN_PKG_E_FORMAT;            /* cannot happen: chunk 0 is required */

    int rc = mn_pkg_check_payload(&s_ota.hdr, slot_read, NULL);           /* 7 */
    if (rc == MN_PKG_OK) {                                                 /* 8 */
        struct mcuboot_img_header mh;
        if (boot_read_bank_header(PARTITION_ID(slot1_partition), &mh, sizeof(mh)) != 0 ||
            mh.h.v1.image_size == 0 || mh.h.v1.image_size >= s_ota.hdr.image_len)
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
                 "(%u B, key %02x%02x%02x%02x) — FORTH: ota-apply",
                 s_ota.hdr.version.major, s_ota.hdr.version.minor, s_ota.hdr.version.revision,
                 (unsigned)s_ota.hdr.version.build, id[0], id[1], id[2], id[3], id[4], id[5],
                 id[6], id[7], (unsigned)s_ota.hdr.image_len, s_ota.hdr.key_id[0],
                 s_ota.hdr.key_id[1], s_ota.hdr.key_id[2], s_ota.hdr.key_id[3]);
    return 0;
}

static const mn_xfer_sink_t s_ota_sink = {
    .claim = ota_claim, .on_chunk = ota_chunk, .on_done = ota_done,
};

/* Forth: ota-status ( -- )  ota-apply ( -- )  — privileged via FORTH verb */
static void w_ota_status(void) {
    if (!s_ota.staged) {
        mn_emit_event("# ota: slot1 not staged%s", s_ota.active ? " (receiving)" : "");
        return;
    }
    const uint8_t *id = mn_pkg_sha128(&s_ota.hdr);
    mn_emit_event("# ota: slot1 STAGED v%u.%u.%u+%u sha128 %02x%02x%02x%02x%02x%02x%02x%02x… "
                  "variant %s fw %s",
                  s_ota.hdr.version.major, s_ota.hdr.version.minor, s_ota.hdr.version.revision,
                  (unsigned)s_ota.hdr.version.build, id[0], id[1], id[2], id[3], id[4], id[5],
                  id[6], id[7], s_ota.hdr.variant, s_ota.hdr.fw_version);
}

static void w_ota_apply(void) {
    if (!s_ota.staged) { mn_emit_event("-ERR E_BAD_STATE nothing staged"); return; }
    int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (rc) { mn_emit_event("-ERR E_INTERNAL boot_request_upgrade rc=%d", rc); return; }
    mn_emit_event("!OTA applying: rebooting into the staged image (TEST; confirms when healthy)");
    k_msleep(300);
    sys_reboot(SYS_REBOOT_COLD);
}

void mn_ota_sink_init(void) {
    mn_xfer_set_sink(&s_ota_sink);
    forth_register_word("ota-status", w_ota_status);
    forth_register_word("ota-apply", w_ota_apply);
}
