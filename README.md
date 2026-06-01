# Project MagNET

**An open, local-first platform for IoT data and hyperlocal, hive-centric AI.**

Project MagNET is an open-source effort to **capture, sync, and act on IoT data locally first** — without surrendering it to proprietary clouds. It pairs a cross-platform app and data-sink services with a growing fleet of reference-design firmware, and explores a distinctive idea: **hyperlocal, hive-centric AI**, where ordinary edge devices self-organize and share intelligence like cells in a nervous system.

> 📚 **Documentation:** <https://projectmagnet-github-docs.pages.dev/>

## Goals & objectives

The aim is to let people — and companies — control how their IoT data is captured, stored, and used, and to define their own security posture. An open server stack makes it easy to keep data on your own subnet, or in a cloud you choose. A follow-on release adds an open version of IoToneKit, letting users define UX for IoT devices via profile "kits."

On top of that data substrate, MagNET investigates **distributed edge intelligence**: every device has *some* intelligence, storage, contextual awareness, and goals, but no device needs to be a brain on its own — each can act as a cell, neuron, or synapse in a larger swarm.

### Pillars

- **Local-first** — your data lives where you are; sync is opportunistic and under your control.
- **Hyperlocal context** — awareness scoped to *here and now*: locations, sightings, and nearby-compute offloading via a transaction model.
- **Hive-centric AI** — devices form a hive under a ruler, hot-swap roles over the air, and share memory.
- **Every device a cloud of services** — discoverable over BLE or Wi-Fi: *detect → connect → provision → control → collect*.
- **You own your data & security** — secure transport and verified session-key exchange, on by default; each node decides how much to trust a given data sink.
- **Open & interoperable** — zeroconf/Bonjour, Thread, Zigbee, LoRa/Meshtastic, and open APIs.

## Repository layout

| Path | What it is |
|---|---|
| [`magnet_app/`](magnet_app/) | Cross-platform app (Flutter) — device scanning, provisioning, and data views. Requires the Flutter SDK plus iOS/Android native tooling. |
| [`datasync-proto/`](datasync-proto/) | Rust prototype of the data-sync framework and **data sink** — a place to store data on your subnet (finite ring buffer or "unlimited"), with trusted/untrusted modes. A non-trivial Rust building block for a RESTful (later MQTT) service. |
| [`reference-designs/`](reference-designs/) | Firmware for ESP32 / -S3 / -C3 / -C6 and nRF52 boards (see below), including the **Hive AI** prototype. |
| [`specs/`](specs/) | Design specifications — Unified Device Model, device self-registration, the Vitals proposal, and more. |
| [`tools/`](tools/) | Repository utilities. |

### Featured reference designs

- **[Hive AI](reference-designs/MagNET_M5DialFiddlerCrab/)** — a swarm of ESP32-class "biologic" nodes that discover a ruler, prove their lineage to join, hot-swap signed Forth role bundles, and share memory. The most fully documented part of the project. ([docs](https://projectmagnet-github-docs.pages.dev/docs/hive-ai/))
- **PetWear** (`MagNET_PetWear_XIAO*`) — wearable companion designs on XIAO ESP32-C6 / nRF52.
- **Thread / CoAP mesh** (`MagNET_Thread_COaP_*`) — leader, light, switch, and scan nodes on ESP32-C6.
- **Data sink & sync** (`MagNET_Wifi6_*`, `MagNET_TimeServer_M5StampS3`) — Wi-Fi 6 sync, a UDP datasink, and a time server.
- **Beacons & sensors** (`MagNET_BeaconPlus_XIAOESP32C6`, `MagNET_IMUCAL_XIAONRF52`) and a **Zigbee P2P** experiment.

## Getting started

### Magnet App

See [`magnet_app/`](magnet_app/) — requires the Flutter SDK and either iOS or Android native SDK tooling.

### Data sink (datasync-proto)

See [`datasync-proto/`](datasync-proto/). The data sink stores data on a subnet with persistence guarantees and a trust model: security is on by default (secure transport + verified session keys), but a client node decides whether it requires it. The first "proto" exists to grow a reusable, socket.io-like framework in Rust for later, more concrete work.

### Reference-design firmware

Most firmware uses **PlatformIO** with the `espidf` framework. Each project has its own README; the Hive AI prototype includes a full [bench bring-up walkthrough](https://projectmagnet-github-docs.pages.dev/docs/hive-ai/bring-up/).

## Roadmap

Work is tracked publicly in the [issue tracker](https://github.com/IoTone/ProjectMagNET/issues). A themed snapshot:

| Theme | Highlights |
|---|---|
| **Vision & specs** | Core specs for discovery/provisioning/control/datasync/OTA ([#2](https://github.com/IoTone/ProjectMagNET/issues/2)), Architecture Overview ([#65](https://github.com/IoTone/ProjectMagNET/issues/65)), MAG\*Net AI ([#80](https://github.com/IoTone/ProjectMagNET/issues/80)) & Hive-Centric AI ([#89](https://github.com/IoTone/ProjectMagNET/issues/89)), Local-First ([#92](https://github.com/IoTone/ProjectMagNET/issues/92)), OTA ([#90](https://github.com/IoTone/ProjectMagNET/issues/90)), Generative UI ([#94](https://github.com/IoTone/ProjectMagNET/issues/94)) |
| **Hyperlocal & distributed AI** | Contextual awareness ([#79](https://github.com/IoTone/ProjectMagNET/issues/79)), every device a cloud of services ([#71](https://github.com/IoTone/ProjectMagNET/issues/71)), locations/sightings ([#18](https://github.com/IoTone/ProjectMagNET/issues/18), [#19](https://github.com/IoTone/ProjectMagNET/issues/19)), compute advertisement ([#54](https://github.com/IoTone/ProjectMagNET/issues/54)), rules bases ([#14](https://github.com/IoTone/ProjectMagNET/issues/14), [#28](https://github.com/IoTone/ProjectMagNET/issues/28)) |
| **Spatial / WebXR / audio** | WebXR of things ([#91](https://github.com/IoTone/ProjectMagNET/issues/91)), digital twins ([#43](https://github.com/IoTone/ProjectMagNET/issues/43)), audio HUD/alerts ([#44](https://github.com/IoTone/ProjectMagNET/issues/44), [#6](https://github.com/IoTone/ProjectMagNET/issues/6)) |
| **App** | Onboarding ([#16](https://github.com/IoTone/ProjectMagNET/issues/16)), data viewer ([#7](https://github.com/IoTone/ProjectMagNET/issues/7)), desktop BLE ([#67](https://github.com/IoTone/ProjectMagNET/issues/67)), widgets & UX ([#31](https://github.com/IoTone/ProjectMagNET/issues/31), [#47](https://github.com/IoTone/ProjectMagNET/issues/47), [#50](https://github.com/IoTone/ProjectMagNET/issues/50)) |
| **Discovery & provisioning** | zeroconf/Bonjour ([#36](https://github.com/IoTone/ProjectMagNET/issues/36)), QR ([#66](https://github.com/IoTone/ProjectMagNET/issues/66)) & NFC ([#70](https://github.com/IoTone/ProjectMagNET/issues/70)) provisioning, device profiles ([#23](https://github.com/IoTone/ProjectMagNET/issues/23)) & catalog ([#17](https://github.com/IoTone/ProjectMagNET/issues/17)) |
| **Data sink, sync & storage** | Sink node service ([#8](https://github.com/IoTone/ProjectMagNET/issues/8)), thin ESP32 sink ([#52](https://github.com/IoTone/ProjectMagNET/issues/52)), sync adaptor ([#45](https://github.com/IoTone/ProjectMagNET/issues/45)), serialization ([#61](https://github.com/IoTone/ProjectMagNET/issues/61)) |
| **Networking & mesh** | LoRa/Meshtastic ([#41](https://github.com/IoTone/ProjectMagNET/issues/41), [#58](https://github.com/IoTone/ProjectMagNET/issues/58)), protocol interop ([#34](https://github.com/IoTone/ProjectMagNET/issues/34), [#35](https://github.com/IoTone/ProjectMagNET/issues/35)), P2P chat → PlatformIO ([#87](https://github.com/IoTone/ProjectMagNET/issues/87)) |
| **Hardware** | Wearable AI companions ([#93](https://github.com/IoTone/ProjectMagNET/issues/93)), air sensor ([#9](https://github.com/IoTone/ProjectMagNET/issues/9)), energy harvesting ([#62](https://github.com/IoTone/ProjectMagNET/issues/62)), power viz ([#56](https://github.com/IoTone/ProjectMagNET/issues/56)) |

The [Roadmap page](https://projectmagnet-github-docs.pages.dev/docs/roadmap/) in the docs carries the full themed list. GitHub is always the source of truth.

## Contributing

Issues and pull requests are welcome — start from the [issue tracker](https://github.com/IoTone/ProjectMagNET/issues). Some items are tagged **help wanted** (e.g. [#31](https://github.com/IoTone/ProjectMagNET/issues/31)). Please open or reference an issue so work stays coordinated and discoverable.

## License

Apache 2.0 — see [`LICENSE.txt`](LICENSE.txt).
