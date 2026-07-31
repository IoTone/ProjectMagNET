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
#include "forth_core.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* internals from magnet_core.c */
void              mn_core_set_putc(mn_putc_fn p);
SemaphoreHandle_t mn_tx_mutex(void);

#define MN_LINE_MAX 512   /* §11.3: default max HCP line length */

typedef enum { LINK_HCP, LINK_FORTH } link_mode_t;

static mn_getc_fn  s_getc = NULL;
static mn_putc_fn  s_putc = NULL;       /* raw transport putc (for Forth out) */
static link_mode_t s_mode = LINK_HCP;

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

/* ---- HCP command handling ---- */
static void emit_caps(const char *tag) {
    respond(tag,
        "+OK proto=2.1 fw=0.3.0-ec maxline=512 "
        "transports=usbcdc verbs=STATUS,CAPS,HELP,PING,CHAT,DM,PEERS,WHOAMI,NAME,"
        "MODE,SUB,UNSUB,CHANNEL,SYSINFO,MESH,BENCH,SELFTEST,STATS,STRESS,HEARTBEAT,FORTH "
        "events=ready,state,chat,dm,cmd,peer,role,heartbeat,warn queue=4 mode=HCP");
}

static void emit_help(const char *tag) {
    mn_write_line("# HCP verbs: STATUS CAPS HELP PING CHAT <text> DM <ipv6> <text> PEERS WHOAMI");
    mn_write_line("#            NAME <name> MODE TERSE|HUMAN SUB/UNSUB <classes> CHANNEL LIST|SHOW");
    mn_write_line("#            SYSINFO MESH BENCH SELFTEST STATS [RESET] STRESS <secs> <len>");
    mn_write_line("#            HEARTBEAT <secs|0> FORTH");
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
    SemaphoreHandle_t m = mn_tx_mutex();          /* recursive — mn-* words that
                                                     print re-enter mn_write_line */
    if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY);
    forth_eval(line);
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
    xTaskCreate(dispatcher_task, "mn_link", 4096, NULL, 5, NULL);
}
