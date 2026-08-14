# Extended transfer (Type 6) — photos over the mesh

**Status: HARDWARE-VALIDATED, two-node over-the-air, 2026-08-11 (fw 0.7.0-eh) —
resolves design proposal Open Q10 / §11.8 (the photo gap).**

Two-node bench (desktop: Waveshare C6 `93c6899e` + Waveshare LCD-1.47
`1e4f466f`, real 802.15.4 link, RSSI −52):
- 50 KB (153 chunks) A→B: sha256-identical, **5.44 KiB/s**. B→A: 5.38 KiB/s.
- **Transfer under saturation** (receiver simultaneously running `STRESS 25
  200` multicast flood): completed byte-identical at 1.41 KiB/s. The sender
  had 132/290 chunk sends bounced by OT backpressure and re-paced them; the
  receiver logged `dup=4` — NACK retransmits arriving after the original,
  dropped by the bitmap — with `rx err=0` on both sides. The recovery
  machinery demonstrably fired and the payload survived.
- Heap byte-identical (sender) / clean transient dip (receiver) after all runs.

Throughput tracks RF conditions and channel occupancy rather than the protocol.
Repeat runs of the same 50 KB transfer on the same two boards have ranged
4.7–5.4 KiB/s, and the saturation case 1.1–1.4 KiB/s. Correctness did not vary:
every run reassembled sha256-identical.

**Real-photo battery (same day, 10/10):** actual JPEGs 16 KB / 67 KB / 300 KB
both directions at ~5.3 KiB/s, back-to-back sessions, photo intact with chat
flowing both ways mid-transfer, abort signalling, `rx err=0`.
`tools/chat-bench/` integrates photo sending into the browser rig (downscale
in-page, progress both sides, inline preview on arrival).

Single-node validation (Waveshare C6, transfer to own ML-EID through the full
envelope→AEAD→CoAP→OT stack): 1 B / 336 B / 10,752 B (exact window) / 50 KB /
200 KB all hash-identical, **~6.0 KiB/s** sustained, `tx err=0 rx err=0 dup=0`
over 836 frames, heap byte-identical before/after (min-ever dip 1.5 KB).
Failure paths verified: no-session DATA → `E_BAD_STATE`, bad peer →
`E_NO_PEER`, concurrent BEGIN → `E_BUSY`, unreachable peer → 6 INIT retries →
`!XFER_FAIL timeout` with clean session teardown.

The v2.1 envelope's 4-bit fragment field caps an app-layer transfer at ~17 KB;
upstream (PONY-Cyberdeck-25 #7) wants photo sharing. Type 6 is the §11.8
"extended transfer": a 16-bit chunk index carried in the payload (not the
envelope header), **unicast CON only**, one concurrent transfer per direction,
window/NACK-bitmap recovery. The E-B saturation tables dictate every choice
here: fragmented **multicast** loses ~7 % even on a quiet channel and collapses
under load, while CON unicast rides CoAP's own ARQ — so Type 6 never touches
the multicast group.

The node stays a modem (§4.7 Option B): **the host holds the file**. The
receiving node forwards each chunk up its host link as an event and keeps only
a bitmap; the sending node buffers one window (32 chunks = 10.5 KiB) so it can
retransmit without re-asking the host. Host-side re-encode is still the rule
(§11.8): send a ~30–100 KB re-encode, not a camera original.

## Numbers

| Parameter | Value | Why |
|---|---|---|
| Chunk payload | **336 B** | base64(336) = 448 chars → both the host `XFER DATA` line and the `!XFER` event stay under the 512-byte HCP line cap |
| Window | **32 chunks** (10.5 KiB) | sender-side RAM; the STATUS bitmap field is 8 B (64 bits), so the wire format already allows widening |
| Max chunks | **4096** (≈ 1.31 MB) | receiver bitmap = 512 B static; photos re-encoded per §11.8 are 30–100 KB ≈ 90–300 chunks |
| Pacing | 8 chunks / 250 ms tick (≈ 10.7 KB/s offered) | sits under the measured ~13 msg/s (large-payload) OT acceptance ceiling per node |
| Frame on the wire | 16 hdr + 5 + 336 + 8 MIC = **365 B** | ~4 802.15.4 fragments, CON ⇒ MAC-ACK per fragment + CoAP retry per chunk |
| Timeouts | sender: no STATUS in 2.5 s → resend unacked (6 rounds max); receiver: 700 ms gap → NACK; either: 30 s idle → abort | |

Encryption is the normal channel AEAD — every chunk is one envelope frame and
consumes one nonce counter, so the §11.1.5 invariant holds untouched.
Retransmitted chunks get fresh counters; receiver-side dedup is by chunk index
bitmap, not by envelope counter.

## Wire format — Type 6 payload (first byte = subtype)

```
0x01 INIT   (sender → receiver, CON, retried ~1 s until STATUS or 6 tries)
  [0]=0x01 [1..2]=xfer_id [3..6]=total_len u32 [7..8]=total_chunks u16
  [9..10]=chunk_len u16 [11]=meta_len (0..64; larger ⇒ frame dropped) [12..]=meta (e.g. filename)

0x02 DATA
  [0]=0x02 [1..2]=xfer_id [3..4]=chunk_idx u16 [5..]=chunk bytes
  (all chunks are chunk_len long except the last)

0x03 STATUS (receiver → sender, CON)
  [0]=0x03 [1..2]=xfer_id [3..4]=window_base u16
  [5..12]=bitmap u64 LE-bit (bit i = base+i received) [13]=code
  code: 0=in-progress (bitmap is the NACK/ACK picture), 1=complete,
        2=abort/unsupported, 3=busy (a transfer is already inbound)

0x04 ABORT  (either direction)
  [0]=0x04 [1..2]=xfer_id [3]=reason
```

All integers big-endian except the bitmap, which is serialized little-endian
(bit i of the u64 = chunk base+i). The receiver's STATUS always describes **the window
containing its first missing chunk**; a STATUS whose base is *past* the
sender's current window therefore means "window fully received, advance" — no
separate ack subtype needed.

## Sequence

```
host A                node A                    node B               host B
XFER BEGIN p len ───► INIT ────────────────────► session, bitmap ──► !XFER_BEGIN
◄─ +OK xid …          ◄──────────────── STATUS(0, empty, code 0)
XFER DATA <b64> ───►  buffer chunk               (auto-accepted)
… ×window …           tick: ≤8 unacked DATA ───► set bit, forward ─► !XFER idx/total <b64>
                      ◄──────── STATUS(next-missing window, bitmap)
                      advance, !XFER_NEXT ─► host feeds next window
…                     last window acked / ◄──── STATUS code 1 ─────► !XFER_DONE
!XFER_SENT ◄──────────┘
```

Loss recovery: a lost DATA chunk leaves a hole in the receiver's bitmap; the
700 ms gap timer sends a STATUS whose bitmap NACKs exactly the holes, and the
sender's next tick resends only those. A lost STATUS is covered by the
sender's 2.5 s timeout (resend unacked → duplicate chunks drop on the bitmap).

## Host surface (HCP)

| Command | Response | Notes |
|---|---|---|
| `XFER BEGIN <peer-ipv6> <total_len> [<meta>]` | `+OK xid=<hex> chunks=<n> chunk=336 window=32` | READY only; one outbound at a time |
| `XFER DATA <b64>` | `+OK <buffered>/<window>` | exactly chunk-sized (except final); `-ERR E_BUSY` = window full, wait for `!XFER_NEXT` |
| `XFER ABORT` | `+OK aborted` | aborts the active outbound (or inbound if none) and tells the peer; `-ERR E_BAD_STATE` if nothing is active |
| `XFER STATUS` | `# xfer …` lines + `+OK` | both directions' live state |

Events (class `xfer`, SUB/UNSUB as usual):

```
!XFER_BEGIN <from_id> <xid> <total_len> <chunks> <chunk_len> <meta>
!XFER <from_id> <xid> <idx>/<total> <b64chunk>          (receiver, per chunk)
!XFER_DONE <from_id> <xid> len=<n>                       (receiver, complete)
!XFER_NEXT <xid> <base>                                  (sender: feed next window)
!XFER_SENT <xid> len=<n>                                 (sender, fully acked)
!XFER_FAIL <xid> <reason>       (either direction — see the reason table below)
```

| `!XFER_FAIL` reason | Side | Meaning |
|---|---|---|
| `timeout` | both | sender: 6 INIT tries, or 6 resend rounds, or 30 s idle. receiver: 30 s without a chunk |
| `busy` | sender | the peer already has an inbound transfer (STATUS code 3) |
| `refused` | sender | the peer rejected the INIT geometry (STATUS code 2) |
| `peer-abort` | both | the peer sent X_ABORT for this session |
| `aborted` | both | this node's own host issued `XFER ABORT` |
| `superseded` | receiver | the same peer opened a new transfer over this one — its abort was lost, or it restarted |

The receiver auto-accepts (channel membership is the trust boundary, same as
chat); if a transfer is already inbound it answers STATUS code 3 and the
sender fails fast with `!XFER_FAIL busy`. Inbound chunks are forwarded as
events immediately and in whatever order they arrive — **the host reassembles
by index** (it has the RAM; the node keeps only the bitmap).

`tools/xfer.py` is the reference host implementation (send a file, receive to
a file, and a single-node loopback self-test against the node's own ML-EID —
the same trick SELFTEST uses, which is why Type 6 frames are exempt from the
self-drop in `handle_rx`).

## Failure handling

Every session ends in exactly one terminal event, and both ends are built so
that a single lost frame cannot wedge a node until it reboots.

- **Lost DATA.** The receiver's 700 ms gap timer sends a STATUS whose bitmap
  NACKs exactly the holes, and the sender's next tick resends only those.
- **Lost STATUS.** The sender's 2.5 s timeout resends everything unacked, up to
  6 rounds. Duplicate chunks drop on the receiver's bitmap.
- **Lost ABORT.** `X_ABORT` is retried 3 times at 50 ms, because an abort
  issued right after a chunk burst can bounce off OT backpressure. If it is
  lost anyway, an INIT from the same peer with a new `xfer_id` supersedes the
  stale inbound session (`!XFER_FAIL <xid> superseded`) rather than answering
  busy, so a sender that aborted or crashed can restart at once.
- **Host stops feeding.** A sender whose host dies exactly at a window
  boundary has `fill=0`, so the resend machinery never arms. A 30 s idle timer
  covers that case: it aborts, tells the peer, and frees the session. Without
  it the session leaks and every later `XFER BEGIN` returns `E_BUSY`.
- **Peer disappears.** The receiver's own 30 s idle abort sends a best-effort
  `X_ABORT` so the sender fails immediately instead of grinding through its
  full resend ladder.
- **`xfer_id` collision.** A restarted transfer can draw the same random 16-bit
  id as the session it replaces. A repeat INIT counts as a duplicate only if
  `total_len`, `total_chunks` and `chunk_len` all match; otherwise it
  supersedes. Without that check the new data would be recorded against the old
  bitmap.

### Receive-side validation

Type 6 parses bytes chosen by a peer, so the receive path checks them before it
acts on them.

- `meta_len` in INIT is bounded against the receiver's buffer, and an oversized
  value drops the frame.
- `meta` is reduced to printable ASCII without spaces before it reaches an
  event line, so a filename cannot shift the field split or inject a line that
  looks like host protocol.
- Chunk index and length are checked against the announced geometry, and the
  geometry is checked for self-consistency before a session opens.
- `X_STATUS` and `X_ABORT` are matched on the session's peer device id as well
  as on `xfer_id`. The xid is a 16-bit value that every channel member can see,
  so on its own it would let one member forge a COMPLETE into another member's
  transfer, making the host report a photo delivered that never arrived, or
  force an abort. The inbound session takes the id from INIT and the outbound
  one pins it from the first STATUS it accepts.

Channel membership stays the trust boundary, as it is for chat. These checks
keep one member from stepping on another member's transfer. They are not
visible on the wire, so a node that performs them interoperates with one that
does not.

## Deliberate limits (E-H)

- One transfer per direction per node — not per peer pair. Multiplexing can
  land later without a wire change (sessions are keyed by xfer_id).
- No multicast advertise-then-pull (§11.8's group-photo flow) yet: that's a
  Type 1 notice + per-peer `XFER BEGIN`, all host-side, once this exists.
- No resume across reboots: a transfer is seconds-to-minutes; the host retries.
- Forth has no xfer words yet — the use case is host-driven (photos), and §12.3
  gives Forth nothing bigger than a frame anyway.
