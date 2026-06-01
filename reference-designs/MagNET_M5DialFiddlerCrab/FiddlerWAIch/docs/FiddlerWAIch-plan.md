# FiddlerWAIch — watchOS 10+ Standalone App

## Status (2026-04-18): Phase I code complete, paused on real-device networking block

Simulator testing confirms the app connects to HiveMQ public and receives messages correctly. Real-device testing on the available Apple Watch Series 5 (watchOS 10.x) is blocked by what appears to be a watchOS third-party networking restriction: `URLSessionWebSocketTask` and `NWConnection` both fail with `NSURLErrorNotConnectedToInternet` / `ENETDOWN` while plain `URLSession.dataTask` HTTP succeeds. `NWPathMonitor` reports `path=NO NET, iface=other` for our app even though the watch has a valid Wi-Fi DHCP lease and Apple's own apps work. Full analysis + proof probes + proposed workarounds live in `../README.md#watchos-third-party-networking-limitation`.

**To resume:** either switch to HTTP long-poll through a relay server (Option A in the README) or retest on newer watch hardware.

## Context

Native Apple Watch dashboard that mirrors the M5StickC Plus Crawdad (multi-session Claude Code monitor). Standalone watchOS 10+ app in Swift/SwiftUI — no iOS companion, no ESP32. Subscribes directly to the existing HiveMQ broker over MQTT-over-WebSocket and displays each Claude Code session as a swipeable page with an expressive 8-bit synthwave fiddler crab.

Japanese-only UI (no English anywhere). Haptics replace sound. Prompt preview (first 15 words of the user's most recent prompt) replaces the meaningless session UUID. No WiFi settings — the existing MQTT infrastructure is reused.

## Architecture

- **Transport**: MQTT over WebSocket → `wss://broker.hivemq.com:8884/mqtt`. Subscribe `iotj/cl/openwr/updates/<mac4>/#` (same topic as the ESP32 devices). CocoaMQTT 2.1.x via SwiftPM (supports watchOS).
- **Lifecycle**: foreground-only live monitoring. On `ScenePhase.active` → connect. On `.background` → disconnect. Reconnect on resume within ~2s. Documented limitation in README (watchOS suspends apps aggressively).
- **Message schema** (extended — 7th field is new): `state|model|session_pct|weekly_pct|reset_epoch|client_host|prompt_preview`. ESP32 parser ignores extras, so backward/forward compatible. `sessionId` still comes from the MQTT topic's last segment.
- **Standalone**: `WKApplication=YES`, `WKWatchOnly=YES`, no embedded iOS target.
- **Bundle ID**: `com.example.fiddlerwaich` (user renames in Xcode).
- **Localization**: `ja` only. `CFBundleDevelopmentRegion=ja`, `CFBundleLocalizations=[ja]`. No Base.lproj, no English fallback.

## Project Structure

Root: `/Users/dkords/dev/projects/iotone/ProjectMagNET/reference-designs/MagNET_M5DialFiddlerCrab/FiddlerWAIch/`

```
FiddlerWAIch/
├── FiddlerWAIch.xcodeproj/
├── FiddlerWAIch Watch App/
│   ├── FiddlerWAIchApp.swift            @main, ScenePhase → mqtt.connect/disconnect
│   ├── Models/
│   │   ├── Session.swift                struct Session (id, mac4, model, state, promptPreview, timer fields, colorIndex, firstSeen, lastUpdate)
│   │   ├── SessionState.swift           enum (idle=0, working=2, needInput=3, finished=5, error=7) + localizedLabel + eyeStyle
│   │   └── MQTTMessage.swift            parser for 6- or 7-field pipe-delimited payload; extracts sessionId from topic
│   ├── State/
│   │   ├── SessionStore.swift           @MainActor ObservableObject: sessions[], currentIndex, connection state; ingest/prune/transition-haptic logic
│   │   └── AppSettings.swift            @AppStorage mac4, broker, clientId
│   ├── Networking/
│   │   ├── MQTTClient.swift             CocoaMQTTWebSocket wrapper, PassthroughSubject<MQTTMessage>
│   │   └── MQTTConfig.swift             broker URL, port 8884, topic pattern
│   ├── Views/
│   │   ├── ContentView.swift            router: OnboardingView if mac4 empty else SessionPagerView
│   │   ├── SessionPagerView.swift       TabView(.page) iterating store.sessions, IndexBadge + ConnectionDot at top
│   │   ├── SessionView.swift            single-page layout (see Layout below)
│   │   ├── EmptyStateView.swift         idle crawdad + "セッションなし"
│   │   ├── OnboardingView.swift         first-launch mac4 entry
│   │   ├── SettingsView.swift           long-press on pager opens this — edit mac4
│   │   ├── CrawdadView.swift            48×48 pixel grid via SwiftUI Canvas, state-driven eyes
│   │   └── Components/
│   │       ├── IndexBadge.swift         "3/8" pixel-style
│   │       ├── PromptPreview.swift      up to 3 lines, monospaced, Digital Crown scroll on overflow
│   │       ├── ConnectionDot.swift      8×8 colored square
│   │       └── RadialRingsView.swift    outer time ring + inner token% ring hugging safe-area perimeter
│   ├── Haptics/
│   │   └── HapticManager.swift          WKInterfaceDevice.play wrappers with 2s debounce per (sessionId,state)
│   ├── Theme/
│   │   ├── Colors.swift                 synthwave palette + sessionPalette[5]
│   │   └── PixelFont.swift              monospaced rounded system font modifier
│   ├── Resources/
│   │   ├── Assets.xcassets/             AppIcon only (no sprite sheets — Canvas-based)
│   │   └── ja.lproj/Localizable.strings all user-visible strings
│   └── Info.plist                       WKApplication=YES, ja, watchOS 10
├── hooks/
│   └── claude-crawdad-hook.sh           extended copy with prompt_preview field
└── README.md
```

## Per-Session Page Layout (~205×251 at 46mm) — with Radial Rings

Borrowing the M5Dial radial aesthetic: two concentric rings hug the watch's safe-area perimeter, turning the rounded-rect bezel into an analog gauge.

```
      ╭─ outer ring: session color, time (0-60m sweep) ─╮
     ╱  ╭─ inner ring: token %, bright→dim fade ─╮      ╲
    ╱  ╱                                         ╲      ╲
   │  │    3/8         ●接続済                    │      │
   │  │  ─────────────────────────────            │      │
   │  │  opus   作業中          02:47             │      │
   │  │  ─────────── (session color)              │      │
   │  │                                           │      │
   │  │   [Fix the bug in the MQTT                │      │
   │  │    reconnect backoff so it…]              │      │
   │  │                                           │      │
   │  │         ▄██░░░░██▄                        │      │
   │  │        ██░◉░░◉░██                         │      │
   │  │         ▀██████▀                          │      │
   │  │                                           │      │
   │  │   12%     b7a4        mini-pc             │      │
   │   ╲                                         ╱      │
    ╲   ╲_______________________________________╱      ╱
     ╲_____________________________________________╱
```

### Ring specs (`RadialRingsView.swift`)

Rendered via SwiftUI `Canvas` using `Path.addArc` following the watch's rounded-rect safe area (use `ContainerRelativeShape` or a manual rounded-rect path offset inward).

**Outer ring — working time sweep**:
- Width: 4 pt
- Inset from screen edge: 2 pt
- Color: session-assigned color (CYAN/MAGENTA/HOT_PINK/NEON_GREEN/YELLOW), round-robin
- Progress: `workingElapsed` mod 3600 s → 0–360° sweep. Every hour the ring wraps and starts over, with a brief 0.3s flash at wrap to mark the hour.
- **WORKING state**: ring animated, smooth 1 Hz redraw
- **Non-WORKING state**: ring frozen at last value, 50% opacity
- **Stale session (>5min)**: ring dimmed to 20%
- Direction: clockwise starting at 12 o'clock

**Inner ring — token consumption %**:
- Width: 3 pt
- Inset: 8 pt from outer ring (≈14 pt from screen edge)
- Color gradient: NEON_GREEN (0%) → YELLOW (50%) → RED (100%) — linear through HSV
- Progress: `session_pct / 100` → 0–360° sweep. If `session_pct == -1` (unknown), hide the inner ring entirely.
- Pulse at 1 Hz when `session_pct >= 90%` (approaching rate limit)
- Solid at 100% in RED (rate-limited)

**Rationale for two rings**: they encode the two most anxiety-inducing numbers in a Claude Code session (time elapsed, tokens remaining) without adding vertical text. Glanceable at distance. Mirrors the M5Dial's radial affordance.

**Rationale for hour-wrap on time ring**: sessions commonly last 2–8 hours; a single 0–∞ ring becomes useless past 60 min. Hour-wrap gives continuous motion and a per-hour rhythm. The `02:47` digital timer in the header is the authoritative source for absolute elapsed time.

### Content layout adjustment

Because the rings consume ~14 pt on all sides, the content rectangle shrinks slightly (~177×223 pt at 46mm). All existing elements reflow to fit. Small (40mm) watches drop the inner ring entirely if `session_pct` is -1, and use 3 pt outer / 2 pt inner widths to preserve content area.

### Inline text (same as original layout but inset)

```
  3/8        ●接続済              28pt: IndexBadge + ConnectionDot
  opus   作業中         02:47     22pt: model / state label / timer
  ─────────────  (session color)  2pt accent underline
                                  
  [Fix the bug in the MQTT        PromptPreview, 3 lines × 14pt
   reconnect backoff so it        monospaced, tail-truncated,
   doesn't retry forever…]        Digital Crown scrolls if longer
                                  
         ▄██░░░░██▄               CrawdadView ~80×60pt
        ██░◉░░◉░██                body static, eyes state-driven
         ▀██████▀                 
                                  
   12%     b7a4      mini-pc      14pt dim: pct / mac4 / host
```

Stale sessions (>5min) overlay 40% dim. Empty state: centered idle-eyed crawdad + `セッションなし` + brand `アイオートン`. Rings also dim/hide when no session is active.

## Crawdad Pixel Art (SwiftUI Canvas, 48×48 grid)

Body sprite hardcoded as pixel data array; renderer draws integer-scaled filled rects. Body static across states; only eyes (6×6 socket regions at x=18, x=30) animate via `TimelineView(.animation)`.

Eye behaviors per `SessionState`:
- **IDLE (closed)** — horizontal 4px line through each socket, HOT_PINK. Static.
- **NEED_INPUT (looking around)** — 2×2 CYAN pupil on 4×4 white cycles through [left, center, right, center] on 0.5s timeline.
- **WORKING (squinting)** — 3×1 NEON_GREEN line with 1px gap. Occasional blink removes gap briefly.
- **FINISHED (wide→small)** — one-shot: 6×6 YELLOW fills socket, shrinks to 2×2 over 1.2s, holds.
- **ERROR (asymmetric)** — left eye 4×4 RED wide with 2×2 pupil, pulsing 1Hz opacity; right eye closed (4px line).

Session-assigned round-robin color (CYAN/MAGENTA/HOT_PINK/NEON_GREEN/YELLOW) tints antennae tip and small claw accent, so swiping between sessions visibly changes the crawdad.

## Feedback: Haptics + Audio (`FeedbackManager.swift`)

Unified feedback layer with a mode toggle in `SettingsView`:

```
フィードバック:
  ● 振動 (haptics — default)
  ○ 音 (audio chirps/clicks/beeps)
  ○ 両方 (both)
  ○ オフ (silent)
```

Stored in `@AppStorage("feedbackMode")` as an enum. `FeedbackManager.fire(event:)` dispatches to haptics and/or audio based on the mode.

### Haptics (`WKInterfaceDevice.play`)

| Trigger | Haptic |
|---|---|
| → WORKING | `.directionUp` |
| → FINISHED | `.success` |
| → NEED_INPUT (visible session) | `.notification` |
| → NEED_INPUT (background session) | `.click` |
| → ERROR | `.failure` |
| → IDLE | (silent) |
| Swipe between sessions | `.click` |
| MQTT connected | `.start` |
| MQTT disconnected | `.stop` |

### Audio chirps (matches M5Stack tone patterns)

Match the existing M5StickC Plus speaker tones from `main.cpp`. Ship as pre-rendered 16 kHz mono CAF files in `Resources/Sounds/` (generated offline from sine-wave specs). Playback via `AVAudioPlayer` with a small pool of reusable players.

| Trigger | Pattern | Source file |
|---|---|---|
| → WORKING | 800 Hz, 50 ms (soft chirp) | `chirp_working.caf` |
| → FINISHED | 1200 Hz 80 ms → 120 ms gap → 1800 Hz 100 ms (ascending) | `chirp_finished.caf` |
| → NEED_INPUT | 2000 Hz, 100 ms (high chirp) | `chirp_needinput.caf` |
| → ERROR | 1000 Hz 80 ms → 120 ms gap → 600 Hz 100 ms (descending) | `chirp_error.caf` |
| Swipe | 400 Hz, 20 ms (click) | `click_swipe.caf` |
| Connected | 1500 Hz, 60 ms (short beep) | `beep_connected.caf` |
| Disconnected | 500 Hz, 120 ms (low beep) | `beep_disconnected.caf` |

Generation script (`scripts/generate-sounds.sh` — sox or Python):
```bash
# Example: 800 Hz, 50 ms, 16 kHz mono, 6 ms fade in/out (prevents click artifact)
sox -n -r 16000 -c 1 chirp_working.caf synth 0.050 sine 800 fade t 0.006 0.050 0.006
```

Playback requires `AVAudioSession.sharedInstance().setCategory(.ambient, options: .mixWithOthers)` so music/podcasts aren't interrupted.

Debounce (both haptic and audio): suppress repeat feedback for same `(sessionId, state)` within 2s.

## Synthwave Palette (`Colors.swift`)

```swift
static let bg        = Color(red: 0.047, green: 0.016, blue: 0.078)  // #0C0414
static let cyan      = Color(red: 0.0,   green: 1.0,   blue: 1.0)    // #00FFFF
static let magenta   = Color(red: 1.0,   green: 0.0,   blue: 1.0)    // #FF00FF
static let hotPink   = Color(red: 1.0,   green: 0.412, blue: 0.706)  // #FF69B4
static let neonGreen = Color(red: 0.224, green: 1.0,   blue: 0.078)  // #39FF14
static let yellow    = Color(red: 1.0,   green: 1.0,   blue: 0.0)    // #FFFF00
static let red       = Color(red: 1.0,   green: 0.157, blue: 0.157)  // #FF2828
static let dimGray   = Color(red: 0.118, green: 0.078, blue: 0.157)  // #1E1428
static let sessionPalette: [Color] = [cyan, magenta, hotPink, neonGreen, yellow]
```

## Japanese Strings (`ja.lproj/Localizable.strings`)

```
"app.name"                    = "フィドラーWAIch";
"state.idle"                  = "待機";
"state.working"               = "作業中";
"state.need_input"            = "入力待ち";
"state.finished"              = "完了";
"state.error"                 = "エラー";
"conn.connecting"             = "接続中";
"conn.connected"              = "接続済";
"conn.disconnected"           = "切断";
"empty.no_sessions"           = "セッションなし";
"settings.title"              = "設定";
"settings.device_id"          = "デバイスID";
"onboarding.title"            = "ようこそ";
"onboarding.device_id_prompt" = "デバイスIDを入力";
"onboarding.save"             = "保存";
"brand.tagline"               = "アイオートン";
```

## MQTT Client (`MQTTClient.swift`)

- `CocoaMQTTWebSocket(uri: "/mqtt")` with `enableSSL=true`, host `broker.hivemq.com`, port `8884`.
- Client ID: `"fiddlerwaich-<shortUUID>"` (persisted in `AppSettings` for stable reconnect).
- On `didConnectAck` → subscribe `iotj/cl/openwr/updates/<mac4>/#` QoS 1.
- On `didReceiveMessage` → parse → `Task { @MainActor in store.ingest(msg) }`.
- Custom reconnect backoff: 1, 2, 4, 8, 16, 30s capped; reset on success; cancel on explicit disconnect.
- Changing `mac4` → unsubscribe old, subscribe new.

## Session Lifecycle (`SessionStore.swift`)

Identical semantics to M5StickC `main.cpp`:
- First message with new `sessionId` → create Session, assign `colorIndex = nextColorIndex % 5`.
- State transitions drive timer: entering WORKING starts timer; leaving it accumulates elapsed.
- Stale after 5min (40% dim, no haptics); remove after 30min.
- Cap 8 sessions: evict oldest non-WORKING on overflow; if all WORKING, evict oldest.
- Prune timer: 10s `Timer.publish` while app active.

## Hook Script Extension

File: `MagNET_M5DialFiddlerCrab/M5StickCPlus-Blinky_Crawdad_OpenWR/hooks/claude-crawdad-hook.sh` (extend in place — backward-compatible).

Insert before publish:
```bash
PROMPT_PREVIEW=""
if [ -n "$TRANSCRIPT" ] && [ -f "$TRANSCRIPT" ]; then
    LAST_USER="$(jq -rs '
        [.[] | select(.type=="user") | .message.content // .content // ""]
        | map(if type=="array" then (map(.text // "") | join(" ")) else . end)
        | map(select(length > 0))
        | last // ""
    ' "$TRANSCRIPT" 2>/dev/null)" || LAST_USER=""
    PROMPT_PREVIEW="$(printf '%s' "$LAST_USER" \
        | tr '\n\r\t|' '    ' \
        | awk '{ for(i=1;i<=NF && i<=15;i++) printf "%s%s", (i>1?" ":""), $i }' \
        | cut -c1-200)"
fi
MSG="${STATE}|${SHORT_MODEL}|${SESSION_PCT}|${WEEKLY_PCT}|${RESET_EPOCH}|${CLIENT_HOST}|${PROMPT_PREVIEW}"
```

Also copy to `FiddlerWAIch/hooks/` for colocation. README points at the canonical M5StickC copy.

## Settings / Configuration

- First launch: empty `AppSettings.mac4` → `OnboardingView` with title `ようこそ`, prompt `デバイスIDを入力`, 4-char hex field, `保存` button.
- Ongoing: long-press (0.8s) on `SessionPagerView` root → `SettingsView`.
- Storage: `@AppStorage("mac4")`, `@AppStorage("broker")`, `@AppStorage("clientId")`. No Keychain (HiveMQ public has no creds).
- Validation: mac4 matches `^[0-9a-fA-F]{4}$`; save disabled otherwise; lowercased before storage.

## Phased Implementation

1. **Scaffold** — Create Xcode project, watchOS 10 target, ja-only localization config, add CocoaMQTT via SwiftPM, placeholder views. Verify simulator build.
2. **Models + Store** — `Session`, `SessionState`, `MQTTMessage` parser with unit tests for 6- and 7-field payloads. `SessionStore` ingest/prune.
3. **MQTT client** — Wire CocoaMQTT to HiveMQ WSS with a hardcoded test mac4; log received messages. Milestone: real ESP32 traffic flows into the watch log.
4. **Basic UI** — `SessionPagerView` with `TabView(.page)`, plain-text `SessionView`, `IndexBadge`, `ConnectionDot`. Swipe paging works.
5. **Crawdad + Radial rings** — `CrawdadView` static body, eye states, animations. `RadialRingsView` outer time sweep + inner token% ring. In-app debug state picker.
6. **Feedback (haptics + audio)** — `FeedbackManager` with mode toggle, `HapticManager`, `AudioManager`; generate CAF chirps via `scripts/generate-sounds.sh`; wire to transitions and swipes. Add `NotificationScheduler` stub (no-op) for Phase II APNs. Test on hardware.
7. **Japanese** — Replace every literal with `LocalizedStringKey`, populate `Localizable.strings`, grep for residual English.
8. **Onboarding + Settings** — First-run flow, long-press settings, mac4 change triggers resubscribe.
9. **Hook extension** — Update `claude-crawdad-hook.sh`, test end-to-end prompt preview delivery.
10. **Polish** — Stale/expired dimming, empty state, connection pulsing, prompt crown-scroll, accessibility, README.

## Critical Files

| File | Action |
|------|--------|
| `FiddlerWAIch/FiddlerWAIch.xcodeproj/` | New — Xcode project |
| `FiddlerWAIch/FiddlerWAIch Watch App/Networking/MQTTClient.swift` | New — CocoaMQTT WSS wrapper |
| `FiddlerWAIch/FiddlerWAIch Watch App/State/SessionStore.swift` | New — session ingestion, timers, haptic triggers |
| `FiddlerWAIch/FiddlerWAIch Watch App/Models/MQTTMessage.swift` | New — 6/7-field parser + topic sessionId extraction |
| `FiddlerWAIch/FiddlerWAIch Watch App/Views/SessionView.swift` | New — single-session page layout |
| `FiddlerWAIch/FiddlerWAIch Watch App/Views/CrawdadView.swift` | New — 48×48 pixel Canvas renderer with state-driven eyes |
| `FiddlerWAIch/FiddlerWAIch Watch App/Views/Components/RadialRingsView.swift` | New — dual concentric rings (time sweep + token%) around safe-area perimeter (M5Dial-inspired) |
| `FiddlerWAIch/FiddlerWAIch Watch App/Feedback/FeedbackManager.swift` | New — unified haptic+audio dispatcher, reads feedbackMode |
| `FiddlerWAIch/FiddlerWAIch Watch App/Feedback/HapticManager.swift` | New — WKHapticType dispatch with debounce |
| `FiddlerWAIch/FiddlerWAIch Watch App/Feedback/AudioManager.swift` | New — AVAudioPlayer pool, ambient session, CAF playback |
| `FiddlerWAIch/FiddlerWAIch Watch App/Feedback/NotificationScheduler.swift` | New — Phase I no-op stub, Phase II APNs/local dispatcher |
| `FiddlerWAIch/FiddlerWAIch Watch App/Resources/Sounds/*.caf` | New — 7 pre-rendered tone files matching M5Stack patterns |
| `FiddlerWAIch/scripts/generate-sounds.sh` | New — sox-based CAF generator for reproducibility |
| `FiddlerWAIch/FiddlerWAIch Watch App/Resources/ja.lproj/Localizable.strings` | New — all user-visible strings |
| `FiddlerWAIch/FiddlerWAIch Watch App/Info.plist` | New — ja, WKApplication, watchOS 10 |
| `M5StickCPlus-Blinky_Crawdad_OpenWR/hooks/claude-crawdad-hook.sh` | Extend — add PROMPT_PREVIEW 7th field |
| `FiddlerWAIch/README.md` | New — setup, Xcode signing, mac4 onboarding |

Existing references (read only):
- `M5StickCPlus-Blinky_Crawdad_OpenWR/src/main.cpp` — session_t fields, state constants, colors, timer logic
- `components/craw_mqtt/craw_mqtt.c` — topic/payload parsing reference

## Verification

1. **Parser unit tests**: 6-field legacy → empty preview; 7-field → populated; malformed → nil without crash.
2. **End-to-end on watch**:
   - Build for Apple Watch Series 10 simulator or hardware.
   - First launch → `OnboardingView`, enter 4-char hex mac4, save.
   - On dev machine run Claude Code with `CRAW_TOPIC=iotj/cl/openwr/updates/<mac4>`.
   - Trigger each state and verify:
     - `UserPromptSubmit` → `待機` / eyes closed.
     - `PreToolUse` → `作業中` / squinting / timer runs / `.directionUp` haptic.
     - `Notification` → `入力待ち` / looking around / `.notification` haptic.
     - `Stop` → `完了` / wide→small eyes / `.success` haptic.
   - Second Claude terminal → index shows `1/2`; swipe right → `.click` haptic.
3. **Backward compat**: run unmodified 6-field hook → watch shows sessions with empty `promptPreview` placeholder `—`. ESP32 M5StickC still works against the extended 7-field hook.
4. **Background**: lock watch, wait 30s, raise wrist → reconnect within ~2s, sessions repopulate from next ESP32 publish.
5. **Localization audit**: launch with `-AppleLanguages '(en)'` → still fully Japanese. Visual pass: no English characters except digits and hex mac4.

## Risks

- **CocoaMQTT watchOS behavior** — claimed supported but primarily iOS-exercised. Contingency: ~300-line custom MQTT 3.1.1 over `URLSessionWebSocketTask` (CONNECT, SUBSCRIBE, PUBLISH receive, PINGREQ). Build only if CocoaMQTT fails.
- **40mm watch crowding** — if prompt preview crowds crawdad, shrink crawdad to 60×45pt on small screens.
- **Haptic fatigue** with 8 churning sessions — mitigated by 2s per-session/state debounce and "only visible gets notifications". Add master toggle in `SettingsView` if needed.
- **ESP32 parser strictness** — if the M5StickC parser rejects 7-field payloads, one-line fix to relax to `>= 6`.

---

## Locked-Screen / Background Notifications

**The honest answer: watchOS makes this hard for our architecture.** The app needs an active MQTT subscription to know a state change occurred, but watchOS suspends the app ~30–60 s after wrist-down. Without a running app there is no event to notify on.

Available paths:

| Option | Works when locked? | Cost |
|---|---|---|
| **Local notifications scheduled while active** | Only for events scheduled *in advance* (timers, calendar) — not reactive to MQTT | Free, trivial |
| **WKExtendedRuntimeSession** | Up to 1 hour active with screen dimmable | Restricted to categories (workout, mindfulness, smart alarm, physical therapy) — our use case doesn't qualify, would be rejected by App Store review but works for ad-hoc dev builds |
| **Complications + background refresh** | Refreshes every ~10 min max; shows state on watch face | Free, but latency is terrible for approve/deny prompts |
| **APNs push notifications** | ✅ True locked-screen alerts | Requires a server that converts MQTT → APNs; breaks "no companion / no server" architecture |
| **iOS companion + WatchConnectivity** | iOS app persists the MQTT connection, sends local notifications to paired watch | User explicitly rejected iOS companion for runtime |

### Phase I recommendation

Ship **without** locked-screen notifications. Document it clearly in the README as a known limitation: *"Raise wrist or tap to see current state. Apple Watch does not allow this app to run in the background; notifications while locked require Phase II infrastructure."*

### Phase II options (pick one when adding this)

1. **Optional APNs relay** — add a tiny Node/Python service to the Phase II responder daemon. When it sees a `NEED_INPUT` or `ERROR` MQTT message for a registered device token, it sends an APNs push. Watch receives push even when locked. Requires the dev machine (or a tiny VPS) to run the relay. **This is the most practical path.**
2. **iOS companion** — the settings-only iOS app from the previous section gains a runtime role: maintain MQTT subscription on iPhone, send WatchConnectivity messages to watch, post local notifications. Reverses the "standalone" constraint.
3. **Complication** — add a watch face complication that shows session count + worst state. Refreshes on background budget (~every 10 min). Glanceable but not a notification.

**Phase I deliverable**: add a local-notification-friendly hook so Phase II can plug in. `FeedbackManager.fire(event:)` already routes all state-change events; add a `NotificationScheduler` stub that is a no-op in Phase I but becomes the APNs/local dispatcher in Phase II. Minimal code, future-proofs the architecture.

---

## Safe Defaults & Settings UX

watchOS text entry is painful (no camera for QR, keyboard only on Series 7+, dictation unreliable for URLs). Config strategy must avoid both UX pain *and* accidental connection to a public broker.

### Phase I default: localhost, not HiveMQ

Change the default broker from `wss://broker.hivemq.com:8884/mqtt` to **`ws://localhost:1883/mqtt`** — fails silently until the user runs their own broker. No accidental public traffic.

Ship three presets in `SettingsView`:

```
ブローカー選択:
  ● ローカル         ws://localhost:1883/mqtt     (default)
  ○ プライベート     <user-entered URL>
  ○ HiveMQ 公開     wss://broker.hivemq.com:8884/mqtt
```

Selecting "HiveMQ 公開" triggers a warning sheet `注意：公開サーバー` with body explaining telemetry is world-readable; requires tapping `理解した` to confirm. Stored in `@AppStorage("brokerPreset")` so the warning only fires on re-selection.

Custom URL entry uses whatever input method the hardware supports (scribble / QWERTY / dictation) — tedious but rarely needed once set. Validate with `URL(string:)` and reject non-`ws://` / `wss://` schemes.

### On-watch input feasibility per field

| Field | Input method | Reasonable? |
|---|---|---|
| mac4 (4 hex chars) | Scribble or Crown hex picker | ✅ |
| Preset broker | Tap from list | ✅ |
| Custom broker URL | QWERTY (Series 7+) or dictation | ⚠ painful but occasional |
| Client ID | Auto-generated, never entered | ✅ |

### Phase II: commands refuse public broker

When Phase II adds command publishing (approve/deny/cancel), `MQTTClient.publishCommand()` short-circuits with an error if `brokerPreset == .hivemqPublic`. UI renders approve/deny buttons greyed with label `公開サーバーでは無効` (disabled on public broker). Forces users onto a private broker before any actionable input leaves the watch.

### Optional Phase II: minimal iOS companion for settings only

If custom-URL entry via scribble proves intolerable in practice, add a **settings-only iOS target** to the same Xcode project:

- One screen: broker URL, mac4, broker credentials (TLS cert, username/password).
- Writes to a shared **App Group** (`group.com.example.fiddlerwaich`) via `UserDefaults(suiteName:)`.
- Watch app reads the same App Group UserDefaults — no WatchConnectivity needed.
- Runtime still standalone — iPhone only used during pairing, can be uninstalled after.
- Adds ~100 lines of Swift + one Xcode target. Strictly optional; Phase I ships without it.

### Phased Implementation — additions

Insert between steps 8 and 9:
- **8a. Broker presets + warning sheet** — three-option picker, HiveMQ confirmation flow, `@AppStorage` for preset.

Phase II addition:
- **Public-broker command lockout** — `MQTTClient.publishCommand()` refuses when preset == public; UI greys buttons with Japanese disabled label.

### Critical file additions

| File | Action |
|------|--------|
| `FiddlerWAIch/FiddlerWAIch Watch App/State/AppSettings.swift` | Extend — add `brokerPreset` enum (.localhost, .custom, .hivemqPublic), `publicBrokerAcknowledged: Bool` |
| `FiddlerWAIch/FiddlerWAIch Watch App/Views/BrokerPicker.swift` | New — preset picker with confirmation sheet |
| `FiddlerWAIch/FiddlerWAIch Watch App/Views/PublicBrokerWarningView.swift` | New — Japanese warning with 理解した button |
| (optional) `FiddlerWAIch/FiddlerWAIch iOS Companion/` | New — settings-only iOS target, App Group-backed |

