# MagNET Hanasu v2 - Design Proposal

## Status: DRAFT
## Date: 2026-03-27
## Target Platform: ESP-IDF (prototypes on Arduino)

---

## 1. Current State Analysis

### What Exists (v0.0.6)

The current prototype (`MagNET_Thread_COaP_hanasu_esp32c6.ino`) is a P2P chat system running on ESP32-C6 devices using OpenThread + CoAP over IEEE 802.15.4. Key characteristics:

- **Network formation**: Scans for an existing PAN on channel 24; if not found, becomes leader; otherwise joins as child
- **Messaging**: CoAP PUT to multicast address `ff05::abcd` for broadcast, unicast for DMs
- **Security**: Hardcoded network key (`00112233445566778899aabbccddeeff`) — link-layer AES-CCM only
- **Payload**: Hex-encoded text with `chat>` prefix, max ~256 bytes
- **Leader election**: First node becomes leader; no recovery if leader goes offline
- **Addressing**: 1-1 via `@IPv6` prefix, 1-many via multicast (currently broken for DMs)

### Known Gaps (from README)

| Gap | Impact |
|-----|--------|
| No application-layer encryption | Any device with the network key can read/spoof all traffic |
| Hardcoded network key & channel | No way for users to create private groups |
| No leader failover | Network degrades when leader disappears |
| DMs broken | Parser bug in `@IPv6` handling |
| No binary payload support | Only hex-encoded text strings |
| 256-byte payload limit | No fragmentation/reassembly |
| Arduino dependency | Blocks scalability testing and production use |

---

## 2. Requirements Summary

| # | Requirement | Priority |
|---|-------------|----------|
| R1 | Strong encryption (application-layer) | Must |
| R2 | Self-organizing nodes / zero-conf | Must |
| R3 | Private channel IDs — any node can specify, others can join | Must |
| R4 | Leader failover and network repair | Must |
| R5 | Free-form text chat payloads | Must |
| R6 | Proprietary M2M command protocol | Must |
| R7 | Binary payloads with eventual consistency | Must |
| R8 | 1-1 and 1-many communication modes | Must |
| R9 | UART host control channel — any node can be controlled by a host device over UART | Must |

---

## 3. Thread Network Fundamentals (Constraints)

Understanding these hard constraints is essential before evaluating approaches.

### 3.1 IEEE 802.15.4 Physical Limits

```
IEEE 802.15.4 Frame: 127 bytes max
├── MAC Header:        ~23 bytes (with security)
├── 6LoWPAN Header:    ~7-40 bytes (compressed IPv6 + UDP)
├── CoAP Header:       ~4-12 bytes (header + token + options)
├── App Payload:       ~50-70 bytes (best case, single frame)
└── MIC/FCS:           ~6 bytes
```

**Single-frame usable payload: ~50-70 bytes** (varies with header compression).

### 3.2 6LoWPAN Fragmentation

When a CoAP message exceeds 127 bytes, 6LoWPAN fragments it:

- **FRAG1 header**: 4 bytes (datagram size + tag)
- **FRAGN header**: 5 bytes (datagram size + tag + offset)
- **Max reassembly buffer**: 2048 bytes (implementation dependent; OpenThread default is 1280)
- **Fragment timeout**: Typically 60 seconds

Each fragment is an independent 802.15.4 transmission — more fragments = more collision probability on the radio channel.

### 3.3 Network Scale

| Parameter | Limit | Notes |
|-----------|-------|-------|
| Max routers | 32 | Routing table must fit in one 802.15.4 frame |
| Max children per router | 511 (spec) / 64 (OpenThread default) | RAM-limited |
| Theoretical max devices | ~16,384 | 32 x 511 |
| Practical target range | 32-250 | Before multicast storm and routing overhead dominate |
| Thread keeps active routers | 16-23 | Self-regulates to avoid overhead |

### 3.4 Thread Self-Healing (Built-in)

Thread already provides:
- **Automatic leader election** among routers (weighted by power source, stability, border router capability)
- **Leader failover**: If leader disappears, another router is elected within seconds
- **Route repair**: Neighboring nodes detect link failure and re-route
- **REED promotion**: Router-Eligible End Devices auto-promote to Router when needed

**Key insight**: R4 (leader failover) is largely solved by Thread itself. The current prototype's problem is that it hardcodes leader/child roles at startup and doesn't leverage Thread's native self-healing.

---

## 4. Design Approaches

### 4.1 Encryption (R1)

#### Approach A: DTLS 1.2 (CoAPS)

CoAP Secure uses DTLS to encrypt the transport between two endpoints.

| Aspect | Detail |
|--------|--------|
| Standard | RFC 6347 (DTLS 1.2) + RFC 7252 Section 9 |
| Cipher suites | `TLS_PSK_WITH_AES_128_CCM_8` (PSK), `TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8` (cert) |
| Overhead | ~25-100 bytes per record (handshake is 1-3KB) |
| Multicast | **Not supported** — DTLS is point-to-point only |
| RAM cost | ~8-15KB per session |
| ESP-IDF support | Yes, via mbedTLS (bundled) |

**Verdict**: Good for 1-1 encrypted channels. Cannot encrypt multicast traffic. Would need to establish separate DTLS sessions with every peer for group communication — does not scale.

#### Approach B: OSCORE (RFC 8613)

Object Security for Constrained RESTful Environments. Application-layer encryption using COSE.

| Aspect | Detail |
|--------|--------|
| Standard | RFC 8613 |
| Crypto | AEAD: AES-CCM-16-64-128 (default) |
| Overhead | ~15-25 bytes (Partial IV + kid + AEAD tag) |
| Multicast | **Yes** — via Group OSCORE (RFC 9203 / draft-ietf-core-oscore-groupcomm) |
| RAM cost | ~2-4KB per security context |
| ESP-IDF support | No native library; would need to port or implement |

**Verdict**: Ideal for constrained devices. Supports both unicast and multicast. Lower overhead than DTLS. But no off-the-shelf ESP-IDF library exists — requires implementation effort.

#### Approach C: Custom AES-CCM-128 at Application Layer

Use the same AES-CCM-128 primitive that Thread uses at the link layer, but applied to application payloads with a shared group key.

| Aspect | Detail |
|--------|--------|
| Standard | Custom (uses standard AES-CCM primitive) |
| Crypto | AES-128-CCM with 8-byte MIC |
| Overhead | ~12-16 bytes (4-byte nonce counter + 8-byte MIC + flags) |
| Multicast | **Yes** — all nodes sharing the key can decrypt |
| RAM cost | ~500 bytes per context |
| ESP-IDF support | Yes, `mbedtls_ccm_*` APIs available |

**Verdict**: Smallest overhead, easiest to implement on ESP-IDF, supports multicast. Trade-off: no standardized key exchange or context management — must design our own.

#### Recommendation: Hybrid C + A

1. **Group encryption (1-many)**: Custom AES-CCM-128 using a key derived from the channel passphrase (Approach C). All nodes on a channel share the same application-layer key.
2. **Private encryption (1-1)**: DTLS-PSK for sensitive unicast sessions where forward secrecy matters (Approach A). Optional — can fall back to AES-CCM with per-pair derived keys for lower overhead.
3. **Future path**: Migrate to OSCORE + Group OSCORE when ESP-IDF library support matures (Approach B).

**Key derivation**: Channel passphrase -> HKDF-SHA256 -> 128-bit AES key + 64-bit salt. This ties encryption to channel membership.

---

### 4.2 Private Channels / Zero-Conf (R2, R3)

#### The Problem

Currently, all nodes use a hardcoded network key and channel. We need:
- Any node can create/specify a "channel ID" (human-readable passphrase)
- Other nodes can join by entering the same channel ID
- No central coordinator required

#### Approach A: One Thread Network, Application-Layer Channels

All nodes join a single Thread network. Channels are implemented as **application-layer multicast groups** with encryption keys derived from the channel passphrase.

```
Channel ID: "team-alpha-2026"
        │
        ├── HKDF-SHA256 ──> AES-128 Key (for encrypting payloads)
        ├── Hash ──> Multicast Group Address (ff05::xxxx, derived from channel ID)
        └── Nodes subscribe to that multicast address via `otIp6SubscribeMulticastAddress()`
```

| Aspect | Detail |
|--------|--------|
| Network key | Shared across all nodes (or well-known default) |
| Channel isolation | Via app-layer encryption — nodes without the passphrase cannot decrypt |
| Multi-channel | A node can join multiple channels simultaneously |
| Zero-conf | Yes — just enter passphrase, derive key + multicast addr, subscribe |
| Limitation | Multicast traffic visible (though encrypted) to all Thread nodes on the network |

#### Approach B: Separate Thread Networks Per Channel

Each channel ID maps to a unique Thread network with its own network key.

```
Channel ID: "team-alpha-2026"
        │
        ├── HKDF-SHA256 ──> Thread Network Key
        ├── Hash ──> PAN ID
        └── Hash ──> Channel Number (11-26)
```

| Aspect | Detail |
|--------|--------|
| Network key | Unique per channel, derived from passphrase |
| Channel isolation | At the link layer — only members can even see decrypted frames |
| Multi-channel | **Not possible** — a device can only be on one Thread network at a time |
| Zero-conf | Yes — derive network params from passphrase |
| Limitation | Cannot bridge channels; radio channel collision possible |

#### Approach C: Hybrid — Default Network + On-Demand Private Networks

- A well-known "discovery" Thread network exists (default key, default channel) for node discovery and coordination
- When a private channel is requested, nodes negotiate and form a separate Thread network
- Nodes can switch between discovery network and private network

| Aspect | Detail |
|--------|--------|
| Discovery | Always available on known network |
| Privacy | Strong — private network has unique key |
| Multi-channel | Requires network switching (seconds of downtime) |
| Complexity | High — managing multiple network contexts |

#### Recommendation: Approach A (Application-Layer Channels)

Rationale:
- Simplest to implement and operate
- Supports multi-channel membership (a node can monitor multiple channels)
- Zero-conf: passphrase alone derives everything needed
- Application-layer encryption provides adequate privacy for the use cases
- Thread's link-layer encryption still protects against external (off-network) attackers
- **Only limitation**: nodes on the same Thread network can see encrypted multicast traffic for channels they haven't joined — but cannot decrypt it

**Channel derivation scheme**:

```
Input:    channel_passphrase (UTF-8 string, 4-64 chars)
Salt:     "MagNET-Hanasu-v2" (fixed, 16 bytes)

channel_key    = HKDF-SHA256(passphrase, salt, "channel-key", 16)   // AES-128 key
channel_mcast  = ff05:: + first_4_bytes(SHA256(passphrase))          // Multicast addr
channel_id_short = first_8_bytes(SHA256(passphrase)) in hex          // Display ID
```

---

### 4.3 Leader Failover / Network Recovery (R4)

#### Current Problem

The prototype decides leader vs. child role at startup based on a scan. If the leader goes offline, children do not re-elect.

#### Solution: Let Thread Handle It

Thread's native behavior already solves this:

1. **Remove hardcoded role assignment**. All nodes should start as Router-Eligible End Devices (REEDs).
2. **Thread automatically**: promotes REEDs to Routers when needed, elects a Leader from among Routers, re-elects if Leader disappears.
3. **Application layer**: should not track or depend on `isLeader`. Any node can send/receive regardless of Thread role.

**Implementation changes**:
- Remove `isLeader` flag and leader-specific behavior
- All nodes run identical code path (both CoAP server + client)
- Use `dataset init new` only on first-ever network creation; thereafter nodes join existing
- Monitor `otThreadGetDeviceRole()` for diagnostic display only (LED color)
- Set `routereligible enable` on all nodes

**Network partition recovery**:
- If a partition occurs (e.g., leader isolated), Thread will form two partitions with independent leaders
- When partitions merge, Thread's Merge protocol automatically reconciles (lower partition ID wins)
- Application layer should handle duplicate message detection (see R7)

---

### 4.4 Message Format and Protocol (R5, R6, R7, R8)

#### Proposed Unified Message Format

All communication (chat, M2M commands, binary data) uses a single envelope format carried as a CoAP payload. We use a compact binary format to maximize usable payload within the 802.15.4 constraints.

```
MagNET Message Envelope (Binary)
┌──────────────────────────────────────────────────────────┐
│ Byte 0:    Version + Type (4 bits each)                  │
│ Byte 1:    Flags                                         │
│ Bytes 2-3: Sequence Number (uint16, big-endian)          │
│ Bytes 4-7: Message ID (uint32, random)                   │
│ Byte 8:    Channel Hash (first byte of channel key hash) │
│ Byte 9:    Fragment Info (total_frags:4 | frag_idx:4)    │
│ Bytes 10-N: Payload (encrypted if channel key set)       │
│ Bytes N+1 to N+8: AES-CCM MIC (if encrypted)            │
└──────────────────────────────────────────────────────────┘
Total envelope overhead: 10 bytes (+ 8 bytes MIC if encrypted = 18 bytes)
```

**Field definitions**:

| Field | Size | Description |
|-------|------|-------------|
| Version | 4 bits | Protocol version (currently 1) |
| Type | 4 bits | 0=chat, 1=m2m_cmd, 2=m2m_resp, 3=binary_xfer, 4=ack, 5=ping, 6-15=reserved |
| Flags | 8 bits | Bit 0: encrypted, Bit 1: requires_ack, Bit 2: is_fragment, Bit 3: is_final_fragment, Bits 4-7: reserved |
| Sequence Number | 16 bits | Per-sender monotonic counter for ordering and dedup |
| Message ID | 32 bits | Random ID; fragments of same message share this ID |
| Channel Hash | 8 bits | Quick filter — discard before attempting decryption if wrong channel |
| Fragment Info | 8 bits | High nibble: total fragments (1-15), Low nibble: fragment index (0-14) |
| Payload | Variable | Content (plaintext or encrypted) |
| MIC | 8 bytes | AES-CCM Message Integrity Code (present when encrypted flag set) |

#### Payload Size Budget

With the envelope format above, within a single 802.15.4 frame:

```
IEEE 802.15.4 frame:             127 bytes
- MAC header + security:          ~23 bytes
- 6LoWPAN compressed IPv6+UDP:    ~12 bytes  (best case IPHC)
- CoAP header + token + options:  ~12 bytes  (PUT, 4-byte token, Uri-Path)
= Available for app payload:      ~80 bytes
- MagNET envelope header:         10 bytes
- AES-CCM MIC:                     8 bytes
= Usable payload per frame:      ~62 bytes (single frame, no 6LoWPAN fragmentation)
```

With 6LoWPAN fragmentation (up to OpenThread's 1280-byte reassembly buffer):

```
Max CoAP payload after 6LoWPAN reassembly:  ~1200 bytes
- MagNET envelope header:                      10 bytes
- AES-CCM MIC:                                  8 bytes
= Max single-message payload:               ~1182 bytes
```

With application-layer fragmentation (15 fragments max):

```
Max reassembled payload:  15 x ~62 bytes = ~930 bytes (single-frame fragments)
                      or: 15 x ~1182 bytes = ~17,730 bytes (fragmented fragments)
```

**Recommended operating modes**:

| Mode | Max Payload | Fragmentation | Use Case |
|------|-------------|---------------|----------|
| Compact | 62 bytes | None | Short chat, M2M commands |
| Standard | ~1182 bytes | 6LoWPAN only | Longer text, small binary |
| Extended | ~17KB | App-layer + 6LoWPAN | Binary transfer, firmware |

#### Message Types Detail

**Type 0: Chat (R5)**
```
Payload: UTF-8 text, free-form
Example: "Hello everyone!" (15 bytes)
Delivery: CoAP NON-confirmable PUT to multicast (1-many) or unicast (1-1)
```

**Type 1: M2M Command (R6)**
```
Payload structure:
  Byte 0:     Command namespace (0=system, 1=lighting, 2=sensor, 3-255=user-defined)
  Byte 1:     Command ID (within namespace)
  Bytes 2-3:  Parameter length
  Bytes 4+:   Command parameters (namespace-specific encoding)

Example - Lighting control:
  Namespace: 0x01 (lighting)
  Command:   0x02 (set_color)
  Params:    [R, G, B] = [0xFF, 0x00, 0x80]
  Total: 7 bytes

Delivery: CoAP CON (confirmable) PUT for commands requiring acknowledgment
```

**Type 2: M2M Response (R6)**
```
Payload: Response to a command (same structure as command, with status byte prepended)
  Byte 0:     Status (0=ok, 1=error, 2=busy, 3=unsupported)
  Bytes 1+:   Response data
```

**Type 3: Binary Transfer (R7)**
```
Payload: Raw binary data
Uses app-layer fragmentation (Fragment Info field)
Each fragment is independently transmitted and can arrive out of order
Receiver reassembles using Message ID + Fragment Index

Eventual consistency mechanism:
  - Sender retransmits fragments not ACKed within timeout
  - Receiver tracks received fragments via bitmask
  - Receiver requests missing fragments via Type 4 (ACK) with NACK bitmask
  - Transfer completes when all fragments received
  - Stale/duplicate fragments detected via Message ID + Sequence Number
```

**Type 4: ACK / NACK**
```
Payload:
  Bytes 0-3:  Message ID being acknowledged
  Byte 4:     ACK type (0=full_ack, 1=partial_nack)
  Bytes 5-6:  Fragment bitmask (which fragments received, for NACK)
```

#### 1-1 vs 1-Many (R8)

| Mode | CoAP Target | Confirmation | Notes |
|------|-------------|--------------|-------|
| 1-many (broadcast) | `ff05::<channel_mcast>` | NON-confirmable | All channel members receive |
| 1-1 (direct) | Peer's mesh-local IPv6 | CON-confirmable | Only target receives |
| 1-many (selective) | `ff05::<channel_mcast>` | NON | Encrypted for specific sub-group key |

---

### 4.5 CoAP Resource Design

Replace the current dual-resource (`Lamp` + `chat`) approach with a unified resource:

```
CoAP Resources:
  /magnet         PUT: Send a MagNET message (any type)
                  GET: Retrieve node status / capabilities

  /magnet/discover  GET: Node discovery (returns EUI64, channels, capabilities)
```

All message routing is handled by the MagNET envelope — the CoAP resource just receives the binary payload and dispatches by type.

---

### 4.6 UART Host Control Channel (R9)

Any MagNET node can optionally be controlled by an external host device over UART. This enables scenarios where a more capable device (phone, laptop, Raspberry Pi, cyberdeck) uses a MagNET node as a radio modem to participate in the mesh.

#### Design

The UART interface acts as a **transparent bridge** between the host and the MagNET protocol. The host sends commands; the node executes them on the mesh and relays incoming messages back to the host.

#### Node Lifecycle and Readiness

UART becomes active early in boot (as soon as `Serial.begin()` / `uart_driver_install()` runs), but MagNET services take time to initialize — Thread attach alone can take 5-120 seconds. The host must know when the node is ready to accept commands, and which commands are valid at each stage.

**Lifecycle states**:

```mermaid
stateDiagram-v2
    [*] --> BOOTING : Power On

    state BOOTING {
        direction LR
        note right of BOOTING
            UART active.
            Accepts: STATUS, CHANNEL SET
            Emits: +STATE BOOTING &lt;ver&gt; &lt;eui64&gt;
        end note
    }

    state CONFIGURING {
        direction LR
        note right of CONFIGURING
            NVS read complete.
            Accepts: STATUS, CHANNEL SET/JOIN/LEAVE
            Emits: +STATE CONFIGURING
            If NVS has channel: immediate transition
            If not: wait up to 10s for UART input
        end note
    }

    state ATTACHING {
        direction LR
        note right of ATTACHING
            Thread stack started, joining/forming network.
            Accepts: STATUS
            Mesh commands return +ERR ATTACHING
            Emits: +STATE ATTACHING &lt;channel_id&gt;
        end note
    }

    state READY {
        direction LR
        note right of READY
            All services operational.
            All commands accepted.
            CoAP resources registered.
            Emits: +STATE READY &lt;role&gt; &lt;channel_id&gt;
            Heartbeat active.
        end note
    }

    state DEGRADED {
        direction LR
        note right of DEGRADED
            Thread detached or partitioned.
            Attempting re-attach.
            Accepts: STATUS, CHANNEL commands
            Mesh commands queued (max 4)
            Emits: +STATE DEGRADED &lt;reason&gt;
            Heartbeat active.
        end note
    }

    BOOTING --> CONFIGURING : Hardware init complete\n(GPIO, NVS read)
    CONFIGURING --> ATTACHING : Channel resolved\n(NVS / UART / default timeout)
    ATTACHING --> READY : Thread role assigned\n(child / router / leader)
    ATTACHING --> ATTACHING : Retry after 120s timeout
    READY --> DEGRADED : Thread detached /\nnetwork partitioned
    DEGRADED --> READY : Re-attached to mesh
    DEGRADED --> ATTACHING : CHANNEL SET received\n(restart with new channel)
    READY --> ATTACHING : CHANNEL SET received\n(switch channel)
    CONFIGURING --> CONFIGURING : CHANNEL SET received\n(update before attach)
```

**State descriptions**:

| State | Accepted Commands | Emitted Event |
|-------|-------------------|---------------|
| **BOOTING** | `STATUS`, `CHANNEL SET` | `+STATE BOOTING <ver> <eui64>` |
| **CONFIGURING** | `STATUS`, `CHANNEL SET/JOIN/LEAVE` | `+STATE CONFIGURING` |
| **ATTACHING** | `STATUS` | `+STATE ATTACHING <channel_id>` |
| **READY** | All commands | `+STATE READY <role> <channel_id>` |
| **DEGRADED** | `STATUS`, `CHANNEL *`, mesh commands queued (max 4) | `+STATE DEGRADED <reason>` |

**State events emitted automatically** — the host never needs to poll:

| Event | When | Example |
|-------|------|---------|
| `+STATE BOOTING <ver> <eui64>` | Immediately on UART init | `+STATE BOOTING 2.0.0 0011223344556677` |
| `+STATE CONFIGURING` | Hardware init done, waiting for channel | `+STATE CONFIGURING` |
| `+STATE ATTACHING <chan_id>` | Thread stack starting | `+STATE ATTACHING a1b2c3d4` |
| `+STATE READY <role> <chan_id>` | Mesh joined, all services up | `+STATE READY router a1b2c3d4` |
| `+STATE DEGRADED <reason>` | Lost mesh connectivity | `+STATE DEGRADED detached` |

**Heartbeat**: Once in READY or DEGRADED state, the node emits a periodic heartbeat so the host can detect a hung or crashed node:

```
+HEARTBEAT <state> <uptime_secs> <role> <peer_count>
```

Default interval: every 30 seconds. Configurable via UART command `HEARTBEAT <interval_secs>` (0 to disable).

**Command validity by state**:

| Command | BOOTING | CONFIGURING | ATTACHING | READY | DEGRADED |
|---------|---------|-------------|-----------|-------|----------|
| STATUS | Yes | Yes | Yes | Yes | Yes |
| CHANNEL SET | Yes | Yes | No* | Yes | Yes |
| CHANNEL JOIN/LEAVE/LIST | No | Yes | No | Yes | Yes |
| CHAT / DM | No | No | No | Yes | Queued** |
| CMD | No | No | No | Yes | Queued** |
| XFER | No | No | No | Yes | No |
| PEERS | No | No | No | Yes | Yes |
| PING | No | No | No | Yes | No |
| RAW | No | No | No | Yes | No |
| HEARTBEAT | No | Yes | Yes | Yes | Yes |

\* CHANNEL SET during ATTACHING triggers a restart of the attach process with the new channel.
\** Queued commands are held (up to 4) and sent when state returns to READY. If queue is full, returns `+ERR QUEUE_FULL`.

**Host startup handshake** (recommended pattern):

```
1. Host opens UART
2. Host sends: STATUS\n
3. If no response within 2s, node may still be in hardware init — retry
4. Node responds: +STATE <current_state> ...
5. If state is BOOTING or CONFIGURING:
     Host can send CHANNEL SET if needed
     Host waits for +STATE READY
6. If state is READY:
     Host proceeds with normal operation
7. If state is DEGRADED:
     Host waits for +STATE READY, or issues CHANNEL SET to force re-attach
```

#### UART Protocol

Text-based for ease of debugging, parseable by any language:

```
Host -> Node (commands):
  STATUS                               Query node state, always valid
  HEARTBEAT <interval_secs>            Set heartbeat interval (0=disable)
  CHAT <message>                       Send multicast chat
  DM <ipv6_addr> <message>             Send unicast chat
  CMD <namespace> <cmd_id> <params>    Send M2M command (multicast)
  CMD <ipv6_addr> <ns> <cmd_id> <p>    Send M2M command (unicast)
  XFER <dest> <base64_data>            Initiate binary transfer
  CHANNEL JOIN <passphrase>            Join a channel
  CHANNEL LEAVE <passphrase>           Leave a channel
  CHANNEL LIST                         List active channels
  CHANNEL SET <passphrase>             Set default channel + persist to NVS/EEPROM
  PEERS                                List known peers on current channel(s)
  PING <ipv6_addr>                     Ping a specific node
  RAW <hex_envelope>                   Send a raw MagNET envelope (advanced)

Node -> Host (events):
  +STATE <state> [<details>...]        Lifecycle state change (see above)
  +HEARTBEAT <state> <uptime> <role> <peers>  Periodic aliveness
  +CHAT <channel_hash> <sender_eui64> <sender_ipv6> <message>
  +DM <sender_eui64> <sender_ipv6> <message>
  +CMD <channel_hash> <sender_eui64> <ns> <cmd_id> <params>
  +XFER <sender_eui64> <msg_id> <frag_idx>/<total> <base64_chunk>
  +XFER_DONE <msg_id> <base64_full>
  +ROLE <role_name>                    Role changed (leader/router/child/detached)
  +PEER_JOIN <eui64> <ipv6>            New peer discovered
  +PEER_LEAVE <eui64>                  Peer timed out
  +ERR <error_message>                 Error condition
  +OK                                  Command acknowledged
```

**UART parameters**: 115200 baud, 8N1 (matching current prototype). Lines terminated with `\n`.

**Security**: The UART channel itself is **out of scope** for security. The physical UART connection is assumed trusted. The host is responsible for securing its own side (e.g., if exposing the UART over USB to a multi-user OS).

**Implementation notes**:
- The existing Serial-based command parsing in `loop()` is the seed for this — it already reads from UART and sends CoAP
- Formalize the command/event protocol above to replace the ad-hoc `chat>` prefix parsing
- The `+STATE BOOTING` event should be the very first thing emitted after `Serial.begin()` — before any other init — so the host knows the node is alive even if Thread takes 2 minutes to attach
- Host libraries (Python, Node.js, Rust) can wrap the UART text protocol into higher-level APIs
- The node's autonomous behavior (LED, button) continues to work independently of UART — UART is additive

---

### 4.7 Edge Routing

#### The Problem

A pure Thread mesh has no internet connectivity. To bridge MagNET traffic to IP networks (Wi-Fi, Ethernet, cloud), an edge router is needed. The ESP32-C6 has both 802.15.4 and Wi-Fi radios, but using both simultaneously is constrained by shared RF front-end and driver limitations.

#### Option A: ESP32-C6 as Native Border Router

Use the ESP32-C6's dual radio capability (802.15.4 + Wi-Fi) to run an OpenThread Border Router (OTBR) natively.

| Aspect | Detail |
|--------|--------|
| Hardware | Single ESP32-C6 |
| Software | ESP-IDF `esp_openthread` + `esp_wifi` + OTBR components |
| Capability | Full Thread Border Router (NAT64, DNS-SD, multicast forwarding) |
| Limitation | Shared RF path — Wi-Fi and 802.15.4 on same antenna, potential interference |
| Limitation | RAM-constrained for full OTBR stack (~320KB free after Thread + Wi-Fi) |
| Maturity | Espressif has OTBR examples for ESP32-C6, but marked experimental |
| Best for | Small deployments, prototyping, single-room setups |

#### Option B: Wi-Fi Device + UART-Connected Thread Node

A more capable Wi-Fi device (Raspberry Pi, ESP32-S3, laptop, phone) connects to a MagNET node over UART and bridges traffic to IP.

```
┌─────────────┐    UART/USB    ┌──────────────┐    802.15.4    ┌──────────┐
│  Host Device │◄─────────────►│ MagNET Node  │◄──────────────►│  Mesh    │
│  (Wi-Fi/ETH) │               │  (ESP32-C6)  │               │  Network │
│              │               │              │               │          │
│  - Bridge SW │               │  - R9 UART   │               │  - Nodes │
│  - Cloud API │               │    protocol  │               │          │
│  - Web UI    │               │  - Full mesh │               │          │
└─────────────┘               └──────────────┘               └──────────┘
```

| Aspect | Detail |
|--------|--------|
| Hardware | Any Wi-Fi/Ethernet device + any MagNET-capable 802.15.4 node |
| Software | Host runs bridge software using R9 UART protocol; node runs standard MagNET firmware |
| Capability | Full bridge — forward MagNET messages to MQTT, HTTP, WebSocket, etc. |
| Limitation | Two devices needed; UART bandwidth caps throughput (~11.5 KB/s at 115200 baud) |
| Advantage | No RF interference — dedicated radios for each network |
| Advantage | Host can be any platform (Pi, laptop, phone via BLE-UART adapter) |
| Advantage | MagNET node runs standard firmware — no special border router build |
| Best for | Production deployments, flexible integration, cyberdeck use case |

#### Option C: Dedicated Thread Border Router (Non-ESP32)

Use a commercial or open-source Thread Border Router (e.g., Raspberry Pi + Nordic nRF52840 dongle running OTBR, or Apple HomePod/Google Nest as Thread BR).

| Aspect | Detail |
|--------|--------|
| Hardware | Raspberry Pi + nRF52840 USB dongle, or commercial Matter/Thread BR |
| Software | OpenThread Border Router (ot-br-posix) |
| Capability | Full OTBR with IPv6 routing, NAT64, DNS-SD, mDNS |
| Limitation | Does not understand MagNET application protocol — only provides IPv6 connectivity |
| Limitation | MagNET nodes are reachable via IPv6, but app-layer encryption means BR can't inspect payloads |
| Best for | Interoperability with Matter/HomeKit ecosystem; pure IPv6 routing without app-layer bridging |

#### Recommendation

**Option B (UART bridge) as the primary approach**, with Option A available for prototyping.

Rationale:
- Option B leverages R9 (UART host control) which is already a requirement — the edge router is just a host device running bridge software
- No special firmware variant needed — every MagNET node can be a bridge endpoint
- Decouples the Thread radio from the IP network radio, avoiding RF coexistence issues
- The PONY Cyberdeck use case naturally fits this model (cyberdeck is the host, ESP32-C6 is the radio)
- UART throughput (11.5 KB/s) exceeds the practical Thread mesh throughput (~10 KB/s effective) so is not a bottleneck

**Bridge software responsibilities** (runs on host):
- Parse R9 UART events (`+CHAT`, `+CMD`, etc.)
- Forward to upstream (MQTT, WebSocket, HTTP POST, local DB)
- Accept downstream commands and translate to UART commands
- Handle reconnection if UART drops (USB unplug/replug)
- Optionally expose a local web UI for monitoring

---

### 4.8 Channel Selection and Persistence

#### The Problem

The current prototype hardcodes `OT_CHANNEL "24"` and `OT_NETWORK_KEY` at compile time. We need a runtime mechanism for channel selection that:
- Persists across reboots
- Can be set by a host over UART
- Can be set by a MagNET protocol command from a peer
- Has a sensible default if nothing is configured

#### Storage: NVS (ESP-IDF) / EEPROM (Arduino)

On ESP-IDF, the Non-Volatile Storage (NVS) library provides key-value persistence in flash. On Arduino, the `Preferences` library (ESP32) or raw EEPROM emulation serves the same purpose.

**Stored parameters**:

| Key | Type | Size | Description |
|-----|------|------|-------------|
| `mn_chan_pass` | string | 4-64 bytes | Channel passphrase (from which all params are derived) |
| `mn_chan_set` | uint8 | 1 byte | Flag: 0=not configured, 1=configured |
| `mn_boot_cnt` | uint32 | 4 bytes | Boot counter (diagnostic) |

From the stored passphrase, the node derives at runtime:
- Thread network key (HKDF)
- Multicast group address
- AES-CCM application key
- Display channel ID

#### Boot Sequence with Channel Selection

```
POWER ON
    │
    ▼
┌──────────────────────┐
│ Read NVS/EEPROM      │
│ mn_chan_set ?         │
└──────┬───────────────┘
       │
       ├── mn_chan_set == 1 ──────────────────────────────────┐
       │   Channel passphrase found.                          │
       │   Derive network params. Start Thread immediately.   │
       │                                                      ▼
       │                                              ┌──────────────┐
       │                                              │ JOIN NETWORK │
       │                                              └──────────────┘
       │
       └── mn_chan_set == 0 (or NVS empty) ──────────┐
           No channel configured.                     │
           Enter WAIT mode.                           ▼
                                              ┌──────────────────────┐
                                              │ WAIT 10 SECONDS      │
                                              │ LED: slow amber pulse │
                                              │ Listen on UART for:   │
                                              │   CHANNEL SET <pass>  │
                                              └──────┬───────────────┘
                                                     │
                                    ┌────────────────┼────────────────┐
                                    │                │                │
                              UART command      10s timeout      Button press
                              received          (no input)       (if available)
                                    │                │                │
                                    ▼                ▼                ▼
                              ┌──────────┐   ┌──────────────┐  ┌──────────────┐
                              │ Save to  │   │ Use default  │  │ Use default  │
                              │ NVS,     │   │ passphrase:  │  │ (same as     │
                              │ derive,  │   │ "magnet"     │  │  timeout)    │
                              │ join     │   │ Derive, join │  │              │
                              └──────────┘   └──────────────┘  └──────────────┘
```

#### Default Channel

If no channel is configured and no UART input is received within 10 seconds of boot:

- **Default passphrase**: `"magnet"` (well-known, deterministic)
- **Derived network key**: `HKDF-SHA256("magnet", "MagNET-Hanasu-v2", "network-key", 16)`
- **Derived multicast addr**: `ff05::` + first 4 bytes of `SHA256("magnet")`

This means any unconfigured MagNET node will join the same default network — useful for out-of-box demos and development, but not secure. Users are expected to set a private passphrase for any real deployment.

#### Changing Channels

Channels can be changed via three mechanisms:

**1. UART command (from host)**:
```
CHANNEL SET <new_passphrase>
```
- Saves passphrase to NVS/EEPROM
- Responds `+OK`
- Node restarts Thread with new derived parameters
- Takes effect immediately (no reboot required, but Thread re-attach takes ~5-30 seconds)

**2. MagNET protocol command (from peer)**:
```
Type 1 (M2M Command), Namespace 0x00 (system), Command 0x01 (set_channel)
Payload: UTF-8 passphrase for new channel
```
- Received over the mesh from another node
- Saves to NVS/EEPROM
- Schedules a channel switch after a configurable delay (default: 5 seconds, to allow ACK)
- This enables a "fleet migration" scenario where an admin node tells all peers to move to a new channel

**3. Compile-time default** (development only):
```c
#define MAGNET_DEFAULT_CHANNEL_PASS "magnet"
```
- Used only if NVS is empty and no UART input within 10 seconds
- Can be overridden per-build for specific deployments

#### EEPROM Layout (Arduino Prototype)

For Phase 0 / Phase 1 on Arduino where `Preferences` may not be available:

```
EEPROM Address Map (64 bytes total):
  0x00:       Magic byte (0xMN = 0x4D4E) — indicates valid config
  0x02:       Config version (uint8, currently 1)
  0x03:       Channel passphrase length (uint8, 4-64)
  0x04-0x43:  Channel passphrase (up to 64 bytes, null-terminated)
```

On ESP-IDF, use `nvs_get_str()` / `nvs_set_str()` with namespace `"magnet"` and key `"chan_pass"`.

---

### 4.9 Simultaneous Message Handling and Node Load Limits

#### The Problem

The current prototype processes messages synchronously in a single `loop()` iteration: `checkUserButton()` -> `otCOAPListen()` -> `Serial.available()`. The OpenThread CLI interface is a serial stream — if multiple CoAP messages arrive while the node is busy (e.g., during the LED ramp animation which takes ~145ms with `delay(5)` x 29 steps), incoming CLI data accumulates in the UART receive buffer and may be truncated or lost.

#### Current Architecture Bottlenecks

**1. Blocking LED animations**: The lamp on/off ramp uses `delay(5)` in a loop (~145ms total). During this time, `otCOAPListen()` is not called, so incoming CoAP notifications queue in the OpenThread CLI serial buffer.

**2. Single-message-per-loop**: `otCOAPListen()` reads exactly one line from the OpenThread CLI per call via `readBytesUntil('\n', ...)`. If multiple messages arrive between loop iterations, they queue — but the 256-byte `cliResp` buffer means long messages can be truncated.

**3. `otExecCommandMulti()` blocks up to 2 seconds**: When sending a command, the node blocks in a while-loop waiting for "Done". Any incoming messages during this window are buffered but not processed until the next `otCOAPListen()` call.

**4. Arduino Serial buffer**: The default Arduino serial RX buffer is 256 bytes (configurable via `SERIAL_BUFFER_SIZE`). The OpenThread CLI produces verbose output — a single incoming CoAP PUT notification is ~120-180 bytes. Two concurrent messages can overflow the buffer.

#### What Happens Under Load

| Incoming rate | Behavior |
|---------------|----------|
| 1 msg / sec | Works fine. Each loop iteration processes one message. |
| 2-5 msg / sec | Messages queue in serial buffer. Slight delay in processing but generally OK if messages are short. |
| 5-10 msg / sec | Serial buffer overflow likely. Messages truncated or lost. Partial lines cause parse failures in `otCOAPListen()`. |
| 10+ msg / sec | Node becomes unresponsive. Buffer constantly overflows. Button presses may not register due to `loop()` being starved by CLI reads. |

#### Theoretical Single-Node Throughput

Working backwards from the constraints:

```
OpenThread CLI baud rate to internal stack: not UART-limited (in-process)
Arduino loop() cycle time (idle):           ~10ms (delay(10) at end of loop)
Arduino loop() cycle time (processing msg): ~20-50ms (parse + LED flash)
Arduino loop() cycle time (LED ramp):       ~155ms (blocking animation)

Messages processable per second (idle):     ~50-100
Messages processable per second (with LED): ~6-20
Messages processable per second (ramp):     ~6 (blocked during animation)
```

**Practical max incoming rate before degradation: ~10 messages/second** with current code. This is the per-node receive limit, not the network-wide limit.

For a network of N nodes all chatting at 1 msg/sec multicast, each node receives N-1 messages/sec. This means the **current code can handle ~10 actively chatting nodes** before message loss occurs.

#### Mitigations (In Priority Order)

**1. Non-blocking LED feedback** (fixes the biggest bottleneck):
Replace the blocking `delay(5)` ramp with a state-machine approach that updates one LED step per `loop()` iteration:

```cpp
// Instead of:
for (int16_t c = 16; c < 248; c += 8) {
    pixels.setPixelColor(0, pixels.Color(c, c, c));
    pixels.show();
    delay(5);  // BLOCKS - can't receive messages
}

// Use:
static int16_t rampValue = -1;  // -1 = not ramping
if (rampValue >= 0) {
    pixels.setPixelColor(0, pixels.Color(rampValue, rampValue, rampValue));
    pixels.show();
    rampValue += rampDirection * 8;
    if (rampValue > 248 || rampValue < 16) rampValue = -1;  // done
}
```

This frees ~145ms per lamp event for message processing.

**2. Drain the CLI buffer** — process all available messages per loop, not just one:

```cpp
void otCOAPListen() {
    while (OThreadCLI.available()) {  // Process ALL pending messages
        char cliResp[256] = {0};
        size_t len = OThreadCLI.readBytesUntil('\n', cliResp, sizeof(cliResp));
        // ... process message ...
    }
}
```

**3. Increase serial buffer size** (compile-time):

```cpp
// In platformio.ini or build flags:
-DSERIAL_BUFFER_SIZE=1024
```

This gives headroom for ~5-8 queued CoAP notifications.

**4. Remove `delay(10)` from end of `loop()`**:
The 10ms idle delay at the end of `loop()` is unnecessary and limits throughput. Replace with `yield()` or remove entirely. The ESP32 RTOS will yield automatically.

**5. Rate-limit outgoing multicast** (prevents self-inflicted storms):
Add a minimum interval between outgoing multicast sends (e.g., 100ms). This doesn't fix incoming floods but prevents one chatty node from overwhelming others.

#### Load Limits After Mitigations

| Mitigation applied | Max incoming msgs/sec | Max actively chatting nodes |
|--------------------|----------------------|----------------------------|
| Current code (no fixes) | ~10 | ~10 |
| + Non-blocking LED | ~30 | ~30 |
| + Drain buffer loop | ~50 | ~50 |
| + Larger serial buffer | ~50 (higher burst tolerance) | ~50 |
| + Remove delay(10) | ~80-100 | ~80-100 (theoretical) |
| ESP-IDF native (Phase 1) | ~200+ | ~200+ (dedicated task, no CLI overhead) |

The Arduino CLI-based architecture is inherently limited by string parsing overhead. The move to ESP-IDF with direct OpenThread C API calls (Phase 1) eliminates the CLI serial bottleneck entirely — messages arrive as callbacks, not as text to parse.

#### Collision Handling

When two nodes press their buttons simultaneously (or near-simultaneously), both send a multicast lamp command. Each node receives the other's command but not its own (multicast doesn't loop back). This creates a potential state divergence:

```
Node A presses: sends "1" (ON),  receives B's "0" (OFF) → lamp ends OFF
Node B presses: sends "0" (OFF), receives A's "1" (ON)  → lamp ends ON
```

This is inherent to any system without distributed consensus. For lamp toggling it's cosmetic — the next button press re-synchronizes. For the v2 protocol (Section 4.4), the sequence number and message ordering in the envelope provide a deterministic tiebreaker.

---

## 5. Approach Comparison Matrix

| Requirement | Approach A: Minimal (patch current) | Approach B: App-Layer Channels (recommended) | Approach C: Full OSCORE Stack |
|---|---|---|---|
| R1 Encryption | Custom AES-CCM only | Custom AES-CCM (group) + DTLS-PSK (1-1) | OSCORE + Group OSCORE |
| R2 Zero-conf | Thread scan + hardcoded key | Passphrase-derived everything | Same as B |
| R3 Private channels | Single channel only | Multi-channel via multicast groups | Same as B |
| R4 Leader failover | Remove hardcoded roles | Same + partition merge handling | Same |
| R5 Chat | Fix existing | New binary envelope | Same |
| R6 M2M protocol | Ad-hoc | Structured command namespace | Same |
| R7 Binary + EC | Not supported | App-layer fragmentation + retry | Block-wise transfer (RFC 7959) |
| R8 1-1 / 1-M | Fix DM parsing | Unified via envelope + addressing | Same |
| R9 UART host control | Ad-hoc serial parsing | Formalized text protocol | Same |
| Impl effort | 1-2 weeks | 5-7 weeks | 9-13 weeks |
| Standards compliance | Low | Medium | High |
| ESP-IDF readiness | Arduino only | ESP-IDF native | Needs OSCORE port |

**Recommendation: Approach B** — best balance of capability, implementation effort, and the constraints of the target hardware. Approach C is the long-term ideal but blocked by OSCORE library availability on ESP-IDF.

---

## 6. Implementation Phases

### Phase 0: Leader Election Fix + Failover (Arduino Prototype)

Solve the leader election and failover problem in the current Arduino codebase before migrating to ESP-IDF. This de-risks the most critical networking issue early with the fastest iteration loop.

**Goal**: Any node can become leader. If the leader disappears, the network self-heals without manual intervention.

**Changes to current prototype**:

1. **Eliminate the leader/child code split**. Currently `setupLeaderNode()` and `setupChildNode()` run different OpenThread command sequences and set different CoAP resources. Unify into a single `setupNode()` path:
   - All nodes execute the same dataset commands (channel, network key, commit, ifconfig up, thread start)
   - All nodes register both the `Lamp` and `chat` CoAP resources (server + client on every node)
   - Remove the `isLeader` boolean and all code branches that depend on it
   - LED color becomes purely diagnostic: read `otGetDeviceRole()` in `loop()` and set green=leader, blue=router, cyan=child, red=detached

2. **Fix the scan-based role decision**. The current logic scans twice and decides leader vs. child based on whether the target channel is found. Replace with:
   - On boot: configure dataset with known channel + network key
   - Call `thread start` — OpenThread will automatically join an existing network OR form a new one if no peers are found
   - Remove the manual scan loop entirely — Thread's MLE (Mesh Link Establishment) handles discovery natively
   - If no network is found within Thread's attach timeout (~120s), the node self-promotes to leader automatically

3. **Enable router eligibility on all nodes**:
   - `routereligible enable` — already done for children, now do it for all nodes
   - This allows Thread to promote any REED to Router, and any Router to Leader as needed

4. **Verify failover**:
   - Test with 3+ nodes
   - Identify the leader (green LED), power it off
   - Confirm another node becomes leader within ~30-60 seconds (Thread's Partition Leader Timeout)
   - Confirm multicast chat still works after failover
   - Confirm that when the original leader powers back on, it rejoins as a router (not a competing leader)

5. **Handle the "all nodes become leader" bug** (from Known Issues):
   - Root cause: if multiple nodes boot simultaneously and none finds the channel in scan, all call `setupLeaderNode()` which calls `dataset init new` — creating separate networks
   - Fix: never call `dataset init new`. Instead, all nodes use the same fixed dataset (channel + network key). The first node to start will become leader; others will attach to it. If two start simultaneously, Thread's partition merge protocol will reconcile them into one network.

6. **Handle the "no node becomes leader" bug**:
   - Root cause: race condition where scan finds a stale/phantom network that no longer exists, so node becomes child but can never attach
   - Fix: after calling `thread start`, monitor role with a timeout. If role is still `OT_DEVICE_ROLE_DETACHED` after 120 seconds, call `thread stop`, `thread start` to retry. Thread's native retry logic should handle most cases.

**Deliverables**:
- Single unified `setupNode()` function replacing `setupLeaderNode()` / `setupChildNode()`
- Verified 3-node failover test (leader power cycle, network recovers)
- Verified simultaneous boot test (3 nodes powered on together, converge to 1 leader)
- Document observed failover timing

**Why Phase 0 (before ESP-IDF migration)**:
- Arduino iteration is faster (compile + flash in IDE, serial monitor built-in)
- Validates that Thread's self-healing actually works with our hardware before investing in the ESP-IDF port
- The fix is small (remove code, don't add) — low risk of creating Arduino-specific debt
- If failover doesn't work reliably, we need to know now — it may change the architecture

### Phase 1: Foundation (ESP-IDF Migration + Thread Self-Healing)
- Port Phase 0 result to ESP-IDF using `esp_openthread` APIs
- All nodes run identical firmware (carried over from Phase 0)
- Verify leader failover still works on ESP-IDF
- Implement basic CoAP PUT/GET on `/magnet` resource using OpenThread CoAP API

### Phase 1.5: Channel Selection + UART Protocol
- Implement NVS/EEPROM channel persistence (Section 4.8)
- Implement boot sequence with 10-second wait, default fallback
- Implement formalized UART command/event protocol (Section 4.6, R9)
- Replace ad-hoc `chat>` serial parsing with structured command parser
- `CHANNEL SET`, `STATUS`, `PEERS` commands operational
- Verify: flash a node, connect UART, set channel, reboot, node joins correct network

### Phase 2: Message Protocol + Chat
- Implement the binary envelope format
- Implement Type 0 (chat) with free-form UTF-8
- Implement 1-1 (unicast) and 1-many (multicast) delivery
- UART commands: `CHAT <message>` for multicast, `DM <addr> <message>` for unicast
- Sequence numbers for dedup

### Phase 3: Channels + Encryption
- Implement channel passphrase -> key derivation (HKDF-SHA256)
- Implement channel passphrase -> multicast address derivation
- Implement AES-CCM-128 payload encryption/decryption using mbedTLS
- UART command: `channel join <passphrase>` / `channel leave <passphrase>`
- Multi-channel support (node tracks multiple channel contexts)

### Phase 4: M2M Protocol + Binary Transfer
- Implement Type 1/2 (M2M command/response)
- Define command namespaces (system, lighting, sensor)
- Implement Type 3 (binary transfer) with app-layer fragmentation
- Implement Type 4 (ACK/NACK) for reliable delivery
- Eventual consistency: fragment tracking, retry, reassembly

### Phase 5: Edge Routing
- Implement bridge software for host device (Python reference implementation)
- Host reads UART events, forwards to MQTT / WebSocket / HTTP
- Host accepts downstream commands, translates to UART
- Test with Raspberry Pi + ESP32-C6 as UART-bridge edge router (Section 4.7, Option B)
- Optional: test ESP32-C6 native border router (Option A) for comparison

### Phase 6: Hardening + Scale Testing
- Test with 32+ nodes
- Measure multicast storm effects
- Tune CoAP retransmission parameters
- Add DTLS-PSK for sensitive 1-1 channels
- OTEL logging integration

---

## 7. Limitations

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| **802.15.4 bandwidth**: ~250 kbps raw, ~100 kbps effective | Large binary transfers are slow (~10KB/sec theoretical) | App-layer fragmentation, prioritize small messages |
| **Single radio**: ESP32-C6 has one 802.15.4 radio | Cannot be on two Thread networks simultaneously | Use Approach A (app-layer channels) not Approach B |
| **Multicast storm**: N nodes each sending to multicast = N^2 radio transmissions | Degrades above ~50 actively chatting nodes | Rate limiting, message coalescing, suppress duplicate forwarding |
| **No forward secrecy on group channel**: Compromised passphrase exposes all past messages | Historical messages decryptable | Key rotation (periodic re-derive from passphrase + epoch) |
| **6LoWPAN reassembly buffer**: 1280 bytes default in OpenThread | Limits single-message size | App-layer fragmentation for larger payloads |
| **Fragment loss on multicast**: NON-confirmable multicast cannot be individually ACKed | Binary transfers over multicast may lose fragments | Use unicast for reliable binary transfer; multicast for best-effort |
| **RAM constraints**: ESP32-C6 has 512KB SRAM | Limits concurrent channel contexts and reassembly buffers | Cap at ~8 simultaneous channels, 4 concurrent binary transfers |
| **No internet connectivity**: Pure Thread mesh, no border router assumed | Cannot bridge to cloud/internet without additional hardware | Out of scope; border router can be added later |
| **Clock drift**: No NTP, no synchronized time | Sequence numbers work for ordering but not timestamping | Optionally sync time from a node with RTC or border router |

---

## 8. Scale Analysis

### Theoretical Maximum

| Config | Nodes | Notes |
|--------|-------|-------|
| Single router (star) | 64 | 1 router + 63 children (OpenThread default) |
| Small mesh | 250 | ~8 routers, ~30 children each |
| Medium mesh | 500 | ~16 routers, ~30 children each |
| Full mesh | 16,384 | 32 routers x 511 children (Thread spec max) |

### Practical Limits by Use Case

| Use Case | Recommended Max Nodes | Bottleneck |
|----------|----------------------|------------|
| Active chat (all sending) | 30-50 | Multicast storm, radio collisions |
| Mixed chat (10% active) | 200-300 | Router memory, routing table |
| M2M command/control (periodic) | 500+ | Depends on message frequency |
| Passive sensor reporting | 1000+ | With sleepy end devices, staggered reporting |

### Multicast Scaling Math

For N nodes all actively chatting at 1 msg/sec each:
- Each message = 1 CoAP multicast PUT
- Each router forwards to all neighbors
- Approximate radio transmissions per message: ~2*R (R = number of routers)
- Total radio utilization: N * 2R * ~100 bytes * 8 bits / 250,000 bps

Example: 50 nodes, 16 routers, 1 msg/sec each:
```
50 * 32 * 800 / 250,000 = 5.12 = ~5% radio utilization
```
At 50% utilization, collisions become significant. This suggests **~500 messages/second** network-wide throughput ceiling, or roughly **50 nodes chatting at 1 msg/sec** before degradation.

---

## 9. Packet Format Summary

### Quick Reference Card

```
┌─────────────────────────────────────────────────┐
│           MagNET v2 Message Envelope            │
├─────────────────────────────────────────────────┤
│ Offset  Size   Field                            │
│ 0       4 bit  Version (1)                      │
│ 0       4 bit  Type (0-15)                      │
│ 1       1 byte Flags                            │
│ 2       2 byte Sequence Number (BE)             │
│ 4       4 byte Message ID (random)              │
│ 8       1 byte Channel Hash                     │
│ 9       1 byte Fragment (total:4 | index:4)     │
│ 10      N byte Payload                          │
│ 10+N    8 byte AES-CCM MIC (if encrypted)       │
├─────────────────────────────────────────────────┤
│ Header: 10 bytes | MIC: 8 bytes | Total OH: 18  │
└─────────────────────────────────────────────────┘

Payload Capacity (single 802.15.4 frame):  ~62 bytes
Payload Capacity (6LoWPAN reassembled):    ~1182 bytes
Payload Capacity (15 app-fragments):       ~930 bytes (single-frame)
                                           ~17 KB (with 6LoWPAN fragmentation)
```

### Can Packets Be Recombined?

**Yes, at two levels:**

1. **6LoWPAN fragmentation** (transparent, handled by Thread stack): Reassembles up to 1280 bytes from multiple 127-byte 802.15.4 frames. Automatic — no application code needed.

2. **Application-layer fragmentation** (MagNET envelope): The Fragment Info field supports splitting a message into up to 15 fragments, each independently transmitted as a CoAP message. Receiver reassembles using shared Message ID. This allows payloads up to ~17KB.

**Trade-offs by fragmentation level**:

| Level | Max Payload | Latency | Reliability | Complexity |
|-------|-------------|---------|-------------|------------|
| None (single frame) | ~62 bytes | Lowest | Highest | None |
| 6LoWPAN only | ~1182 bytes | Low | High (stack handles retry) | None |
| App-layer only | ~930 bytes | Medium | Medium (must handle retry) | Medium |
| Both | ~17KB | High | Lower (compound failure) | High |

**Recommendation**: Use 6LoWPAN fragmentation as the primary mechanism (automatic, reliable). Reserve app-layer fragmentation for payloads >1KB where the 6LoWPAN reassembly buffer is insufficient. For most chat and M2M use cases, 62-1182 bytes is sufficient.

---

## 10. Open Questions

1. **Key rotation**: Should channel keys rotate periodically? If so, what epoch granularity (hourly, daily)?
2. **Node identity**: Should nodes have persistent identities beyond EUI-64? (e.g., user-assigned names stored in NVS)
3. **Message persistence**: Should nodes store-and-forward messages for offline peers?
4. **OTA updates**: Should the M2M binary transfer support firmware updates?
5. **Interop**: Should the M2M command protocol be documented as an open spec, or remain proprietary?
6. **Power management**: Are any nodes battery-powered sleepy end devices? (Affects multicast reception — SEDs miss multicast by design.)
7. **UART baud rate**: Should higher baud rates (230400, 460800) be supported for edge routing throughput? Would need NVS-persisted config.
8. **Multi-channel fleet migration**: When a system command tells peers to switch channels, what happens to peers that miss the message? (Eventual consistency problem — may need retransmit on both old and new channel briefly.)
9. **Default passphrase security**: Should the well-known default `"magnet"` passphrase trigger a persistent warning LED pattern to remind users to configure a private channel?

---

## Appendix A: ESP-IDF API Mapping

Key ESP-IDF / OpenThread APIs for implementation:

| Function | API |
|----------|-----|
| Thread init | `esp_openthread_init()` |
| Set network key | `otThreadSetNetworkKey()` |
| Set channel | `otLinkSetChannel()` |
| Start Thread | `otThreadSetEnabled()` |
| CoAP start | `otCoapStart()` |
| CoAP add resource | `otCoapAddResource()` |
| CoAP send request | `otCoapSendRequest()` |
| Subscribe multicast | `otIp6SubscribeMulticastAddress()` |
| Get device role | `otThreadGetDeviceRole()` |
| AES-CCM encrypt | `mbedtls_ccm_encrypt_and_tag()` |
| AES-CCM decrypt | `mbedtls_ccm_auth_decrypt()` |
| HKDF | `mbedtls_hkdf()` |
| SHA-256 | `mbedtls_sha256()` |

## Appendix B: Comparison with Current Prototype

| Aspect | Current (v0.0.6) | Proposed (v2) |
|--------|-------------------|---------------|
| Platform | Arduino | ESP-IDF |
| Role assignment | Hardcoded at boot | Thread auto-managed |
| Encryption | Link-layer only | Link + app-layer AES-CCM |
| Channels | Single hardcoded | Multi-channel via passphrase |
| Message format | Hex-encoded text string | Binary envelope |
| Max payload | ~256 bytes (arbitrary limit) | ~1182 bytes (6LoWPAN) / ~17KB (fragmented) |
| Chat | `chat>` prefix, text only | Type 0, UTF-8, any content |
| M2M control | Ad-hoc `lamp>0/1` | Structured command namespaces |
| Binary data | Not supported | Type 3 with fragmentation + eventual consistency |
| 1-1 messaging | Broken | CoAP CON to unicast IPv6 |
| 1-many messaging | CoAP NON to multicast | Same, with channel-derived multicast address |
| Leader failover | None | Thread-native (Phase 0) |
| UART protocol | Ad-hoc `chat>` prefix | Formalized command/event text protocol |
| Channel persistence | Compile-time only | NVS/EEPROM with 10s boot wait + default fallback |
| Edge routing | None | UART bridge to Wi-Fi host (Option B) |
| Node discovery | Network scan only | CoAP GET `/magnet/discover` |
