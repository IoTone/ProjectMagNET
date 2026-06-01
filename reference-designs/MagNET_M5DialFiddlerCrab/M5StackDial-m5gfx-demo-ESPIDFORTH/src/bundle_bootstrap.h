#ifndef BUNDLE_BOOTSTRAP_H
#define BUNDLE_BOOTSTRAP_H

// Bundle bootstrap (Step 4A) — at boot, the Dial pre-seeds its in-memory
// KV table with all signed bundle envelopes that were embedded into the
// firmware via EMBED_FILES. After this runs, any peer that issues
// KV_GET key="bundle:<name>" gets the envelope back without the Dial
// needing to fetch from a Scribe.
//
// The function is idempotent — calling it again is a no-op once the table
// already has each entry.

#ifdef __cplusplus
extern "C" {
#endif

void bundle_bootstrap(void);

#ifdef __cplusplus
}
#endif
#endif
