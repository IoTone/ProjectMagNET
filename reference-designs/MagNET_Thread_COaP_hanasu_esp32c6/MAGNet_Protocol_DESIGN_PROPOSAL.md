# MagNET Hanasu v2 - Design Proposal

## Status: DRAFT (rev 2.2)
## Date: 2026-03-27 (rev 2.1: 2026-06-14, rev 2.2: 2026-07-30)
## Target Platform: ESP-IDF (prototypes on Arduino)

> **Rev 2.1 note**: Sections 1–10 are the original v2 proposal. **Section 11 supersedes
> and formalizes** the cryptography (§4.1, §4.2) and the host control protocol (§4.6) with
> normative specs, plus the multi-transport "add-on to any device" model and revised secure
> defaults. Where §11 conflicts with an earlier section, §11 wins. Several Open Questions
> (§10) are resolved there.
>
> **Rev 2.2 note**: Adds **R10** (on-device Forth scripting/automation — formalizes §12 as a
> requirement, not just a migration plan), the **seed-phrase credential path** (§11.1.2 Path C),
> and **§11.8 upstream alignment** with PONY-Cyberdeck-25 #7 (including the photo-transfer gap).
> Corrects the usable-RAM figure in §7 per the §12.10 spike measurement.

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
| R9 | Host control channel — any node can be controlled by a host device (UART originally; generalized to multi-transport HCP in §11.2: USB-CDC, UART, BLE-GATT, WebSocket) | Must |
| R10 | On-device scripting/automation — a Forth REPL + event hooks (ESPIDFORTH) so a node can run user automation in reaction to mesh traffic, and power users/LLM drivers get an interactive control surface over the same host link (§12) | Must (rev 2.2) |

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
| **RAM constraints**: ESP32-C6 has 512KB SRAM total but only **~320KB usable DRAM** (PlatformIO-reported; confirmed by the §12.10 spike) | Limits concurrent channel contexts and reassembly buffers | Cap at ~8 simultaneous channels, 4 concurrent binary transfers; size the Forth heap against 320KB (§12.5) |
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

> **Superseded**: this card shows the original v2 10-byte envelope. The normative wire format
> is the **v2.1 envelope in §11.1.4** (16-byte header, 4-byte sender device_id, 32-bit persistent
> counter, epoch byte, 16-bit selector). Kept for history only.

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
9. **Default passphrase security**: Should the well-known default `"magnet"` passphrase trigger a persistent warning LED pattern to remind users to configure a private channel? **[Resolved §11.5 — yes.]**
10. **Photo/video-sized transfers** (rev 2.2): the 4-bit fragment field caps app-layer transfers at ~17 KB, but upstream (PONY-Cyberdeck-25 #7) wants photo/video sharing. **[Direction sketched §11.8 — Type 6 extended transfer, 16-bit chunk index, host-side re-encode; to be specified before Phase 4 hardening.]**

---

## 11. Revised Recommendations & Formal Specifications (rev 2.1)

This section formalizes the parts of the design that were under-specified and most risky:
the cryptography and the host-facing control protocol. It also introduces the **multi-transport
host model** required for the "add-on to any computer, phone, or VR headset" goal.

Normative keywords (MUST / SHOULD / MAY) are used in the RFC 2119 sense.

### 11.0 Why these changes

| Area | Problem in v2 draft | Rev 2.1 fix |
|------|--------------------|-------------|
| AES-CCM nonce | Undefined construction; shared key + RAM counter ⇒ **guaranteed nonce reuse** across senders/reboots ⇒ catastrophic AEAD break | §11.1.4 explicit nonce = `sender_id ‖ persistent_counter ‖ …`, counter persisted in NVS |
| Key derivation | HKDF (fast) + fixed public salt ⇒ offline dictionary attack on kid-grade passphrases | §11.1.2 PBKDF2/Argon2id stretch, salt bound to channel name |
| Public oracle | mcast addr & channel_hash derived from raw `SHA256(passphrase)` ⇒ confirm guesses with no ciphertext | §11.1.3 routing IDs derived from the **secret root**, not the passphrase |
| Sender auth | Shared group key ⇒ any member can impersonate any member; `set_channel` = fleet hijack | §11.1.5 per-device Ed25519 identity; signed + allow-listed admin commands |
| Replay | Dedup state in RAM, lost on reboot | §11.1.6 persistent monotonic counter + per-peer high-water mark |
| Host reach | UART-only; most phones/VR headsets can't host USB-serial | §11.2 transport-agnostic protocol with USB-CDC, **BLE-GATT**, WebSocket bindings |
| REPL robustness | `+`-prefixed events and responses intermixed; free-text errors | §11.3 sigil-separated classes, correlation tags, stable error codes, self-description |
| Onboarding | Type a brute-forceable passphrase; silent default-network join | §11.4 QR/pairing carries a high-entropy secret; §11.5 secure defaults |

---

### 11.1 Cryptographic Specification

#### 11.1.1 Key hierarchy (overview)

```
channel credential
   │  (passphrase typed, OR 256-bit secret from QR/pairing)
   ▼
root_secret  (256 bit)               ← high-entropy after stretch
   ├─ HKDF(info="selector")  → channel_selector (16 bit, public on wire)
   ├─ HKDF(info="mcast")     → multicast group  (ff05::/derived, public on wire)
   └─ HKDF(info="chan-base") → channel_base_key (256 bit, secret)
                                   └─ HKDF(info="epoch"‖e) → epoch_key[e] (128 bit AES-CCM)

device identity (per node, generated first boot, stored in NVS)
   Ed25519 keypair (sk, pk)
   device_id = SHA256(pk)[0:4]      ← 4-byte short ID, used in envelope + nonce
```

Two independent trust planes:
- **Channel plane** (symmetric, group): confidentiality + integrity *against non-members*.
- **Identity plane** (asymmetric, per-device): authenticity *within* the group, and admin authorization.

#### 11.1.2 Deriving `root_secret` from a credential

The credential reaches a node by one of two paths. **Pairing is the preferred path**; the typed
passphrase is the constrained fallback.

**Path A — Pairing / QR (preferred, used by the phone/app onboarding flow):**
The provisioning device (phone, laptop, headset companion app) generates a **256-bit random**
`root_secret` directly. No password stretching is needed on the node — it receives the full-entropy
secret over the (bonded) host link or an out-of-band QR/NFC blob. The companion app MAY also run a
strong, memory-hard KDF (Argon2id, m≥64 MB) if a human-memorable phrase must back the secret — that
cost is paid on the capable host, never on the C6.

**Path B — Typed passphrase (fallback / dev):**
The node stretches the passphrase itself. Because the ESP32-C6 has only ~512 KB SRAM, memory-hard
KDFs are impractical on-device; use PBKDF2 tuned to ~1 s of wall time (C6 has HW SHA-256 accel):

```
salt        = HKDF-Extract(ikm = utf8(channel_name), salt = "MagNET/v2.1/chan-salt")
root_secret = PBKDF2-HMAC-SHA256(password = utf8(passphrase),
                                 salt     = salt,
                                 iters    = 600000,   // tune to ≈1 s on target, store iters in NVS
                                 dkLen    = 32)
```

- `channel_name` (a short public label, default = the passphrase itself if no separate name given)
  binds the salt so the **same passphrase on different channels yields different keys** and defeats
  precomputed rainbow tables.
- Stretching is performed **once at join**; the result is cached in NVS. Per-message crypto never
  re-runs the KDF.
- PBKDF2 is not memory-hard — it raises offline guessing cost by ~6 orders of magnitude vs. a single
  HKDF, not infinitely. Therefore **Path B SHOULD warn** the user toward pairing for anything private,
  and the UX SHOULD encourage long passphrases (the onboarding app can show an entropy meter).

**Path C — Seed phrase, 12–24 short words (rev 2.2; matches PONY-Cyberdeck-25 #7):**
The credential is a sequence of 12–24 words drawn from a fixed 2048-word list (BIP39-style).
Each word contributes 11 bits, so **12 words ≈ 132 bits and 24 words ≈ 264 bits of entropy** —
this is a *full-entropy* credential like Path A, not a weak passphrase like Path B, yet it can be
read aloud, written on paper, or typed by a kid. Derivation:

```
normalized   = lowercase(words joined with single spaces)   // canonical form
root_secret  = HKDF-SHA256(ikm = utf8(normalized),
                           salt = "MagNET/v2.2/seed-phrase", info = "root", L = 32)
```

- **No PBKDF2 stretch is needed or used** — the entropy is already in the words. Stretching would
  only add join latency on the C6.
- Nodes/apps SHOULD offer "generate a channel" = pick N words at random from the list (the node
  has `esp_fill_random`; the word list costs ~13 KB of flash) and display/share them.
- HCP: accepted anywhere a `<cred>` is accepted; a credential of ≥12 space-separated words from
  the list is auto-detected as Path C (else it is treated as a Path B passphrase and stretched).
  `!WARN weak-credential` is emitted for Path B, never for Path A/C.

#### 11.1.3 Public routing identifiers (no more oracle)

`channel_selector` and the multicast group are derived from `root_secret` (secret), **not** from the
passphrase. An outsider who sees the selector/mcast on the wire cannot test passphrase guesses against
them, because computing them requires the secret they're trying to find.

```
channel_selector = HKDF(root_secret, info="selector")[0:2]   // 16-bit, replaces 8-bit channel_hash
mcast_suffix     = HKDF(root_secret, info="mcast")[0:4]
multicast_group  = ff05:0:0:0:0:0: <mcast_suffix as 32-bit group ID>   // site-local scope
```

Selector widened 8→16 bits to cut cross-channel false-positive decrypt attempts at scale.
Collisions remain possible (and harmless — the AEAD tag rejects the wrong key); they only cost a
wasted decrypt attempt.

#### 11.1.4 Envelope v2.1 (binary) — supersedes §4.4 / §9

```
MagNET Message Envelope v2.1
┌────────┬──────┬──────────────────────────────────────────────────────────┐
│ Offset │ Size │ Field                                                      │
├────────┼──────┼──────────────────────────────────────────────────────────┤
│ 0      │ 1 B  │ Version(4 hi) | Type(4 lo)                                 │
│ 1      │ 1 B  │ Flags                                                      │
│ 2      │ 4 B  │ Sender device_id  (SHA256(pk)[0:4])                        │
│ 6      │ 4 B  │ Counter (uint32 BE) — persistent monotonic per sender     │
│ 10     │ 1 B  │ Epoch (uint8) — channel key epoch (forward secrecy)       │
│ 11     │ 2 B  │ Channel selector (uint16 BE)                              │
│ 13     │ 1 B  │ Fragment (total:4 hi | index:4 lo)                        │
│ 14     │ 2 B  │ Message ID (uint16) — fragment-group correlation (random) │
│ 16     │ N B  │ Payload (AES-CCM ciphertext if Flags.ENCRYPTED)           │
│ 16+N   │ 8 B  │ AES-CCM MIC      (present iff Flags.ENCRYPTED)             │
│ +8     │ 64 B │ Ed25519 signature(present iff Flags.SIGNED)               │
└────────┴──────┴──────────────────────────────────────────────────────────┘
Header = 16 B. Overhead: +8 MIC (encrypted), +64 sig (signed/admin).
```

**Flags (byte 1):**

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | ENCRYPTED | Payload is AES-CCM ciphertext; MIC present |
| 1 | SIGNED | Ed25519 signature trailer present |
| 2 | ADMIN | Sensitive/privileged command (MUST be SIGNED and allow-listed) |
| 3 | REQUIRES_ACK | Sender expects a Type-4 ACK |
| 4 | IS_FRAGMENT | App-layer fragment (see Fragment field) |
| 5 | IS_FINAL_FRAG | Last fragment of the group |
| 6–7 | reserved | MUST be 0 |

Changes from v2: explicit **sender device_id** (no longer relying on spoofable IPv6 source),
**32-bit persistent counter** replaces the 16-bit per-loop sequence number (serves both nonce and
replay roles), **epoch** byte for key rotation, **16-bit selector**, optional **signature trailer**.

#### 11.1.5 AEAD construction (normative)

Cipher: **AES-128-CCM**, 8-byte MIC (`mbedtls_ccm_*`). Key: `epoch_key[Epoch]` (§11.1.1).

```
nonce (13 bytes) = device_id(4) ‖ counter(4) ‖ epoch(1) ‖ channel_selector(2) ‖ 0x0000(2)
AAD              = envelope bytes [0 .. 15]      (the full 16-byte header, authenticated not encrypted)
ciphertext,MIC   = AES-CCM-Encrypt(epoch_key, nonce, AAD, plaintext)
```

**Nonce-uniqueness invariant (the whole point):** `(device_id, counter)` MUST be unique for the
lifetime of a key. This holds because `device_id` is unique per node and `counter` is a persistent
monotonic uint32 (§11.1.6). Every transmitted frame — including each app-layer fragment — consumes a
fresh counter value. Implementations MUST NOT transmit if the counter cannot be persisted (fail
closed). On `epoch` rollover the counter MAY reset (the key differs).

#### 11.1.6 Replay protection & counter persistence

- Each node keeps `counter` in NVS. To avoid a flash write per message, reserve counter blocks:
  persist `counter + 1024`, hand out values from RAM, and persist the next block before exhausting
  it. A crash wastes at most one block (acceptable — uint32 ≫ device lifetime).
- Receivers keep a bounded **per-sender high-water table** `last_counter[device_id]` (LRU, e.g. 64
  entries). A frame with `counter ≤ last_counter[sender]` is a replay → **drop**. Unknown sender →
  accept and record (trust-on-first-use). Note the LRU bound: a replay of a long-idle, evicted
  sender can slip through once; ADMIN frames mitigate this by also being signed.

#### 11.1.7 Sender authentication & admin authorization

- **Identity:** each node generates an Ed25519 keypair on first boot (`nvs: mn_dev_sk/mn_dev_pk`).
- **Chat / routine M2M:** group AEAD only (no signature) — keeps them inside one frame. Authenticity
  is "some channel member sent this," which is sufficient for chat.
- **Privileged commands** (ADMIN flag) — `system/set_channel`, `system/reboot`, OTA, `admin/*` —
  MUST set SIGNED, carry the Ed25519 signature trailer, and the signer's `pk` MUST be on the node's
  **admin allow-list**. Frames failing either check are dropped *before* execution.
- **Allow-list provisioning:** the first admin key is installed during pairing/onboarding (TOFU on the
  bonded host link). Further keys via `ADMIN ADD <pubkey-hex>` (host link) or a signed `admin/grant`
  mesh command from an existing admin. This converts §4.8's "any peer can move the fleet" into "only
  an authorized operator can."
- Signature covers `header ‖ ciphertext ‖ MIC` (or `header ‖ plaintext` if unencrypted).

#### 11.1.8 Forward secrecy via epochs (resolves Open Q1)

- `epoch_key[e] = HKDF(channel_base_key, info = "epoch" ‖ uint32(e))`.
- Default rotation: **daily** epoch (coarse; no synchronized clock required — see below), or
  on-demand via a signed `admin/rotate` command that bumps the epoch fleet-wide.
- Without NTP, derive `e` from a monotonic source agreed at join (e.g. epoch advances on `admin/rotate`
  only — fully deterministic), OR from a border-router-provided time when present. Receivers keep the
  current and previous epoch keys (overlap window) to tolerate skew; older epoch keys are zeroized,
  so a later passphrase compromise does not decrypt traffic from expired epochs.

---

### 11.2 Multi-transport host model ("add-on to any device")

The "R9 UART protocol" is generalized into the **MagNET Host Control Protocol (HCP)** — one line-based
grammar (§11.3) with several transport bindings. The same commands/events flow over whichever pipe a
given host can offer. This is what makes the node an add-on to *any* computer, phone, or headset.

| Binding | Use case | Notes |
|---------|----------|-------|
| **USB-CDC-ACM** | Laptops, Pi, cyberdeck, dev | Appears as a serial port; 115200 default, host MAY request higher. Superset of the raw-UART binding. |
| **Raw UART** | Wired embedding, no USB | The original §4.6 binding; identical grammar. |
| **BLE-GATT** | **Phones (esp. iOS), VR headsets (Quest/Vision Pro), WebBluetooth** | First-class. Most phones/headsets cannot host USB-serial; BLE is the realistic link. |
| **WebSocket** | When the node/BR is on Wi-Fi | HCP lines as WS text frames; reuse the same parser in browser/WebXR clients. |

> **Coexistence caution (project history):** on the C6 the BLE, Wi-Fi and 802.15.4 radios share the
> 2.4 GHz front end, and we've previously seen BLE coupling and razor-thin DRAM on similar parts.
> The BLE-GATT binding MUST define whether BLE stays up during meshing or is torn down after
> provisioning; default = **BLE provisioning-only, torn down once a channel is set**, with a build
> flag to keep it resident for phone/headset live use.

#### 11.2.1 BLE-GATT binding (normative)

```
Service:  MagNET HCP            UUID  6d61676e-2d68-6370-0001-000000000000   (provisional)
  Char 1: HCP-CMD   write / write-without-response   host → node
          UTF-8 bytes; node buffers until '\n'; one logical command per line.
  Char 2: HCP-EVT   notify                            node → host
          UTF-8 lines; node chunks to (MTU-3); client reassembles at '\n'.
  Char 3: HCP-INFO  read                              static CAPS string (see CAPS, §11.3.6)
```

- Client SHOULD request an ATT MTU ≥ 247 to reduce chunking (default 23 ⇒ 20-byte payload).
- **Bonding:** writes to HCP-CMD that change configuration (CHANNEL SET, ADMIN ADD, RAW) REQUIRE LE
  Secure Connections bonding. This implements the rev-2.1 stance that the host link is
  **privileged-but-pairable**, not blindly trusted (revises §4.6's "UART assumed trusted").
- The node MUST NOT emit the channel secret over any binding (see `CHANNEL SHOW`, §11.3.5).

---

### 11.3 Host Control Protocol (HCP) — the REPL, formalized

Design target: a single text protocol that is **equally clean for a small LLM driver, a chat-app
adapter, and a human at a serial monitor**. The properties that make that possible:

1. **One logical message per line**, UTF-8, `\n`-terminated. Max line length **512 bytes** default
   (negotiable via CAPS); an over-length input line ⇒ `-ERR E_LINE_TOO_LONG`.
2. **Class sigil = first character** — a parser always knows what kind of line it has:

   | Sigil | Class | Direction | Meaning |
   |-------|-------|-----------|---------|
   | (none) | command | host → node | a verb + args |
   | `+` | ok-response | node → host | the single success reply to a command |
   | `-` | err-response | node → host | the single failure reply to a command |
   | `!` | event | node → host | **unsolicited** async (chat in, state change, peer join…) |
   | `#` | comment/log | node → host | human-friendly, parsers MUST ignore |

   This cleanly separates **solicited replies from spontaneous events** — the property an LLM/chat
   adapter needs to never confuse "answer to my command" with "a message just arrived."
3. **Exactly one terminal response** (`+` or `-`) per command. Never zero, never two. Events are
   out-of-band and never substitute for a response.
4. **Correlation tags (optional):** a command MAY be prefixed with `@<tag>` (1–8 chars `[A-Za-z0-9]`).
   The node echoes it on the matching response: `@<tag> +OK …` / `@<tag> -ERR …`. Lets a driver match
   replies even when events interleave. Events never carry a tag.
5. **Stable machine error codes** (`E_*`) plus a human message: `-ERR <CODE> <free text>`.
6. **Self-describing:** `CAPS` (machine) and `HELP` (human) so an LLM can introspect verbs, limits,
   and supported transports without prior knowledge.
7. **Subscriptions / backpressure:** `SUB`/`UNSUB` select event classes so a slow or token-limited
   client isn't flooded by group chat.
8. **Greeting on connect:** node emits `!READY …` immediately so a fresh client knows state without
   polling.

#### 11.3.1 Grammar (ABNF, informative)

```
command   = [ tag SP ] verb *( SP arg ) LF
tag       = "@" 1*8(ALPHA / DIGIT)
verb      = 1*UPALPHA                    ; e.g. CHAT, CHANNEL, STATUS
arg       = quoted / token               ; quoted = DQUOTE *qchar DQUOTE  (for spaces/UTF-8 text)
response  = ( "+" / "-" ) ... LF         ; optionally tag-prefixed: tag SP ("+"/"-") ...
event     = "!" name *( SP field ) LF
comment   = "#" *VCHAR LF
```

- Free-text payloads (chat, DM) take the **rest of the line** verbatim after the fixed args — no
  quoting needed for the trailing message; this keeps human typing natural (`CHAT hello everyone!`).
- Binary uses base64 tokens. Large transfers are chunked one fragment per line (XFER).

#### 11.3.2 Commands (host → node)

| Verb | Form | Valid states | Description |
|------|------|--------------|-------------|
| `STATUS` | `STATUS` | all | Current state, role, channel, peer count. |
| `CAPS` | `CAPS` | all | Machine-readable capabilities (§11.3.6). |
| `HELP` | `HELP [verb]` | all | Human help text (emitted as `#` lines + `+OK`). |
| `PING` | `PING` | all | Liveness → `+PONG`. |
| `MODE` | `MODE TERSE\|HUMAN` | all | TERSE suppresses `#` comment lines. Default HUMAN. |
| `SUB` | `SUB <class>[,<class>…]` | all | Subscribe event classes: `chat,dm,cmd,state,peer,xfer,all`. |
| `UNSUB` | `UNSUB <class>[,…]` | all | Unsubscribe. |
| `HEARTBEAT` | `HEARTBEAT <secs>` | ≥CONFIGURING | Heartbeat interval (0=off). |
| `CHANNEL JOIN` | `CHANNEL JOIN <cred>` | CONFIGURING/READY/DEGRADED | Join channel (passphrase or `qr:<base64>`). |
| `CHANNEL SET` | `CHANNEL SET <cred>` | most (see §4.6) | Set default channel, persist, (re)attach. |
| `CHANNEL LEAVE` | `CHANNEL LEAVE <name>` | CONFIGURING/READY/DEGRADED | Leave a channel. |
| `CHANNEL LIST` | `CHANNEL LIST` | ≥CONFIGURING | List joined channels (names/selectors only). |
| `CHANNEL SHOW` | `CHANNEL SHOW <name>` | ≥CONFIGURING | Public info only — **never** the secret. |
| `CHAT` | `CHAT <text…>` | READY (DEGRADED:queue) | Multicast chat to active channel. |
| `DM` | `DM <peer> <text…>` | READY | Unicast chat (peer = device_id or ipv6). |
| `CMD` | `CMD [<peer>] <ns> <id> <hexparams>` | READY (DEGRADED:queue) | M2M command. |
| `XFER` | `XFER <peer> <b64>` | READY | Binary transfer (auto-fragmented). |
| `PEERS` | `PEERS` | READY/DEGRADED | Known peers (device_id, name, ipv6, last-seen). |
| `NAME` | `NAME <display-name>` | ≥CONFIGURING | Set this node's display name (resolves Open Q2). |
| `WHOAMI` | `WHOAMI` | all | device_id, pubkey, name, fw version. |
| `ADMIN ADD` | `ADMIN ADD <pubkey-hex>` | READY (bonded) | Add admin key to allow-list. |
| `ADMIN LIST` | `ADMIN LIST` | READY | List admin keys. |
| `RAW` | `RAW <hex-envelope>` | READY (bonded) | Inject a raw envelope (advanced). |

State-gating (which commands are accepted in BOOTING/CONFIGURING/ATTACHING/READY/DEGRADED) follows the
table in §4.6, extended with the verbs above. Commands invalid in the current state return
`-ERR E_BAD_STATE <state>`; queued commands (DEGRADED) return `+QUEUED <n>` then later a `!RESULT`.

#### 11.3.3 Events (node → host, unsolicited, sigil `!`)

```
!READY proto=2.1 fw=<ver> id=<device_id> name=<name> state=<state>
!STATE <state> [<details…>]                 ; lifecycle transition (replaces +STATE)
!HEARTBEAT <state> <uptime_s> <role> <peers>
!CHAT <chan> <from_id> <from_name> <text…>
!DM   <from_id> <from_name> <text…>
!CMD  <chan> <from_id> <ns> <id> <hexparams>
!XFER <from_id> <msg_id> <idx>/<total> <b64chunk>
!XFER_DONE <from_id> <msg_id> <b64full|len=<n>>
!PEER_JOIN <id> <name> <ipv6>
!PEER_LEAVE <id>
!ROLE <role>
!RESULT @<tag|->  <ok|err> <…>              ; deferred result of a queued (DEGRADED) command
                                            ; (the @tag here is a FIELD inside the event body —
                                            ;  events are still never tag-PREFIXED, rule 4 holds)
!WARN <code> <text…>                        ; e.g. default-channel-insecure
```

> **Migration:** rev 2.1 moves lifecycle/inbound traffic from `+STATE`/`+CHAT` (which collided with
> the `+OK` response class) to the `!` event class. The §4.6 `+`-prefixed event names are retained as
> a **compat alias** only when `MODE` legacy is set; new clients MUST use `!`.

#### 11.3.4 Error codes (stable)

| Code | Meaning |
|------|---------|
| `E_SYNTAX` | Unparseable line / bad arg count |
| `E_UNKNOWN_VERB` | No such command |
| `E_BAD_STATE` | Command not valid in current lifecycle state |
| `E_LINE_TOO_LONG` | Input exceeded max line length |
| `E_QUEUE_FULL` | DEGRADED command queue full (max 4) |
| `E_NO_CHANNEL` | Operation needs a joined channel |
| `E_NO_PEER` | Unknown/unreachable peer |
| `E_NOT_BONDED` | Privileged op requires a bonded/secured link |
| `E_NOT_ADMIN` | Caller/signer not on admin allow-list |
| `E_BUSY` | Resource busy (e.g. transfer in progress) |
| `E_RATE_LIMITED` | Host multicast send exceeded the §4.9 token bucket (burst 8, 10/s) — added rev 2.2 after the E-B saturation measurements |
| `E_INTERNAL` | Unexpected fault |

#### 11.3.5 Secrets handling

`CHANNEL SHOW`/`LIST`/`STATUS` expose only **public** values (name, selector, multicast group, member
count). The node MUST NOT print the passphrase, `root_secret`, `epoch_key`, or device private key over
any HCP binding. `CHANNEL SET`/`JOIN` accept a secret as input but echo only `+OK <name> <selector>`.

#### 11.3.6 CAPS (machine self-description)

`CAPS` returns one `+OK` line of `key=value` pairs (stable keys), so an LLM driver configures itself:

```
+OK proto=2.1 fw=2.1.0 id=0011aabb name=lab-c6 maxline=512 mtu=247 \
   transports=usbcdc,ble,uart verbs=STATUS,CAPS,HELP,PING,MODE,SUB,UNSUB,CHAT,DM,CMD,XFER,\
   CHANNEL,PEERS,NAME,WHOAMI,ADMIN,RAW events=ready,state,chat,dm,cmd,xfer,peer,role,heartbeat,warn \
   queue=4 channels_max=8 xfers_max=4
```

#### 11.3.7 Worked transcripts

**LLM / programmatic driver (TERSE, correlation tags):**
```
→ @a1 MODE TERSE
← @a1 +OK
→ @a2 CHANNEL JOIN team-rocket-2026
← @a2 +OK team-rocket-2026 selector=3f7c
→ @a3 SUB chat,state
← @a3 +OK
→ @a4 CHAT anyone seen the blue capsule?
← @a4 +OK
← !CHAT team-rocket 0011aabb lab-c6 yep it's on shelf 3      ← unsolicited, no tag → not a4's reply
```

**Human in a chat app (HUMAN mode — `#` lines render as hints, `!CHAT` becomes a bubble):**
```
← !READY proto=2.1 fw=2.1.0 id=77ccd1e0 name=desk-dongle state=READY
→ CHAT hello everyone!
← +OK
← !CHAT team-rocket 0011aabb lab-c6 hi! welcome
← # tip: type  DM lab-c6 <msg>  to reply privately
```

The adapter logic is identical for both: route `+`/`-` (optionally by `@tag`) to the
command that's awaiting a reply; render `!` lines as incoming activity; show/hide `#` lines by taste.

---

### 11.4 Onboarding (resolves Open Q2 partially; defines the kid/adult flow)

Goal: a child or non-technical adult joins a private, secure channel **without typing a
brute-forceable string**.

1. **Pair** the dongle to a host once (USB plug, or BLE bond, or scan a QR on the dongle's
   label/screen). Bonding establishes the privileged host link (§11.2.1).
2. The **companion app** (web via WebBluetooth/WebSerial, or native) either generates a 256-bit
   `root_secret` or stretches a chosen phrase, then pushes it via `CHANNEL SET qr:<base64>`.
3. To add a friend: the app shows a **QR / share-link** encoding the channel credential; the friend's
   app scans it and runs the same `CHANNEL SET`. No passphrase typing, no wire oracle.
4. The typed-passphrase path (`CHANNEL JOIN <phrase>`) remains for power users and headless/dev use,
   with an entropy hint.

A small reference **host SDK** (TypeScript for web/WebXR, Swift, Python) SHOULD wrap HCP so "add-on to
any device" is a library call, not a per-platform serial-parsing exercise. WebBluetooth + WebSerial
specifically bridge HCP into the browser-based WebXR clients this project already builds.

### 11.5 Secure & friendly defaults (resolves Open Q6, Q9)

| Default | Value | Rationale |
|---------|-------|-----------|
| Out-of-box channel | `"magnet"` (well-known) **but** flagged insecure | Zero-config demo still works… |
| Insecure-channel indicator | Persistent amber LED pattern **+** periodic `!WARN default-channel-insecure` | …while nagging toward a private channel (Open Q9: **yes**). |
| First private setup | Pairing/QR, not typed phrase | Kid/adult-proof, high entropy. |
| Boot wait for channel | 10 s (per §4.8) then default | Unchanged; warning makes the default safe-by-visibility. |
| Sleepy/battery nodes | Multicast **not** delivered to SEDs by Thread; battery add-ons MUST either run as MED/router (USB-powered) **or** use poll-based catch-up `GET /magnet/recent` | Resolves Open Q6 — a battery dongle that's an SED silently misses all broadcast chat unless it polls. |
| Heartbeat | 30 s | Host hang detection. |
| HCP mode | HUMAN (parser-safe; `#` lines ignorable) | Friendly default; drivers switch to TERSE. |
| Admin model | Empty allow-list until first pair | No privileged mesh control until an operator is provisioned. |

### 11.6 Scale additions (refines §7, §8)

- **Multicast suppression:** adopt **Trickle (RFC 6206)**-style dedup-and-suppress forwarding +
  message coalescing instead of naive rebroadcast. This is the lever that moves the active-node
  ceiling from ~50 toward the 200+ range.
- **Large-group degradation:** small groups peer-to-peer multicast; above ~30 active senders, route
  through a border-router-backed pub/sub (ties into §4.7 Option B host).
- **Channel selector** widened to 16 bits (§11.1.3) to cut false-positive decrypts; remember all
  channels still share one Thread network's storm budget (§7).
- **Counter (32-bit)** removes the 16-bit sequence wrap concern from §4.4.

### 11.7 Revised implementation phase deltas

| Phase | Rev 2.1 change |
|-------|----------------|
| Phase 1.5 | Implement **HCP** (§11.3) as the serial command layer from the start (sigil classes, tags, error codes, CAPS/HELP) — not the §4.6 `+`-mixed scheme. |
| Phase 3 | Use the §11.1 key hierarchy (PBKDF2/pairing, epoch keys, derived routing IDs) and the **v2.1 envelope**; implement the AEAD nonce invariant + NVS counter blocks. |
| **Phase 3.5 (new)** | Identity plane: Ed25519 keypair, device_id, signed ADMIN commands, allow-list, replay high-water table. |
| **Phase 3.6 (new)** | **BLE-GATT HCP binding** + bonding; minimal WebBluetooth reference client. |
| Phase 5 | Reference **host SDK** (TS/Swift/Python) wrapping HCP across USB-CDC / BLE / WebSocket. |
| Phase 6 | Trickle-based multicast suppression; SED catch-up `GET /magnet/recent`. |

### 11.8 Upstream alignment — PONY-Cyberdeck-25 #7 (rev 2.2)

MagNET Hanasu is the working answer to the PONY Cyberdeck "OFFLINE P2P Mesh" feature
(https://github.com/IoTone/PONY-Cyberdeck-25/issues/7), offered as an **add-on option**: an
ESP32-C6 module speaking HCP over UART/USB to the cyberdeck (the deck exposes an
Arduino-compatible GPIO port, so the raw-UART binding drops straight on). Requirement-by-
requirement:

| #7 requirement | Status in this spec |
|----------------|--------------------|
| Chat messaging | ✅ R5 / Type 0, §4.4 |
| 1-1 private | ✅ R8 unicast + §11.1 (DTLS-PSK optional for forward secrecy) |
| 1-N group | ✅ R8 multicast channels, §4.2 |
| Simple configuration by "naming" a network | ✅ R2/R3 — channel = passphrase/credential, everything derived (§4.2, §11.1) |
| Simple discovery by finding a network | ✅ Thread native MLE attach + `/magnet/discover`, `PEERS` |
| Key from a **seed phrase of 12–24 short words** | ✅ §11.1.2 **Path C** (rev 2.2) — full-entropy, no stretch needed |
| No network hopping / bridging required | ✅ matches the app-layer-channels choice (§4.2 A); edge routing (§4.7) stays optional |
| **Photo sharing** | ⚠️ **GAP** — see below |
| Video sharing (non-streaming) | ⚠️ same gap, worse (file sizes) |
| Networking hardware < $15 | ✅ ESP32-C6 modules/devkits are $3–10; XIAO ESP32C6 / M5NanoC6 ≈ $6–10 |
| Works as add-on to low-cost ARM/RISC-V board | ✅ R9 HCP over UART/USB-CDC/BLE; R10 gives the deck a scriptable REPL on the module itself |

**The photo/video gap (Open Q10).** The v2.1 envelope's 4-bit fragment field caps an app-layer
transfer at 15 fragments ≈ **17 KB** — enough for icons/thumbnails, not photos (50 KB–5 MB).
Physics also matters: at ~10 KB/s effective 802.15.4 throughput a 500 KB photo takes ~50 s of
airtime. Direction (to be specified before Phase 4 hardens):

- Add a **Type 6: extended transfer** with a 16-bit chunk index carried in the payload header
  (65k chunks ⇒ multi-MB files), unicast CON only, single concurrent transfer per peer pair,
  NACK-bitmap catch-up per 64-chunk window (same eventual-consistency machinery as Type 3).
- Senders SHOULD down-scale images on the host side (the cyberdeck/phone has the CPU; the mesh
  should carry a ~30–100 KB re-encode, not a camera original) — same philosophy as §11.1.2:
  spend the cost on the capable host.
- Multicast photo share = advertise (`Type 1` notice) + per-peer unicast pull, not multicast
  flooding of fragments (§7 fragment-loss row).

---

## 12. Migration to ESPIDFORTH (Forth control layer)

This section is the migration plan for re-platforming the Hanasu node onto **ESPIDFORTH**
(`../MagNET_M5DialFiddlerCrab/ESPIDFORTH`) — the ESP-IDF/PlatformIO Forth engine already used as
the MagNET "Hive AI" prototype foundation. It replaces the Arduino prototype and realizes the
ESP-IDF migration that §6 Phase 1 calls for, while making Forth the on-device control/automation
surface. As of rev 2.2 this is a formal requirement (**R10**, §2), not just a migration plan.

### 12.0 Architectural decisions (settled)

| Fork | Decision | Consequence |
|------|----------|-------------|
| Role of Forth | **Control + scripting layer over a C core** | Crypto (§11.1), OpenThread, CoAP, envelope, NVS live in audited C. Forth is the host-control surface + on-device automation, exposed as FFI words. Forth never touches key material directly. |
| REPL vs HCP | **Dual-mode, one link** | Default = structured HCP (§11.3 sigil framing) for LLM/chat-app drivers; the `FORTH` command drops into the raw `ok>` REPL for power users; an escape returns to HCP. One pipe, two modes. |

Rationale: the security-critical paths (nonce discipline, Ed25519, KDF) must be in reviewable C, not
interpreted Forth. Forth earns its place as (a) the "solid REPL for a small LLM or a human" that §11.3
already specifies and (b) an on-device automation language — a node can run user Forth in reaction to
mesh events (the "Hive AI" payoff) without any of the protocol logic living in Forth.

### 12.1 Target layering

```
┌──────────────────────────────────────────────────────────────────────┐
│ Transport bindings (§11.2):  USB-CDC-ACM │ UART │ BLE-GATT │ WebSocket │
└───────────────┬──────────────────────────────────────────────────────┘
                │ one byte stream (getchar/putchar), serialized TX writer
┌───────────────▼──────────────────────────────────────────────────────┐
│ Link dispatcher (C)  — owns the stream, runs the mode state machine    │
│   • HCP mode (default): parse sigil-framed lines, emit +/-/!/# (§11.3) │
│   • FORTH mode: feed lines to forth_eval(), print ok>                  │
│   • event pump: CoAP-RX callbacks → ! events (HCP) or Forth hooks      │
└───────┬───────────────────────────────────────────┬───────────────────┘
        │ calls                                       │ FFI (forth_register_word)
┌───────▼───────────────────────┐          ┌──────────▼────────────────────┐
│ MagNET core (C)                │◄────────►│ MagNET Forth vocabulary (C)    │
│  envelope v2.1 (§11.1.4)       │  shared  │  mn-chat mn-join mn-cmd …      │
│  AES-CCM + nonce disc.(§11.1.5)│  funcs   │  mn-on-chat (event→Forth xt)   │
│  Ed25519 identity (§11.1.7)    │          │  gpio-* i2c-* (hardware)       │
│  KDF / epoch keys (§11.1.2/8)  │          └────────────────────────────────┘
│  channel mgr, replay table     │
│  CoAP /magnet, NVS persistence │      ┌────────────────────────────────────┐
└───────┬────────────────────────┘      │ ESPIDFORTH engine (components/forth)│
        │                                │  forth_init / forth_eval / FFI      │
┌───────▼───────────────────────┐       └────────────────────────────────────┘
│ esp_openthread + IEEE 802.15.4 │
│ mbedTLS · NVS  (ESP-IDF 5.3.1) │
└────────────────────────────────┘
```

The MagNET-core C functions are called **identically** by the HCP dispatcher and by the Forth FFI
words — one implementation, two front-ends. This is what keeps HCP and Forth in lock-step.

### 12.2 Repository & build structure

Reuse the ESPIDFORTH PlatformIO layout. New (or re-pointed) project:

```
MagNET_Hanasu_esp32c6/            ; ESP-IDF/PlatformIO project (replaces the .ino)
  platformio.ini                  ; extends magnet_base (IDF 5.3.1, espressif32@6.9.0) — KEEP THE PIN
  sdkconfig.defaults              ; + sdkconfig.defaults.esp32c6 (OpenThread on, see §12.6)
  components/
    forth/                        ; ESPIDFORTH component — git submodule or vendored copy
    magnet/                       ; NEW C component: envelope, crypto, identity, channel, coap,
                                  ;   hcp dispatcher, forth_vocab, transport bindings
  main/
    main.c                        ; app_main bringup (§12.6)
```

Constraints carried over from the fleet:
- **Keep the `magnet_base` pin** (framework-espidf @ 3.50301.0). Do **not** let a transitive
  dependency drag the tree to IDF 5.4 — that reintroduces the efuse-collision build break documented
  in the fleet notes. (No bmi270/sensor_hub here, so the usual trigger is absent — just don't add it.)
- **Board = ESP32-C6.** This is non-negotiable for a Thread node: only C6/H2 have the 802.15.4 radio.
  The PSRAM-equipped S3 used elsewhere in MagNET **cannot** be a native Thread node, so the
  RAM-relief of PSRAM is unavailable — heap discipline (§12.6) is mandatory, not optional.

### 12.3 MagNET Forth vocabulary (FFI)

Lowercase, case-sensitive (matches both ESPIDFORTH core words and the E4TH convention). Each word is a
thin C wrapper that pops args (Forth strings are `c-addr u`) and calls the shared MagNET-core function.

| Word | Stack effect | Maps to |
|------|--------------|---------|
| `mn-status` | ( -- ) | print state/role/channel/peers |
| `mn-whoami` | ( -- ) | device_id, pubkey, name, fw |
| `mn-join` | ( c-addr u -- f ) | join channel by credential |
| `mn-set-channel` | ( c-addr u -- f ) | set+persist+reattach |
| `mn-leave` | ( c-addr u -- ) | leave channel |
| `mn-channels` | ( -- ) | list joined channels (public info only) |
| `mn-chat` | ( c-addr u -- ) | multicast chat (Type 0) |
| `mn-dm` | ( peer c-addr u -- ) | unicast chat |
| `mn-cmd` | ( peer ns id c-addr u -- ) | M2M command (Type 1) |
| `mn-peers` | ( -- ) | known peers |
| `mn-name!` | ( c-addr u -- ) | set display name (Open Q2) |
| `mn-admin-add` | ( c-addr u -- ) | add admin pubkey (bonded link only) |
| `mn-rotate` | ( -- ) | bump epoch (signed admin) |
| `mn-on-chat` | ( xt -- ) | register a Forth word run on inbound chat |
| `mn-on-cmd` | ( xt -- ) | register a Forth word run on inbound M2M cmd |
| `gpio-output` / `gpio-set` / `i2c-*` | (per-word) | hardware FFI (ESPIDFORTH pattern) |
| `mn-sysinfo` | ( -- ) | chip / IDF / heap / Forth-heap / uptime / reset reason |
| `mn-mesh` | ( -- ) | Thread detail: partition, RLOC16, ML-EID, chan/PAN, neighbors + RSSI |
| `mn-bench` | ( -- ) | envelope-codec µs/op + radio TX-call latency |
| `mn-selftest` | ( -- f ) | envelope roundtrip + event pump + **CoAP loopback to own ML-EID**; 0 = pass |
| `mn-heartbeat!` | ( secs -- ) | `!HEARTBEAT` interval (0 = off), the §4.6 heartbeat |

The diagnostics words are mirrored 1:1 as HCP verbs (`SYSINFO`, `MESH`, `BENCH`, `SELFTEST`,
`HEARTBEAT <secs>`) — same C functions, two front-ends (§12.1). Note `SELFTEST`'s pump and
loopback stages only run from HCP mode: at the `ok>` prompt the dispatcher holds the TX writer
across the eval, so those stages report *skipped* instead of false-failing.

**The automation hook is the payoff.** `mn-on-chat` / `mn-on-cmd` let a node *react* to mesh traffic
in user Forth, without any protocol logic leaving C:

```
ok> : maybe-light  s" lights on" str= if 4 gpio-set then ;
ok> ' maybe-light mn-on-cmd
```

Inbound frames are decrypted/verified/dedup'd in C, then the registered Forth `xt` is invoked **on the
Forth task** (the C RX callback marshals via a queue — Forth MUST NOT run in the lwIP/Thread callback
context). User scripts can be persisted to NVS/SPIFFS and auto-run at boot (`mn-autorun`).

### 12.4 Dual-mode link (HCP ⇄ Forth)

A single C **link dispatcher task** owns the transport stream; it does *not* hand the raw stream to the
blocking `forth_repl()`. Instead it reads one line at a time and routes by mode:

- **HCP mode (default).** Parse the line per §11.3 (sigil/tag/verb). Dispatch to the MagNET-core
  function; emit exactly one `+`/`-` response. The verb `FORTH` switches to Forth mode.
- **FORTH mode.** Pass the line to `forth_eval()`; print engine output and `ok>`. A lone `.hcp` line
  (or a configurable escape) returns to HCP mode. `WARN`: while in Forth mode the strict HCP framing
  guarantees do not hold — it is for humans/power use, not the LLM driver path.
- **Event pump.** Inbound mesh traffic and lifecycle changes are emitted as `!` events (HCP mode) or
  routed to Forth hooks (§12.3). **All** writes — command responses, events, and Forth output — go
  through **one serialized TX writer** (a mutex-guarded queue) so an async `!CHAT` can never interleave
  mid-line with a `+OK` or with Forth output.

**Required tiny ESPIDFORTH changes (done):** `forth_set_io(getchar, putchar)` so `forth_eval()` output
is routed without entering the blocking `forth_repl()` (point `put_char` at the serialized TX writer);
and `s"` / `type` so FFI string args can be driven from the REPL (the stub had only `."`). Both landed
in `components/forth/forth_core.{h,cpp}` — `s" hello team" mn-chat` now works.

### 12.5 Memory budget & bringup order (C6, no PSRAM)

C6 SRAM is ~512 KB; OpenThread + mbedTLS will claim a large share, so the Forth dictionary heap must
shrink from ESPIDFORTH's default 100 KB.

| Consumer | Budget (target) | Notes |
|----------|-----------------|-------|
| ESP-IDF + FreeRTOS + NVS | ~baseline | |
| esp_openthread + 802.15.4 + lwIP/6LoWPAN | ~60–90 KB | dominant new cost |
| mbedTLS (CCM ctx tiny; PBKDF2/Ed25519 transient) | ~a few KB resident, stack spikes at join/sign | run KDF once at join |
| MagNET core (channels ≤8, replay LRU 64, reassembly buffers) | ~8–16 KB | cap per §7 |
| **Forth dictionary heap** | **48–64 KB** (down from 100 KB) | tune via `mem`; this is the adjustable knob |
| Forth data/return stacks + REPL/dispatcher task stacks | ~12–18 KB | |

**Bringup order is load-bearing** (matches the fleet lesson "forth_init before BLE/WiFi or the
dictionary heap starves," and "claim the big contiguous block first"):

```c
void app_main(void) {
    transport_init();                 // USB-CDC/UART up first → emit !STATE BOOTING immediately
    forth_init(FORTH_HEAP);           // 1. claim Forth dict heap while RAM is unfragmented
    forth_set_io(tx_getc, tx_putc);   //    route Forth I/O through the serialized writer
    magnet_register_forth_vocab();    // 2. add mn-* FFI words
    nvs_init(); identity_load_or_gen();// 3. NVS + Ed25519 identity (before radios)
    magnet_core_init();               // 4. channel mgr, replay table, CoAP resource objects
    link_dispatcher_start();          // 5. HCP dispatcher task (default mode) + TX writer + event pump
    openthread_start();               // 6. radios LAST — by now heap is committed
    // (optional) ble_provisioning_start();  // §11.2.1, provisioning-only by default on C6
}
```

### 12.6 sdkconfig deltas for the C6 Thread build

Starting from ESPIDFORTH's `sdkconfig.defaults.esp32c6` (which currently disables WiFi/BLE and has no
Thread), enable:

```
CONFIG_OPENTHREAD_ENABLED=y
CONFIG_IEEE802154_ENABLED=y
CONFIG_OPENTHREAD_CLI=y              # convenient during E-Phase A bringup; can disable later
CONFIG_MBEDTLS_CCM_C=y              # AES-CCM (already needed by Thread)
CONFIG_MBEDTLS_HKDF_C=y
CONFIG_MBEDTLS_PKCS5_C=y           # PBKDF2 (§11.1.2)
# Ed25519: enable mbedTLS Edwards-curve support, OR add the libsodium component
CONFIG_ESP_WIFI_ENABLED=n           # no WiFi unless this node is also a border router
CONFIG_BT_ENABLED=n                 # flip to y only for the BLE-GATT binding (provisioning-only default)
# PlatformIO caches sdkconfig.<env> — after editing defaults, `rm sdkconfig.esp32c6` then rebuild.
```

> Reminder from the fleet: PlatformIO caches the generated `sdkconfig.<env>`; edits to
> `sdkconfig.defaults.*` are ignored until you delete the cached file and rebuild.

### 12.7 Migration phases

These augment §6 / §11.7. Each is independently demoable. The Arduino Phase 0 (leader-election fix,
§6) can be validated first on the existing `.ino` *or* folded into E-Phase B in C — recommend folding
in, since the unified-role logic is small and Thread's self-healing is identical under ESP-IDF.

| Phase | Goal | Key deliverable / exit test |
|-------|------|-----------------------------|
| **E-A: Skeleton + coexistence spike** | Prove ESPIDFORTH + esp_openthread fit and run together on C6 | `ok>` over USB-CDC **and** Thread attaches; `mem` shows healthy free heap with the radio up. De-risks the #1 unknown (RAM + IDF Thread). `s"`/`type`/`forth_set_io` added ✓; build links clean ✓ (flash 28.6%, static RAM 31%); on-HW attach + runtime heap still to confirm. |
| **E-B: MagNET C core (plaintext)** | Unified single-role node (§6 Phase 0 logic in C), `/magnet` CoAP resource, v2.1 envelope unencrypted, multicast chat | 3-node failover + multicast chat; `mn-chat`/`mn-status`/`mn-peers` FFI words work at `ok>`. |
| **E-C: HCP dispatcher + dual-mode** | Realize §11.3 — sigil framing, tags, error codes, CAPS/HELP, serialized TX writer, event pump; `FORTH` escape | LLM-style transcript (§11.3.7) drives chat over USB-CDC; `FORTH` drops to REPL and back. |
| **E-D: Crypto + identity** | §11.1: KDF, epoch keys, AES-CCM with nonce discipline + NVS counter blocks, Ed25519 identity, signed/allow-listed admin | `mn-join` private channel; two channels isolate; replay rejected; only allow-listed key can `set-channel`. |
| **E-E: Forth automation** | `mn-on-chat`/`mn-on-cmd` marshaled to Forth task; persist + autorun user scripts | A node runs user Forth in reaction to an inbound M2M command (e.g. drives a GPIO). |
| **E-F: Extra transports + SDK** | BLE-GATT binding (§11.2.1) + bonding; WebSocket; reference host SDK (TS/Swift/Python) | Phone/WebBluetooth client joins a channel and chats via the same HCP grammar. |
| **E-G: Scale + engine + hardening** | Trickle suppression (§11.6), SED catch-up `GET /magnet/recent`; swap stub for full ESP32forth v7.0.8.0 **iff** the stub blocks real scripts | 32+ node soak; documented failover/throughput. |

### 12.8 Risks & mitigations

| Risk | Mitigation |
|------|-----------|
| **C6 RAM pressure** (Forth heap + OpenThread + mbedTLS, no PSRAM relief possible) | Shrink Forth heap to 48–64 KB; claim it first (§12.5); measure with `mem` every phase; treat E-A as a go/no-go gate. |
| **IDF OpenThread is greenfield here** (fleet used Arduino `OThreadCLI`, no `esp_openthread` precedent) | E-A spike with `CONFIG_OPENTHREAD_CLI=y` for fast bringup, then move to native CoAP C API; lean on Espressif `ot_*` examples. |
| **Stub Forth engine maturity** (likely no `s"`, arrays, dictionary words FFI needs) | Extend stub minimally in E-A; defer full v7.0.8.0 port to E-G unless it blocks earlier. |
| **Forth run from RX callback context** | Never call `forth_eval` from the Thread/lwIP callback — marshal the event to the Forth task via a queue. |
| **Interleaved output corrupting the line protocol** | Single serialized TX writer for responses + events + Forth output. |
| **Temptation to put crypto in Forth** | Architecturally barred — Forth has no key-material words; KDF/AEAD/sign are C-only (decision §12.0). |
| **IDF version drift → efuse collision** | Keep the `magnet_base` 5.3.1 pin; add no 5.4-forcing deps. |

### 12.9 Open decisions

1. **Devkit/board variant**: bare ESP32-C6-DevKitC, XIAO ESP32-C6, or m5 nano-C6? (Pin maps differ;
   XIAO pad↔GPIO translation applies.)
2. **Script persistence medium**: NVS blob vs SPIFFS/LittleFS for user Forth autorun scripts.
3. **BLE on C6 alongside Thread**: provisioning-only-then-torn-down (default) vs resident for live
   phone/headset use — decide given 2.4 GHz coexistence and the RAM budget.
4. **Ed25519 source**: mbedTLS Edwards support vs adding the libsodium component (flash/RAM cost).
5. **Full ESP32forth port timing** — only if the stub blocks real automation scripts (E-G).

### 12.10 Implementation status

**E-Phase B code landed (2026-07-30): builds clean.** `firmware-idf/` now implements the
plaintext MagNET core on top of the E-A scaffold: v2.1 envelope pack/unpack
(`magnet_envelope.{h,c}`), unified single-role Thread bringup (fixed dev dataset —
channel 24 / PAN 0x4d4e / the v0.0.6 well-known key, so mixed benches with the Arduino
prototype mesh together), CoAP `/magnet` resource, `ff05::abcd` multicast subscribe,
`CHAT` (NON multicast) + `DM <ipv6>` (CON unicast) over HCP and `mn-chat`/`mn-dm` from
Forth, RX → `!CHAT`/`!DM`/`!PEER_JOIN`/`!ROLE` events, peer table, and duplicate-drop
cache. Footprint: flash 792 KB (28.8%), static RAM 104.7 KB (32.0%) — +5 KB/+4 KB over
the E-A spike. **Deadlock discipline**: OT callbacks post into an event-pump queue
(§12.4) and never take the TX mutex — a Forth task holding the TX mutex while sending
(which takes the OT lock) can no longer ABBA-deadlock against an OT-context emit.
**E-B exit test PASSED on hardware (2026-07-30, 3× M5NanoC6):** runtime RAM go/no-go
**GO** (245 KB free with OT up + 64 KB Forth heap reserved); 3-node mesh with one
leader; multicast chat in every direction; DM isolation; **leader failover** verified
by physically unplugging the leader (survivors re-elect in ~2.5 min via partition
merge; chat continues; old leader rejoins as router). Selftest (envelope roundtrip,
event pump, CoAP loopback to own ML-EID) passes. See `firmware-idf/README.md` for
the full scorecard and bench gotchas. **E-B is closed.**

**E-C landed and validated (2026-07-30, fw 0.3.0-ec, 16/16 on the 4-node bench):**
`NAME` + system/announce (Type 1, ns 0 cmd 0x02) so `!CHAT`/`PEERS` show display
names (Open Q2 at E-C level, name persisted in NVS); `MODE TERSE|HUMAN`;
`SUB`/`UNSUB` event classes (filters host emission only — mesh processing and
counters unaffected); `STATUS`/`WHOAMI` return machine-readable `+OK key=value`
lines; DEGRADED queueing (max 4, `+QUEUED n`, replay + `!RESULT` on READY —
code-complete, not yet exercised on hardware since it requires inducing
DEGRADED); and the §4.9 **token-bucket rate limit** on host multicast (burst 8,
refill 10/s, `-ERR E_RATE_LIMITED`; STRESS bypasses it by design).

**E-D part 1 landed and validated (2026-07-31, 15/15 on the 4-node bench) — the
mesh is encrypted.** Implemented per §11.1: credential paths A (qr:), B
(PBKDF2, 100k iters — tune toward 1 s), C (≥12-word seed phrase, HKDF only)
auto-detected; root_secret → selector / ff05::derived-mcast / epoch-0 key;
AES-128-CCM with the exact §11.1.5 nonce and 16-byte-header AAD; **persistent
counter with NVS block-reserve (1024), fail-closed**; §11.1.6 monotonic
high-water replay table (TOFU, LRU 16); `CHANNEL SET/JOIN <cred>` +
`mn-set-channel`; root cached in NVS (no re-stretch on reboot); default
"magnet" channel emits `!WARN default-channel-insecure`. Verified on hardware:
same passphrase ⇒ same selector on independent nodes; cross-channel isolation
both directions; zero MIC failures; channel survives reboot. **Decision (was
§12.9 Q4): identity signatures use deterministic ECDSA P-256** — IDF 5.3.1's
mbedTLS has no Ed25519; P-256 is native + C6 HW-accelerated. Read
Ed25519 references in §11.1 as ECDSA-P256 (raw r‖s, 64 B) going forward.
**Remaining E-D part 2:** device keypair + device_id from pubkey hash, signed
ADMIN commands + allow-list, epoch rotation.

#### E-Phase A spike scaffold — status (historical)

Scaffold exists at **`firmware-idf/`** in this project. It pulls the ESPIDFORTH
`forth` component in via `EXTRA_COMPONENT_DIRS` (not vendored), adds a
`components/magnet/` C component (state + serialized TX writer, dual-mode HCP/Forth
dispatcher, `mn-*` FFI vocabulary, gated `esp_openthread` bringup), and a `src/main.c`
that follows the §12.5 bringup order. Build with `pio run -e esp32c6`; a
`esp32c6_noot` env compiles OpenThread out for a footprint baseline. See
`firmware-idf/README.md` for go/no-go criteria.

Required upstream change made: `forth_set_io()` added to ESPIDFORTH
(`components/forth/forth_core.{h,cpp}`) so `forth_eval()` output routes without
entering the blocking `forth_repl()` (the dual-mode requirement from §12.4).

**Build result (2026-06-15, esp32c6, IDF 5.3.1): builds clean.** Static footprint
with OpenThread FTD + mbedTLS + Forth + MagNET: **Flash 787 KB / 2.625 MB factory
(28.6%)**, **static RAM 100.7 KB / 320 KB (30.8%)**. Flash is a non-issue. Static
RAM leaves ~227 KB; minus the 64 KB Forth heap ≈ 160 KB for OpenThread runtime +
mbedTLS + task stacks. The static number is NOT the verdict — OpenThread's heavy
allocations are at runtime (attach, message pool, DTLS); the on-hardware
`# heap[after openthread_start]` free-RAM line is the real go/no-go.

**Storage finding (the pre-commit check):** ESPIDFORTH's default
`CONFIG_PARTITION_TABLE_SINGLE_APP` gives a **1 MB** factory app. The 787 KB image
fits it, but with no OTA and no growth room — hence the custom `partitions.csv`
(2.625 MB factory + 960 KB LittleFS scripts). The devkit is an **8 MB / 320 KB-RAM**
C6, so mesh OTA (Open Q4) is viable on this hardware via the dual-slot table in
`firmware-idf/README.md` — no BOM change needed. Note the RAM ceiling is **320 KB**
(PlatformIO-reported usable DRAM), tighter than the 512 KB SRAM total — size the
Forth heap against 320 KB.

**Two PlatformIO build gotchas (fixed in the spike, generally applicable):**
(1) Don't `set(EXTRA_COMPONENT_DIRS)` in the root CMakeLists — it clobbers
PlatformIO's `src/`→`main` mapping ("Couldn't find the main target"); symlink
external components into `components/` instead. (2) The IDF `openthread` component's
space-containing `OPENTHREAD_BUILD_DATETIME` define is mis-quoted by PlatformIO and
breaks every `openthread/*.cpp` (`invalid digit "9" in octal constant`); a pre-build
script (`patch_openthread_datetime.py`) sanitizes it idempotently.

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
| PBKDF2 (passphrase stretch, §11.1.2) | `mbedtls_pkcs5_pbkdf2_hmac_ext()` |
| Ed25519 keypair (identity, §11.1.7) | `mbedtls_pk` Ed25519 / or `crypto_sign_keypair` (libsodium component) |
| Ed25519 sign / verify | `mbedtls_pk_sign()` / `mbedtls_pk_verify()` (or libsodium `crypto_sign_detached`) |
| CSPRNG (root_secret, keys) | `esp_fill_random()` / `mbedtls_ctr_drbg_random()` |
| NVS counter persistence (§11.1.6) | `nvs_set_u32()` / `nvs_get_u32()` (block-reserve pattern) |
| BLE-GATT HCP server (§11.2.1) | NimBLE (`esp-nimble`) GATT svc/chr |

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
