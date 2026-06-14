#ifndef MAGNET_LINEAGES_H
#define MAGNET_LINEAGES_H

/* magnet_lineages — mycology-themed firmware family keys.
 *
 * The CHALLENGE/RESPONSE puzzle layer (Layer 2 in docs/MagNET-Generations.md)
 * uses these 32-byte DNA keys to gate hive joins: a peer must prove it knows
 * the key for some lineage the ruler still recognises.
 *
 * Bumping MAGNET_GEN_MAJOR requires:
 *   1. Adding a new {codename, key[32]} row to MAGNET_LINEAGES[] in
 *      magnet_lineages.c (DO NOT delete or reorder existing rows — old
 *      biologics are still valid hive members).
 *   2. Adding the matching #define in include/magnet_gen.h's #if ladder.
 *
 * The keys here are *not* the same as CRAW_HIVE_DEV_SECRET. The hive secret
 * protects the wire (frame HMAC). The lineage key proves descent from the
 * same firmware family. A leaked secret breaks wire auth; a leaked lineage
 * key only erodes the tribe filter for one MAJOR — and the next MAJOR
 * rotates it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAGNET_LINEAGE_KEY_BYTES   32
#define MAGNET_LINEAGE_NAME_MAX    16

typedef struct {
    const char *codename;                                /* "spore", "hyphae", ... */
    const uint8_t key[MAGNET_LINEAGE_KEY_BYTES];         /* 32-byte DNA */
} magnet_lineage_t;

/* NULL-terminated table (final entry has codename == NULL). */
extern const magnet_lineage_t MAGNET_LINEAGES[];

/* Look up a lineage by codename (e.g. "spore"). Returns pointer into the
 * static table on hit, NULL on miss. The codename is the suffix of the
 * gen string after the dash: "0.5.0-spore" → "spore". */
const magnet_lineage_t *magnet_lineage_find(const char *codename);

/* Extract the lineage codename portion from a full gen string.
 * "0.5.0-spore" → "spore". Returns 0 on success (out NUL-terminated),
 * -1 on malformed input or insufficient buffer. */
int magnet_lineage_from_gen(const char *gen_str, char *out, size_t out_len);

/* Compute the puzzle response: HMAC-SHA256(lineage_key, puzzle || node_id || ts_str).
 *
 *   key       — 32-byte DNA key from MAGNET_LINEAGES[]
 *   puzzle    — base64 string (just bytes; we hash the base64 representation
 *               so node and ruler don't disagree on the decode).
 *   node_id   — joiner's id, e.g. "MagNET-biologic-a1b2"
 *   ts        — unix epoch seconds (the ts on the CHALLENGE frame; binds
 *               the response to one round-trip and prevents capture-replay).
 *   out_hex   — caller-provided buffer for the 64+1 hex digit result.
 *
 * Returns 0 on success, -1 on parameter error, -2 on hash failure.
 */
int magnet_lineage_compute_response(const uint8_t key[MAGNET_LINEAGE_KEY_BYTES],
                                    const char *puzzle,
                                    const char *node_id,
                                    int64_t ts,
                                    char *out_hex,
                                    size_t out_hex_len);

#ifdef __cplusplus
}
#endif
#endif
