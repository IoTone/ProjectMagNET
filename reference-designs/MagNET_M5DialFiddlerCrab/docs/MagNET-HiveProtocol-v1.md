# MagNET Hive Protocol v1

**Status**: draft (Phase-4 Milestone B).
**Scope**: minimum-viable ruler discovery + node join + role request.
Covers R5–R7 of `MagNET_M5DialFiddlerCrab/README.md`. Role-bundle signing and execution (R8–R12) are deferred to Milestone C.

## Goals and non-goals

**Goals**
- A spawn node that already has WiFi (via Milestone A BLE provisioning) can find a ruler on the same LAN without configuration.
- Join is authenticated by a pre-shared secret — a stolen mac4 alone cannot impersonate a node.
- The wire format is inspectable (text-based), so `nc`, Wireshark, and a laptop script can all play ruler or node for debugging.

**Non-goals (v1)**
- Forward secrecy / key rotation (add in v2).
- Multi-ruler consensus. The ruler auto-accepts any valid HMAC; R6 "consensus" is stubbed.
- WAN / cross-subnet operation. mDNS + TCP means LAN only.
- Byzantine tolerance of malicious nodes. We trust holders of the shared secret.

## Topology

```
┌──────────────────┐              ┌──────────────────┐
│  Ruler (M5Dial)  │              │  Node (C3U)      │
│  - mDNS advert   │◄──discover───│  - mDNS query    │
│  - TCP listener  │◄──JOIN───────│  - TCP client    │
│  - role store    │──WELCOME────►│  - session state │
└──────────────────┘              └──────────────────┘
```

Both sides use the `craw_hive` component. A single ruler serves many nodes; a node talks to one ruler at a time.

## Discovery — mDNS

The ruler advertises:

```
_magnet-ruler._tcp.local   port 7447   TXT: ver=1, hive=<hive_id>
```

Port `7447` is chosen because it sits above the ephemeral range and spells "hive" on a phone keypad. Configurable at ruler start; nodes discover both hostname and port from the SRV record so changing the port on the ruler is transparent.

TXT record fields:
- `ver=1` — protocol version. Nodes MUST ignore services with a higher `ver` than they speak.
- `hive=<hive_id>` — 16-char slug identifying the hive. A node configured for hive `beehive-1` ignores a ruler advertising `hive=lab-test`. Allows multiple hives to co-exist on one LAN.

A node scans for up to 3 s, picks the first matching `(ver, hive)` service, and resolves its A record. If no ruler is found, it retries on a 10 s backoff (capped).

## Transport — length-prefixed JSON over TCP

After TCP connect:

```
[4-byte big-endian uint32 length N] [N bytes of UTF-8 JSON]
```

Max `N` = 4096 bytes in v1. Larger payloads (role bundles) will use a dedicated streaming frame type in v2 — for v1 we simply cap.

No TLS. The HMAC authenticates the sender; payloads are readable on the wire by design (ease of debugging). Role bundles in a later milestone will carry their own signature, making wire-level confidentiality separate from authenticity.

## Authentication — HMAC-SHA256

Every message (both directions) carries an `auth` field. It is HMAC-SHA256 of the remaining fields serialized in a canonical form:

```
hmac = HMAC-SHA256(shared_secret, "<type>|<nonce>|<ts>|<payload-json>")
```

- `shared_secret` — 32-byte key, distributed during BLE provisioning (new char in v1.1 of `craw_ble_provision`) or hardcoded for bringup.
- `nonce` — 16 random bytes, hex-encoded (32 chars). Replay protection: the receiver rejects a `(nonce, sender_id)` pair seen in the last 60 s.
- `ts` — unix seconds. Must be within ±30 s of the receiver's clock. Nodes SNTP-sync on WiFi join; rulers have an authoritative clock.
- `payload-json` — canonical: keys sorted alphabetically, no whitespace. cJSON's `cJSON_PrintUnformatted` after sort is the v1 implementation.

On verification failure the receiver sends `{"type":"REJECT","reason":"auth"}` and closes the connection.

## Message types

All messages share this envelope:

```json
{
  "type":    "<msg type>",
  "from":    "<sender id>",
  "to":      "<receiver id or *>",
  "nonce":   "<hex>",
  "ts":      <unix seconds>,
  "payload": { ... },
  "auth":    "<hex>"
}
```

`sender id` = `MagNET-biologic-<MAC4>` for nodes, `MagNET-ruler-<MAC4>` for rulers. Both derived from the device's WiFi MAC so they are stable across reboots.

### HELLO (node → ruler)

```json
"payload": {
  "role_requested": "spawn",
  "chip":           "ESP32-C3",
  "fw":             "0.1.0",
  "hive":           "beehive-1",
  "caps":           ["led", "button"]
}
```

- `role_requested` — one of `spawn`, `worker`, `scribe`, `parrot`, `beeper`, `warrior`, `spy`, `pet`, `ml_phd`. First-time joiners SHOULD request `spawn`; rulers can promote via ROLE_GRANT (R10).
- `caps` — free-form capability tags the ruler can use to decide role assignments. `led`, `display`, `speaker`, `imu`, `thermometer`, etc. This is where the real-device-demo framing (fitness tracker, temp sensor) connects: each device reports its sensors as caps; a scribe role matches caps to hive needs.

### WELCOME (ruler → node)

```json
"payload": {
  "session_id": "<uuid-v4>",
  "role":       "spawn",
  "heartbeat":  30
}
```

- `session_id` — UUID the node includes in subsequent messages.
- `role` — initial role granted. In v1 this mirrors `role_requested`.
- `heartbeat` — the node must send a PING at least every N seconds or the ruler evicts it.

### REJECT (ruler → node, or either direction on auth failure)

```json
"payload": {
  "reason": "auth" | "hive_mismatch" | "full" | "ts_skew" | "replay"
}
```

### PING (node → ruler, ruler → node)

Keep-alive. Empty `payload`. Receiver responds with PING.

### ROLE_REQUEST (node → ruler)

Explicit re-role request. Payload mirrors HELLO.

### ROLE_GRANT (ruler → node)

```json
"payload": {
  "role":        "scribe",
  "bundle_url":  null,      // v2: URL or b64 blob to download role logic
  "bundle_sig":  null
}
```

v1 grants only change the role label. The actual Forth role bundle arrives in Milestone C.

## Session state machine (node side)

```
     ┌─────────┐   WiFi up     ┌───────────┐
     │ OFFLINE │──────────────►│ DISCOVER  │
     └─────────┘               └─────┬─────┘
                     mDNS hit         │
                                      ▼
                                ┌───────────┐  HMAC sent
                                │ CONNECTING│──────────┐
                                └───────────┘          │
              REJECT / timeout                          ▼
         ┌────────────────────────────────────────┐ ┌───────────┐
         │  BACKOFF (10s, cap 120s)               │◄│ WELCOME   │
         └────────────────────────────────────────┘ └─────┬─────┘
                                                          │
                                                          ▼
                                                    ┌───────────┐
                                                    │  JOINED   │
                                                    │ (+pings)  │
                                                    └───────────┘
```

The ruler keeps a simple table of `(session_id, node_id, role, last_seen)`; entries older than `3 × heartbeat` are pruned.

## Shared-secret distribution

For bringup the secret is compile-time constant (`CRAW_HIVE_DEV_SECRET` in `craw_hive.h`). In v1.1 we extend `craw_ble_provision` with an extra characteristic `hive_secret` (write-only, 32 bytes, stored in NVS under `craw_hive.secret`). The ruler receives it the same way, or holds it as a user-entered hex string via its display UI.

When the node connects to WiFi and finds no stored `hive_secret`, it defaults to `CRAW_HIVE_DEV_SECRET` and logs a warning. Production deployments must provision a real secret.

## Threat model (v1)

| Attacker | Mitigation |
|---|---|
| Random device on LAN sniffing traffic | Payloads readable by design. No secrets on the wire. |
| Random device impersonating a node | No shared secret → HMAC fails → REJECT. |
| Replay of captured HELLO | Nonce + timestamp window. |
| Ruler impersonation | All ruler responses also carry HMAC; a fake ruler cannot forge WELCOME. |
| Node with leaked secret | Out of scope. Rotating the secret requires re-provisioning all nodes (v2 adds rotation). |
| DoS (flood of HELLOs) | Ruler rate-limits per source IP: 10 HELLOs / minute / IP. |

## Real-device extension path

The protocol is intentionally role-generic. To demo a fitness tracker:
- Node advertises `caps: ["imu","hr","button"]`
- Requests role `pet` (the cute-companion role in the README)
- In Milestone C the ruler grants a Forth bundle that knows how to read IMU via `ffi:read-imu` and stream heartbeats back over a new `PUBLISH` message type.

A temperature sensor is `caps: ["temp","humidity"]` asking for `scribe` — same codepath. The hive protocol never knows what a thermometer is; it only knows how to route bundles and messages.

## Implementation notes

- **HMAC** via mbedTLS `mbedtls_md_hmac` (already in ESP-IDF).
- **mDNS** via `espressif/mdns` managed component.
- **JSON** via cJSON (already in ESP-IDF).
- **UUID** — simple 16-byte random via `esp_random`, hex-formatted; RFC 4122 compliance not required for v1.
- **TCP** via lwIP BSD sockets directly. Small surface area; avoids pulling in `esp_http_server` for no reason.

## Open questions for v2

1. Should we add an encrypted control channel once role bundles can contain executable code? Probably yes — sign payloads with Ed25519 and encrypt with AES-GCM using a session key derived at join.
2. Multicast ruler election when no ruler exists? The README says "any node can request nomination to Ruler" — v2 needs a small bully-algorithm or Raft-lite.
3. Shared memory / NDN layer (R16). Likely a separate protocol atop the same TCP session, multiplexed by message type.
