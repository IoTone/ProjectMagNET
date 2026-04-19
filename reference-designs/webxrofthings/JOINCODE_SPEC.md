# Join-Code Onboarding — V1.1 Implementation Specification

Status: Specification · Date: 2026-04-18 · Implements: `XR_UX-proposal1.md §2`

---

## Overview

This specification details the implementation plan for the join-code onboarding flow — the first experience a user has when entering a dataspace. The design is fully described in `XR_UX-proposal1.md §2`; this document adds implementation-level detail: component structure, protocol, state machine, error handling, and integration points.

The design philosophy: **TOTP-inspired, authenticator-free.** The dataspace generates a rotating short code; the user types it (or scans it) to join. No app store, no account, no third-party authenticator.

---

## 1. User Flow (from §2.2)

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

## 2. Components

### 2.1 Join Panel (`src/onboarding/JoinPanel.ts`)

A floating three-mesh-ui panel, ~40 cm wide at arm's length (~1.2 m from the user), containing:

```
ThreeMeshUI.Block (root, width: 0.4, height: 0.28, padding: 0.02)
├── Block (titleRow)
│   └── troika Text "Join a dataspace"     (TEXT.primary, fontSize 0.024)
├── Block (codeRow, contentDirection: "row")
│   ├── CodeSlot × 6                       (each 0.04 wide, 0.05 tall)
│   │   └── troika Text (single char)      (TEXT.emphasis, fontSize 0.032)
├── Block (statusRow)
│   └── troika Text "Enter the code..."    (TEXT.muted, fontSize 0.014)
├── Block (actionsRow, contentDirection: "row")
│   ├── Button "Submit"                    (primary action)
│   ├── Button "Clear"                     (secondary)
│   └── Button "Scan QR"                   (tertiary, if device supports)
├── Block (recentsRow)
│   └── Chip × 3 (recently joined)         (stored in localStorage)
└── Block (footer)
    └── troika Text "🔒 secure · hlxr.org" (TEXT.dim, fontSize 0.010)
```

**State machine:**

```
IDLE → ENTERING (user starts typing) → SUBMITTING (code sent to server)
  → ACCEPTED (session minted, manifest loading)
  → REJECTED (code invalid/expired, show error, return to ENTERING)
  → ERROR (network failure, show retry)
```

**Interaction:**
- Each CodeSlot is a Hoverable registered with Interact. Pinch a slot → it becomes the "active" slot (highlighted border). A virtual keyboard or slot-wheel appears for input.
- "Submit" button fires the join request.
- "Clear" resets all slots.
- "Scan QR" (if available) launches the system-level QR scanner via a deep link.

### 2.2 Code Input — Slot Wheel (`src/onboarding/SlotWheel.ts`)

Per §2 Open Question 3: typing 6 chars in HMD is painful. The slot wheel is a vertical "fruit machine" selector per slot:

```
SlotWheel (per active slot)
├── Up arrow button (previous char)
├── Current char display (large, TEXT.emphasis)
├── Down arrow button (next char)
└── Character set: A-Z, 2-9 (30 chars, ambiguity-stripped per §2.1)
```

- Pinch up/down arrows to cycle characters.
- Or: XR controller thumbstick up/down cycles, trigger confirms and advances to next slot.
- Or: physical keyboard input (for Quest with paired BT keyboard) — chars auto-fill slots.

**Character set (30 chars, ambiguity-stripped):**
`A B C D E F G H J K M N P Q R S T U V W X Y Z 2 3 4 5 6 7 8 9`
(No `0/O`, `1/I/L`)

### 2.3 Join Server Protocol (`src/onboarding/protocol.ts`)

**Endpoints (relative to `hlxr.org` or self-hosted):**

```
POST /api/v1/join
  Request:  { code: "ABC123" }
  Response: { status: "accepted", token: "...", manifest_url: "...", dataspace: "kords-livingroom" }
          | { status: "rejected", reason: "expired" | "invalid" | "rate_limited" }
          | { status: "challenge", challenge: "...", publicKeyRequired: true }  // private dataspaces (§2.4)

POST /api/v1/join/verify
  Request:  { challenge: "...", signature: "...", publicKey: "..." }
  Response: { status: "accepted", token: "...", manifest_url: "..." }
          | { status: "rejected", reason: "signature_invalid" }
```

**Token:**
- JWT, 1 hour TTL, contains: `dataspace_id`, `session_id`, `issued_at`, `capabilities[]`.
- Sent as `Authorization: Bearer <token>` on all subsequent API calls and WebSocket connections.
- Stored in `sessionStorage` (not `localStorage`) — cleared on tab close.

**Rate limiting:**
- 5 attempts per IP per minute.
- On rate limit: `status: "rejected", reason: "rate_limited"`, panel shows "Too many attempts, wait 60s."

### 2.4 Private Dataspace Flow (`src/onboarding/pkiChallenge.ts`)

Per §2.4:
1. Server returns `status: "challenge"` with a random challenge string.
2. Client generates an ephemeral ECDSA P-256 keypair via WebCrypto:
   ```typescript
   const keypair = await crypto.subtle.generateKey(
     { name: 'ECDSA', namedCurve: 'P-256' },
     true, ['sign', 'verify']
   );
   ```
3. Client signs the challenge:
   ```typescript
   const sig = await crypto.subtle.sign(
     { name: 'ECDSA', hash: 'SHA-256' },
     keypair.privateKey,
     new TextEncoder().encode(challenge)
   );
   ```
4. Client sends `{ challenge, signature, publicKey }` to `/api/v1/join/verify`.
5. Server validates → returns token.
6. Status footer shows "🔐 verifying..." during step 3-5, then "🔐 verified" on success.

### 2.5 Manifest Loading (`src/onboarding/loader.ts`)

After receiving the token + `manifest_url`:
1. Fetch manifest: `GET manifest_url` with `Authorization: Bearer <token>`.
2. Validate against `manifest.schema.json`.
3. Call `registerAllBuilders()` + `loadManifest(manifest)`.
4. Add loaded marks to the scene.
5. Transition: Join panel fades out, dataspace materializes with enter animation (marks spring in from center, §M14 transitions).

## 3. Visual Design

### 3.1 Panel Appearance

- Background: `0x2a2520` (warm dark brown, visible on passthrough) @ 95% opacity.
- Border: `EDGE.link` (`0xb8a380`) @ 90% opacity, 1.5mm width.
- Border radius: 8mm.
- All text uses `TEXT.*` palette.
- Code slots: individual blocks with `TEXT.muted` borders when empty, `TEXT.primary` border when active, `TEXT.accent` border when filled.

### 3.2 Status Indicators

| State | Status text | Color |
|---|---|---|
| Idle | "Enter the code shown on the host device" | `TEXT.muted` |
| Entering | "3 of 6 characters" | `TEXT.body` |
| Submitting | "Joining..." | `TEXT.warn` |
| Accepted | "✓ Connected to kords-livingroom" | `TEXT.accent` |
| Rejected (expired) | "Code expired — ask for a new one" | `TEXT.error` |
| Rejected (invalid) | "Code not recognized" | `TEXT.error` |
| Rate limited | "Too many attempts — wait 60s" | `TEXT.error` |
| PKI verifying | "🔐 Verifying identity..." | `TEXT.warn` |
| Network error | "Connection failed — tap to retry" | `TEXT.error` |

### 3.3 Recently Joined

- Up to 3 chips stored in `localStorage` under key `hlxr:recent`.
- Each chip shows: dataspace name + last-joined timestamp.
- Pinch a chip → attempt to rejoin (fetch manifest directly with stored token if still valid, else show code entry).
- Chips use `0x2a2520` background, `TEXT.muted` border, `TEXT.body` text.

## 4. Placement & Anchoring

- The Join panel is the **first thing the user sees** after entering an XR session from `hlxr.org`.
- Placed 1.2 m in front of the user at eye level (same `placeAnchorInFrontOfUser` logic).
- If the user has a recently-joined dataspace, the panel shows but the "rejoin" chip is highlighted as a quick path.
- Once a dataspace is joined, the panel slides back (z += 0.5 over 400ms) and fades out. The dataspace content materializes in its place.
- The panel can be re-summoned from the toolbar via a new "Join" button.

## 5. Integration Points

### 5.1 With existing prototype

- **Toolbar:** Add "Join" button between "Gallery" and "Charts". Opens the Join panel. While the panel is active, Gallery/Charts scenes are hidden.
- **Manifest system:** `loadManifest()` is already implemented. The join flow's output is a manifest URL → feed directly into the existing loader.
- **Scene transition:** Use the tween framework (`tweenMeshes`) for fade-in/out of the join panel and spring-in of dataspace marks.
- **Audio:** Play a success chime (new procedural buffer, similar to hover tick but longer + ascending pitch) on accepted join.

### 5.2 With the Hyperlocal Context Engine (future)

- The join server is the **Hyperlocal Context Engine** described in `PROPOSAL.md`.
- For the prototype, mock the server: a small Express/Fastify server that generates rotating codes, validates joins, and serves static manifests.
- The mock server can run on the same machine as the dev server, or be a separate process.

### 5.3 With the XR browser scope (§7)

- The `hlxr-browser` fork (§7.3 Tier 1) would make the Join panel the browser's **home screen** — boot directly into it instead of a URL bar.
- Deep links: `hlxr.org/join?code=ABC123` → auto-fills the code and submits.
- QR scan: the browser-level QR scanner feeds into the same deep link.

## 6. Files to Create

| File | Purpose |
|---|---|
| `src/onboarding/JoinPanel.ts` | Main panel UI (three-mesh-ui + troika) |
| `src/onboarding/SlotWheel.ts` | Per-slot character selector |
| `src/onboarding/protocol.ts` | Join API client (fetch + WebCrypto) |
| `src/onboarding/pkiChallenge.ts` | Private dataspace PKI flow |
| `src/onboarding/loader.ts` | Post-join manifest fetch + scene transition |
| `src/onboarding/types.ts` | Shared types (JoinState, JoinResult, etc.) |
| `server/mock-join-server.ts` | Mock server for development (Express/Fastify) |

## 7. Files to Modify

| File | Change |
|---|---|
| `src/main.ts` | Add "Join" toolbar button, wire JoinPanel, scene transitions |
| `src/ui/Toolbar.ts` | Add "Join" button slot |
| `src/manifest/loader.ts` | Accept auth token in fetch headers |
| `index.html` | Add `?code=` URL param handling for deep links |

## 8. Testing Plan

### 8.1 Smoke tests
- `m-join-idle` — panel visible with empty slots
- `m-join-entering` — 3 of 6 chars filled
- `m-join-accepted` — success state with dataspace name
- `m-join-rejected` — error state

### 8.2 On-device
- Quest 3: slot-wheel cycling via pinch, keyboard input via BT keyboard, submit flow
- Spectacles: slot-wheel cycling via pinch (no keyboard), panel visibility on passthrough

### 8.3 Mock server
- Start: `node server/mock-join-server.ts`
- Generates a new 6-char code every 60s, logs to stdout
- Accepts any code that matches current or previous rotation (grace period)
- Returns a static manifest (the room-dataspace example)
- Runs on port 3001; the dev server proxies `/api/v1/` to it

## 9. Open Design Decisions

1. **Slot wheel vs virtual keyboard:** Spec recommends slot wheel (§2 Open Question 3). Could also offer a QWERTY virtual keyboard as an alternative for users who prefer it. Start with slot wheel only; add keyboard as a future option.

2. **Code display on host device:** The dataspace owner's device shows the rotating code. For the prototype, the mock server logs it to stdout. For production, the context engine would show it on a web dashboard, an E-ink display, or a companion app.

3. **Token refresh:** JWT has 1h TTL. Should the client auto-refresh before expiry? For the prototype, no — session expires, user re-enters code. For production, add a refresh endpoint.

4. **Multi-dataspace:** Can a user be joined to multiple dataspaces simultaneously? The prototype already supports this (dataspace federation, M7). The join flow should allow joining additional dataspaces without leaving the current one.

5. **Offline join:** If the user has a cached manifest and a still-valid token, skip the code entry entirely. The "recently joined" chip path handles this.

## 10. Dependencies

- **three-mesh-ui** — panel layout (already in project)
- **troika-three-text** — text rendering (already in project)
- **WebCrypto API** — ECDSA keypair generation + signing (built into browsers)
- **Express or Fastify** — mock join server (new dev dependency)
- **jsonwebtoken** — JWT generation on the server side (new server dependency)

## 11. Estimated Effort

| Phase | Scope | Effort |
|---|---|---|
| Phase 1 | JoinPanel UI + SlotWheel + mock submit (no real server) | 1 session |
| Phase 2 | Mock join server + real protocol flow + manifest loading | 1 session |
| Phase 3 | PKI challenge flow + recently-joined chips + polish | 1 session |

Phase 1 is demo-able: the panel renders, slots cycle, "submit" shows a fake success. Phase 2 connects it to real data. Phase 3 adds security and convenience.
