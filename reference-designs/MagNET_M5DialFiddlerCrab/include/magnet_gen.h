#ifndef MAGNET_GEN_H
#define MAGNET_GEN_H

/* MagNET firmware generation — single source of truth.
 *
 * Three orthogonal version concepts you'll see in this codebase:
 *
 *   proto    — wire format compatibility (CRAW_HIVE_PROTO_VERSION).
 *              Bumps only when HELLO/WELCOME/KV/ROLE_GRANT field schemas
 *              change. Currently 1.
 *
 *   gen      — overall firmware "generation" (this file). MAJOR.MINOR.PATCH
 *              SemVer. Travels with HELLO; logged, displayed, used by role
 *              bundles via min_gen.
 *
 *   role     — runtime function (spawn / scribe / spy / ...). Changes per
 *              ROLE_GRANT, persisted in the role_bundle component's NVS.
 *
 * Bump rules:
 *   MAJOR — incompatible NVS schemas, new required caps, removed words.
 *   MINOR — new optional features, new caps, new Forth words.
 *   PATCH — bug fixes only.
 *
 * MAJOR also implies a new mycology lineage codename (see below). Each
 * MAJOR has its own 32-byte "DNA" key used by the CHALLENGE/RESPONSE
 * puzzle layer in components/craw_hive/magnet_lineages.[ch].
 */

#define MAGNET_GEN_MAJOR    0
#define MAGNET_GEN_MINOR    5
#define MAGNET_GEN_PATCH    0

/* Mycology codenames per MAJOR. Adding a new codename is a deliberate
 * decision tied to a MAJOR bump — old codenames stay valid as fallback
 * lineages so older firmware can still join the hive. */
#define MAGNET_LINEAGE_SPORE      "spore"      /* gen 0.x — sporulation, the seed stage */
#define MAGNET_LINEAGE_HYPHAE     "hyphae"     /* gen 1.x — first network filaments */
#define MAGNET_LINEAGE_MYCELIUM   "mycelium"   /* gen 2.x — full underground network */
#define MAGNET_LINEAGE_FRUITING   "fruiting"   /* gen 3.x — visible reproductive body */
#define MAGNET_LINEAGE_SPOROCARP  "sporocarp"  /* gen 4.x — mature reproducer */

/* Pick our current lineage based on MAJOR. Update this block in concert
 * with the lineage-key table in magnet_lineages.c when bumping MAJOR. */
#if   MAGNET_GEN_MAJOR == 0
  #define MAGNET_GEN_LINEAGE      MAGNET_LINEAGE_SPORE
#elif MAGNET_GEN_MAJOR == 1
  #define MAGNET_GEN_LINEAGE      MAGNET_LINEAGE_HYPHAE
#elif MAGNET_GEN_MAJOR == 2
  #define MAGNET_GEN_LINEAGE      MAGNET_LINEAGE_MYCELIUM
#elif MAGNET_GEN_MAJOR == 3
  #define MAGNET_GEN_LINEAGE      MAGNET_LINEAGE_FRUITING
#elif MAGNET_GEN_MAJOR == 4
  #define MAGNET_GEN_LINEAGE      MAGNET_LINEAGE_SPOROCARP
#else
  #define MAGNET_GEN_LINEAGE      "unknown"
#endif

/* Two stringification levels needed because the # operator stringifies
 * the literal token, not its expansion. */
#define MAGNET_GEN_STR_(a,b,c,name)   #a "." #b "." #c "-" name
#define MAGNET_GEN_STR_EXPAND(a,b,c,name) MAGNET_GEN_STR_(a,b,c,name)

#define MAGNET_GEN_STR \
    MAGNET_GEN_STR_EXPAND(MAGNET_GEN_MAJOR, MAGNET_GEN_MINOR, MAGNET_GEN_PATCH, \
                          MAGNET_GEN_LINEAGE)

#endif
