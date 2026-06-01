```
        ┌─────────────────────────────────────────────────────────┐
        │                                                         │
        │                  T H E   W E B X R                      │
        │                      O F   T H I N G S                  │
        │                                                         │
        │        ──────────────────────────────────────────       │
        │                                                         │
        │           XR UX Proposal 1 — Onboarding &               │
        │               Hyperlocal Experiences                    │
        │                                                         │
        │                     👤 · 🏠 · 🏛 · 🌐                    │
        │              personal · room · hall · net               │
        │                                                         │
        └─────────────────────────────────────────────────────────┘
```

<p align="center"><em>A field guide for joining dataspaces, exploring hyperlocal context,<br/>
and reclaiming the WebXR stack from the gatekeepers.</em></p>

---

## Author

**David J. Kordsmeier**
`dkords@gmail.com` · [@dkords on GitHub](https://github.com/dkords)
Project MagNET · `reference-designs/webxrofthings`

Contributing designers and collaborators (by subject area):
- Design concept inspiration: **Adam Varga** ([LinkedIn post](https://www.linkedin.com/posts/dmvrg_unity-m5stack-arduino-activity-7341335884333531138-oHRI/))
- Open-source browser stewardship (reference): **Igalia / Wolvic team**, and the archived **Mozilla Reality** team
- Collaborators wanted: accessibility, localization (especially Japanese), open-hardware reference devices

---

## Document Metadata

| | |
|---|---|
| **Title** | XR UX Proposal 1 — Onboarding & Hyperlocal Experiences |
| **Status** | Draft |
| **Date** | 2026-04-15 |
| **Parent document** | [`PROPOSAL.md`](./PROPOSAL.md) — "The WebXR of Things" |
| **Target stack** | WebXR · three.js · three-mesh-ui · proposed `d3-spatial` |
| **Browser target** | Wolvic (fork), long-term Servo; upstream contributions to Chromium |
| **License** | same as repository |

---

## Spatially-Aware Table of Contents

The content of this book is organized by the *hyperlocal scale* at which the reader is operating. Each section is tagged with its primary spatial zone, the requirement IDs from `PROPOSAL.md` that it addresses, and the dominant interaction surface. In the spatial edition, this TOC renders as a layered radial map: the reader stands at the center, and each chapter orbits at its natural distance — from wrist (👤) to network (🌐).

Legend:  👤 personal (within reach)  ·  🏠 room (within a space)  ·  🏛 hall / venue  ·  🌐 network  ·  🛠 platform / browser  ·  🧭 meta / index

| # | Section | Scale | Reqs addressed | Primary surface |
|---|---------|-------|----------------|-----------------|
| — | **Splash & Author** | 🧭 | — | this page |
| — | **Spatially-Aware TOC** *(you are here)* | 🧭 | — | radial map |
| 1 | [Design Goals](#1-design-goals) | 🧭 | R10–R14, R17, R19 | principles |
| 2 | [Onboarding: "Join Code" — TOTP-Style, Authenticator-Free](#2-onboarding-join-code--totp-style-authenticator-free) | 👤 → 🏠 | R20–R24 | floating Join panel |
| 2.1 | [The model](#21-the-model) | 🧭 | R20, R22, R24 | — |
| 2.2 | [Onboarding flow](#22-onboarding-flow-hmd-user-public-dataspace) | 👤 | R10, R11 | browser → panel → scene |
| 2.3 | [The Join panel (three-mesh-ui)](#23-the-join-panel-three-mesh-ui) | 👤 | R25 | Block tree at arm's length |
| 2.4 | [Private dataspaces](#24-private-dataspaces-r23) | 👤 | R23 | PKI challenge, WebCrypto |
| 2.5 | [Device join](#25-device-join-r24) | 🏠 | R24, R31, R32 | out-of-band secret |
| 3 | [Continuous-Awareness HUD](#3-continuous-awareness-hud) | 👤 | R11 | peripheral strip |
| 4 | [Use Case Walkthroughs](#4-use-case-walkthroughs) | — | R25–R29 | — |
| 4.1 | [UC1 — Personal Dataspace (Fitness Wearables)](#41-uc1--personal-dataspace-fitness-wearables) | 👤 | R13, R14, R16, R17 | wrist anchor, micro-charts |
| 4.2 | [UC2 — Room-Scale Dataspace](#42-uc2--room-scale-dataspace-home--lighting--data) | 🏠 | R13, R14, R29, R31 | device pins, control pucks |
| 4.3 | [UC3 — Conference Poster Session](#43-uc3--conference-poster-session-interactive-data--experiences) | 🏛 | R14, R27, R28, R29 | manifest-driven artifact bloom |
| 4.4 | [UC4 — Airplane Seat (brief)](#44-uc4--airplane-seat-briefly) | 🏛 | R7, R10 | seat-anchored pins |
| 5 | [The `d3-spatial` Concept](#5-the-d3-spatial-concept) | 👤 / 🏠 / 🏛 | R28 | charts as scene objects |
| 6 | [Visualization Prior Art to Lean On](#6-visualization-prior-art-to-lean-on) | 🧭 | — | references |
| 7 | [Scope: Reviving an Open-Source XR Browser](#7-scope-reviving-an-open-source-xr-browser) | 🛠 | R1–R6, R8, R9 | fork strategy |
| 7.1 | [Candidate Codebases](#71-candidate-codebases) | 🛠 | R2, R3 | comparison table |
| 7.2 | [Firefox Reality — What's Actually Wrong](#72-firefox-reality--whats-actually-wrong-today) | 🛠 | — | defect list |
| 7.3 | [Proposed Fork Scope — "hlxr-browser"](#73-proposed-fork-scope--hlxr-browser-working-name-built-on-wolvic) | 🛠 | R8, R9 | tiered roadmap |
| 7.4 | [Work Not In Scope](#74-work-not-in-scope) | 🛠 | — | — |
| 7.5 | [Governance & Upstream Posture](#75-governance--upstream-posture) | 🛠 | R2, R5 | patch flow |
| 7.6 | [Open Risks](#76-open-risks) | 🛠 | — | — |
| 8 | [Spatial Audio — Research Appendix](#8-spatial-audio--research-appendix) | 👤 → 🏠 | R13, R29 | positional audio, ambisonic beds |
| 9 | [Spatial Hierarchies and Graphs — Design Notes](#9-spatial-hierarchies-and-graphs--design-notes) | 👤 → 🌐 | R14, R28 | hierarchies, graphs, full D3 taxonomy |
| 9.1 | [Tree (node-link)](#91-tree-node-link-diagram) | 🧭 | — | wall / radial / extruded |
| 9.2 | [Treemap](#92-treemap) | 🧭 | — | extruded city-block |
| 9.3 | [Tidy tree](#93-tidy-tree) | 🧭 | — | cylindrical wrap |
| 9.4 | [Tangled tree](#94-tangled-tree) | 🧭 | — | z-separated tangle arcs |
| 9.5 | [Sunburst](#95-sunburst) | 🧭 | — | stacked discs / cone stack / spherical |
| 9.6 | [Force-directed graph](#96-force-directed-graph) | 🧭 | — | d3-force-3d |
| 9.9 | [Extended mark catalog — full D3 taxonomy, graded for 3D](#99-extended-mark-catalog--the-full-d3-taxonomy-graded-for-3d) | 🧭 | — | ridgeline, packing, hexbin, parallel, sankey, edge bundle |
| 10 | [Open Questions](#10-open-questions) | 🧭 | — | review checklist |
| 11 | [What Goes Into PROPOSAL.md "UI Spec V1"](#11-what-goes-into-proposalmd-ui-spec-v1) | 🧭 | R25–R29 | hand-off map |

The in-XR rendering of this TOC — as a radial, gaze-navigable chapter map — is specified as a concrete artifact under **[UC3 — Conference Poster Session](#43-uc3--conference-poster-session-interactive-data--experiences)**, where "a book as a dataspace" is the exemplar experience.

---

This proposal addresses the user-facing experience for **joining a dataspace** and **interacting with hyperlocal context** in WebXR. It proposes a TOTP-inspired but authenticator-free onboarding flow, and walks through three of the four use cases (UC1, UC2, UC3) defined in `PROPOSAL.md`. The goal is to ground the upcoming "UI Spec V1" in a concrete, testable UX that satisfies R10–R14, R19–R24, R25–R28, and R29.

---

## 1. Design Goals

1. **Zero-install join.** A user with a WebXR-capable HMD or Mixed Reality glasses should be able to join a dataspace in under 15 seconds, with no app store, no account, and no third-party authenticator app.
2. **Memorable, sharable codes.** Per R20, dataspace identifiers must be short and speakable. We borrow the *time-bounded short code* idea from TOTP, but replace the secret-on-device model with a transient, server-issued token bound to the dataspace.
3. **Single mental model across personal / room / network scale.** The same join-and-explore loop applies whether the dataspace is a wristband (UC1), a room (UC2), or a poster session (UC3).
4. **Spatial-first data exploration.** Charts, dashboards, and device controls live as 3D affordances in the user's space, not as 2D iframes pinned to a panel. We propose a **spatial D3** layer (call it `d3-spatial`) on top of three.js for this.
5. **Continuous awareness.** Per R11, the user always sees connection, security, and QoS state in a non-intrusive HUD.

---

## 2. Onboarding: "Join Code" — TOTP-Style, Authenticator-Free

### 2.1 The model

TOTP's strength is short, time-bounded, easy-to-type codes. Its weakness for our use case is that it requires a pre-shared secret in an authenticator app — an unacceptable barrier for a guest joining a poster session or sitting down in an airplane seat.

We invert the TOTP model:

| TOTP | Hyperlocal Join Code |
|------|---------------------|
| Secret pre-shared with user device | No pre-shared secret |
| User generates code from local secret | **Dataspace** generates code from its own secret |
| Server verifies user-generated code | **User submits dataspace-generated code** to claim a session |
| 30-second rotation | 60-second rotation, configurable per dataspace |
| 6 digits | 6 alphanumeric chars (ambiguity-stripped: no `0/O`, `1/I/l`) |

The dataspace owner's device (a phone, an M5 device, a kiosk display, a wristband E-ink screen) shows the current code. The joining user reads the code (or scans an adjacent QR that wraps the same code plus the dataspace URL) and enters it via the HMD's spatial keyboard or controller. The hyperlocal context engine validates the code, mints a session token, and returns the dataspace manifest.

This satisfies R20 (memorable, short), R22 (any entity can join a public dataspace), and R24 (devices register via shared secret — the secret here is the rotating code, not a long-lived key).

### 2.2 Onboarding flow (HMD user, public dataspace)

```
   ┌────────────────┐    ┌──────────────────┐    ┌─────────────────┐
   │ 1. Open hlxr.org │ →  │ 2. "Join" panel  │ →  │ 3. Enter 6-char │
   │  in HMD browser  │    │  appears in MR   │    │  code from host │
   └────────────────┘    └──────────────────┘    └─────────────────┘
                                                          │
                                                          ▼
   ┌────────────────┐    ┌──────────────────┐    ┌─────────────────┐
   │ 6. Explore the │ ←  │ 5. Manifest loads,│ ← │ 4. Server mints │
   │   dataspace    │    │ scene materializes│    │ session token   │
   └────────────────┘    └──────────────────┘    └─────────────────┘
```

### 2.3 The Join panel (three-mesh-ui)

A single floating panel, ~40 cm wide at arm's length, containing:

- **Title:** "Join a dataspace"
- **Code field:** six large slots, gaze/controller targetable. Auto-advance on entry.
- **QR fallback:** a small camera-icon button — if the device exposes a QR scanner (some Meta builds do via the system layer), the user can scan instead.
- **Recently joined:** up to three chips for past dataspaces (stored in `localStorage`, never sent).
- **Status footer:** TLS lock icon, latency in ms, dataspace owner name once resolved.

Component sketch:

```
ThreeMeshUI.Block (root, padding 0.04, backgroundOpacity 0.85)
├── Block (title)              "Join a dataspace"
├── Block (codeRow, contentDirection "row")
│   ├── CodeSlot × 6           (each a Block with a Text child)
├── Block (actionsRow)
│   ├── Button "Scan QR"
│   └── Button "Paste"
├── Block (recents)            chips of past dataspaces
└── Block (statusBar)          🔒 lock · 23 ms · "kords-livingroom"
```

### 2.4 Private dataspaces (R23)

The same flow, but after code submission the server returns a PKI challenge. The HMD generates a session keypair in WebCrypto, signs the challenge, and the server binds the session token to the public key. This is invisible to the user except for an additional "🔐 verifying" step in the status footer.

### 2.5 Device join (R24)

IoT devices receive a longer-lived shared secret out-of-band (provisioning sticker, BLE pair, etc.) and use the same `/join` endpoint with the secret instead of the rotating code. The UX surface is just an LED or single-pixel state — out of scope for this UX proposal, but the protocol is the same.

---

## 3. Continuous-Awareness HUD

A 4 cm-tall strip anchored to the user's lower-left peripheral (toggleable to wrist-anchor), showing:

- Dataspace name and scale icon (👤 personal · 🏠 room · 🌐 network)
- Lock state (🔒 TLS, 🔐 PKI, ⚠ degraded)
- Latency sparkline (last 30 s, drawn with `d3-spatial`)
- Battery / device count

The HUD is the **only** persistent UI. Everything else is summoned on demand by gaze + pinch or controller trigger on a target.

---

## 4. Use Case Walkthroughs

### 4.1 UC1 — Personal Dataspace (Fitness Wearables)

**Scene:** User at home, wearing an HMD and a wrist wearable (M5 or similar) that hosts its own personal dataspace.

**Onboarding:** Wearable's tiny display shows a rotating 6-char code. User opens `hlxr.org`, enters the code. Because the dataspace owner = the user (verified by a one-time pairing prior), all subsequent joins from the same HMD are auto-approved via stored device key — no code re-entry.

**Spatial UX:**
- Wearable appears as a **glowing anchor** floating ~10 cm above the actual wrist (using WebXR hand tracking when available, otherwise a draggable handle).
- Pinch the anchor → a radial "device card" expands: heart rate, SpO2, step count, battery.
- Each metric is a `d3-spatial` micro-chart: a 6 cm-wide ribbon plotting the last 60 minutes.
- Long-press a metric → it detaches and pins to world-space, persisting across sessions for that dataspace.
- A **timeline arc** sweeps from the wearable to a translucent disc on the floor, where the user can scrub a day's worth of data with their foot or controller.

**Why this matters:** validates R13, R14, R16 (user introspects all data), R17 (no leak — the wearable *is* the server).

### 4.2 UC2 — Room-Scale Dataspace (Home / Lighting & Data)

**Scene:** User enters a room with a self-hosted hyperlocal context engine (Raspberry Pi, M5StampS3, etc.) advertising a public dataspace via mDNS *and* a printed/displayed code on a small kiosk.

**Onboarding:** User reads the code from the kiosk (or scans QR), enters it into the floating Join panel. Server returns a manifest listing N devices and their self-described UI capabilities (R13).

**Spatial UX:**
- The room scan (Meta Quest scene mesh / AVP room model when available) is overlaid with **device pins** at the world coordinates each device claims (or that the user manually drops during a one-time room-tagging pass).
- Each pin uses a glyph for its type: 💡 lights, 🌡 climate, 🔊 audio, 📷 camera (off by default per R9 spirit).
- Gaze + pinch on a 💡 pin → a `three-mesh-ui` control puck materializes adjacent to the lamp itself: hue wheel, brightness slider, scene presets. Controls reflect the device's self-described schema; the client doesn't ship per-device code.
- A **room dashboard** is summoned by a palm-up gesture: a horizontal `d3-spatial` ribbon along one wall showing temperature, humidity, occupancy over 24 h. Users can grab any series and pull it forward into a larger free-floating chart.
- Multi-user (R29): when a second HMD joins the same dataspace, their avatar appears as a soft sphere with a name tag; control puck interactions propagate via the engine's pub/sub, with last-writer-wins on simple controls and a coordination lock on critical ones (R31).

### 4.3 UC3 — Conference Poster Session (Interactive Data & Experiences)

**Scene:** A dozen presenters in a hall, each with a small dataspace tied to their poster. A printed QR + 6-char code on each poster.

**Onboarding:** User walks up, reads or scans, joins. The hall itself may host a "parent" dataspace listing all posters; joining it gives a **map view** (a low minimap floating to the user's left) of which posters are currently active.

**Spatial UX:**
- Each poster's dataspace publishes a **scene manifest**: a list of 3D artifacts (point clouds, GLTF models, datasets) and `d3-spatial` view definitions.
- The poster surface acts as a portal: stepping within ~1 m triggers the artifacts to bloom out into the user's space — molecules to inspect, networks to traverse, time series to scrub.
- A **citation thread** runs from each artifact back to a small text panel with author + DOI; pinch to copy the DOI to the clipboard (when the browser allows).
- "Take it with you" — user pinches a small pocket icon on any artifact; a reference (not the data) is saved to their personal dataspace for later re-fetch. Per R17, no data leaves the poster's dataspace without this explicit gesture.
- Presenter mode: the poster owner has a private control dataspace (R23) to spotlight artifacts for all current visitors simultaneously.

**Exemplar artifact — "a book as a dataspace" (the spatially-aware TBOC):**

A poster or a long-form proposal (this document, for instance) can itself be published as a dataspace whose primary artifact is a **radial chapter map** — a table of contents that is spatially aware. This is the canonical demo for UC3, because it exercises every surface the use case needs: manifest-driven bloom, `d3-spatial` arcs, presenter spotlight, and "take a reference with you."

- The reader stands at the origin. Chapter markers orbit at radii proportional to each chapter's **scale tag** (👤 ≈ 0.3 m · 🏠 ≈ 1.5 m · 🏛 ≈ 4 m · 🌐 / 🛠 ≈ horizon billboard). A reader glancing around the room immediately reads the shape of the book: how much of it is intimate, how much is room-scale, how much is infrastructural.
- Each marker is a `three-mesh-ui` Block containing the section number, title, and scale glyph. Requirement-ID chips hang beneath each marker as tiny pills, so the book's "graph of obligations to the parent PROPOSAL" is visible at a glance.
- Gaze + pinch on a marker triggers a **smooth dolly** to that chapter's demo scene (or opens the matching anchor in the 2D fallback for non-XR readers).
- A **breadcrumb arc** — drawn with `d3-spatial`'s arc mark — traces the reader's path through the book so they can retrace or branch. This is a spatial analogue of a reading history, and it doubles as the presenter's live "where is my audience" view.
- The HUD strip (§3) remains pinned during TOC navigation so connection state is never lost while the reader is browsing chapters.
- Manifest schema: each chapter publishes `{ id, title, scaleTag, radiusM, reqs[], anchor, demoSceneUrl? }`. The renderer is generic — any book, poster, or catalog that emits this manifest gets the same radial TOC for free.

This is the "TBOC that is spatially aware": it is not just an index, it is the first interactive surface a reader encounters *inside* any UC3 dataspace, and it reuses the same design vocabulary (Blocks, glyphs, `d3-spatial` arcs, gaze-pinch dolly, breadcrumb) that every other chapter of this proposal specifies. The reference implementation for this proposal SHOULD ship with exactly this TOC rendering the document you are reading.

### 4.4 UC4 — Airplane Seat (briefly)

Out of scope for the first round of fidelity, but the same Join-code + manifest model applies. The seat back displays a code; once joined, the user gets in-flight data, entertainment manifests, and seat controls as device pins anchored to the seat itself. Notable constraint: offline-tolerant manifests, since aircraft Wi-Fi is intermittent.

---

## 5. The `d3-spatial` Concept

D3.js's strength is the data-join + scale + selection model, not its SVG output. We propose a thin layer that:

- Reuses `d3-scale`, `d3-array`, `d3-shape`, `d3-time` directly (they have no DOM dependency).
- Replaces SVG renderers with three.js `BufferGeometry` builders for: line, area, bar, scatter, arc, ribbon, surface, force-directed graph.
- Exposes layouts as **three.js groups** that can be parented into `three-mesh-ui` blocks (so a chart is a first-class child of a panel) *or* placed in world space.
- Provides interaction primitives (raycaster-based hover, brush, lasso) that emit the same event shape as D3 selections.
- Animates via `d3-transition` against a per-frame tick, not SVG attributes.

A minimal API sketch:

```js
import { spatialChart, scaleLinear, scaleTime } from 'd3-spatial';

const chart = spatialChart()
  .x(scaleTime().domain([t0, t1]).range([0, 0.4]))   // 40 cm wide
  .y(scaleLinear().domain([0, 200]).range([0, 0.1])) // 10 cm tall
  .mark('line')
  .data(heartRateSeries);

panel.add(chart.object3D);     // drop into a three-mesh-ui Block
chart.on('brush', range => …); // works with controller or hand pinch
```

This is the visualization workhorse for all three use cases — wrist micro-charts (UC1), wall ribbons (UC2), poster artifacts (UC3).

---

## 6. Visualization Prior Art to Lean On

- **three-mesh-ui** — primary widget layer (panels, text, buttons, flex-like layout).
- **three.js InstancedMesh** — for high-cardinality datasets (point clouds in UC3 posters).
- **d3-force-3d** (existing fork of d3-force) — directly usable as the layout engine for `d3-spatial` graph marks.
- **deck.gl** — worth studying for its layer model; we are NOT adopting it (canvas/WebGL2-2D centric), but its layer-as-data-binding approach informs `d3-spatial`.
- **A-Frame components ecosystem** — for inspiration on declarative scene composition; we stay imperative with three.js for control.
- **WebXR hand input + transient-pointer** — preferred interaction primitives over controller-only.

---

## 7. Scope: Reviving an Open-Source XR Browser

The known limitations in `PROPOSAL.md` (no WebBLE, no Web MIDI, no Web USB/Serial, no user-managed CA store, no URI schemes beyond `https://`, hostile browser UX on HMDs) are not fixable from inside a web page. To deliver the onboarding and in-session experience described in §2–§6 at the quality bar we want, we either (a) lobby vendors indefinitely, or (b) fork an open-source XR browser and ship the fixes ourselves. This section scopes option (b).

### 7.1 Candidate Codebases

| Project | License | WebXR | Platform Reach | Status | Notes |
|---------|---------|-------|----------------|--------|-------|
| **Firefox Reality** (`MozillaReality/FirefoxReality`) | MPL-2.0 | Yes (WebVR era, needs WebXR uplift) | Quest 1/Go, Pico, Vive Focus, HTC Viveport | **Defunct** (archived 2022) | GeckoView-based. Good UX foundation, outdated engine. |
| **Wolvic** (`Igalia/wolvic`) | MPL-2.0 | Yes (WebXR) | Quest, Pico, Huawei, Lynx, HTC | **Active** (Igalia-maintained fork of FxR) | The de facto continuation of Firefox Reality. Strongest candidate. |
| **Servo** (`servo/servo`) | MPL-2.0 | Partial / experimental | Desktop, experimental Android | **Active** (Linux Foundation) | Long-term play; not shippable to HMDs today but architecturally the cleanest. |
| **Chromium (WebXR paths)** | BSD-3 | Yes | Everywhere | Active | Fork cost is enormous; realistically we contribute upstream, not fork. |

**Recommendation:** **Wolvic first**, because it inherits the Firefox Reality UX work the user is asking about and is already shipping on the HMDs that matter (Quest, Pico). Keep a watching brief on Servo as the 3–5 year target for a clean-room WebXR browser. Treat Chromium as upstream-only (file bugs, land patches, don't fork).

### 7.2 Firefox Reality — What's Actually Wrong Today

- **Archived Feb 2022.** Last release lagged both WebXR-spec evolution and current Quest OS ABIs.
- **GeckoView version is stale** — missing modern content-process sandboxing and post-2022 web platform features.
- **WebVR, not WebXR-first.** Retrofit needed.
- **No WebBLE, no Web MIDI, no WebUSB/Serial, no WebHID.**
- **No user-manageable root CA store** — blocks self-hosted `hlxr.org` setups with private CAs (enterprise, home-lab).
- **Input model predates Quest hand tracking 2.0 and transient-pointer input.**
- **UX chrome** is pre-passthrough-era; the 2D tab strip in a dome is the wrong metaphor for MR.

Wolvic has fixed a meaningful subset of this (modern GeckoView, active WebXR, maintained Quest/Pico builds) but the device-API and onboarding-UX gaps remain.

### 7.3 Proposed Fork Scope — "hlxr-browser" (working name, built on Wolvic)

Tiered so we can ship tier 1 quickly and park tiers 2–3 behind funding.

**Tier 1 — Onboarding & Hyperlocal Baseline (target: 3–4 months, 1–2 engineers)**

1. **First-run join experience.** Boot directly into a spatial Join panel (§2.3) instead of a 2D home screen. The browser's "home" *is* the hyperlocal onboarding.
2. **Deep link + QR scanner** at the OS-integration layer, so a scanned code skips the URL bar entirely and lands on `hlxr.org/join?code=…`.
3. **User-managed root CA store** with a spatial UX for import / trust / revoke. Unblocks R7 (self-hosted).
4. **Local network permission prompt** (mDNS / `.local`) so dataspace discovery doesn't require going through `hlxr.org` for every join.
5. **WebSocket over `wss://` keepalive tuning** for the HUD's continuous-awareness stream — current Wolvic behavior drops connections on background.
6. **Spatial keyboard rework** — 6-wheel "slot machine" mode for join codes (per Open Question §8.3, now answered as tier-1 scope).
7. **Pass-through-by-default** when entering a WebXR session (R12).

**Tier 2 — Device APIs (target: +6 months)**

8. **WebBLE** (Bluetooth GATT). The single highest-leverage API for IoT. Gecko has a prototype; productionize and ship behind a per-origin permission UX that matches the HMD's modality.
9. **Web MIDI.** Cheap to add once the permission model from WebBLE is in place.
10. **WebUSB + Web Serial.** Harder due to Android host-USB quirks on standalone HMDs; scope as research spike first.
11. **WebHID.** Folded in with USB/Serial.

**Tier 3 — Shared & Offline Experience (target: +6 months)**

12. **Offline manifest cache** — per-dataspace service worker lifetime policy surfaced to the user. Directly supports UC4 (aircraft).
13. **Multi-origin session federation** so a user can be joined to UC1 + UC2 simultaneously (Open Question §8.4) without the security model collapsing.
14. **Avatar / presence primitives** at the browser chrome level so R29 doesn't require every dataspace to reinvent a lobby.
15. **WebXR camera-access experiments** (gated, opt-in, per-origin, time-boxed) to re-enable AR-marker workflows.

### 7.4 Work Not In Scope

- **Rendering engine rewrite.** We inherit Gecko via Wolvic. Do not fork Gecko.
- **OS-level passthrough / SLAM.** We consume what Quest/Pico/Lynx expose.
- **App store / distribution.** Sideload + existing Wolvic distribution channels (Meta Horizon Store, Pico Store, APK) are sufficient for a PoC.
- **Closed-platform ports** (Apple Vision Pro, Snap Spectacles). Track upstream WebXR support; do not invest engineering until those platforms allow third-party browsers.

### 7.5 Governance & Upstream Posture

- Fork publicly, license-compatible with MPL-2.0 upstream.
- **Rebase onto Wolvic weekly**, not a permanent divergence. Every tier-1 fix that is generically useful (CA store UX, spatial keyboard) is submitted upstream to Wolvic *first*, carried as a patch only if upstream declines.
- Device-API work (WebBLE etc.) is submitted to Gecko upstream; we land the permission-UX layer locally.
- Maintain a conformance test harness (R5) as a separate repo so the browser work and the dataspace spec work evolve together but not in lockstep.

### 7.6 Open Risks

- **Igalia is the de facto Wolvic steward.** If our roadmap diverges from theirs, cost rises sharply. Mitigation: engage with Igalia early; sponsor work where possible instead of forking.
- **Meta/ByteDance/etc. can ship OS-level changes** that break third-party browsers. Mitigation: the same conformance suite also catches regressions in first-party browsers, making our case publicly.
- **Device-API permission models are unsettled** across the spec community. Shipping ahead of consensus risks having to rework permission UX later. Mitigation: keep the permission UX layer *above* the API plumbing so it can be swapped.

---

## 8. Spatial Audio — Research Appendix

Audio is half of XR. Silence is disorienting inside a headset; positional sound cues let users locate interactive surfaces without staring at them. The best-practices issue (IoTone/AwesomeSpatialDesign#7) already mandates audio feedback on every interaction; this section scopes *spatial* audio for the hyperlocal stack.

### 8.1 The landscape (as of 2026-04)

| Option | License | Status | What it gives us | Verdict for hlxr |
|---|---|---|---|---|
| **`THREE.PositionalAudio`** (built-in) | MIT (three.js) | Active | WebAudio `PannerNode` with HRTF panning, per-source. Attached to any `Object3D`. Distance model / rolloff / cone all exposed. | **Primary baseline.** Ships with three.js, zero extra deps. Good enough for per-mark hover ticks, device pings, and presence chimes. |
| **WebAudio `PannerNode` (`panningModel: 'HRTF'`) directly** | W3C standard | Active | What `PositionalAudio` wraps. Browser-provided HRTF dataset, no ambisonic support. | Use directly when we want channel-level control (e.g. an ambient bed that isn't Object3D-anchored). |
| **Omnitone** (`npm i omnitone@1.3.0`) | Apache-2.0 | Last release 1.3.0; origin repo moved from `GoogleChromeLabs` — treat as community-maintained. | Ambisonic (first-order and higher) decoding + HRTF binaural rendering. Can decode `.ogg` / `.wav` ambisonic recordings. | **Adopt for room-scale ambience (UC2, UC3).** A pre-recorded ambisonic bed of a poster hall or living room is cheaper and more convincing than synthesizing per-source audio for every ambient object. |
| **Resonance Audio Web SDK** (`resonance-audio`) | Apache-2.0 | Last release **2017**. Not archived in name, archived in practice. | Google's acoustic modeling — room reverb, source directivity, occlusion approximation. | **Do not adopt.** Even with the useful acoustic model, we cannot depend on a 9-year-stale library. Track its successor if one appears; in the meantime, approximate room reverb with a WebAudio `ConvolverNode` + short IR. |
| **Google A-Frame audio components** | MIT (A-Frame) | Active within A-Frame ecosystem | Declarative wrappers around three's PositionalAudio + Resonance Audio. | Different ecosystem. We note the concepts (positional, ambisonic bed, distance-attenuated triggers) but don't pull A-Frame into the three.js build. |
| **Howler.js** | MIT | Active | Simple 3D panning via PannerNode. | Fine for a game; we already have `THREE.PositionalAudio` in-tree. Adopting Howler just to avoid `THREE.Audio` is noise. |
| **Meta / Oculus Audio SDK** | Proprietary | Active | Best-in-class HRTF + reverb on Quest native. | **Not available on the web.** Mentioned only to make clear we cannot match native fidelity; we design toward "good enough in WebAudio" rather than chasing parity. |

### 8.2 Recommended stack

1. **`THREE.PositionalAudio`** as the default per-mark, per-device, per-avatar audio source. One `AudioListener` on the camera; one `PositionalAudio` per interactive object.
2. **Procedural buffers** for short UI sounds (hover tick, brush engage, join-code accepted). Generate with `AudioBuffer` + sine/triangle + exponential envelope — no asset pipeline, no localization surface. (The reference prototype already does this; see `src/audio/SpatialHoverAudio.ts`.)
3. **Omnitone** for per-dataspace ambient beds when a dataspace wants to publish one. The dataspace manifest gains an optional `ambisonicBedUrl` field (ambix-format `.ogg` preferred). Off by default — the user opts in from the HUD.
4. **`ConvolverNode` + short IR** for cheap room reverb on UI sounds when a dataspace signals "indoor / reverberant" in its manifest. Avoids Resonance Audio while still giving a hint of spatial envelope.
5. **AudioContext initialization on first user gesture** — non-negotiable per browser policy and the best-practices issue.

### 8.3 Design principles

- **Audio is a layer, not a scene.** A mark's visual state (hover, brush, select) triggers a short sound *parented to the mark's Object3D* so the HRTF pan is correct. The sound is not "about" the camera, it's "about the thing."
- **Keep UI ticks short (≤ 80 ms) and dry.** Reverb applied indiscriminately turns a dataspace into a cathedral. Reserve reverb for ambient beds and long-form content.
- **Respect the user's prefs.** Global volume + per-category toggles (UI, presence, ambient) in the HUD. Per the best-practices issue: offer multiple UI sound sets (soft tick / mechanical click / silent).
- **Do not auto-play ambient beds.** User must opt in — an ambient bed is a persistent spatial signal and auto-starting it is as intrusive as auto-playing video.
- **Use audio for service discovery cues sparingly.** A new device joining the dataspace could play a 200 ms spatial chime at its pinned location — helpful once, annoying at scale. Rate-limit.

### 8.4 Known gaps and risks

- **No shared reverb acoustic model.** Without Resonance Audio, we approximate. A dataspace owner who cares about realistic acoustics can ship their own `ConvolverNode` IR in the manifest.
- **Listener orientation** inside WebXR: `AudioListener` on the camera is correct only if the camera's world matrix is kept current. In XR sessions, `renderer.xr.getCamera()` returns a Group — we may need to attach the listener to that group rather than the main camera to track head rotation properly. Verify on-device before shipping.
- **Mobile Safari and some HMD browsers** throttle / low-resolution-pan non-HRTF paths. Always set `panningModel: 'HRTF'` explicitly.
- **Accessibility.** Users who can't perceive HRTF cues (monaural hearing, hearing aids) need a visual equivalent for every audio-delivered signal. The inspector card already doubles every sonic cue with a visible state change — keep that invariant.
- **No native support for Meta's deeper spatial audio features** (acoustic propagation, material-based occlusion). Don't claim parity. This is a "legible and correct, not cinematic" target.

### 8.5 Integration into the spec

- The UI Spec V1 (§9) should add a new subsection **V1.8 — Spatial audio model**, capturing: listener attachment, per-mark tick conventions, manifest fields (`ambisonicBedUrl`, `reverbIrUrl`, `acousticEnvironment: 'indoor'|'outdoor'|'auto'`), and the user-prefs surface.
- The open-source browser scope (§7) already implicitly covers audio via WebAudio; no additional browser work is needed for the baseline. *If* we later want shared ambisonic decoding off the main thread, an `AudioWorklet`-based Omnitone port would be a natural Tier 2 browser contribution.

### 8.6 Prototype status

The reference prototype at `prototype/d3-spatial/` wires `THREE.PositionalAudio` into the hover path: each mark receives a `PositionalAudio` node, initialized on first user gesture, playing a 60 ms procedural tick on hover-in, spatialized by the mark's world position. Omnitone ambient beds and `ConvolverNode` reverb are not yet wired — they are the next audio milestone.

---

## 9. Spatial Hierarchies and Graphs — Design Notes

The four marks in the current prototype (`line`, `bar`, `scatter`, `arc`) cover time series and 2D point data. They do not cover **hierarchy** or **relationship** — and most of the interesting data inside a hyperlocal dataspace is hierarchical (device topology, service tree, room → zone → sensor) or relational (who-talks-to-whom on the network, BLE-proximity graphs, citation networks at UC3). This section scopes how we intend to ship each of the classic D3 hierarchy/graph visualizations as a first-class **spatial** mark on top of `three.js` + the `d3-spatial` layer.

### 9.0 Shared spatial-design vocabulary

Every hierarchy/graph mark in this catalog should obey the same rules so the reader doesn't have to re-learn the grammar for each viz:

- **The reader is the origin.** Layouts are described relative to the reader's head — not to a world axis or a screen rectangle.
- **Scale tags drive radii.** Personal-scale data (👤) lands at ~0.3 m; room-scale (🏠) at ~1.5 m; hall-scale (🏛) at ~4 m; network-scale (🌐) at the horizon. A hierarchy's root sits at the tag's natural radius; its children orbit outward one or two tag steps.
- **Depth = hierarchy depth.** When the viz uses the z-axis for something, it's depth in the tree, not "time" or "value." If a viz needs time on z, pick a different viz.
- **Emissive intensity = importance.** Selected / focused / recent nodes get +0.3–0.6 emissive; everything else rests at +0.0–0.15. This is the same scale the prototype's hover feedback uses, so the reader's eye training carries over.
- **Troika for all text.** Three-mesh-ui for any surface that needs borders / padding / layout.
- **Hover + brush + inspector are the same three primitives** as §2–§5, reused. Any new mark type plugs into the existing `Interact` state machine by registering its root `THREE.Group`.
- **Audio mapping rule.** On hover of a node, play a procedural tick whose pitch is a function of **depth** (tree/sunburst/treemap) or **degree** (graph). Spatial position = the node's world position. This gives the reader a sonogram of the hierarchy just by sweeping their ray across it.

### 9.1 Tree (node-link diagram)

**What it is.** Classic parent-child node-link layout — Reingold-Tilford, cluster, or radial.

**Spatial form — three candidates, pick per dataspace:**
1. **Wall tree** — root at top, children flow downward, laid flat on a plane 1.2 m in front of the reader. Good for shallow trees (≤ 4 levels), bad for deep ones (reader has to crane their neck).
2. **Radial tree** — root at reader's eye, children orbit at increasing radii. 360° around the reader is readable because the reader can turn. This is the default recommendation.
3. **Extruded tree** — like the wall tree but depth = hierarchy level, so each generation sits on a successive Z-plane receding from the reader. Children connect to their parent with a diagonal tube. Good for browsing a tree that has both depth *and* sibling count.

**three.js primitives.**
- Nodes: `InstancedMesh` of low-poly spheres (12–16 segs), one instance per node. Radius scales with subtree weight.
- Edges: `TubeGeometry` along a `CatmullRomCurve3` through [parent, 0.3 * parent + 0.7 * child, child] — gives a gentle S-curve. Batch as a single `BufferGeometry` with per-edge draw-range when the tree is static.
- Labels: `troika-three-text` pinned to each node with `billboard` behavior (face the camera) so labels stay legible as the reader circles.

**d3 building block.** `d3-hierarchy`'s `tree()` and `cluster()` layouts. Both return `{x, y}` in 2D; the 3D layer maps those into the chosen spatial form:
- Wall tree: `(x, y) → (x * scaleX, y * scaleY, 0)`.
- Radial tree: `(x, y) → (cos(x) * y, 0, sin(x) * y)` with the reader at origin.
- Extruded tree: `(x, depth) → (x * scaleX, 0, -depth * layerGap)`.

**Interaction.** Hover a node → inspector shows node name, depth, subtree count, parent. Pinch (or click) a node → collapse/expand its children (D3 already has `toggleChildren` idioms; the 3D layer just animates radii with `d3-transition`). Brush → drag a cone from the reader; everything inside the cone is selected.

**Use case mapping.** UC2 (room): the device topology — gateway → access point → sensor → reading — is a natural shallow tree. UC3 (poster): the citation tree of an artifact. The TBOC is a tree too; its radial rendering in §4.3 is an instance of this mark.

### 9.2 Treemap

**What it is.** Space-filling rectangle subdivision where area = value.

**Spatial form — extruded 3D treemap ("city block").**
- Each rectangle becomes a rectangular prism. **x, y = 2D treemap layout; z = extrusion height, driven by a second data dimension** (e.g. x,y = storage usage; z = age of the data). Two variables per cell instead of one. This is the treemap's killer feature in 3D — you get twice the information density for the same footprint.
- Place the treemap on a plane 1.5 m in front of the reader for room-scale, laid horizontally like a city seen from above; walk around it by head-turning or by pinch-grabbing the whole slab and rotating it.
- Cushion shading (Van Wijk) translates naturally: use a `MeshStandardMaterial` with moderate roughness and a soft directional light; the concavity reads as real depth.

**three.js primitives.**
- `InstancedMesh` of unit boxes, transform per cell.
- Borders: `LineSegments` along cell edges, `renderOrder 995`, `depthTest: false` for crisp separation.
- Labels: only on cells above a size threshold (e.g. > 2 cm on shortest edge at reading distance); otherwise hidden and revealed on hover.

**d3 building block.** `d3-hierarchy.treemap()` — returns `{x0, y0, x1, y1}` for each node. The 3D layer adds `(x1 - x0, y1 - y0, extrusion(z))`.

**Interaction.** Hover a cell → scale bump 1.05 (vertical only — looks like the building "pops up"), emissive, inspector card. Pinch → drill into the cell, the current level fades out while children reflow the full space. Pinch-and-hold → "lift" the cell to inspect it in your hand.

**Use case mapping.** UC1 (personal): breakdown of time spent — deep work / meetings / exercise — with z = intensity. UC2 (room): device power consumption — floor area = avg watts, z = peak watts. UC3 (poster): file sizes of a poster's asset bundle; lets presenters diagnose bloat at a glance.

### 9.3 Tidy tree

**What it is.** Reingold-Tilford "tidy" tree layout — siblings as tightly packed as possible without overlap, parent centered over children. The most visually "calm" of the tree layouts.

**Spatial form.**
- **Cylindrical tidy tree** — the layout wraps around a cylindrical surface at the reader's scale radius. Time or traversal order runs vertically; the hierarchy unfolds horizontally around the reader. Deep trees become tall cylinders; branchy trees become wide cylinders.
- **Belt tidy tree** — same idea but the cylinder is a flat belt at eye height, 40 cm tall. Cheaper render, easier to label, loses the "360° overview" affordance.

**three.js primitives.**
- Nodes: `InstancedMesh` of pills (`CapsuleGeometry`) rather than spheres, so labels can ride along their length.
- Edges: `TubeGeometry` segments lofted along an arc on the cylinder surface (parametric on the cylinder's θ).
- Surface: optional faint `CylinderGeometry` with `wireframe: true, opacity: 0.15` so the reader sees the layout plane.

**d3 building block.** `d3-hierarchy.tree()` with wrap-around: after the layout, re-map x to θ on the cylinder.

**Interaction.** Same as §9.1 tree. Additional: pinch-and-drag-around-cylinder rotates the tree in θ so a different branch faces the reader. A "north marker" (small arrow above the root) indicates the home position.

**Use case mapping.** UC3 (hall): attendance / participation tree at a conference — sessions → papers → authors → affiliations. The cylinder lets a reader turn in place and see the whole program.

### 9.4 Tangled tree

**What it is.** A tree where some edges cross level boundaries, connecting distant cousins (a DAG-with-a-spine). Classic example: a family tree with adoption / remarriage; in tech, a call graph where a utility is reused across many modules. D3 community renderings (Rougeux, Niessner) are typically 2D with carefully routed bezier "tangles" to keep the tree spine readable.

**Spatial form — z-separated tangle.** This is the visualization that gains the *most* from 3D.
- Render the tree spine on a flat plane at z = 0 using a tidy tree layout (§9.3).
- For every tangle edge (cross-level), route it on an arc that leaves the plane into +z (behind the spine, away from the reader), curves over to the target node, and returns.
- Depth of the arc = how many levels the tangle spans: a +1 generation cousin is 5 cm behind the plane; a deep cross-hierarchy reference is 20 cm. The reader sees tangle depth as actual depth — no more "spaghetti-over-tree" 2D problem.
- Color-code the tangles by kind (family: red; call-graph: blue; citation: green).

**three.js primitives.**
- Tree spine: as in §9.1 wall tree.
- Tangle arcs: `TubeGeometry` along a `CatmullRomCurve3` through `[source, midpoint + (0, 0, depth), target]` with `tension 0.8` so they don't overshoot.
- Optionally add a faint back-plane at z = -0.25 m to give the tangles a "ceiling" they arc toward.

**d3 building block.** `d3-hierarchy.tree()` for the spine; author-supplied edge list for the tangles; no core d3 module covers the routing, we compute it locally.

**Interaction.** Hover a node → highlight every tangle touching it (color stays, others dim to 0.1 opacity). Hover a tangle → highlight both endpoints. Brush a vertical slab → select all nodes whose tangles cross that slab (great for "show me all the calls into my module").

**Use case mapping.** UC2 (room): device-to-service graph where several devices reuse the same service endpoints — spine = device tree, tangles = shared services. UC3 (hall): multi-author papers where authors span panels.

### 9.5 Sunburst

**What it is.** Radial partition of a hierarchy — each ring is a level, each arc is a node, arc angle = value.

**Spatial form — three candidates:**
1. **Stacked discs** (default). Each level is a flat donut at a fixed z; deeper levels sit further from the reader. A level-3 sunburst has three translucent discs stacked in depth. Reader sees through the whole hierarchy at once.
2. **Truncated cone stack**. Instead of flat discs, each level is a truncated cone extruded in +z, so the sunburst looks like a child's toy — concentric cones nesting outward. Gains: better label surfaces (the cone's outer face tilts toward the reader); more obvious drill-down affordance.
3. **Spherical sunburst**. Project the partition onto a sphere centered on the reader. Reader turns in place to inspect; the closer the ring to the viewing direction, the more detail (LOD-driven). Expensive but memorable; reserved for UC3 poster centerpieces.

**three.js primitives.**
- Segments: custom `BufferGeometry` built from the partition coords. Each segment is a thick ring sector — generated as extruded path or as two triangle-strip arcs + caps.
- `InstancedMesh` is awkward here because every segment has a different shape; fall back to a single merged `BufferGeometry` and a `useAttribute` for per-segment color.
- Labels: troika text along the arc's mid-line, billboarded, shown only for arcs > 10° and wide enough in world space.

**d3 building block.** `d3-hierarchy.partition()` with a polar coordinate pass: `(x0, x1, y0, y1) → (θ0, θ1, r0, r1)`.

**Interaction.** Hover a segment → emissive, inspector shows `path / to / this / node` + value + share-of-parent. Pinch a segment → zoom: it becomes the new root, siblings rotate out, children unfurl. Pinch-and-hold the root → zoom out one level.

**Use case mapping.** UC1 (personal): time-use pie-with-drilldown. UC2 (room): energy budget by device family. UC3 (hall): participant geography by continent → country → institution.

### 9.6 Force-directed graph

**What it is.** Nodes are masses, edges are springs, a physics simulation settles them into a low-energy layout. In 2D this is the go-to for showing "who is connected to whom when I have no good prior layout."

**Spatial form — genuinely 3D, not 2D-in-3D.**
- The simulation runs in full 3D using **`d3-force-3d`** (an existing fork of `d3-force` that adds `forceZ` and a 3D version of `forceCenter` / `forceManyBody` / `forceLink`). No modification needed.
- Place the simulation's origin at the reader's scale-tag radius (e.g. 1.5 m for room-scale). The graph settles into a volumetric cluster *in front of* the reader, not around them — so they can step back and see the whole, or lean in to inspect a dense cluster.
- Render nodes as `InstancedMesh` spheres, radius by degree or PageRank. Render edges as `LineSegments` for the baseline (cheap) and optionally promote the hovered/focused edges to `TubeGeometry` for legibility.
- Add gentle ambient rotation (0.05 rad/s) so the reader can always see the non-occluded side. Lock rotation on pinch-grab.

**three.js primitives.**
- `d3-force-3d` simulation; on each tick, update an `InstancedMesh.setMatrixAt(i, ...)` for nodes and rebuild the edge `BufferGeometry` positions (single buffer, updated each tick with `needsUpdate = true`).
- Halo sprites on hubs (top-k degree) using `Sprite` with a soft circular texture, so they read as "important" at a glance.

**d3 building block.** `d3-force-3d`. Also `d3-scale-chromatic` for community colors if we run modularity.

**Interaction.** Hover a node → inspector with name, degree, neighbors. Pinch-hold a node → "pin" it (`node.fx/fy/fz` set); the physics keeps running around the pinned node. Pinch-and-drag → physically move a node and watch the graph rearrange. Brush a 3D sphere (controller trigger + grow the sphere while held) → select all nodes inside.

**Use case mapping.** UC1 (personal): contact graph; who you've spoken to this week. UC2 (room): Bluetooth proximity graph of devices currently in the space; node clusters reveal device groupings. UC3 (hall): co-authorship graph of the poster session's papers. UC4 (in-flight): origin-destination graph of traffic through the seat's network.

### 9.7 Comparison — when to pick which

| Data shape | Reader task | Best mark |
|---|---|---|
| Strict hierarchy, shallow (≤ 4 levels) | "Show me the whole thing" | **Wall tree** (§9.1) |
| Strict hierarchy, deep + branchy | "Let me wander into a branch" | **Radial tree** or **Cylindrical tidy** (§9.1, §9.3) |
| Hierarchy + a second quantity per node | "Which of these is *heavy*?" | **Treemap** (§9.2) |
| Hierarchy + cross-references | "What depends on what?" | **Tangled tree** (§9.4) |
| Hierarchy + proportion-of-whole | "What's the budget split?" | **Sunburst** (§9.5) |
| No hierarchy, only relationships | "Who's connected to whom?" | **Force-directed graph** (§9.6) |
| Time series | "How did this change?" | `line` (§5, shipped) |
| Categorical quantities | "How much of each?" | `bar` (§5, shipped) |
| Raw 2D points | "What's the distribution?" | `scatter` (§5, shipped) |
| Progress / trail | "Where have I been?" | `arc` (§5, shipped) |

### 9.8 Shared implementation primitives that unlock all six

All six of these marks can be built against the same foundation. Items the prototype already has — carry forward unchanged:

- `Chart` fluent API → extend with `mark('tree' | 'treemap' | 'tidytree' | 'tangledtree' | 'sunburst' | 'force')`.
- Hover / brush / inspector state machine → unchanged; each new mark registers its root `THREE.Group` with `Interact`.
- `d3-spatial` charting layer → becomes `d3-spatial-hierarchy` sub-module for these.
- Scale-tag placement, dataspace focus dim, audio tick — all reusable.

Items to add:

- **Edge bundle / tube builder** — a batched `BufferGeometry` builder that takes `[source, control..., target][]` and returns a single mesh with draw-range metadata. Shared by tree, tangled tree, force-directed, and the breadcrumb arc.
- **Billboarded text cluster** — render many troika `Text` objects in view-facing orientation without re-syncing every frame. Shared by tree, treemap, sunburst.
- **3D physics tick scheduler** — `d3-force-3d` tick ≠ render frame. Ticks at 30 Hz while render runs at 90 Hz; interpolate between ticks. Shared by force-directed and any animated-relaxation layout we later add.
- **Drill-down transition** — grab an interior node, make it the new root, reflow the rest. Shared by tree, treemap, sunburst.

### 9.9 Extended mark catalog — the full D3 taxonomy, graded for 3D

The six marks above cover the hierarchy and graph corners of the design space. The D3 community has evolved a broader taxonomy — captured in Yan Holtz's `d3-graph-gallery.com` and the Observable `@d3/gallery` — organized by **what question the reader is asking**, not by geometry. We adopt that taxonomy here and grade every mark for **3D-readiness** (how much new information a spatial rendering reveals that a 2D rendering cannot).

3D-readiness scale: ★☆☆☆☆ = neutral (3D is cosmetic), ★★★★★ = transformative (3D reveals a signal 2D cannot).

#### Distribution — "what does the shape of this variable look like?"

| Mark | Shipped? | 3D form | Readiness | Notes |
|---|---|---|---|---|
| Histogram | — | Extruded bars (1D → 2D surface of bins) or voxel field | ★★★☆☆ | Useful but the bar mark already covers 1D case. |
| Density (KDE) | — | Smooth curve → extruded ribbon; 2D KDE → surface | ★★★★☆ | 2D density as a 3D surface is a real win. |
| Violin | — | Solid of revolution around the category axis — a **genuinely volumetric** shape | ★★★★★ | This is 3D-native: the 2D violin is the silhouette of a real 3D object. |
| Boxplot | — | Extruded rectangles + whiskers | ★★☆☆☆ | 3D adds nothing material. |
| **Ridgeline** | — | Offset each density curve in **depth** instead of y; reader sees a mountain range | ★★★★★ | Best-in-class payoff. With parallax, comparison across rows is effortless. |

#### Correlation — "do these two (or N) variables move together?"

| Mark | Shipped? | 3D form | Readiness | Notes |
|---|---|---|---|---|
| Scatter | ✅ (`scatter` mark) | Already 3D-capable | — | Can add a genuine z-axis for 3-variable scatter. |
| Connected scatter | — | Tube through points in temporal order; like `line` but on (x, y) pairs | ★★★★☆ | Time becomes depth — trail of a trajectory in phase space. |
| Bubble | — | `InstancedMesh` of spheres, radius by value | ★★★☆☆ | A size-encoded scatter. |
| Heatmap | — | Cells with extrusion by value | ★★★★☆ | A close cousin of treemap; extrusion + color doubles information. |
| Correlogram | — | Grid of small scatter panels; each panel becomes a 3D tile | ★★☆☆☆ | The "small multiples" intent doesn't gain from depth. |
| Density 2D | — | Volumetric scalar field — mesh isosurface or splatted points | ★★★★★ | The 3D density field is a real object; isosurfaces give instant topology. |

#### Ranking — "who is biggest / most / most recent?"

| Mark | Shipped? | 3D form | Readiness | Notes |
|---|---|---|---|---|
| Bar | ✅ | Already in prototype | — | — |
| Lollipop | — | Vertical rods with spheres on top | ★★★☆☆ | Clean alternative to bars; sphere reads as a pin in space. |
| Circular bar | — | Bars extruded radially outward from a disc at reader eye | ★★★★☆ | The reader's eye sweeps 360° to rank — natural use of head rotation. |
| Spider / radar | — | Skeleton polyhedron per observation; layered translucent shells to compare | ★★★★☆ | True 3D polyhedra clearly separate observations; 2D radar overlaps are a mess. |
| Wordcloud | — | Words distributed on a sphere around reader; head rotation = browsing | ★★★★☆ | Already well-explored in the Unity / Three.js community. Typography-heavy; needs troika LOD. |
| Parallel coordinates | — | Each axis is a vertical rod in space at different positions; lines are tubes | ★★★★★ | 2D parallel-coords overplot catastrophically; 3D lets the reader physically lean to separate crossings. |

#### Part of a whole — "how is this divided?"

| Mark | Shipped? | 3D form | Readiness | Notes |
|---|---|---|---|---|
| Treemap | §9.2 | Extruded city-block | ★★★★☆ | Scoped. |
| Dendrogram | §9.1 | Wall / radial / extruded tree | ★★★★☆ | Scoped. |
| Sunburst | §9.5 | Stacked discs / cone stack / spherical | ★★★★☆ | Scoped. |
| Pie / doughnut | — | Extruded cake | ★☆☆☆☆ | 3D pies are aesthetically canonical but analytically useless. Skip. |
| **Circular packing** | — | **Nested spheres in 3D** — volume encodes value, nesting encodes hierarchy | ★★★★★ | The single most 3D-native hierarchical mark after force-directed. Volume comparison is much more accurate than area. |

#### Evolution — "how did this change over time?"

| Mark | Shipped? | 3D form | Readiness | Notes |
|---|---|---|---|---|
| Line | ✅ | Tube along CatmullRomCurve3 | — | Shipped. |
| Area | — | Ribbon with depth | ★★★☆☆ | Extrude area in +z for fill volume; close to density ribbon. |
| Stacked area | — | Stacked ribbons in +z, one series per depth plane | ★★★★☆ | The 2D version stacks in y; the 3D version stacks in z, freeing y for the actual value. Each series is readable on its own plane. |
| Streamchart | — | Ribbon tower — each stream is an independent 3D ribbon whose y-baseline moves | ★★★★☆ | Same idea as stacked area; streams don't occlude each other. |

#### Geographic — "where?"

| Mark | Shipped? | 3D form | Readiness | Notes |
|---|---|---|---|---|
| Map (outline) | — | GeoJSON as extruded polygons on a floor disc | ★★★☆☆ | The map is just the substrate. |
| Choropleth | — | Region polygons extruded by value — the classic 3D-map "prism tower" look | ★★★★☆ | Instantly legible; well understood. |
| Hexbin map | — | Extruded hex prisms on a floor disc or globe | ★★★★★ | 3D hexbins are one of the most effective 3D visualizations, full stop. |
| Cartogram | — | Region shapes distorted by value; extrude or rescale | ★★★☆☆ | 3D doesn't help distort; the information is already encoded in shape. |
| Connection / flow map | — | Great-circle arcs or splines over a globe / map | ★★★★☆ | Natural 3D; `TubeGeometry` along a geodesic. |
| Bubble map | — | Spheres sized by value, anchored to points | ★★★☆☆ | Bubble + map composition. |

Geographic marks are out of scope for *strictly* hyperlocal use cases (UC1–UC3 do not need a globe). They are in scope for **the HUD minimap mentioned in UC3** (hall-scale floor plan of the conference) and for **UC4** (flight path). Worth scoping, not worth prioritizing before the hierarchy marks ship.

#### Flow — "what moves from where to where?"

| Mark | Shipped? | 3D form | Readiness | Notes |
|---|---|---|---|---|
| Network (force) | §9.6 | d3-force-3d | ★★★★★ | Scoped. |
| Tangled tree | §9.4 | z-separated tangle arcs | ★★★★★ | Scoped. |
| **Sankey** | — | Flow tubes with varying **cross-sections**, routed between stages | ★★★★★ | 2D sankeys collapse flows that would not occlude in 3D; depth separation and tube-thickness give the reader a genuine sense of flow volume. |
| Chord diagram | — | Vertical ring of labels around reader; chords arc through the interior; pinch a chord to trace | ★★★★☆ | The natural home for a chord diagram is a 360° ring around the head. |
| Arc diagram | — | Points on a line + arcs above the line; arcs extrude in +z by distance or strength | ★★★★☆ | Simple and effective; reuse our `arc` mark primitive. |
| **Edge bundling** | — | Hierarchical edge bundling in 3D — bundles route along the tree spine and burst away from it in depth | ★★★★★ | Uses the same "z = tangle depth" trick as §9.4; can render *very* dense graphs legibly. |

#### The top-tier 3D-native marks (★★★★★)

If we had to pick the six highest-leverage marks to add next after §9.1–§9.6, the d3-graph-gallery taxonomy tells us to pick:

1. **Circular packing** (Part of a Whole) — nested spheres; the 3D hierarchy viz most people haven't seen.
2. **Ridgeline** (Distribution) — depth-offset densities, gives a mountain range.
3. **Hexbin map** (Geographic) — extruded hex prisms; works on any floor-plan or map substrate.
4. **Parallel coordinates** (Ranking) — physically separated axes; solves 2D's overplot catastrophe.
5. **Sankey** (Flow) — 3D flow tubes with cross-sections; occlusion-free.
6. **Edge bundling** (Flow) — pairs with §9.4's tangle idea.

Each of these gives a signal the reader cannot see in 2D.

### 9.10 Demo order (when we ship M8+)

Suggested order, smallest-to-largest incremental risk. Marked **★** = top-tier 3D-native from §9.9.

- **M8** — Tree (wall + radial). Least new infrastructure; validates the edge-bundle builder and billboarded label cluster that almost every later mark depends on.
- **M9** — Treemap. Pure layout code, reuses the bar mark's instancing.
- **M10** — **★ Circular packing.** Nested spheres; small code, high visual payoff, uses the same `InstancedMesh` spheres as scatter + force.
- **M11** — Sunburst. Shares drill-down transition with Treemap; adds the arc-segment geometry builder.
- **M12** — **★ Ridgeline.** Each row is a density ribbon; the 3D offset trick is tiny engineering for a huge perceptual win.
- **M13** — Force-directed graph. Adds the physics tick scheduler.
- **M14** — **★ Parallel coordinates.** Vertical axis rods + connecting tubes; reuses edge-bundle builder from M8.
- **M15** — Tidy tree (cylindrical). Reuses tree infra + θ remap.
- **M16** — **★ Sankey.** Flow tubes with varying cross-section; first mark that requires a stage-layout solver (`d3-sankey`).
- **M17** — **★ Tangled tree.** Z-separated tangle arcs; depends on tidy-tree spine + edge-bundle builder.
- **M18** — **★ Hexbin map.** First geographic mark; needs a floor-disc substrate and `d3-geo`. Scope trigger: UC3 poster-session floor plan or UC4 flight path.
- **M19** — **★ Edge bundling.** Pairs with M17's tangle technique against a hierarchy.
- **M20+** — The long tail: chord diagram (360° ring), arc diagram (reuses `arc`), streamchart / stacked area (reuses `line` + depth), radar (polyhedra), violin (solid of revolution). Triage per dataspace demand.

Skip list — marks we've decided *not* to ship in 3D because the extra dimension adds no information: **pie / doughnut** (3D pie is a pastry, not a chart), **boxplot** (nothing gained), **cartogram** (distortion already encodes the value), **correlogram** (small-multiples don't benefit from depth).

---

## 10. Open Questions

1. **Code rotation cadence:** 30 s (TOTP-classic) feels too tight for someone fumbling to type in HMD; 60 s is the proposed default. Should the dataspace be able to advertise its own cadence?
2. **Code entropy:** 6 alphanumeric chars after ambiguity stripping ≈ 30 bits. Adequate for short-lived public joins; insufficient as a sole factor for private dataspaces (hence PKI handshake in §2.4).
3. **Spatial keyboard ergonomics:** typing 6 chars in HMD is still painful. Should the default input be a 6-wheel "slot machine" instead of a QWERTY?
4. **Multi-dataspace simultaneity:** can a user be joined to UC1 (their wearable) and UC2 (the room) at the same time? Proposal says yes; the HUD then shows a stack of dataspace chips and routes interactions to the nearest target.
5. **Avatar fidelity for R29:** soft sphere is the safe minimum; gesture-mirroring hands are a nice-to-have but raise privacy questions.
6. **Offline / intermittent connectivity (UC4):** how much of a manifest can be cached and replayed without violating freshness expectations on device state?

---

## 11. What Goes Into PROPOSAL.md "UI Spec V1"

Once this proposal is reviewed and revised, the following sections should land in `PROPOSAL.md` under `### UI Spec V1`. Status reflects what the `prototype/d3-spatial/` reference implementation has shipped as of 2026-04-18.

- **V1.1** Join-code onboarding flow (from §2) — **not yet implemented.** The join panel, QR scanner, and rotating-code protocol exist only as design in §2. No code in the prototype.
- **V1.2** Continuous-awareness HUD spec (from §3) — **partially implemented.** The dataspace chip strip (`DataspaceHud`), audio HUD, and debug HUD are shipped. Missing: latency sparkline, battery/device count, lock-state icon.
- **V1.3** Device introspection UX — the pin + control-puck pattern (R25, from UC2) — **not yet implemented.** No device pins or control pucks exist. The room-scale scenario remains design-only.
- **V1.4** People introspection UX — the avatar sphere + name tag (R26, from UC2/UC3) — **not yet implemented.** Multi-user presence is not wired.
- **V1.5** Service introspection UX — manifest-driven artifact bloom (R27, from UC3) — **partially implemented.** The manifest schema (`DataspaceManifest`, `MarkSpec`) and manifest loader (`loadManifest`, `registerAllBuilders`) are shipped. Manifest-driven artifact *bloom* (proximity-triggered appearance within ~1 m of a poster) is not wired.
- **V1.6** Data dashboard UX — `d3-spatial` charts as first-class scene objects (R28, from all UCs) — **implemented.** Four chart marks (line, bar, scatter, arc) plus seven spatial viz marks (tree, treemap, sunburst, circular packing, force graph, ridgeline, sankey) are shipped. Fluent Chart API, per-node interaction (hover, select, drag, drill-in/out), animated transitions, live data streaming, breadcrumb trails, and per-viz HUD buttons all work. Full interaction via desktop mouse and XR controller ray. Multi-hand support for force graph. FingertipGrab for hand-tracking direct manipulation. XRBrush for sweep-select.
- **V1.7** Shared-experience coordination model (R29, from UC2/UC3) — **not yet implemented.** No multi-user synchronization, avatar mirroring, or coordination locks. Dataspace federation is visual-only (focus dim).
- **V1.8** Spatial audio model (from §8) — **implemented.** `THREE.PositionalAudio` per mark with procedural sine-tick on hover. `AmbientBed` wraps Omnitone FOA renderer with head-pose rotation. In-memory 4-channel procedural drone ships as test bed. AudioListener re-parented to XR camera on session start. Manifest schema includes `ambisonicBedUrl` and `acousticEnvironment` fields. User opt-in required for ambient beds.
- **V1.9** Manifest schema (from §9, §10) — **implemented.** `DataspaceManifest` type with `version`, `name`, `scaleTag`, `owner`, `ambisonicBedUrl`, `acousticEnvironment`, `marks[]`, and `joinCode` config. `MarkSpec` type covering all 11 shipped mark types plus 4 future types (`parallel`, `tangled-tree`, `edge-bundle`, `hexbin`). Inline and URL data sources. Builder registration pattern decouples manifest from rendering. Example manifest included in `src/manifest/schema.ts`.

Each section in the final spec should include: a labeled wireframe, the three-mesh-ui component tree, the interaction state machine, and the minimum manifest schema the dataspace must publish for that surface to render.
