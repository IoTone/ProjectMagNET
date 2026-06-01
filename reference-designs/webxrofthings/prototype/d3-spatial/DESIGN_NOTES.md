# Design Notes — Deferred Decisions

Captured: 2026-04-21

---

## 1. Toolbar occlusion by gallery content

**Problem:** The gallery grid (4×3, 12+ cells) now extends vertically enough that the toolbar at the bottom is behind or overlapped by gallery content. When the user activates a scene (gallery or charts), the toolbar is hard to reach.

**Proposed solutions (pick one):**

### Option A — Wrist-anchored toolbar (recommended)

Move the toolbar to the user's non-dominant wrist. It appears when the user glances at their wrist (same palm-up detection as HandMenu from P1.3).

- Reuse the `HandMenu` infrastructure already built
- Toolbar buttons become DataspaceMenu items on the wrist
- No screen-space occlusion — the toolbar is always accessible regardless of scene content size
- Natural gesture: "check your watch" to see the menu

### Option B — Camera-locked hamburger with chevron indicator

A small, semi-transparent indicator locked to the camera view (HUD-attached, not world-space). Tapping it opens the full toolbar.

**Indicator design:**
- Three vertical chevrons `>>>` (not hamburger lines — chevrons feel more spatial/directional)
- Positioned at lower-left or lower-right of the user's view
- Very transparent when idle (15-20% opacity) — just enough to remind the user it exists
- On gaze or hover: brightens to 80% opacity
- On pinch: toolbar panel slides in from the edge, world-locked at comfortable distance
- On pinch again or after 5s idle: toolbar slides out, indicator returns

**Implementation sketch:**
- The indicator is a small three-mesh-ui Block parented to the XR camera (follows head)
- It's NOT the toolbar itself — it's a toggle that shows/hides the world-locked toolbar
- Avoids the best-practices issue warning about camera-locked UI (the indicator is tiny and non-interactive until tapped; the actual toolbar is still world-locked when opened)

### Option C — Hybrid

- Wrist menu for XR (hand-tracking available)
- Camera-locked chevron indicator for controllers (no wrist joint data)
- Falls back to floating world-locked toolbar on desktop

**Decision needed:** Which option to implement. Recommendation: start with Option B (camera-locked chevron) since it works on all platforms, then add wrist attachment as an enhancement.

---

## 2. Multi-dataspace server with dynamic join keys

**Problem:** Currently the mock server supports one dataspace (`demo-room`) with one rotating code sequence. For a real deployment, we need N dataspaces on one server, each with its own join code.

**Proposed architecture:**

### Server-side

```
GET /api/v1/dataspaces
→ [
    { id: "kords-livingroom", name: "Living Room", scaleTag: "room", codeAvailable: true },
    { id: "conf-hall-a",      name: "Hall A",      scaleTag: "hall", codeAvailable: true },
    { id: "dkords-wrist",     name: "My Wrist",    scaleTag: "personal", codeAvailable: false, locked: true },
  ]

GET /api/v1/dataspaces/:id/code
→ { code: "ABC123", expiresIn: 280 }
  (generates/returns the current rotating code for that specific dataspace)

POST /api/v1/join
  Request:  { code: "ABC123", dataspace?: "kords-livingroom" }
  Response: { status: "accepted", token: "...", manifest_url: "/api/v1/dataspaces/kords-livingroom/manifest", dataspace: "kords-livingroom" }
```

### Code management

Each dataspace has its own independent code rotation:
```typescript
interface DataspaceEntry {
  id: string;
  name: string;
  scaleTag: string;
  manifestPath: string;
  codeRotationSeconds: number;
  currentCode: string;
  previousCode: string | null;
  codeGeneratedAt: number;
  locked?: boolean;        // if true, code doesn't rotate — fixed code set by owner
  fixedCode?: string;      // the fixed code (only if locked)
}
```

### "Locked code" flag

For development/demo purposes, a dataspace can have a **locked code** that never rotates:

```json
{
  "id": "demo-room",
  "name": "Demo Room",
  "locked": true,
  "fixedCode": "AAAAAA"
}
```

This is useful for:
- Demo setups where you print the code on a sign
- Development where you don't want to look up the current code
- Kiosk displays that show a permanent QR

In production, locked codes would be disabled or require additional authorization (e.g., the dataspace owner sets the lock via an admin API).

### Dynamic client-side key fetch

For a more seamless UX, the HMD could fetch available dataspaces and their codes automatically:

1. User opens `hlxr.org` → Join panel appears
2. If on the same network as a context engine, the client discovers it via mDNS or a known URL
3. Client fetches `GET /api/v1/dataspaces` → shows a list of available dataspaces instead of (or alongside) the code-entry slots
4. User pinches a dataspace from the list → client fetches that dataspace's code automatically → submits it → joins

This bypasses manual code entry entirely when the client can reach the server. The code still exists as a fallback for cross-network joins (e.g., someone tells you the code verbally).

### Client-side changes needed (future)

- JoinPanel gains a "nearby dataspaces" section that auto-populates from the server
- Each listed dataspace is a pinchable chip (like the recently-joined chips)
- Pinching a chip auto-fills the code and submits
- Falls back to manual entry if the server isn't reachable

### NOT changing now

This is a design note only. Current implementation stays as-is:
- One dataspace per server
- Sequential codes starting from AAAAAA
- 5-minute rotation
- No locked-code flag yet

---

## 3. Toolbar button labels for reference

Current toolbar order: Join | Gallery | Charts | Morph | Recenter | Set Floor

If we move to a wrist/chevron model, these become context-sensitive:
- **App-level:** Join, Recenter, Set Floor (always available)
- **Content-level:** Gallery, Charts, Morph (scene switchers — maybe become a sub-menu)
- **Dataspace-level:** items from the manifest HUD (Refresh, Leave, etc.)

The three levels could be visually grouped or placed on different surfaces (app = wrist, content = floating near scene, dataspace = per-mark HUD).
