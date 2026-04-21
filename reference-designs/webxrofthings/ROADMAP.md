# d3-spatial Roadmap

Status: Active · Updated: 2026-04-19

---

## Completed (M0–M20)

See `prototype/d3-spatial/STATUS.md` for full details. Summary: 48 source files, 97 screenshots, 15 mark types, join-code panel, layout morph, multi-hand interaction, fingertip grab, XR brush, live data streaming, manifest schema, breadcrumbs, per-viz HUD, spatial audio.

---

## Phase 1 — Connect the join flow end-to-end

### P1.1 Mock join server + real protocol (V1.1 Phase 2)
- Express/Fastify server on port 3001
- Generates rotating 6-char codes (60s rotation, configurable)
- `/api/v1/join` endpoint validates code, returns JWT + manifest URL
- Vite dev server proxies `/api/v1/` to the mock server
- JoinPanel submits to real endpoint instead of mock validation
- On success: fetches manifest from the URL, loads dataspace via `loadManifest()`
- On failure: shows real error from server (expired, invalid, rate-limited)

### P1.2 Manifest-driven gallery
- `?manifest=url` query param OR post-join manifest URL loads a gallery from any conformant JSON
- Remove hard-coded `sampleTree` / `sampleGraph` / etc. from the default gallery path
- The gallery becomes a rendering of whatever the manifest describes
- Keep the hard-coded gallery available as a fallback / demo mode (`?scene=demo`)
- Validates `manifest.schema.json` end-to-end

### P1.3 Configurable dataspace HUD / hand menu

Each dataspace needs its own menu — not the global toolbar, but a per-dataspace context menu that the dataspace manifest can configure.

**Design options (choose one or hybrid):**

**Option A — Manifest-configured HUD panel:**
- The `DataspaceManifest` gains a `hud` field describing menu items:
  ```json
  "hud": {
    "items": [
      { "id": "refresh", "label": "Refresh data", "icon": "refresh", "action": "reload-marks" },
      { "id": "audio", "label": "Ambient audio", "icon": "audio", "action": "toggle-ambient" },
      { "id": "share", "label": "Share code", "icon": "share", "action": "show-join-code" },
      { "id": "leave", "label": "Leave", "icon": "exit", "action": "leave-dataspace" }
    ],
    "position": "bottom" | "wrist" | "hand-menu"
  }
  ```
- Renderer builds a three-mesh-ui panel from the manifest items
- Actions are a fixed vocabulary the renderer knows how to execute
- Dataspace owner controls WHAT appears, renderer controls HOW it renders

**Option B — Hand menu (wrist-anchored):**
- A menu that appears when the user looks at their palm (palm-up gesture detection)
- Or: attached to the wrist joint, visible when the user rotates their wrist toward their face
- Items come from the manifest `hud` field (same as Option A)
- Quest hand-tracking provides wrist joint; menu parents to it
- Falls back to a floating panel near the non-dominant hand if no wrist joint

**Option C — Hybrid (recommended):**
- Default: floating panel at lower-left of the dataspace anchor (like the current toolbar but per-dataspace)
- If hand-tracking is active: attach to wrist joint, show on palm-up gesture
- Items from the manifest; fallback items (Refresh, Leave) always present
- Global toolbar (Gallery/Charts/Morph/Join/Recenter/Set Floor) stays as the app-level chrome
- Dataspace HUD is the content-level chrome

**Implementation:**
- New manifest field: `DataspaceManifest.hud`
- New component: `src/ui/DataspaceHud.ts` (not to be confused with the existing `DataspaceHud` which is the chip strip — rename that to `DataspaceChips`)
- New component: `src/ui/HandMenu.ts` for wrist-anchored variant
- Wire to hand-tracking joints when available

### P1.4 Real device integration + live data sources

Connect the prototype to real IoT devices and live data instead of canned samples.

**Target devices (from Project MagNET reference designs):**
- M5 temperature/humidity sensor → live `line` chart of sensor readings
- M5 lighting controller → interactive `bar` chart of zone brightness + control pucks
- BLE wearable (heart rate) → live `line` chart streaming via WebBLE (Quest doesn't support WebBLE, so bridge through the join server's WebSocket)

**Architecture:**
```
┌─────────────┐    ┌──────────────┐    ┌────────────────┐    ┌───────────┐
│ IoT Device  │ →  │ Context      │ →  │ d3-spatial      │ →  │ HMD       │
│ (M5/BLE)    │    │ Engine       │    │ renderer        │    │ (Quest)   │
│             │    │ (join server)│    │ (manifest+WSS)  │    │           │
└─────────────┘    └──────────────┘    └────────────────┘    └───────────┘
```

- IoT devices push data to the context engine via MQTT/CoAP/HTTP
- Context engine exposes data as WebSocket streams in the manifest:
  ```json
  { "source": "url", "url": "wss://hlxr.org/ds/room/temp", "shape": "series", "refreshInterval": 0 }
  ```
- `loadManifest()` already supports URL data sources; extend with WebSocket subscriptions
- On each WS message: call `chart.updateData(newData)` (live streaming already works from M17)
- The manifest's `refreshInterval: 0` means "real-time stream, not polling"

**Implementation steps:**
1. Extend `src/manifest/loader.ts` to handle WebSocket data sources (subscribe, pipe to mark)
2. Build a bridge service that reads from MQTT/CoAP and exposes WSS
3. Create a real manifest for an actual room (David's living room — UC2)
4. Test with a real M5 sensor pushing data through the pipeline

**What this proves:** the full stack from physical device → context engine → manifest → spatial viz → HMD works end-to-end. Not canned data; a real room with real sensors updating in real time in XR.

---

## Phase 2 — Multi-user + shared experience

### P2.1 V1.7 Shared presence
- WebSocket signaling server (extend the join server)
- When two users join the same dataspace, server relays cursor positions
- Each user sees the other's beam + reticle as a ghost avatar (soft sphere + name tag)
- Interactions propagate: if user A drills into a node, user B sees the drill-in
- Last-writer-wins for simple controls; coordination lock for critical ones (per R31)

### P2.2 Multi-dataspace simultaneous join
- User joined to UC1 (wearable) AND UC2 (room) at the same time
- HUD chip strip shows both; interactions route to the nearest target
- Already partially implemented in M7 (dataspace federation + focus dim)

---

## Phase 3 — Production quality

### P3.1 Bundle splitting
- Dynamic imports for d3-force-3d, Omnitone, each viz builder
- Tree-shake unused marks in production builds

### P3.2 Performance profiling
- 90 fps validation at 500+ force nodes on Quest
- Octree-based hit testing if raycaster becomes a bottleneck
- LOD for text labels (hide at distance, show on approach)

### P3.3 Accessibility
- Visible equivalent for every audio cue
- Reduced-motion user preference (skip tweens, instant layout)
- High-contrast mode (boost all opacities, thicker edges)

### P3.4 Security
- V1.1 Phase 3: PKI challenge for private dataspaces
- Cross-dataspace data isolation (not just visual dim)
- Token refresh before expiry
- Rate limiting enforcement on the client side

---

## Phase 4 — Platform expansion

### P4.1 hlxr-browser (§7)
- Wolvic fork with join-panel as home screen
- WebBLE, Web MIDI tier 2 features
- User-managed root CA store

### P4.2 Apple Vision Pro
- Safari WebXR test pass
- Note platform gaps
- visionOS spatial computing features (shared spaces, window placement)

### P4.3 Additional HMD platforms
- Pico (via Wolvic)
- Lynx
- Future open-hardware XR devices
