# Ear ⇄ host wire contract — ATVV v1.0 (as implemented)

The Ear is a standard **Android TV Voice-over-BLE (ATVV) v1.0** remote. Nothing
here is MagNET-specific; any ATVV v1.0 host should work. Sources: Google's
spec as reverse-documented and implemented by the open-source Linux host
[ATVVoice](https://github.com/zhanglongqi/ATVVoice) (Rust, BlueZ D-Bus →
PipeWire, MIT; `src/protocol/{types,v10}.rs`), cross-checked against
Nordic's nRF Smart Remote `ble_atvv`. Implementation: `src/ear_atvv.c`.

## GATT

| | UUID | Props | Security |
|---|---|---|---|
| ATVV service | `AB5E0001-5A21-4F05-BC7D-AF01F617B664` | — | (in scan response) |
| TX (host→Ear) | `AB5E0002-…` | write, write-no-rsp | **authenticated (MITM) bond** |
| RX (audio) | `AB5E0003-…` | notify (+empty read) | auth for read; Ear only streams to an authenticated bond |
| CTL (control) | `AB5E0004-…` | notify (+empty read) | same |
| DIS `0x180A` | manufacturer, model, serial (= hive node id), fw rev | read | open |
| BAS `0x180F` | battery level | read, notify | open |

GAP: name `MagNET-Ear-<bt mac4>`, appearance 0x0180 (Generic Remote Control),
LE Secure Connections, Ear IO capability DisplayOnly → host enters the passkey
shown on the Stick. New bonds are accepted only inside a 120 s **pairing
window** opened by side button B (or `ble-pair`); outside it a new host's
pairing is refused (link dropped at the passkey step). With no bonds and the
window closed the Ear doesn't advertise. Bonds persist; hold B 5 s (or
`ble-unpair`) to forget them. (`EAR_DEV_JUSTWORKS` bench builds have no
passkey step, so they don't enforce the window.)

## Host → Ear (TX)

| Opcode | Bytes | Ear behaviour |
|---|---|---|
| GET_CAPS | `0A ver_hi ver_lo 00 codecs models` | reply CAPS_RESP (always v1.0) |
| MIC_OPEN | `0C mode` | AUDIO_START reason 0x00, stream 0x00; auto-stop after **15 s** unless extended |
| MIC_CLOSE | `0D stream` | AUDIO_STOP reason 0x00 |
| MIC_EXTEND | `0E stream` | reset the 15 s MIC_OPEN timeout |

## Ear → host (CTL)

| Opcode | Bytes | Notes |
|---|---|---|
| CAPS_RESP | `0B 01 00 02 03 fs_hi fs_lo 00 00` | v1.0, codecs 0x02 (ADPCM 16 kHz only), model 0x03 Hold-to-Talk, frame size (BE), extra cfg, reserved |
| AUDIO_START | `04 reason 02 stream` | reason 0x00 MIC_OPEN / 0x03 HTT (button A); HTT stream ids 1..0x7F |
| AUDIO_SYNC | `0A 02 frame_hi frame_lo pred_hi pred_lo step_idx` | decoder state **before** frame `frame`; sent before frame 0, every 25 frames, and after any dropped frame |
| AUDIO_STOP | `00 reason` | 0x00 MIC_CLOSE, 0x02 HTT button release, 0x04 pre-empted by a new AUDIO_START, 0x08 MIC_OPEN timeout |
| MIC_OPEN_ERROR | `0C code_hi code_lo` | 0x0F03 notifications not enabled, 0x0F80 HTT in progress, 0x0FFF internal |

## Audio (RX)

- IMA/DVI ADPCM, 16 kHz, 16-bit mono, 4 bits/sample, **high nibble first**.
- **Headerless** frames of exactly `frame size` bytes (CAPS_RESP): 128 B =
  256 samples = 16 ms when the MTU allows (≥ 131), else `MTU-3` (even, ≥ 20).
- Decoder state is continuous across frames; the host resets it only from
  AUDIO_SYNC. Frame numbers advance by 1 per RX notification; a sync whose
  frame number is ahead of the host's count means frames were lost
  (conceal with silence, `tools/ear_rx.py` does).
- Ear pipeline: PDM → DC remove → 120 Hz 2nd-order high-pass → AGC (fast
  attack above ~-4 dBFS, slow release, ceiling = `ear-gain`, default ×16) →
  ADPCM. The mic is powered/clocked only while a stream is open.
- On stream start the Ear requests a 15–30 ms connection interval; on
  connect it requests Data Length Extension (251 B) so each frame is one
  radio packet.
- **Host readiness:** the Ear streams to a host only after that host has
  subscribed to RX + CTL **and sent GET_CAPS on the current connection**.
  Subscriptions restored from a bond don't count, because the host app may
  not be listening yet.
- **Delivery, not drops:** frames queue in a 3 s ring on the Ear and go out
  as fast as the link allows. A slow link adds delay instead of losing
  audio. Frames are lost only if the backlog exceeds 3 s (oldest first,
  then an AUDIO_SYNC), and AUDIO_STOP follows the last queued frame.
- **Talk with no host connected:** button A still opens a session. The Ear
  buffers (≤ 3 s) and advertises at 20–30 ms. When a bonded host reconnects
  and sends GET_CAPS, it gets AUDIO_START (reason 0x03), the buffered frames
  faster than real time, then live audio, then AUDIO_STOP on release. With no
  host within 6 s, the session is discarded. Frame size is fixed at 128 B
  for these, so the host needs MTU ≥ 131.

## Not implemented
- v0.4 hosts (134-byte framed audio, 8 kHz): the Ear always answers v1.0.
- ADPCM 8 kHz codec, PTT (press-to-talk toggle) model, START_SEARCH.
