# Use Case Specifications — Open Decisions

Status: Stub · Date: 2026-04-19 · Owner: David J. Kordsmeier

These are the decisions YOU need to make for each use case before we can build the demo-ready dataspaces. Each section lists what's already built, what's open, and the specific questions to answer.

---

## UC1 — Personal Dataspace (Fitness Wearables)

**What's built:** Line chart mark with live data streaming (HR auto-updates every 2s), per-node hover, inspector card, dataspace federation (UC1 tagged as "wrist" / 👤 personal).

**What's open:**

### Devices
- [ ] Which wearable? (M5 wristband? Commercial BLE HR strap? Ring?)
- [ ] Connection method? (WebBLE is blocked on Quest/Spectacles — needs a bridge. MQTT via context engine? BLE→ESP32→WiFi→WSS?)
- [ ] What data streams? (Heart rate only? SpO2? Steps? Skin temp? Accelerometer?)
- [ ] Sampling rate? (1 Hz? 4 Hz? What's the device capable of?)

### Dataspace identity
- [ ] Dataspace name? (e.g. `dkords-wrist`, `my-vitals`, something memorable per R20)
- [ ] Who owns it? (The wearer — auto-created on first device pairing?)
- [ ] Public or private? (Personal data → private by default? PKI-secured per §2.4?)
- [ ] Join code displayed where? (On the wearable's tiny screen? On a phone companion app?)

### Marks to show
- [ ] Which marks beyond the HR line chart? (SpO2 as a second line? Steps as a bar chart? Activity breakdown as a treemap? Daily summary as a sunburst?)
- [ ] How many marks in the dataspace manifest? (1 minimal? 3-4 for a rich demo?)
- [ ] Time window? (Last 60 min? Last 24h? User-configurable?)

### HUD menu items
- [ ] What actions make sense for a personal dataspace? (Start/stop recording? Share with doctor? Export data? Switch time window?)

### Demo script
- [ ] What's the story? (User puts on HMD, joins their own wrist dataspace, sees live HR, reviews last hour, shares a snapshot?)
- [ ] Duration? (30 seconds? 2 minutes?)
- [ ] What's the "wow" moment? (Seeing your own heartbeat in 3D space? Pinching the timeline to scrub?)

---

## UC2 — Room-Scale Dataspace (Home / Lighting & Data)

**What's built:** Tree mark (device topology), treemap (area-based), bar chart (temperature), force graph (network), spatial audio, dataspace federation (UC2 tagged as "room" / 🏠 room), manifest schema, join-code onboarding.

**What's open:**

### Devices
- [ ] Which room? (David's living room? A conference room? A lab?)
- [ ] What IoT devices? (M5 temp/humidity sensors? Smart lights? HVAC? Motion sensors? Camera?)
- [ ] How many devices? (3-5 for a clean demo? 10+ for a realistic room?)
- [ ] Connection path? (Devices → MQTT broker → context engine → WSS → manifest → d3-spatial?)
- [ ] Self-hosted context engine? (Raspberry Pi? M5StampS3? Cloud VM? Laptop running the mock server extended to real MQTT?)

### Dataspace identity
- [ ] Dataspace name? (e.g. `kords-livingroom`, `lab-west`, `conf-room-3b`)
- [ ] Public or private? (Room-owner controls access? Guests join via code on a kiosk screen?)
- [ ] Join code display? (E-ink display on the wall? Printed QR next to the door? Kiosk tablet?)

### Marks to show
- [ ] Device topology tree — which hierarchy? (gateway → access-point → sensor? Or room → zone → device?)
- [ ] Live sensor data — which charts? (Temperature line over 24h? Humidity? Light level? Occupancy bar chart?)
- [ ] Control surfaces — what can the user CONTROL, not just observe? (Light brightness slider? HVAC set-point? Scene presets?)
- [ ] Network graph — show device-to-device communication? (Which devices talk to which?)
- [ ] Energy flow sankey — power consumption breakdown?

### HUD menu items
- [ ] What room-specific actions? (Toggle all lights? Set scene preset? Arm/disarm security? Check door lock? View camera feed?)

### Demo script
- [ ] What's the story? (Guest enters room, scans code on kiosk, sees room topology, adjusts lighting, checks temperature trend?)
- [ ] Duration?
- [ ] What's the "wow" moment? (Pinching a light node and seeing the real light respond? Walking up to a sensor pin anchored to the real device's location?)

---

## UC3 — Conference Poster Session (Interactive Data & Experiences)

**What's built:** All 15 mark types, drill-in, morph, breadcrumb, manifest-driven gallery, spatial TOC concept (§4.3), per-viz HUD.

**What's open:**

### Venue
- [ ] What conference/event? (A real conference? A simulated poster session? An art exhibition?)
- [ ] How many posters/stations? (3 for a tight demo? 12 for a full session?)
- [ ] Physical layout? (Linear hallway? Open floor? Circular arrangement?)

### Dataspaces
- [ ] One parent dataspace for the hall + one child per poster? Or independent dataspaces per poster?
- [ ] Poster discovery — how does the user find available posters? (Hall-level minimap mark? Proximity-based auto-join? QR per poster?)
- [ ] Each poster publishes its own manifest — what mark types make sense for academic/research content? (Force graph of citations? Treemap of dataset composition? Ridgeline of experimental distributions? 3D scatter of results?)

### Content
- [ ] What data goes in each poster? (Real research data? Synthetic demo data? A mix?)
- [ ] Interactive artifacts — what can the user DO beyond viewing? (Rotate a 3D model? Drill into a dataset? Export a citation?)
- [ ] "Take it with you" (§4.3) — pinch an artifact to save a reference to your personal dataspace. How does this work technically? (Copy DOI to clipboard? Save manifest URL to localStorage? Add to a "favorites" list in the personal dataspace?)

### HUD menu items
- [ ] Per-poster: (Bookmark? Share? View abstract? Contact author? Next poster?)
- [ ] Hall-level: (Map of all posters? Filter by topic? Show only active presenters?)

### Demo script
- [ ] What's the story? (Attendee enters hall, joins hall dataspace, sees minimap, walks to a poster, joins poster dataspace, explores 3D artifacts, saves a reference, moves to next poster?)
- [ ] Duration?
- [ ] What's the "wow" moment? (The spatial TOC as a navigable radial map? A molecule visualization you can rotate? A live dataset that updates during the session?)

---

## UC4 — Airplane Seat (In-Flight Experience)

**What's built:** Basic design in §4.4 of XR_UX-proposal1.md. Manifest schema supports offline cache concept. Not prototyped.

**What's open:**

### Everything
- [ ] Is this in scope for the current demo round? (Or deferred?)
- [ ] If in scope: what's the physical setup? (A chair with a display showing the join code? A mock seatback screen?)
- [ ] What data? (Flight path map? Entertainment catalog? Seat controls? In-flight WiFi stats?)
- [ ] Offline tolerance — how much of the manifest can be cached? (Pre-loaded on boarding? Full manifest cached? Only static marks, no live data?)

---

## Cross-cutting decisions

### Dataspace naming convention
- [ ] What's the pattern? (`owner-location`? `org-room-number`? Free-form?)
- [ ] Maximum length? (Per R20: "easily memorized and short enough to be quickly used")
- [ ] Character restrictions? (Lowercase alphanumeric + hyphens? Same ambiguity-stripped set as join codes?)

### Join/leave UX flow
- [ ] How fast should joining be? (Target: under 15 seconds per §1, Design Goal 1)
- [ ] Multi-dataspace: can the user be in UC1 + UC2 simultaneously? (Already supported technically — but what does the UX look like? Split view? Stacked chips? One active, others backgrounded?)
- [ ] Leaving: instant disconnect? Fade-out transition? Confirmation dialog?
- [ ] Re-joining: "recently joined" chips in the join panel. How many to show? (3 per the spec)

### Context engine deployment
- [ ] For each demo use case, where does the context engine run? (Same machine as the dev server? Separate Pi? Cloud?)
- [ ] Do all use cases share one context engine instance, or one per dataspace?
- [ ] What's the MQTT broker? (Mosquitto on the Pi? Cloud MQTT? The context engine embeds one?)

### Data pipeline
- [ ] Device → context engine protocol? (MQTT for sensors? HTTP POST for batch data? CoAP for constrained devices?)
- [ ] Context engine → d3-spatial protocol? (WSS for real-time? HTTPS + polling for batch? Both, depending on the mark's `refreshInterval`?)
- [ ] Data format? (JSON? CBOR? Protobuf? — JSON is simplest, matches manifest schema)

### Physical setup for demos
- [ ] What hardware is available? (List of M5 devices, sensors, displays for join codes)
- [ ] Network requirements? (WiFi for all devices + HMDs on same LAN? Or everything through cloud?)
- [ ] Backup plan if live devices fail? (Fall back to synthetic data? Pre-recorded data replayed?)

---

## How to use this document

1. **Read each section.** Check the boxes you can answer now.
2. **Fill in your answers** inline (replace the question marks with decisions).
3. **For each answered UC**, I'll create:
   - A manifest JSON (`examples/uc1-wrist.json`, `examples/uc2-room.json`, etc.)
   - A demo script (step-by-step walkthrough with timing)
   - Device wiring code (MQTT → context engine → WSS bridge)
   - Any new marks or interactions specific to that UC
4. **Start with the UC you can test first** — probably UC2 (room) since it uses devices you already have.

The hard-coded demo gallery (`?scene=demo`) remains as a fallback. Each UC becomes a real manifest-driven dataspace that you join via the join-code flow.
