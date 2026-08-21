# Hanasu over Thread against Hanasu over Bluetooth Mesh

The same firmware, above two transport backends, measured on the same bench.

## 1. How much faster Thread is, by measure

There is no single ratio — it depends entirely on what is being measured.

| measure | Thread | BLE Mesh | Thread faster by |
|---|---|---|---|
| bulk transfer, 16,384-byte file | 5,176 bytes/second | 67.9 bytes/second | 76× |
| bulk transfer, 4,096-byte file | 4,470 bytes/second | 69.7 bytes/second | 64× |
| bulk transfer, 1,024-byte file | 2,375 bytes/second | 68.0 bytes/second | 35× |
| delivered rate at saturation, 16-byte payload | 57.4 messages/second | 2.2 messages/second | 26× |
| delivered rate at saturation, 48-byte payload | 35.6 messages/second | 1.4 messages/second | 25× |
| highest offered rate still lossless | 20 messages/second | 2 messages/second | 10× |
| one-way p50 latency, 340-byte payload | 82.0 ms | 1,009 ms | 12× |
| one-way p50 latency, 48-byte payload | 56.7 ms | 252.5 ms | 4.5× |
| one-way p50 latency, 8-byte payload | 41.5 ms | 154.9 ms | 3.7× |

The ratio grows with message size because BLE Mesh pays per 12-byte segment
while Thread is flat until fragmentation. The small-file transfer ratios are
lower only because Thread has not reached steady state at 1,024 bytes; BLE Mesh
is already flat at roughly 68 bytes/second.

The raw radio rates run the other way. BLE's PHY is 1 Mbit/second against
802.15.4's 250 kbit/second, so Thread's higher application throughput comes
despite a radio four times slower — BLE Mesh spends that radio on three
advertising channels, small unconnected PDUs, per-message overhead and flooding
duplicates. The gap belongs to the mesh bearer rather than to Bluetooth: a GATT
connection on the same radio, with a roughly 244-byte MTU, would exceed Thread
comfortably.

## 2. Measured performance in detail

- Unicast, 48-byte payload at 2 messages/second: Thread 56.7 ms one-way p50 at 100 % delivery; BLE Mesh 252.5 ms at 100 %. 24 samples each.
- Transport floor: Thread 8-byte DM at 41.5 ms, 100 %; BLE Mesh 8-byte raw probe carrying no Hanasu envelope at 50.6 ms, 100 %. Different measurements, same order of magnitude — the radio itself is not the limit.
- Delivery against offered rate, 48-byte unicast: Thread 100 % at 1, 2, 5, 10 and 20 messages/second and 69 % at 30; BLE Mesh 100 % at 1 and 2, then 53 % at 5, 33 % at 10, 20 % at 20, 14 % at 30.
- Group multicast, roughly 8 bytes of text: Thread 100 % at 1, 5 and 10 messages/second, 42–45 ms; BLE Mesh 93 %, 33 % and 20 % at the same rates, 133–153 ms. BLE Mesh group traffic is never acknowledged by anything.
- Bulk transfer, byte-intact on both radios: Thread 5,176 bytes/second for 16,384 bytes and 5,308 for 65,536; BLE Mesh 67.9 bytes/second for 16,384 bytes, flat from 1,024 upward. A 32,768-byte photo is about 6 seconds against about 12 minutes.
- Overload, 16-byte payload over 5 seconds: Thread offered 285 per second, accepted 68, delivered 57, with the refusal returned to the caller; BLE Mesh offered 468, accepted 468, delivered 2.2 — the send call reported success for every frame while 99.5 % went nowhere.
- Footprint: the BLE Mesh image is smaller at 122,976 bytes static RAM and 722,173 flash, against 137,212 and 851,553. Free heap on hardware favours Thread, 205,660 bytes against 184,000, because the mesh stack allocates advertising buffers and segmentation contexts dynamically.

## 3. Frame size and segmentation: where the difference comes from

- Lossless single-frame payload is roughly 56 to 62 bytes on Thread and 8 bytes on BLE Mesh, being an 11-byte access payload less a 3-byte vendor opcode. Hanasu's envelope alone is 16 bytes, so on BLE Mesh every real message segments, at 12 bytes per segment with a 32-segment ceiling.
- The Thread figure still needs pinning down. The design proposal's 62 bytes assumes the earlier 10-byte envelope; the shipped envelope is 16, which puts it nearer 56. A payload sweep for the fragmentation knee would settle it and has not run.
- The cliff is sharp: an 8-byte probe costs 50.6 ms, a 9-byte probe 84.9 ms. One byte over the line is 1.7×.
- Across 8 to 340 bytes of payload, Thread runs 41.5 to 82.0 ms at 100 % delivery throughout; BLE Mesh runs 154.9 to 1,009 ms with delivery falling to 50 % above 96 bytes. Past roughly ten segments a message stops completing reliably.
- The envelope cost is visible arithmetically: an 8-byte DM is 32 bytes on the wire after header and MIC, and measured 154.9 ms against the 32-byte raw probe's 151.3 ms.
- Segmented latency is not linear in segment count — 84.9 ms at one segment, 87.6 at two, 151.3 at three, 220.5 at six. The knee between two and three coincides with the SAR Segments Threshold of 3, which governs when the receiver starts acknowledging.
- Thread's per-message cost is per hop on a dedicated channel; BLE Mesh's is per transmission and network-wide, since managed flooding has every relay in range retransmit.
- The BLE Mesh transfer figure is this transfer engine on that radio, not the radio's ceiling: each 96-byte chunk becomes 11 segments after headers, and the engine adds a 250 ms tick and an eight-chunk window acknowledgment cycle.
- BLE Mesh returned no backpressure of any kind, which is the one gap invisible to the application rather than merely slow.
- Fourteen of the sixteen envelope bytes duplicate fields the mesh network layer already carries — SRC, SEQ plus IV Index, NID and AID, SegN and SegO, SeqAuth. An 8-byte frame measures 45–50 ms at 100 % delivery and sustains 30 messages/second, but 5–6 usable bytes fits only ticks and telemetry, never chat text, transfers or signed frames.

## 4. Addressing, routing, role assignment, provisioning, power and ecosystem reach

- Thread is a routed IPv6 network: devices carry addresses, routers carry routing state, and any UDP protocol works on top, which is why CoAP request/response and blockwise transfer were available off the shelf.
- BLE Mesh is managed flooding with no routing state in the base specification. Reachability is TTL-bounded, maximum 127, and the addressable unit is a model on an element rather than a device.
- Its native shape is publish/subscribe of small state to group or virtual addresses, with reliability from redundancy rather than acknowledgment. There is no request/response resource model, which is why Hanasu's RECENT catch-up has no equivalent.
- Address space: unicast 0x0001–0x7FFF, virtual 0x8000–0xBFFF, group 0xC000 upward, all allocated by the provisioner. Thread allows 32 active routers with up to 511 children each.
- Thread's Leader is elected by the stack, not the application. OpenThread's MLE carries election, router promotion and partition merging; the Hanasu backend has six bringup calls and no election code, reading the role back only to report it. The API to force leadership is documented as a testing override that makes a production application non-compliant.
- Recovery from Leader loss is automatic and needs no application code, but is not fast by construction and was not tested here: detection is bounded by Trickle-controlled MLE advertisement intervals of 32 to 256 seconds and a default 120-second network ID timeout.
- BLE Mesh requires a provisioner to issue the NetKey, a unicast address, the IV index and a device key, then bind an AppKey and add subscriptions. Every step was needed during bringup, and without them a node can neither send nor receive an application message.
- A provisioner cannot configure itself — a self-addressed Config Set returned −22 (−EINVAL), since it holds device keys for the nodes it provisioned and none for itself.
- Thread needs no ceremony at all: identical network key, channel and PAN ID everywhere, then self-organising attach, measured at roughly 30 seconds after a reset at 100 %. Hanasu's passphrase-derived channel model has nowhere to live on BLE Mesh.
- Low power differs in kind. Thread has sleepy end devices natively with a parent buffering; BLE Mesh needs a Friend node, and a node without one scans more or less continuously.
- Hanasu applies its own AEAD, so BLE Mesh's double AES-CCM is a redundant third layer whose only product-visible effect is the provisioning ceremony.
- Off-mesh reach differs too: Thread needs a Border Router, a standard product category; BLE Mesh needs a bespoke gateway. For phones, the BLE Mesh GATT proxy is reachable by any handset, though the app must implement the mesh stack.
- Interoperability lives at different layers — Matter over Thread, SIG models on BLE Mesh. Using a vendor model, as Hanasu does, forfeits BLE Mesh's interop as completely as skipping Matter forfeits Thread's, and Matter cannot run over BLE Mesh at all.

## 5. What was measured, and how

- Two ESP32-C6 boards one hop apart, 2026-08-19, driven through the chat-bench bridge so both timestamps come from one process on one clock. Latency is one-way host-to-host, not a radio-level figure.
- Both radios run the same firmware above the six-function transport interface; only the backend file differs, which is what makes the comparison meaningful.
- A host-to-node-to-host PING round trip measures 10.7 ms, so roughly 5 ms of each one-way figure is USB and bridge overhead, identically on both sides.
- The Bluetooth side is Bluetooth Mesh Protocol 1.1, the current mesh specification, running IDF's enhanced SAR transport with a SAR Configuration Server and Client on every node, each configuring its own timing — confirmed by the SAR server's status reply rather than a build flag.
- SAR state, specification default then value used, in milliseconds: segment interval 20 to 10; unicast retransmission interval 200 to 50; increment 50 to 25; multicast interval 100 to 50; receiver segment interval 20 to 10. Acknowledgment delay increment 1 to 0. Retransmission counts left at or above default, unicast 7 and multicast raised from 1 to 2.
- Network profile: one network transmit at 10 ms spacing, relaying off, beacons off, TTL 2, continuous scanning, 10 simultaneous outgoing segmented messages, 60 advertising buffers.
- Transfer geometry is per transport — 336-byte chunks with a 32-chunk window on Thread, 96-byte chunks with an 8-chunk window on BLE Mesh — and the node announces it in its XFER BEGIN reply. A 336-byte chunk is one fragmented CoAP payload on Thread and a 29-segment SAR message on BLE Mesh.
- "BLE 5 mesh" does not mean mesh over Bluetooth 5's headline features. The mesh advertising bearer is legacy 31-byte advertising on the 1M PHY in 1.1 exactly as in 1.0.1, and carrying mesh PDUs over extended advertising is a vendor extension outside the specification, so the 8-byte unsegmented ceiling is the standard rather than a setting. Extended advertising is therefore off.
- Mesh 1.1 features left off because none of them changes a single-hop bench: Directed Forwarding, Subnet Bridge, Remote Provisioning, Private Beacons, Op Codes Aggregator, Large Composition Data.
- Thread side: the standard esp32c6 build, dataset committed at build time, router eligibility on, no commissioner and no operator step.

## 6. How far this should be trusted

- This is one test on one desk over a single evening. It compares two builds of one firmware under identical conditions; it is not a characterisation of either radio in general.
- Two boards means every path is one hop. No multi-hop, no relay behaviour, no node scaling, and no way to evaluate Directed Forwarding — the one Mesh 1.1 feature that might narrow the gap.
- Leader failover was not tested on either side. What exists is attach time after reset, which is a different thing.
- No current rig, so no power-per-message figures. The boards do not move, so no range or mobility data. A desk in a flat is not an anechoic chamber.
- The BLE Mesh results are consistent with published testbed work: an independent nRF52840 study sweeping 1 to 10 packets per second reports flooding delivery collapsing from about 5 packets per second, and the documented SAR defaults of 7 unicast retransmissions against 1 multicast explain the group-versus-unicast gap.
- The Thread performance numbers have no independent corroboration; published OpenThread benchmarks could not be retrieved. The Thread architecture claims did check out against Thread Group and vendor documentation.
- Message types 2 (M2M_RESP) and 4 (ACK) are declared in the envelope specification and emitted by nothing — a firmware gap on both radios, not a transport difference.
- A single ESP32-C6 cannot run both stacks, since Espressif's coexistence table forbids 802.15.4 scan against any BLE state, so this is a fork of the image and never a live A/B on one board.
- Open follow-ups: measure the Thread fragmentation knee to replace the inherited 56-to-62-byte estimate, and re-test with the SAR Segments Threshold moved, since the two-to-three-segment knee suggests it is worth tuning.

## 7. Which applications each architecture suits, and the movable-leader question

- Routing against flooding is the split. Thread spends state to buy predictable per-hop cost; BLE Mesh spends airtime to avoid holding state, so capacity is shared network-wide and falls as traffic rises rather than as topology grows.
- They scale along different axes: BLE Mesh tolerates node count and degrades under traffic, Thread tolerates traffic and is bounded by router and child limits.
- BLE Mesh's 8-byte unit is a different design target, not a small version of Thread's. It suits idempotent state that can be repeated rather than acknowledged, which is what lighting and sensor deployments send.
- Thread's IPv6 substrate makes application protocols a library choice; BLE Mesh's model and opcode abstraction makes them a design exercise.
- Acknowledged group delivery exists on one side only. Thread's multicast measured lossless at its own 10 messages/second ceiling; BLE Mesh group traffic has no acknowledgment mechanism at any rate.
- Interactive and streaming traffic is Thread-shaped. Anything conversational, real-time above a few messages per second, or longer than about ten BLE Mesh segments falls off the reliability cliff rather than merely slowing.
- Bulk data is categorically different rather than incrementally slower, and the standards-track answer on the BLE side is a separate BLOB transfer model rather than the same path made faster.
- Ecosystem reach is asymmetric in opposite directions: Thread reaches IP and Matter through a Border Router, BLE Mesh reaches phones through a proxy any handset can connect to. Neither reaches both.
- Role assignment is the sharpest contrast. Thread's stack elects its Leader, promotes routers and merges partitions with no application involvement; BLE Mesh has no protocol-level equivalent for its provisioner role, so any coordination is application work.
- A fixed provisioner is a build choice rather than a stack limit. These images pick the role at compile time, but ESP-BLE-MESH's fast provisioning can promote an already-provisioned node into a temporary provisioner at runtime, handing it a unicast address range.
- Multiple provisioners can coexist by partitioning that address space, but that is delegation rather than election. There appears to be no election, no automatic failover and no handover protocol in the specification.
- Provisioner state is enumerable in the SDK's node structure, including each node's device key, so handover is implementable by exporting and importing it, and the Bluetooth SIG has a JSON configuration-database format for that. Nothing in the public API does it.
- A network with no ceremony at all would need the internal bt_mesh_prov_complete hook, which exists but is not public API; the public node-side key functions require the device to be provisioned already, so they cannot bootstrap.
- Losing the provisioner leaves existing nodes talking but closes the network: no new members, no reconfiguration, because nothing else holds their device keys.
- Application fit follows: dense, low-duty, small-state control and telemetry sit naturally on BLE Mesh; conversational, interactive, file-bearing or IP-connected applications sit naturally on Thread.
