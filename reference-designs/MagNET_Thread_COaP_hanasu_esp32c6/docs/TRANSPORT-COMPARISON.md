# Hanasu over Thread against Hanasu over Bluetooth Mesh

A bench comparison of the same firmware on two radios, measured 2026-08-19 on
two ESP32-C6 boards one hop apart, driven through the chat-bench bridge so both
timestamps come from one process on one clock.

The Bluetooth side is **Bluetooth Mesh Protocol 1.1**, the current mesh
specification, running IDF's enhanced SAR transport on Bluetooth 5 silicon
(ESP32-C6). It is worth stating plainly that "BLE 5 mesh" does not mean mesh
carried over Bluetooth 5's headline features: the mesh advertising bearer is
legacy 31-byte advertising on the 1M PHY in 1.1 exactly as in 1.0.1, and
carrying mesh PDUs over extended advertising is a vendor extension outside the
specification. The 1.1 features that do apply here are enabled; the ones that
only matter beyond one hop are listed in section 4 as deliberately off.

This is one test on one desk over a single evening. It compares two builds of
one firmware under identical conditions; it is not a characterisation of either
radio in general, and every figure below should be read that way.

## 1. Measured performance: latency, delivery, throughput and footprint

- Unicast 48-byte payload at 2 messages/second: Thread 56.7 ms one-way host-to-host p50 latency at 100 % delivery (24 samples); BLE Mesh 252.5 ms one-way p50 at 100 % delivery (24 samples).
- Transport floor: Thread 8-byte payload DM at 41.5 ms one-way p50, 100 % delivery; BLE Mesh 8-byte raw probe with no Hanasu envelope at 50.6 ms one-way p50, 100 % delivery. Different measurements, same order of magnitude — the radio itself is not the limit.
- Sustained rate, 48-byte payload unicast, delivery percentage at each offered rate in messages/second: Thread 100 % at 1, 2, 5, 10 and 20 messages/second, 69 % at 30; BLE Mesh 100 % at 1 and 2, 53 % at 5, 33 % at 10, 20 % at 20, 14 % at 30.
- Group multicast CHAT with roughly 8 bytes of text: Thread 100 % delivery at 1, 5 and 10 messages/second, 42–45 ms one-way p50; BLE Mesh 93 % at 1 message/second, 33 % at 5, 20 % at 10, at 133–153 ms one-way p50. BLE Mesh group traffic is never acknowledged by anything.
- Bulk transfer, application-payload throughput, every transfer verified byte-intact on both radios: Thread 5,176 bytes/second for a 16,384-byte file (3.17 seconds) and 5,308 bytes/second for 65,536 bytes (12.35 seconds); BLE Mesh 67.9 bytes/second for 16,384 bytes (241 seconds), flat across 1,024 to 16,384 bytes. Ratio 78×. A 32,768-byte photo takes about 6 seconds on Thread and about 12 minutes on BLE Mesh.
- Overload, using the firmware's own STRESS verb with a 16-byte payload, over 5 seconds: Thread 1,425 attempted, 340 accepted, 287 delivered — 285 offered, 68 accepted, 57 delivered per second, with the refusal returned to the caller; BLE Mesh 2,339 attempted, 2,339 accepted, 11 delivered — 468 offered, 468 accepted, 2.2 delivered per second, so the send call reported success for every single frame while 99.5 % went nowhere.
- Image footprint in bytes: BLE Mesh is smaller at 122,976 bytes static RAM and 722,173 bytes flash, against Thread's 137,212 bytes static RAM and 851,553 bytes flash. Free heap measured on hardware favours Thread at 205,660 bytes against 184,000 bytes, a difference of about 22 kilobytes, because the mesh stack allocates advertising buffers and segmentation contexts dynamically.

## 2. Frame size, segmentation and airtime: the mechanisms that appear to produce those numbers

- Lossless single-frame payload: Thread roughly 56 to 62 bytes; BLE Mesh 8 bytes, being an 11-byte access payload less a 3-byte vendor opcode, both verified in the ESP-IDF headers we build against. Hanasu's envelope alone is 16 bytes, so on BLE Mesh every real message segments, always, at 12 bytes per segment with a 32-segment ceiling.
- The Thread figure still needs pinning down: the design proposal's 62-byte number is derived with a 10-byte envelope from the earlier revision, and the shipped envelope is 16 bytes, which puts the budget nearer 56 — while an outside estimate of the raw 802.15.4 payload ceiling suggests the figure may be conservative in the other direction. A payload sweep to find the fragmentation knee is the measurement that settles it, and it has not run yet.
- The segmentation cliff, measured with the raw probe at 5 messages/second: 8 bytes gives 50.6 ms one-way p50, 9 bytes gives 84.9 ms. One byte over the line costs 1.7×.
- Latency against payload, one-way p50 across 8 to 340 bytes of payload: Thread 41.5 ms to 82.0 ms, flat, at 100 % delivery for every size; BLE Mesh 154.9 ms to 1,009 ms, with delivery falling from 100 % to 50 % above 96 bytes — past roughly ten segments a message stops completing reliably.
- The envelope cost shows up arithmetically: an 8-byte DM is 8 payload plus a 16-byte header plus an 8-byte MIC, so 32 bytes on the wire, and it measured 154.9 ms against the 32-byte raw probe's 151.3 ms. The envelope is what turns a small message into a medium one.
- Segmented latency is not linear in segment count. One segment costs 84.9 ms, two 87.6 ms, three 151.3 ms, six 220.5 ms — a knee between two and three segments that coincides with the SAR Segments Threshold of 3, which is the state governing when the receiver starts acknowledging.
- Thread's per-message cost is per hop on a dedicated 250 kbit/second PHY; BLE Mesh's is per transmission, network-wide, because managed flooding means every relay in range retransmits — the stock profile's three network transmits at 20 ms spacing is three times the airtime for redundancy a dense bench does not need.
- The BLE Mesh transfer figure is this transfer engine on that radio, not the radio's ceiling: each 96-byte chunk carries 5 bytes of transfer header, 16 of envelope and 8 of MIC, making 11 segments, and the engine adds a 250 ms tick and an eight-chunk window acknowledgment cycle on top.
- BLE Mesh returned no backpressure of any kind, which is the one gap invisible to the application rather than merely slow.
- Fourteen of the sixteen envelope bytes duplicate fields the BLE Mesh network layer already carries: SRC for sender_id, SEQ plus IV Index for counter, NID and AID for selector, SegN and SegO for the fragment fields, SeqAuth for msg_id. An 8-byte frame measures 45–50 ms one-way p50 at 100 % delivery and sustains 30 messages/second, but at 5–6 bytes of usable payload it fits only game ticks and telemetry, never chat text (8 characters), transfers or signed frames.

## 3. Addressing, routing, role assignment, key provisioning, power modes and ecosystem reach

- Thread is a routed IPv6 network — devices carry addresses, routers carry routing state, and an application can use any UDP protocol on top, which is why CoAP request/response and blockwise transfer were available off the shelf.
- BLE Mesh is managed flooding with no routing state in the base specification: reachability is TTL-bounded, with the maximum TTL of 127 confirmed in the SDK headers, and the addressable unit is a model on an element rather than a device.
- BLE Mesh's native application shape is publish/subscribe of small state to group or virtual addresses, with reliability supplied by redundancy rather than acknowledgment. There is no request/response resource model, which is why Hanasu's RECENT catch-up has no equivalent and the firmware says so instead of failing silently.
- Address space, from the SDK's own macros: unicast 0x0001 to 0x7FFF, virtual 0x8000 to 0xBFFF, group 0xC000 upward, all allocated by the provisioner. Thread's limits are 32 active routers with up to 511 children each.
- Thread's Leader role is elected by the stack, not by the application. OpenThread's MLE implementation carries the election, router promotion and partition merging; Hanasu's Thread backend contains six bringup calls and no election code at all, and reads the role back only to report it. The API to force leadership exists but is documented as a testing override that makes a production application non-compliant.
- Recovery from Leader loss is automatic and needs no application code, but the timescale is unmeasured and not fast by construction: detection is bounded by MLE advertisement intervals, which are Trickle-controlled between 32 and 256 seconds, and by a default 120-second network ID timeout. It was not tested here — the only related observation is a node coming up as child on one boot and router on another with identical firmware.
- BLE Mesh requires a provisioner: something must issue the NetKey, a unicast address, the IV index and a per-node device key, then bind an AppKey to each model and add group subscriptions, or the node can neither send nor receive an application message. Every step of that chain was needed during bringup.
- A provisioner cannot configure itself: a self-addressed Config Set returned error −22 (−EINVAL) on this bench, because it holds device keys for the nodes it provisioned and none for itself, and no public API supplies one.
- Thread needs no provisioning ceremony at all — identical network key, channel and PAN ID on every node, then self-organizing attach, measured at roughly 30 seconds to re-attach after a reset at 100 % success. Hanasu's passphrase-derived channel model has nowhere to live on BLE Mesh.
- Low-power architecture differs in kind: Thread has sleepy end devices natively, with a parent buffering for them; BLE Mesh needs a Friend node for its Low Power Nodes, and any node without one must scan more or less continuously — the profile used here runs scan interval equal to scan window.
- Hanasu applies its own AEAD, so BLE Mesh's double AES-CCM at network and application layers is a redundant third crypto layer whose only product-visible effect is the provisioning ceremony.
- Off-mesh reachability differs in kind too: Thread needs a Border Router, a standard product category; BLE Mesh needs a bespoke gateway application. For phones, BLE Mesh's GATT proxy is at least reachable by any BLE-capable handset, though the app must implement the mesh stack itself.
- Interoperability exists in both but at different layers: Matter is a certified application layer over Thread; BLE Mesh's interop lives in the SIG models. Using a vendor model, as Hanasu does, forfeits BLE Mesh's interop as completely as skipping Matter forfeits Thread's — and Matter cannot run over BLE Mesh at all.

## 4. Exact build and stack settings the BLE Mesh figures were taken under

- Bluetooth Mesh 1.1 with the enhanced SAR transport (transport.enh.c), plus a SAR Configuration Server and Client on every node, each node configuring its own timing — confirmed by the SAR server's own status reply rather than by a build flag.
- SAR state, specification default then tested value, all in milliseconds: segment interval 20 to 10; unicast retransmission interval 200 to 50; unicast retransmission increment 50 to 25; multicast retransmission interval 100 to 50; receiver segment interval 20 to 10. Acknowledgment delay increment 1 to 0 (a unitless state index). Every retransmission count left at or above its default — unicast 7 retransmissions, multicast raised from 1 to 2.
- Network profile: one network transmit at 10 ms spacing, relaying off, beacons off, TTL 2, scanning continuous with scan interval equal to scan window, up to 10 simultaneous outgoing segmented messages, and 60 advertising buffers.
- Transfer geometry is per transport: 336-byte chunks with a 32-chunk window on Thread, 96-byte chunks with an 8-chunk window on BLE Mesh, announced by the node in its XFER BEGIN reply. A 336-byte chunk is one fragmented CoAP payload on Thread and a 29-segment SAR message on BLE Mesh.
- Extended advertising is deliberately off: the mesh advertising bearer is legacy 31-byte advertising PDUs on the 1M PHY, and carrying mesh PDUs in BLE 5 extended advertising is a vendor extension outside Mesh 1.1, so the 8-byte unsegmented ceiling is the standard rather than a setting.
- Mesh 1.1 features left off because none of them changes a two-board single-hop bench: Directed Forwarding, Subnet Bridge, Remote Provisioning, Private Beacons, Op Codes Aggregator, Large Composition Data.
- Thread side: the standard `esp32c6` build, dataset committed at build time, router eligibility on, no commissioner and no operator step.

## 5. How far these results should be trusted, and what the bench cannot show

- Every latency figure is one-way host-to-host, with both timestamps taken in one process on one clock, not a radio-level number. A host-to-node-to-host PING round trip measures 10.7 ms, so roughly 5 ms of each one-way figure is USB and bridge overhead, identically on both radios.
- The BLE Mesh results are consistent with published testbed work: an independent nRF52840 study sweeping 1 to 10 packets per second reports flooding delivery collapsing from about 5 packets per second, which is where these fall off, and the vendor-documented SAR defaults of 7 unicast retransmissions against 1 multicast explain the group-versus-unicast gap.
- The Thread performance numbers have no independent corroboration — published OpenThread latency and throughput benchmarks could not be retrieved, so those rest on this bench alone. The Thread architecture claims did check out against Thread Group and vendor documentation.
- Two boards only, so every path is one hop: no multi-hop, no relay behaviour, no node scaling, and Directed Forwarding — the one Mesh 1.1 feature that might narrow the gap — is untestable here.
- Leader failover was never tested on either side. What exists is attach time after reset, which is a different thing.
- No current-measurement rig, so no power-per-delivered-message figures in milliamps or joules. The boards do not move, so no range or mobility data. A desk in a flat is not an anechoic chamber.
- Message types 2 (M2M_RESP) and 4 (ACK) are declared in the envelope specification and emitted by nothing, which is a firmware gap on both radios rather than a transport difference.
- A single ESP32-C6 cannot run both stacks, since Espressif's coexistence table forbids 802.15.4 scan against any BLE state, so this is a fork of the image and never a live A/B on one board.
- Follow-up already identified: measure the Thread fragmentation knee to replace the inherited 56-to-62-byte estimate, and re-test with the SAR Segments Threshold moved, since the two-to-three-segment knee suggests it is worth tuning.

## 6. Which mesh applications each architecture is built for, and whether the BLE side can have a movable leader

- Routing versus flooding is the architectural split. Thread spends state to buy predictable per-hop cost; BLE Mesh spends airtime to avoid holding state, so its capacity is shared network-wide and falls as traffic rises rather than as topology grows.
- The two scale along different axes. BLE Mesh tolerates node count and degrades under traffic; Thread tolerates traffic and is bounded by router and child limits.
- BLE Mesh's 8-byte unsegmented unit is not a small version of Thread's, it is a different design target. It suits idempotent state updates that can be repeated rather than acknowledged, which is what lighting and sensor deployments send.
- Thread's IPv6 substrate makes application protocols a library choice; BLE Mesh's model and opcode abstraction makes them a design exercise, with no request/response, no resource model and no blockwise transfer to inherit.
- Acknowledged group delivery exists on one side only. Thread's rate-limited multicast measured lossless at its own 10 messages/second ceiling; BLE Mesh group traffic has no acknowledgment mechanism at any rate.
- Interactive and streaming traffic is a Thread-shaped problem. Anything conversational, real-time above a few messages per second, or larger than about ten BLE Mesh segments falls off the reliability cliff rather than merely slowing.
- Bulk data is categorically different, not incrementally slower, and the standards-track answer on the BLE side is a separate BLOB transfer model rather than the same path made faster.
- Ecosystem reach is asymmetric in opposite directions: Thread reaches IP and Matter through a standard Border Router, BLE Mesh reaches phones through a proxy any handset can connect to. Neither reaches both.
- Role assignment is the sharpest architectural contrast. Thread's stack elects its Leader, promotes routers and merges partitions with no application involvement, so an application that wants none of that responsibility gets it for free; BLE Mesh has no protocol-level equivalent for its provisioner role, so any coordination is application work.
- On the movable-leader question specifically: a fixed provisioner is a build choice, not the stack's limit. These images pick the role at compile time, but ESP-BLE-MESH's fast provisioning can promote an already-provisioned node into a temporary provisioner at runtime, handing it a unicast address range through unicast_min and unicast_max.
- Multiple provisioners can therefore coexist by partitioning the address space, but that is delegation rather than election — there appears to be no election, no automatic failover and no handover protocol in the specification, and any coordination between provisioners is application-level.
- Provisioner state is enumerable in the SDK's own node structure, including each node's device key, so a handover is implementable by exporting and importing it, and the Bluetooth SIG has a JSON configuration-database format intended for that. Nothing in the public API does it.
- A network with no ceremony at all would need the internal bt_mesh_prov_complete hook that takes a NetKey, address, flags and IV index directly. It exists in the stack but is not public API, and the public node-side key functions require the device to be provisioned already, so they cannot bootstrap a network.
- Losing the provisioner leaves existing nodes talking to each other but closes the network: no new members and no reconfiguration, because nothing else holds their device keys.
- Application fit follows from all of the above: dense, low-duty, small-state control and telemetry sit naturally on BLE Mesh; conversational, interactive, file-bearing or IP-connected applications sit naturally on Thread.
