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

/* ======================= mesh OTA receive sink (Z-F.2) =====================
 *
 * A Type 6 transfer whose meta is "ota:<32 hex>" is claimed here and written
 * straight into slot1 (the node has no host to stream it to). The hex is the
 * first 128 bits of SHA-256 over the whole signed image — an integrity check
 * on the reassembly; AUTHENTICITY is MCUboot's job (it refuses to boot an
 * image not signed with the firmware key). Applying (mark pending + reboot)
 * is a separate privileged step: Forth `ota-apply` (reached via the bonded /
 * serial FORTH verb), so a channel member can stage but never trigger.
 */
#include <zephyr/drivers/flash.h>
#include <psa/crypto.h>
#include <string.h>
#include "magnet_xfer.h"
#include "forth_core.h"

#define OTA_PAGE        8192u
#define OTA_MAX_PAGES   96u                       /* 712 KB / 8 KB = 89 */
#define OTA_TAIL_RESERVE (2u * OTA_PAGE)          /* move-mode sector + trailer */

static struct {
    const struct flash_area *fa;
    bool     active, staged;
    uint32_t len;
    uint8_t  want[16];
    uint8_t  erased[OTA_MAX_PAGES / 8];
} s_ota;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int page_ready(uint32_t page) {
    if (s_ota.erased[page >> 3] & (1u << (page & 7))) return 0;
    int rc = flash_area_erase(s_ota.fa, page * OTA_PAGE, OTA_PAGE);
    if (rc == 0) s_ota.erased[page >> 3] |= (1u << (page & 7));
    return rc;
}

static bool ota_claim(const uint8_t sender_id[4], const char *meta,
                      uint32_t total_len, uint16_t chunk_len) {
    ARG_UNUSED(chunk_len);
    if (strncmp(meta, "ota:", 4) != 0 || strlen(meta) != 4 + 32) return false;
    s_ota.active = s_ota.staged = false;
    for (int i = 0; i < 16; i++) {
        int hi = hexval(meta[4 + 2 * i]), lo = hexval(meta[5 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        s_ota.want[i] = (uint8_t)(hi << 4 | lo);
    }
    if (!s_ota.fa && flash_area_open(PARTITION_ID(slot1_partition), &s_ota.fa) != 0) {
        mn_post_note("!WARN ota: cannot open slot1");
        return false;
    }
    if (total_len < 1024 || total_len > s_ota.fa->fa_size - OTA_TAIL_RESERVE) {
        mn_post_note("!WARN ota: refused, %u B does not fit slot1", (unsigned)total_len);
        return false;
    }
    memset(s_ota.erased, 0, sizeof(s_ota.erased));
    /* A stale trailer (an earlier staged/tested image) must not survive:
     * MCUboot reads it to decide whether a swap is pending. */
    uint32_t last = s_ota.fa->fa_size / OTA_PAGE - 1;
    if (page_ready(last) != 0) return false;
    s_ota.len = total_len;
    s_ota.active = true;
    mn_post_note("!OTA receiving %u B from %02x%02x%02x%02x into slot1",
                 (unsigned)total_len, sender_id[0], sender_id[1], sender_id[2], sender_id[3]);
    return true;
}

static int ota_chunk(uint16_t idx, uint32_t off, const uint8_t *data, size_t len) {
    ARG_UNUSED(idx);
    if (!s_ota.active || off + len > s_ota.len) return -1;
    for (uint32_t p = off / OTA_PAGE; p <= (off + len - 1) / OTA_PAGE; p++)
        if (page_ready(p) != 0) return -1;
    uint8_t buf[MN_XFER_CHUNK + 4];                /* flash writes are 4-aligned */
    size_t n = (len + 3) & ~(size_t)3;
    memcpy(buf, data, len);
    memset(buf + len, 0xff, n - len);
    return flash_area_write(s_ota.fa, off, buf, n) == 0 ? 0 : -1;
}

static void ota_done(bool complete) {
    if (!s_ota.active) return;
    s_ota.active = false;
    if (!complete) { mn_post_note("!WARN ota: transfer failed, slot1 not staged"); return; }

    /* integrity: SHA-256 over exactly the bytes that were sent */
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    uint8_t blk[512], dig[32];
    size_t dlen = 0;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS;
    for (uint32_t o = 0; ok && o < s_ota.len; o += sizeof(blk)) {
        size_t n = MIN(sizeof(blk), s_ota.len - o);
        ok = flash_area_read(s_ota.fa, o, blk, n) == 0 &&
             psa_hash_update(&op, blk, n) == PSA_SUCCESS;
    }
    ok = ok && psa_hash_finish(&op, dig, sizeof(dig), &dlen) == PSA_SUCCESS;
    psa_hash_abort(&op);
    if (!ok || memcmp(dig, s_ota.want, 16) != 0) {
        mn_post_note("!WARN ota: sha256 mismatch, slot1 not staged");
        return;
    }
    struct mcuboot_img_header h;
    if (boot_read_bank_header(PARTITION_ID(slot1_partition), &h, sizeof(h)) != 0) {
        mn_post_note("!WARN ota: slot1 has no MCUboot header, not staged");
        return;
    }
    s_ota.staged = true;
    mn_post_note("!OTA staged v%u.%u.%u+%u (%u B, sha ok) — FORTH: ota-apply",
                 h.h.v1.sem_ver.major, h.h.v1.sem_ver.minor, h.h.v1.sem_ver.revision,
                 h.h.v1.sem_ver.build_num, (unsigned)s_ota.len);
}

static const mn_xfer_sink_t s_ota_sink = {
    .claim = ota_claim, .on_chunk = ota_chunk, .on_done = ota_done,
};

/* Forth: ota-status ( -- )  ota-apply ( -- )  — privileged via FORTH verb */
static void w_ota_status(void) {
    mn_emit_event("# ota: slot1 %s%s", s_ota.staged ? "STAGED" : "not staged",
                  s_ota.active ? " (receiving)" : "");
}

static void w_ota_apply(void) {
    if (!s_ota.staged) { mn_emit_event("-ERR E_BAD_STATE nothing staged"); return; }
    int rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
    if (rc) { mn_emit_event("-ERR E_INTERNAL boot_request_upgrade rc=%d", rc); return; }
    mn_emit_event("!OTA applying: rebooting into the staged image (TEST; confirms when READY)");
    k_msleep(300);
    sys_reboot(SYS_REBOOT_COLD);
}

void mn_ota_sink_init(void) {
    mn_xfer_set_sink(&s_ota_sink);
    forth_register_word("ota-status", w_ota_status);
    forth_register_word("ota-apply", w_ota_apply);
}
