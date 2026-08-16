/*
 * magnet_link.c — the dual-mode host link (design proposal §11.3 + §12.4).
 *
 *  HCP mode (default): line-based, sigil-framed protocol for LLM/chat-app
 *      drivers. Each command yields exactly one +/- response (optionally
 *      @tag-prefixed). Async traffic arrives as ! events.
 *  FORTH mode: `FORTH` drops into the raw engine; lines go to forth_eval();
 *      `.hcp` (alone) returns to HCP mode.
 *
 * A single dispatcher task owns the input stream. It does NOT cede the stream
 * to the blocking forth_repl(); it drives the engine line-by-line so the event
 * pump keeps working. All output funnels through mn_write_line() (serialized),
 * except Forth output which goes direct-to-putc while the dispatcher holds the
 * TX mutex across the whole forth_eval() call (so it can't be split by events).
 */
#include "magnet.h"
#include "magnet_bot.h"
#include "magnet_led.h"
#include "magnet_xfer.h"
#include "forth_core.h"
#include "craw_role_bundle.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"      /* esp_restart() for FACTORY RESET */

/* internals from magnet_core.c */
void              mn_core_set_putc(mn_putc_fn p);
SemaphoreHandle_t mn_tx_mutex(void);

#define MN_LINE_MAX 512   /* §11.3: default max HCP line length */

typedef enum { LINK_HCP, LINK_FORTH } link_mode_t;

static mn_getc_fn  s_getc = NULL;
static mn_putc_fn  s_putc = NULL;       /* raw transport putc (for Forth out) */
static link_mode_t s_mode = LINK_HCP;

static void handle_hcp_line(char *line);
static void handle_forth_line(char *line);

/* Which transport the line in flight arrived on. The serial link is physically
 * trusted (§4.6); BLE is "privileged but pairable" (§11.2.1), so privileged
 * verbs there need an encrypted link. */
static bool s_from_ble = false;

/* Public entry: any transport can hand us a complete line (§11.2). */
void mn_link_feed_line(const char *line) {
    char buf[MN_LINE_MAX];
    strlcpy(buf, line, sizeof(buf));
    s_from_ble = true;
    if (s_mode == LINK_HCP) handle_hcp_line(buf);
    else                    handle_forth_line(buf);
    s_from_ble = false;
}

/* Verbs that change configuration or state a stranger must not touch. */
static bool verb_is_privileged(const char *verb, const char *rest) {
    if (!strcmp(verb, "NAME") || !strcmp(verb, "ADMIN") ||
        !strcmp(verb, "ROTATE") || !strcmp(verb, "RAW") ||
        !strcmp(verb, "SCRIPT") || !strcmp(verb, "HOOK") ||
        !strcmp(verb, "BUNDLE") ||
        !strcmp(verb, "FORTH") || !strcmp(verb, "STRESS") ||
        !strcmp(verb, "FACTORY")) {
        return true;
    }
    /* CHANNEL SHOW/LIST are public info; SET/JOIN rewrite the node's keys. */
    if (!strcmp(verb, "CHANNEL")) {
        return !strncmp(rest, "SET", 3) || !strncmp(rest, "JOIN", 4);
    }
    return false;
}

/* ---- small helpers ---- */
static void respond(const char *tag, const char *body) {
    char buf[MN_LINE_MAX];
    if (tag && tag[0]) snprintf(buf, sizeof(buf), "@%s %s", tag, body);
    else               snprintf(buf, sizeof(buf), "%s", body);
    mn_write_line(buf);
}

static void respond_err(const char *tag, const char *code, const char *msg) {
    char body[256];
    snprintf(body, sizeof(body), "-ERR %s %s", code, msg ? msg : "");
    respond(tag, body);
}

/* Forth output sink: raw putc. Safe because the dispatcher holds the TX mutex
 * for the entire forth_eval() call (see handle_forth_line). */
static void forth_out(int c) { if (s_putc) s_putc(c); }
static int  forth_in(void)   { return -1; } /* engine is driven by forth_eval, not pull */

/* ---- BUNDLE: signed role bundles over HCP (punch-list H6) ----
 * The verified fleet upgrade path — SCRIPT stays a dev convenience, but
 * fleet-delivered code arrives as RoleBundle v2 envelopes (Ed25519) through
 * the shared craw_role_bundle engine (savepoint rollback, NVS persistence,
 * H5 lifecycle ticker). Envelope JSON exceeds MN_LINE_MAX, so it arrives
 * chunked: BEGIN resets, ADD appends verbatim, COMMIT installs. */
#define MN_BUNDLE_JSON_MAX 6144
static char  *s_bundle_acc = NULL;
static size_t s_bundle_acc_len = 0;

static const char *MN_BUNDLE_CAPS[] = { "thread", "chat", "gpio" };
#define MN_BUNDLE_CAPS_N 3

const char **mn_bundle_caps(int *n) {
    if (n) *n = MN_BUNDLE_CAPS_N;
    return MN_BUNDLE_CAPS;
}

static int bundle_print_cb(const char *name, const char *version, void *ctx) {
    (void)ctx;
    char line[96];
    snprintf(line, sizeof(line), "# bundle %s v%s", name, version);
    mn_write_line(line);
    return 0;
}

/* ---- HCP command handling ---- */
static void emit_caps(const char *tag) {
    respond(tag,
        "+OK proto=2.1 fw=" MN_FW_VERSION " maxline=512 "
        "transports=usbcdc verbs=STATUS,CAPS,HELP,PING,CHAT,DM,PEERS,RECENT,WHOAMI,NAME,"
        "MODE,SUB,UNSUB,CHANNEL,PUBKEY,ADMIN,ROTATE,HOOK,SCRIPT,SYSINFO,MESH,BENCH,SELFTEST,STATS,STRESS,HEARTBEAT,FACTORY,FORTH,TIME,XFER,BUNDLE"
#if MN_ENABLE_BOTS
        ",BOTMODE"
#endif
#if MN_ENABLE_LED
        ",LED"
#endif
        " "
        "events=ready,state,chat,dm,cmd,peer,role,heartbeat,warn,xfer queue=4 "
        "xfer_chunk=336 xfer_window=32 xfer_max_chunks=4096 mode=HCP");
}

static void emit_help(const char *tag) {
    mn_write_line("# HCP verbs: STATUS CAPS HELP PING CHAT <text> DM <ipv6> <text> PEERS WHOAMI");
    mn_write_line("#            RECENT [peer-ipv6]  (no arg: replay own ring as !RCHAT; with peer: fetch+merge)");
    mn_write_line("#            NAME <name> MODE TERSE|HUMAN SUB/UNSUB <classes> CHANNEL LIST|SHOW");
    mn_write_line("#            SYSINFO MESH BENCH SELFTEST STATS [RESET] STRESS <secs> <len>");
    mn_write_line("#            HEARTBEAT <secs|0> FORTH PUBKEY ADMIN ADD|LIST ROTATE");
    mn_write_line("#            HOOK CHAT|CMD <word>|LIST|CLEAR  SCRIPT SET|SHOW|RUN|CLEAR");
    mn_write_line("#            BUNDLE BEGIN | ADD <chunk> | COMMIT | LIST | CLEAR | STOP");
    mn_write_line("#              (signed role bundle over HCP: chunk the envelope JSON");
    mn_write_line("#               through ADD, COMMIT verifies Ed25519 + installs + persists)");
    mn_write_line("#            TIME | TIME SET <epoch> [<tz-min>] | TIME SYNC | TIME PUSH");
    mn_write_line("#            XFER BEGIN <peer-ipv6> <len> [<meta>] | DATA <b64> | ABORT | STATUS");
    mn_write_line("#              (Type 6 extended transfer — docs/EXTENDED-TRANSFER.md;");
    mn_write_line("#               feed 336-byte chunks as b64, wait for !XFER_NEXT per window)");
    mn_write_line("#              (no NTP on a Thread-only mesh: SET seeds this node and");
    mn_write_line("#               pushes to the channel; SYNC pulls from whoever has one)");
#if MN_ENABLE_BOTS
    mn_write_line("#            BOTMODE [<id>|OFF]  (no arg lists bots; Forth: `0 botmode`)");
#endif
#if MN_ENABLE_LED
    mn_write_line("#            LED <r> <g> <b> | OFF  (0-255; on-board LED; Forth: `led!`)");
#endif
    mn_write_line("#            FACTORY RESET CONFIRM  (erases everything, reboots)");
    mn_write_line("# FORTH drops into the Forth REPL; type  .hcp  to return.");
    respond(tag, "+OK");
}

/* Parse one HCP line: [@tag] VERB [args...] */
static void handle_hcp_line(char *line) {
    char tag[16] = {0};
    char *p = line;
    while (*p == ' ') p++;

    if (*p == '@') {                 /* optional correlation tag */
        p++;
        int i = 0;
        while (*p && *p != ' ' && i < (int)sizeof(tag) - 1) tag[i++] = *p++;
        tag[i] = '\0';
        while (*p == ' ') p++;
    }
    if (*p == '\0') { respond_err(tag, "E_SYNTAX", "empty"); return; }

    /* split verb from the remainder (rest-of-line is the free-text arg) */
    char *verb = p;
    char *rest = p;
    while (*rest && *rest != ' ') rest++;
    if (*rest == ' ') { *rest = '\0'; rest++; while (*rest == ' ') rest++; }

    /* §11.2.1: over BLE, configuration requires a bonded link. Read-only
     * verbs stay open so a host can identify a node before pairing. */
    if (s_from_ble && !mn_ble_link_secure() && verb_is_privileged(verb, rest)) {
        respond_err(tag, "E_NOT_BONDED", "pair with this node first");
        return;
    }

    if      (!strcmp(verb, "PING"))   respond(tag, "+PONG");
    else if (!strcmp(verb, "STATUS")) {
        char line[192];
        mn_status_line(line, sizeof(line));
        char body[224];
        snprintf(body, sizeof(body), "+OK %s", line);
        respond(tag, body);
    }
    else if (!strcmp(verb, "CAPS"))   emit_caps(tag);
    else if (!strcmp(verb, "HELP"))   emit_help(tag);
    else if (!strcmp(verb, "WHOAMI")) {
        char line[96];
        mn_whoami_line(line, sizeof(line));
        char body[128];
        snprintf(body, sizeof(body), "+OK %s", line);
        respond(tag, body);
    }
    else if (!strcmp(verb, "PEERS"))  { mn_peers_print(); respond(tag, "+OK"); }
    else if (!strcmp(verb, "RECENT")) {
        /* E-G SED catch-up, two forms:
         *   RECENT              → replay OUR ring to the host (!RCHAT lines) —
         *                         a reconnecting phone backfills its feed
         *   RECENT <peer-ipv6>  → CoAP GET magnet/recent from a peer; frames
         *                         replay through the RX path (dupes drop) */
        if (*rest == '\0') {
            mn_recent_print();
            respond(tag, "+OK");
            return;
        }
        mn_state_t st = mn_get_state();
        if (st != MN_READY && st != MN_DEGRADED) {
            respond_err(tag, "E_BAD_STATE", mn_state_name(st)); return;
        }
        int rc = mn_recent_fetch(rest);
        if (rc == -2)      respond_err(tag, "E_NO_PEER", "bad ipv6 address");
        else if (rc != 0)  respond_err(tag, "E_INTERNAL", "send failed");
        else               respond(tag, "+OK fetching (frames replay as events)");
    }
    else if (!strcmp(verb, "PUBKEY")) {          /* full 65B uncompressed, hex */
        char hex[133];
        const uint8_t *pk = mn_pubkey();
        for (int i = 0; i < 65; i++) sprintf(hex + i * 2, "%02x", pk[i]);
        char body[160];
        snprintf(body, sizeof(body), "+OK %s", hex);
        respond(tag, body);
    }
    else if (!strcmp(verb, "ADMIN")) {
        if (!strncmp(rest, "LIST", 4)) { mn_admin_list_print(); respond(tag, "+OK"); }
        else if (!strncmp(rest, "ADD ", 4)) {
            const char *hex = rest + 4;
            while (*hex == ' ') hex++;
            uint8_t pub[65];
            if (strlen(hex) != 130) { respond_err(tag, "E_SYNTAX", "need 130-hex uncompressed pubkey"); return; }
            for (int i = 0; i < 65; i++) {
                unsigned v;
                if (sscanf(hex + i * 2, "%2x", &v) != 1) { respond_err(tag, "E_SYNTAX", "bad hex"); return; }
                pub[i] = (uint8_t)v;
            }
            if (mn_admin_add(pub) != 0) respond_err(tag, "E_BUSY", "allow-list full (max 4)");
            else respond(tag, "+OK");
        }
        else respond_err(tag, "E_SYNTAX", "ADMIN ADD <pubkey-hex> | LIST");
    }
    else if (!strcmp(verb, "BOTMODE")) {
        /* BOTMODE            → list bots (ascending by id) + which is active
         * BOTMODE <n>        → arm bot n
         * BOTMODE OFF | -1   → disarm */
        if (!MN_ENABLE_BOTS) {
            respond_err(tag, "E_UNSUPPORTED", "not built with MN_ENABLE_BOTS=1");
            return;
        }
        if (*rest == '\0') { mn_bot_list(); respond(tag, "+OK"); return; }
        int id;
        if (!strcasecmp(rest, "OFF")) id = -1;
        else if (sscanf(rest, "%d", &id) != 1) {
            respond_err(tag, "E_SYNTAX", "BOTMODE [<id> | OFF]"); return;
        }
        if (mn_bot_set(id) != 0) respond_err(tag, "E_NO_BOT", "no such bot");
        else respond(tag, "+OK");
    }
    else if (!strcmp(verb, "LED")) {
        /* LED <r> <g> <b>   → set the on-board LED (0-255 per channel;
         * LED OFF          → same as LED 0 0 0
         * plain-LED boards light on any nonzero channel) */
        if (!MN_ENABLE_LED) {
            respond_err(tag, "E_UNSUPPORTED", "not built with MN_ENABLE_LED=1");
            return;
        }
        int r = 0, g = 0, b = 0;
        if (*rest != '\0' && strcasecmp(rest, "OFF") != 0) {
            if (sscanf(rest, "%d %d %d", &r, &g, &b) != 3
                    || r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
                respond_err(tag, "E_SYNTAX", "LED <r> <g> <b> (0-255) | OFF");
                return;
            }
        }
        mn_led_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
        respond(tag, "+OK");
    }
    else if (!strcmp(verb, "TIME")) {
        /* TIME                        → report current time / uptime
         * TIME SET <epoch> [<TZ>]     → set the wall clock, e.g.
         *                               TIME SET 1780000000 JST-9          */
        if (*rest == '\0' || !strncasecmp(rest, "GET", 3)) {
            char info[96], body[128];
            mn_time_info(info, sizeof(info));
            snprintf(body, sizeof(body), "+OK %s", info);
            respond(tag, body);
            return;
        }
        if (!strncasecmp(rest, "SYNC", 4)) {
            /* pull: someone on-channel with a clock answers (jittered) */
            if (mn_time_request() != 0) respond_err(tag, "E_INTERNAL", "send failed");
            else respond(tag, "+OK requested (adopted async as '# time adopted')");
            return;
        }
        if (!strncasecmp(rest, "PUSH", 4)) {
            if (mn_time_push() != 0) respond_err(tag, "E_BAD_STATE", "clock unset");
            else respond(tag, "+OK pushed");
            return;
        }
        if (strncasecmp(rest, "SET ", 4)) {
            respond_err(tag, "E_SYNTAX", "TIME [SET <epoch> [<tz-offset-min>]]");
            return;
        }
        const char *p2 = rest + 4;
        while (*p2 == ' ') p2++;
        long long epoch = 0;
        int tzmin = 0;
        int got = sscanf(p2, "%lld %d", &epoch, &tzmin);
        if (got < 1) { respond_err(tag, "E_SYNTAX", "need unix epoch"); return; }
        if (mn_time_set((int64_t)epoch, got >= 2 ? tzmin : 0) != 0) {
            respond_err(tag, "E_SYNTAX", "implausible epoch or tz offset");
            return;
        }
        char info[96], body[128];
        mn_time_info(info, sizeof(info));
        snprintf(body, sizeof(body), "+OK %s (pushed to mesh)", info);
        respond(tag, body);
    }
    else if (!strcmp(verb, "HOOK")) {
        /* HOOK CHAT|CMD <word> | HOOK CLEAR | HOOK LIST */
        if (!strncmp(rest, "LIST", 4)) { mn_hooks_print(); respond(tag, "+OK"); }
        else if (!strncmp(rest, "CLEAR", 5)) {
            mn_hook_set(0, "", 0); mn_hook_set(1, "", 0);
            respond(tag, "+OK hooks cleared");
        }
        else if (!strncmp(rest, "CHAT ", 5) || !strncmp(rest, "CMD ", 4)) {
            int kind = (rest[0] == 'C' && rest[1] == 'H') ? 0 : 1;
            const char *w = rest + (kind == 0 ? 5 : 4);
            while (*w == ' ') w++;
            if (*w == '\0') { respond_err(tag, "E_SYNTAX", "need word name"); return; }
            if (mn_hook_set(kind, w, strlen(w)) != 0)
                respond_err(tag, "E_SYNTAX", "word name too long (max 32)");
            else respond(tag, "+OK");
        }
        else respond_err(tag, "E_SYNTAX", "HOOK CHAT|CMD <word> | LIST | CLEAR");
    }
    else if (!strcmp(verb, "BUNDLE")) {
        if (!strncmp(rest, "BEGIN", 5)) {
            free(s_bundle_acc);
            s_bundle_acc = malloc(MN_BUNDLE_JSON_MAX);
            s_bundle_acc_len = 0;
            if (!s_bundle_acc) { respond_err(tag, "E_INTERNAL", "no memory"); return; }
            s_bundle_acc[0] = '\0';
            respond(tag, "+OK send BUNDLE ADD <chunk>* then BUNDLE COMMIT");
        }
        else if (!strncmp(rest, "ADD ", 4)) {
            const char *chunk = rest + 4;
            size_t n = strlen(chunk);
            if (!s_bundle_acc) { respond_err(tag, "E_BAD_STATE", "no BUNDLE BEGIN"); return; }
            if (s_bundle_acc_len + n >= MN_BUNDLE_JSON_MAX) {
                respond_err(tag, "E_SYNTAX", "bundle too long (max 6144)"); return;
            }
            memcpy(s_bundle_acc + s_bundle_acc_len, chunk, n);
            s_bundle_acc_len += n;
            s_bundle_acc[s_bundle_acc_len] = '\0';
            respond(tag, "+OK");
        }
        else if (!strncmp(rest, "COMMIT", 6)) {
            if (!s_bundle_acc || s_bundle_acc_len == 0) {
                respond_err(tag, "E_BAD_STATE", "nothing accumulated"); return;
            }
            craw_role_bundle_install_result_t r = {0};
            int rc = craw_role_bundle_install_from_json(s_bundle_acc,
                        MN_BUNDLE_CAPS, MN_BUNDLE_CAPS_N, &r);
            free(s_bundle_acc); s_bundle_acc = NULL; s_bundle_acc_len = 0;
            if (rc == BUNDLE_OK) {
                char ok[96];
                snprintf(ok, sizeof(ok), "+OK %s v%s installed", r.info.name, r.info.version);
                respond(tag, ok);
            } else {
                char err[160];
                snprintf(err, sizeof(err), "rc=%d field=%s at=%s", rc, r.err_field, r.err_detail);
                respond_err(tag, "E_BUNDLE", err);
            }
        }
        else if (!strncmp(rest, "LIST", 4)) {
            craw_role_bundle_iterate(bundle_print_cb, NULL);
            respond(tag, "+OK");
        }
        else if (!strncmp(rest, "CLEAR", 5)) {
            craw_role_bundle_role_stop();
            craw_role_bundle_forget_all();
            respond(tag, "+OK cleared");
        }
        else if (!strncmp(rest, "STOP", 4)) {
            craw_role_bundle_role_stop();
            respond(tag, "+OK role stopped");
        }
        else respond_err(tag, "E_SYNTAX", "BUNDLE BEGIN|ADD <chunk>|COMMIT|LIST|CLEAR|STOP");
    }
    else if (!strcmp(verb, "SCRIPT")) {
        /* SCRIPT SET <src with ; separators> | SHOW | RUN | CLEAR */
        if (!strncmp(rest, "SHOW", 4)) { mn_script_show(); respond(tag, "+OK"); }
        else if (!strncmp(rest, "RUN", 3)) {
            if (mn_script_run() != 0) respond_err(tag, "E_SYNTAX", "no script saved");
            else respond(tag, "+OK");
        }
        else if (!strncmp(rest, "CLEAR", 5)) {
            mn_script_save("", 0);
            respond(tag, "+OK");
        }
        else if (!strncmp(rest, "SET ", 4)) {
            char *src = rest + 4;
            while (*src == ' ') src++;
            if (*src == '\0') { respond_err(tag, "E_SYNTAX", "SCRIPT SET <src>"); return; }
            /* one HCP line can't contain newlines: ';;' separates source lines */
            for (char *c = src; *c; c++)
                if (c[0] == ';' && c[1] == ';') { c[0] = '\n'; memmove(c + 1, c + 2, strlen(c + 2) + 1); }
            if (mn_script_save(src, strlen(src)) != 0)
                respond_err(tag, "E_SYNTAX", "script too long (max 1024)");
            else respond(tag, "+OK saved");
        }
        else respond_err(tag, "E_SYNTAX", "SCRIPT SET <src> | SHOW | RUN | CLEAR");
    }
    else if (!strcmp(verb, "ROTATE")) {          /* signed system/rotate (§11.1.8) */
        if (mn_get_state() != MN_READY) { respond_err(tag, "E_BAD_STATE", mn_state_name(mn_get_state())); return; }
        int rc = mn_rotate();
        if (rc != 0) respond_err(tag, "E_INTERNAL", "sign/send failed");
        else respond(tag, "+OK epoch rotated");
    }
    else if (!strcmp(verb, "NAME")) {
        size_t n = strlen(rest);
        if (n < 1 || n > 16) { respond_err(tag, "E_SYNTAX", "NAME <1..16 chars>"); return; }
        for (const char *c = rest; *c; c++)
            if (*c == ' ') { respond_err(tag, "E_SYNTAX", "no spaces in name"); return; }
        mn_name_set(rest);
        respond(tag, "+OK");
    }
    else if (!strcmp(verb, "MODE")) {
        if      (!strcmp(rest, "TERSE")) { mn_terse_set(true);  respond(tag, "+OK TERSE"); }
        else if (!strcmp(rest, "HUMAN")) { mn_terse_set(false); respond(tag, "+OK HUMAN"); }
        else respond_err(tag, "E_SYNTAX", "MODE TERSE|HUMAN");
    }
    else if (!strcmp(verb, "SUB") || !strcmp(verb, "UNSUB")) {
        if (*rest == '\0') { respond_err(tag, "E_SYNTAX", "need class list"); return; }
        if (mn_sub_update(rest, verb[0] == 'S') != 0)
            respond_err(tag, "E_SYNTAX", "classes: chat,dm,cmd,state,peer,xfer,role,heartbeat,warn,all");
        else respond(tag, "+OK");
    }
    else if (!strcmp(verb, "CHANNEL")) {
        char info[128];
        if (!strncmp(rest, "LIST", 4) || !strncmp(rest, "SHOW", 4)) {
            mn_channel_info(info, sizeof(info));      /* public values only (§11.3.5) */
            char body[160];
            snprintf(body, sizeof(body), "+OK %s", info);
            respond(tag, body);
        } else if (!strncmp(rest, "SET ", 4) || !strncmp(rest, "JOIN ", 5)) {
            const char *cred = rest + (rest[0] == 'S' ? 4 : 5);
            while (*cred == ' ') cred++;
            if (*cred == '\0') { respond_err(tag, "E_SYNTAX", "CHANNEL SET <cred>"); return; }
            mn_write_line("# deriving channel key (Path B takes ~seconds)...");
            int rc = mn_channel_set(cred, strlen(cred), info, sizeof(info));
            if (rc != 0) { respond_err(tag, "E_SYNTAX", "bad credential"); return; }
            char body[160];
            snprintf(body, sizeof(body), "+OK %s", info); /* never echo the secret */
            respond(tag, body);
        } else {
            respond_err(tag, "E_SYNTAX", "CHANNEL SET|JOIN <cred> | LIST | SHOW");
        }
    }
    else if (!strcmp(verb, "FACTORY")) {
        /* FACTORY RESET CONFIRM — wipes every provisioned byte, then reboots.
         * The CONFIRM word is not ceremony: this verb sits one typo away from
         * FACTORY RESET being the tail of some other paste, and it is the only
         * command here that cannot be undone. Same reason `git push --force`
         * makes you say it twice. */
        if (strcmp(rest, "RESET") && strcmp(rest, "RESET CONFIRM")) {
            respond_err(tag, "E_SYNTAX", "FACTORY RESET CONFIRM");
            return;
        }
        if (!strcmp(rest, "RESET")) {
            respond_err(tag, "E_CONFIRM_REQUIRED",
                        "erases channel, name, admins, script, identity and BLE "
                        "bonds — say FACTORY RESET CONFIRM");
            return;
        }
        if (mn_factory_reset() != 0) {
            respond_err(tag, "E_INTERNAL", "nvs erase failed");
            return;
        }
        respond(tag, "+OK erased, rebooting");
        mn_write_line("# factory reset: all provisioned state erased");
        /* Let the response actually leave the node before the CPU does: the
         * USB-CDC FIFO and the BLE notify queue are both still draining. */
        vTaskDelay(pdMS_TO_TICKS(600));
        esp_restart();
    }
    else if (!strcmp(verb, "SYSINFO")) { mn_sysinfo_print(); respond(tag, "+OK"); }
    else if (!strcmp(verb, "MESH"))    { mn_mesh_print(); respond(tag, "+OK"); }
    else if (!strcmp(verb, "BENCH"))   { mn_bench(); respond(tag, "+OK"); }
    else if (!strcmp(verb, "SELFTEST")) {
        if (mn_selftest() == 0) respond(tag, "+OK selftest pass");
        else                    respond_err(tag, "E_INTERNAL", "selftest failed (see # lines)");
    }
    else if (!strcmp(verb, "STATS")) {
        if (!strcmp(rest, "RESET")) { mn_stats_reset(); respond(tag, "+OK stats reset"); }
        else { mn_stats_print(); respond(tag, "+OK"); }
    }
    else if (!strcmp(verb, "STRESS")) {
        /* STRESS <secs> <payload_len> — saturation burst to the channel mcast */
        char *p2 = rest;
        long secs = strtol(rest, &p2, 10);
        long plen = (p2 && *p2) ? strtol(p2, NULL, 10) : 0;
        if (secs <= 0 || plen <= 0) { respond_err(tag, "E_SYNTAX", "STRESS <secs> <payload_len>"); return; }
        if (mn_get_state() != MN_READY) { respond_err(tag, "E_BAD_STATE", mn_state_name(mn_get_state())); return; }
        int rc = mn_stress_start((uint32_t)secs, (uint32_t)plen);
        if (rc == -1)      respond_err(tag, "E_BUSY", "stress already running");
        else if (rc == -2) respond_err(tag, "E_SYNTAX", "len 7..496, secs 1..3600");
        else               respond(tag, "+OK stress started");
    }
    else if (!strcmp(verb, "HEARTBEAT")) {
        if (*rest == '\0') { respond_err(tag, "E_SYNTAX", "HEARTBEAT <secs|0>"); return; }
        char *end = NULL;
        long secs = strtol(rest, &end, 10);
        if (end == rest || secs < 0 || secs > 86400) {
            respond_err(tag, "E_SYNTAX", "secs must be 0..86400"); return;
        }
        mn_heartbeat_set((uint32_t)secs);
        respond(tag, "+OK");
    }
    else if (!strcmp(verb, "CHAT")) {
        if (*rest == '\0') { respond_err(tag, "E_SYNTAX", "CHAT needs text"); return; }
        mn_state_t st = mn_get_state();
        if (st == MN_DEGRADED) {                     /* queue, replay on READY */
            int depth = mn_queue_chat(NULL, rest, strlen(rest));
            if (depth < 0) respond_err(tag, "E_QUEUE_FULL", "max 4");
            else { char b[32]; snprintf(b, sizeof(b), "+QUEUED %d", depth); respond(tag, b); }
            return;
        }
        if (st != MN_READY) { respond_err(tag, "E_BAD_STATE", mn_state_name(st)); return; }
        int rc = mn_chat(rest, strlen(rest));
        if (rc == -4)      respond_err(tag, "E_RATE_LIMITED", "burst 8, 10/s");
        else if (rc != 0)  respond_err(tag, "E_INTERNAL", "send failed");
        else               respond(tag, "+OK");
    }
    else if (!strcmp(verb, "DM")) {
        /* DM <ipv6> <text…> — first token is the peer, rest-of-line is the text */
        char *text = rest;
        while (*text && *text != ' ') text++;
        if (*text == ' ') { *text = '\0'; text++; while (*text == ' ') text++; }
        if (*rest == '\0' || *text == '\0') { respond_err(tag, "E_SYNTAX", "DM <ipv6> <text>"); return; }
        mn_state_t st = mn_get_state();
        if (st == MN_DEGRADED) {
            int depth = mn_queue_chat(rest, text, strlen(text));
            if (depth < 0) respond_err(tag, "E_QUEUE_FULL", "max 4");
            else { char b[32]; snprintf(b, sizeof(b), "+QUEUED %d", depth); respond(tag, b); }
            return;
        }
        if (st != MN_READY) { respond_err(tag, "E_BAD_STATE", mn_state_name(st)); return; }
        int rc = mn_dm(rest, text, strlen(text));
        if (rc == -2)      respond_err(tag, "E_NO_PEER", "bad ipv6 address");
        else if (rc != 0)  respond_err(tag, "E_INTERNAL", "send failed");
        else               respond(tag, "+OK");
    }
    else if (!strcmp(verb, "XFER")) {
        /* Type 6 extended transfer (E-H, docs/EXTENDED-TRANSFER.md) */
        if (!strncmp(rest, "STATUS", 6)) { mn_xfer_status_print(); respond(tag, "+OK"); }
        else if (!strncmp(rest, "ABORT", 5)) {
            if (mn_xfer_abort() != 0) respond_err(tag, "E_BAD_STATE", "no transfer active");
            else respond(tag, "+OK aborted");
        }
        else if (!strncmp(rest, "DATA ", 5)) {
            const char *b64 = rest + 5;
            while (*b64 == ' ') b64++;
            int rc = mn_xfer_data_b64(b64);
            if (rc == -1)      respond_err(tag, "E_BAD_STATE", "no transfer — XFER BEGIN first");
            else if (rc == -2) respond_err(tag, "E_SYNTAX", "bad base64");
            else if (rc == -3) respond_err(tag, "E_BUSY", "window full — wait for !XFER_NEXT");
            else if (rc == -5) respond_err(tag, "E_SYNTAX", "chunk must be 336 B raw (final: remainder)");
            else if (rc < 0)   respond_err(tag, "E_INTERNAL", "");
            else {
                char b[32];
                snprintf(b, sizeof(b), "+OK %d/%d", rc, MN_XFER_WINDOW);
                respond(tag, b);
            }
        }
        else if (!strncmp(rest, "BEGIN ", 6)) {
            if (mn_get_state() != MN_READY) {
                respond_err(tag, "E_BAD_STATE", mn_state_name(mn_get_state())); return;
            }
            char *p2 = rest + 6;
            while (*p2 == ' ') p2++;
            char *peer = p2;
            while (*p2 && *p2 != ' ') p2++;
            if (!*p2) { respond_err(tag, "E_SYNTAX", "XFER BEGIN <peer> <len> [<meta>]"); return; }
            *p2++ = '\0';
            while (*p2 == ' ') p2++;
            char *end = NULL;
            unsigned long tlen = strtoul(p2, &end, 10);
            if (end == p2 || tlen == 0) { respond_err(tag, "E_SYNTAX", "need total_len"); return; }
            const char *meta = end;
            while (*meta == ' ') meta++;
            char info[96];
            int rc = mn_xfer_begin(peer, (uint32_t)tlen, *meta ? meta : NULL,
                                   info, sizeof(info));
            if (rc == -1)      respond_err(tag, "E_BUSY", "transfer already active");
            else if (rc == -2) respond_err(tag, "E_SYNTAX", "bad args (max 4096 chunks)");
            else if (rc == -3) respond_err(tag, "E_NO_PEER", "bad ipv6 address");
            else {
                char b[128];
                snprintf(b, sizeof(b), "+OK %s", info);
                respond(tag, b);
            }
        }
        else respond_err(tag, "E_SYNTAX",
                         "XFER BEGIN <peer> <len> [<meta>] | DATA <b64> | ABORT | STATUS");
    }
    else if (!strcmp(verb, "FORTH")) {
        s_mode = LINK_FORTH;
        respond(tag, "+OK entering forth; type .hcp to return");
        mn_write_line("ok> ");
    }
    else respond_err(tag, "E_UNKNOWN_VERB", verb);
}

/* In FORTH mode, drive the engine line-by-line. Hold the TX mutex across the
 * whole eval so Forth's incremental output stays atomic vs. async events. */
static void handle_forth_line(char *line) {
    if (!strcmp(line, ".hcp")) {
        s_mode = LINK_HCP;
        mn_write_line("# back to HCP");
        return;
    }
    /* mn_forth_exec takes TX mutex (recursive — mn-* words that print re-enter
     * mn_write_line) then the engine mutex, so a mesh-triggered hook can never
     * interleave with a REPL evaluation. */
    SemaphoreHandle_t m = mn_tx_mutex();
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    mn_forth_exec(line);
    forth_out('\r'); forth_out('\n');
    forth_out('o'); forth_out('k'); forth_out('>'); forth_out(' ');
    if (m) xSemaphoreGiveRecursive(m);
}

static void dispatcher_task(void *arg) {
    (void)arg;
    char line[MN_LINE_MAX];
    int  pos = 0;
    bool overflow = false;

    for (;;) {
        int ch = s_getc();
        if (ch < 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (ch == '\r' || ch == '\n') {
            if (pos == 0 && !overflow) continue;     /* skip blank lines */
            line[pos] = '\0';
            if (overflow) {
                respond_err(NULL, "E_LINE_TOO_LONG", "");
            } else if (s_mode == LINK_HCP) {
                handle_hcp_line(line);
            } else {
                handle_forth_line(line);
            }
            pos = 0;
            overflow = false;
        } else if (pos < MN_LINE_MAX - 1) {
            line[pos++] = (char)ch;
        } else {
            overflow = true;                          /* keep draining to EOL */
        }
    }
}

void mn_link_start(mn_getc_fn g, mn_putc_fn p) {
    s_getc = g;
    s_putc = p;
    mn_core_set_putc(p);             /* serialized writer uses the same putc */
    forth_set_io(forth_in, forth_out);
    xTaskCreate(dispatcher_task, "mn_link", 8192, NULL, 5, NULL);   /* 8K: BUNDLE COMMIT runs cJSON + Ed25519 + forth eval here (H6) */
}
