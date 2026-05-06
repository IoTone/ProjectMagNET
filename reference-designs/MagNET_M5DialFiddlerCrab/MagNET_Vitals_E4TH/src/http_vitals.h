/*
 * http_vitals.h — HTTP server exposing the vitals services per the UC3
 * dataspace manifest (specs/MagNET-Vitals-E4TH-proposal.md §6).
 *
 * Lifecycle: start from the WiFi-connected callback once lwip has an IP;
 * stop on disconnect (no-op if already stopped).
 *
 * Endpoints — all GET, all return application/json with `Access-Control-Allow-Origin: *`:
 *
 *   /vitals              snapshot of every signal
 *   /heart-rate          { bpm, presence, timestamp_us }
 *   /heart-rate/history  { samples: [{t,v}, ...] } 60 mins, 1/min
 *   /breathing           { rpm, timestamp_us }
 *   /breathing/history   same shape as HR history
 *   /presence            { present, age_ms }
 *   /lux                 { lux, timestamp_us }
 *   /targets             { count, targets: [{ id, x_m, y_m, dop, cluster }] }
 *   /phases              [[heart...], [breath...], [total...]]   ← streamgraph 'distributions' shape
 *
 *   OPTIONS *            204 + CORS preflight headers (wildcard match)
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t http_vitals_start(void);
void      http_vitals_stop(void);

#ifdef __cplusplus
}
#endif
