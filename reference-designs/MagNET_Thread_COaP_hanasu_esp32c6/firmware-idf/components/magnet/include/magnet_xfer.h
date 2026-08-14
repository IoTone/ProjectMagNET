/*
 * magnet_xfer.h — Type 6 extended transfer (E-H, docs/EXTENDED-TRANSFER.md).
 *
 * Resolves design proposal Open Q10 / §11.8: photo-sized payloads over CON
 * unicast with a 16-bit chunk index, one window buffered on the sender, a
 * bitmap (never the data) on the receiver. The node is a modem — the host
 * holds the file and reassembles by chunk index from !XFER events.
 */
#ifndef MAGNET_XFER_H
#define MAGNET_XFER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MN_XFER_CHUNK       336   /* b64(336)=448 chars keeps lines under 512 */
#define MN_XFER_WINDOW      32    /* sender-side buffer, 10.5 KiB             */
#define MN_XFER_MAX_CHUNKS  4096  /* rx bitmap 512 B; ≈1.31 MB max transfer   */

void mn_xfer_init(void);          /* mutex + tick timer (call before radios)  */

/* ---- host (sender) side — called from the HCP dispatcher ---- */
/* 0 ok (info = "xid=… chunks=… chunk=… window=…"), -1 busy (transfer active),
 * -2 bad args (null peer, zero length, over MN_XFER_MAX_CHUNKS),
 * -3 peer_ipv6 is not a parseable address. A transient send failure is not an
 * error here: the tick retries INIT. */
int  mn_xfer_begin(const char *peer_ipv6, uint32_t total_len,
                   const char *meta, char *info, size_t cap);
/* Buffered count (1..window) on success; -1 no active transfer, -2 bad
 * base64, -3 window full (host waits for !XFER_NEXT; also returned for
 * data past the announced total), -5 wrong chunk size (must be exactly
 * MN_XFER_CHUNK raw except the final chunk) */
int  mn_xfer_data_b64(const char *b64);
int  mn_xfer_abort(void);         /* 0 = something was aborted, -1 = idle     */
void mn_xfer_status_print(void);  /* '#' lines for both directions           */

/* ---- mesh side — called from the pump task only ---- */
void mn_xfer_on_rx(const uint8_t sender_id[4], const uint8_t *pl, size_t len,
                   const char *src_ipv6);
void mn_xfer_tick(void);          /* 250 ms cadence while any session active  */

#ifdef __cplusplus
}
#endif

#endif /* MAGNET_XFER_H */
