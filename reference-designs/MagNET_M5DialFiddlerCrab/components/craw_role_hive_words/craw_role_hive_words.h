#ifndef CRAW_ROLE_HIVE_WORDS_H
#define CRAW_ROLE_HIVE_WORDS_H

// craw_role_hive_words — the hive-KV Forth words role bundles use
// (punch-list H8, split out of craw_role_bundle in H6 so the bundle engine
// itself has no craw_hive dependency and ports to non-WiFi transports —
// Hanasu's Thread nodes carry craw_role_bundle without this component).
//
//   hkv-put$  ( v-addr v-len k-addr k-len -- )        fire-and-forget KV_PUT
//   hkv-get$  ( k-addr k-len -- v-addr v-len -1 | 0 )  value in a static buf
//   hkv-run   ( k-addr k-len -- )  fetch; if non-empty: clear the key, then
//                                  forth_eval the value (at-most-once
//                                  Forth-phrase commands)
//
// Call once from a WiFi-hive node main after forth_init(); safe before the
// hive session is up (the underlying calls fail gracefully until JOINED).

#ifdef __cplusplus
extern "C" {
#endif

void craw_role_bundle_register_hive_words(void);

#ifdef __cplusplus
}
#endif
#endif
