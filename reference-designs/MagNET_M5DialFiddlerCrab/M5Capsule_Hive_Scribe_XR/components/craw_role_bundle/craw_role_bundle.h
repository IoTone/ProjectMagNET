#ifndef CRAW_ROLE_BUNDLE_H
#define CRAW_ROLE_BUNDLE_H
#define CRAW_ROLE_BUNDLE_VERSION "0.1.0"

// craw_role_bundle — Phase-4 Milestone-C step 2.
//
// Parses, verifies, and installs a signed Forth role bundle. See
// docs/MagNET-RoleBundle-v1.md for the wire format. Typical use:
//
//   craw_role_bundle_init();                        // boot path
//   craw_role_bundle_apply_saved(node_caps, n);    // re-install persisted
//
//   // Later, when ROLE_GRANT delivers a bundle (or via REPL):
//   craw_role_bundle_install_result_t r;
//   int rc = craw_role_bundle_install_from_json(json, node_caps, n_caps, &r);
//
// The "node_caps" array is what this node advertises (e.g. ["camera","jpeg"]).
// A bundle whose caps_req aren't a subset is refused with BUNDLE_ERR_CAPS.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BUNDLE_OK             =  0,
    BUNDLE_ERR_PARSE      = -1,  // malformed JSON / missing required field
    BUNDLE_ERR_PROTO      = -2,  // min_proto exceeds our supported version
    BUNDLE_ERR_AUTHOR     = -3,  // author not in trust store
    BUNDLE_ERR_SIG        = -4,  // signature mismatch
    BUNDLE_ERR_BASE64     = -5,  // src_b64 fails to decode
    BUNDLE_ERR_CRC        = -6,  // crc32 over source doesn't match envelope
    BUNDLE_ERR_CAPS       = -7,  // node caps don't cover bundle caps_req
    BUNDLE_ERR_VERSION    = -8,  // refused downgrade
    BUNDLE_ERR_EVAL       = -9,  // forth_eval_n returned non-zero
    BUNDLE_ERR_NVS        = -10, // failed to persist envelope (still installed)
    BUNDLE_ERR_INTERNAL   = -11, // memory / runtime
} craw_role_bundle_err_t;

typedef struct {
    char        name[33];
    char        version[24];
    char        author[33];
    int         min_proto;
    uint32_t    crc32;
    size_t      src_len;        // decoded source size in bytes
} craw_role_bundle_info_t;

typedef struct {
    craw_role_bundle_err_t  status;
    craw_role_bundle_info_t info;       // populated when status==BUNDLE_OK
    char                    err_field[24];   // diagnostic: which field failed
} craw_role_bundle_install_result_t;

// One-time setup. Idempotent.
void craw_role_bundle_init(void);

// Validate + install the bundle. node_caps is an array of cap strings the
// node advertises (e.g. {"camera","jpeg"}) of length n_caps. On BUNDLE_OK
// the bundle's Forth source has been forth_eval_n()'d and persisted to NVS.
// Pass result=NULL if you don't need the diagnostic detail.
int craw_role_bundle_install_from_json(const char *json,
                                       const char **node_caps, int n_caps,
                                       craw_role_bundle_install_result_t *result);

// Re-install all bundles previously persisted to NVS. Called early in boot
// so a node auto-resumes its last role without re-fetching from the hive.
// Returns the number of bundles successfully reapplied.
int craw_role_bundle_apply_saved(const char **node_caps, int n_caps);

// Erase a single role's persisted bundle (does NOT undo a running bundle's
// effects on the live Forth vocabulary — those persist until reboot).
int craw_role_bundle_forget(const char *name);

// Erase ALL persisted bundles. Useful for factory reset.
int craw_role_bundle_forget_all(void);

// Iterate persisted bundles. Callback receives name + version. Return
// non-zero from cb to stop early. Returns count visited.
typedef int (*craw_role_bundle_iter_cb_t)(const char *name, const char *version,
                                          void *ctx);
int craw_role_bundle_iterate(craw_role_bundle_iter_cb_t cb, void *ctx);

// Compute the canonical signing input for a bundle. Caller-allocated buf;
// returns required length (excluding NUL) or -1 on error. Useful for
// integration tests and for matching the Python signer's output.
int craw_role_bundle_signing_input(const char *name, const char *version,
                                   int min_proto, const char *author,
                                   const char *crc32_hex, const char *src_b64,
                                   char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
#endif
