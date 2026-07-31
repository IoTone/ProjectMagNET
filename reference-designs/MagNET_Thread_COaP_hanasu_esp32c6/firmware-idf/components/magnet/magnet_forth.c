/*
 * magnet_forth.c — MagNET Forth vocabulary (FFI).
 *
 * Each word is a thin wrapper that pops args off the Forth stack and calls the
 * SAME mn_* core function the HCP dispatcher uses (design proposal §12.3). One
 * implementation, two front-ends.
 *
 * Words taking strings ((c-addr u)) are driven from the REPL with `s"`, now
 * provided by the engine (forth_core.cpp). Example:  s" hello team" mn-chat
 * The no-arg `mn-hello` remains as a zero-dependency smoke test.
 */
#include "magnet.h"
#include "forth_core.h"

#include <stdint.h>

/* mn-chat ( c-addr u -- )  send multicast chat */
static void w_mn_chat(void) {
    intptr_t u    = forth_pop();
    intptr_t addr = forth_pop();
    if (addr && u > 0) mn_chat((const char *)addr, (size_t)u);
}

/* mn-dm ( addr-caddr addr-u text-caddr text-u -- )  unicast chat
 * e.g.  s" fd00::1234" s" hi there" mn-dm  */
static void w_mn_dm(void) {
    intptr_t tu    = forth_pop();
    intptr_t taddr = forth_pop();
    intptr_t au    = forth_pop();
    intptr_t aaddr = forth_pop();
    if (!aaddr || au <= 0 || au >= 46 || !taddr || tu <= 0) return;
    char ip[46];
    for (intptr_t i = 0; i < au; i++) ip[i] = ((const char *)aaddr)[i];
    ip[au] = '\0';
    mn_dm(ip, (const char *)taddr, (size_t)tu);
}

/* mn-hello ( -- )  send a fixed chat — testable without s" */
static void w_mn_hello(void) {
    static const char msg[] = "hello from forth";
    mn_chat(msg, sizeof(msg) - 1);
}

/* mn-status ( -- ) */
static void w_mn_status(void) { mn_status_print(); }

/* mn-peers ( -- ) */
static void w_mn_peers(void)  { mn_peers_print(); }

/* mn-whoami ( -- ) */
static void w_mn_whoami(void) { mn_whoami_print(); }

/* mn-state ( -- n )  push current lifecycle state enum */
static void w_mn_state(void)  { forth_push((intptr_t)mn_get_state()); }

/* ---- diagnostics / test words ---- */

/* mn-sysinfo ( -- )  chip, IDF, heap, forth heap, uptime */
static void w_mn_sysinfo(void) { mn_sysinfo_print(); }

/* mn-mesh ( -- )  Thread detail: partition, RLOC, ML-EID, neighbors+RSSI */
static void w_mn_mesh(void) { mn_mesh_print(); }

/* mn-bench ( -- )  envelope codec + TX-call latency */
static void w_mn_bench(void) { mn_bench(); }

/* mn-selftest ( -- f )  0 = pass; pump/loopback parts auto-skip at ok>
 * (the dispatcher holds the TX writer across eval — run SELFTEST from HCP
 * for the full sweep) */
static void w_mn_selftest(void) { forth_push((intptr_t)mn_selftest()); }

/* mn-heartbeat! ( secs -- )  set !HEARTBEAT interval, 0 = off */
static void w_mn_heartbeat(void) {
    intptr_t secs = forth_pop();
    if (secs >= 0 && secs <= 86400) mn_heartbeat_set((uint32_t)secs);
}

/* mn-stats ( -- )  print tx/rx counters */
static void w_mn_stats(void) { mn_stats_print(); }

/* mn-stress ( secs len -- f )  start saturation burst; 0 = started */
static void w_mn_stress(void) {
    intptr_t len  = forth_pop();
    intptr_t secs = forth_pop();
    forth_push((intptr_t)mn_stress_start((uint32_t)secs, (uint32_t)len));
}

void mn_register_forth_vocab(void) {
    forth_register_word("mn-chat",   w_mn_chat);
    forth_register_word("mn-dm",     w_mn_dm);
    forth_register_word("mn-hello",  w_mn_hello);
    forth_register_word("mn-status", w_mn_status);
    forth_register_word("mn-peers",  w_mn_peers);
    forth_register_word("mn-whoami", w_mn_whoami);
    forth_register_word("mn-state",  w_mn_state);
    forth_register_word("mn-sysinfo",    w_mn_sysinfo);
    forth_register_word("mn-mesh",       w_mn_mesh);
    forth_register_word("mn-bench",      w_mn_bench);
    forth_register_word("mn-selftest",   w_mn_selftest);
    forth_register_word("mn-heartbeat!", w_mn_heartbeat);
    forth_register_word("mn-stats",      w_mn_stats);
    forth_register_word("mn-stress",     w_mn_stress);
    /* E-Phase D adds: mn-join mn-set-channel mn-cmd mn-admin-add mn-on-chat ... */
}
