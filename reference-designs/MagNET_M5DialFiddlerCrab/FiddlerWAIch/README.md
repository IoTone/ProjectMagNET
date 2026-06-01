# FiddlerWAIch

Standalone Apple Watch dashboard for monitoring multiple concurrent Claude Code sessions. Japanese-only UI, 8-bit synthwave fiddler crab with expressive state-driven eyes, dual radial rings (time sweep + token %), swipe between sessions, haptics or audio chirps.

Mirrors the M5StickC Plus Crawdad behavior — subscribes to the same MQTT topic pattern over WebSocket.

## Project status — PAUSED (2026-04-18)

**Phase I code is feature-complete and verified working in the watchOS simulator.** Development is paused because the available test hardware (Apple Watch Series 5, watchOS 10.x) exhibits a third-party networking limitation that prevents real-device connectivity — see [watchOS third-party networking limitation](#watchos-third-party-networking-limitation) below.

**What works:**
- Full build clean against watchOS 9.0 deployment target
- All UI flows: onboarding, session pager, settings, radial rings, pixel crawdad animation, haptics + audio chirps
- MQTT 3.1.1 WebSocket + TCP transports (both hand-rolled, no external deps)
- Hook script extension publishes per-session topics with 7-field extended payload
- In-app diagnostic tooling: network monitor, IP display, URL probe, rolling log
- Simulator test connects to HiveMQ public broker in <1s and receives messages

**What's blocked:**
- Real-device connectivity on the test Apple Watch. `URLSessionWebSocketTask` and `NWConnection` both return `NSURLErrorNotConnectedToInternet` (-1009) on this hardware even though plain `URLSession.dataTask` HTTP succeeds. Hypothesis: specific routing state (`iface=other` from `NWPathMonitor`) denies privileged network API access to third-party apps.

**To resume:** see [paths forward](#paths-forward).

## How it works

1. You run Claude Code on your Mac.
2. A **hook script** fires on every Claude event (tool use, prompt submitted, finished, etc.) and publishes a one-line MQTT message to a broker.
3. The watch subscribes to that broker over an MQTT-over-WebSocket connection and renders each session as a swipeable page.

The hook script and watch agree on a 4-character hex "channel label" (called `mac4`) so that multiple users or devices on the same broker don't collide. You pick this value; it's arbitrary.

```
Mac running Claude → [hook] → MQTT broker → [WebSocket] → FiddlerWAIch
                         │                                    │
                         └────── topic: iotj/cl/openwr/updates/<mac4>/<session_id>
```

## Requirements

- macOS with a recent Xcode (currently Xcode 26+; older Xcodes may not pair with iOS 26 devices)
- Apple Watch running watchOS 9.0+, paired with an iPhone
- No external Swift packages (MQTT-over-WebSocket is implemented in-target — see `Networking/MQTTPacket.swift` and `MQTTClient.swift`)
- An MQTT broker reachable over WebSocket. Options, easiest first:
  - **Public HiveMQ**: `wss://broker.hivemq.com:8884/mqtt`. Zero setup, but traffic is world-readable — use only for testing.
  - **Local Mosquitto**: `brew install mosquitto && brew services start mosquitto` → `ws://localhost:1883/mqtt` (needs `listener 1883` + `protocol websockets` in `mosquitto.conf`).
  - **Private broker / HiveMQ Cloud**: whatever URL you use.
- For sound regeneration: `sox` (`brew install sox`)
- For project regeneration: `xcodegen` (`brew install xcodegen`)

## Build & run

```sh
cd FiddlerWAIch
xcodegen                                  # regenerate .xcodeproj if you edit project.yml
open FiddlerWAIch.xcodeproj
```

In Xcode:
1. Select a signing **Team** under the target's **Signing & Capabilities** (free Apple ID works — certificates expire every 7 days and you reinstall).
2. Change the **Bundle Identifier** to something under your team if needed (e.g. `com.yourcompany.fiddlerwaich.watchkitapp`).
3. Pick an Apple Watch as the run destination (simulator or paired device).
4. Cmd-R.

See **Known Issues** below if device deployment fails with "OS version lower than deployment target" or "control channel timed out."

## Setup: picking a mac4

The onboarding screen on the watch asks for `デバイスIDを入力` — a 4-character hex string. This is a topic channel label. It can be anything as long as the watch and your Claude hook script agree.

**Any of these work:**

```
b7a4        # arbitrary, memorable
beef        # whimsical
1234        # numbers only
```

**Option: use your Mac's WiFi MAC suffix as a "natural" unique value.**

```sh
ifconfig en0 | grep ether | awk '{print $2}' | tr -d ':' | tail -c 5
# prints: b7a4
```

Or Apple menu → System Settings → Wi-Fi → Details → Hardware Address: the last 4 hex chars.

Type whatever you pick into the watch's onboarding field (scribble, dictation, or the on-screen keyboard). Tap `保存`.

## Setup: Claude Code hook

The hook script lives at `hooks/claude-crawdad-hook.sh` (symlinked from the canonical copy under `M5StickCPlus-Blinky_Crawdad_OpenWR/hooks/`). It publishes a pipe-delimited status message to MQTT every time Claude Code fires a lifecycle event.

### Register the hook with Claude Code

Add to your `~/.claude/settings.json`:

```json
{
  "hooks": {
    "PreToolUse":       [{"command": "/absolute/path/to/FiddlerWAIch/hooks/claude-crawdad-hook.sh"}],
    "PostToolUse":      [{"command": "/absolute/path/to/FiddlerWAIch/hooks/claude-crawdad-hook.sh"}],
    "Stop":             [{"command": "/absolute/path/to/FiddlerWAIch/hooks/claude-crawdad-hook.sh"}],
    "Notification":     [{"command": "/absolute/path/to/FiddlerWAIch/hooks/claude-crawdad-hook.sh"}],
    "UserPromptSubmit": [{"command": "/absolute/path/to/FiddlerWAIch/hooks/claude-crawdad-hook.sh"}],
    "SessionStart":     [{"command": "/absolute/path/to/FiddlerWAIch/hooks/claude-crawdad-hook.sh"}]
  }
}
```

### Configure the hook

The script reads three environment variables. Set them in your shell rc (`~/.zshrc` / `~/.bashrc`):

```sh
export CLAW_BROKER="broker.hivemq.com"       # broker hostname (not URL)
export CLAW_TOPIC="iotj/cl/openwr/updates/b7a4"   # replace b7a4 with your mac4
export CLAW_PLAN=80000                        # token budget estimate for session_pct
```

Required tools on the Mac: `jq`, `mosquitto_pub` (`brew install jq mosquitto`).

### Verify the hook publishes

```sh
mosquitto_sub -h broker.hivemq.com -t "iotj/cl/openwr/updates/b7a4/#" -v
```

Trigger any Claude Code session. You should see lines like:

```
iotj/cl/openwr/updates/b7a4/308c0f30-… 2|opus-4-6|35|-1|0|laptop|Fix the bug in MQTT reconnect
```

If you see these, the publish side works. Whatever doesn't show on the watch is a watch-side subscription issue.

## Setup: watch broker selection

After entering mac4 and tapping `保存`, the app tries to connect to the **default broker: `ws://localhost:1883/mqtt`** — which fails silently if no local broker is running. You'll see sessions only after switching to a reachable broker.

Long-press the session view (or the empty state) to open `設定`. Pick one of:

- **ローカル** (`ws://localhost:1883/mqtt`) — default. Needs a local Mosquitto with WebSocket listener.
- **プライベート** — custom URL you type in.
- **HiveMQ 公開** (`wss://broker.hivemq.com:8884/mqtt`) — a confirmation sheet appears warning that traffic is world-readable (`注意：公開サーバー`). Tap `理解した` to accept.

Pick whichever matches your `CLAW_BROKER` / hook config. For first-time testing the fastest path is HiveMQ public.

## First-run walkthrough (HiveMQ public, no local broker)

1. On the Mac: pick a mac4, e.g. `b7a4`.
2. Add to `~/.zshrc`:
   ```sh
   export CLAW_BROKER="broker.hivemq.com"
   export CLAW_TOPIC="iotj/cl/openwr/updates/b7a4"
   export CLAW_PLAN=80000
   ```
   Reload: `source ~/.zshrc`.
3. Register the hook in `~/.claude/settings.json` (see above).
4. In FiddlerWAIch:
   - Onboarding: enter `b7a4`. Tap `保存`.
   - Long-press → `設定` → select `HiveMQ 公開` → tap `理解した`.
   - Back out. You should see `接続済` (green dot) within a second or two.
5. Run any Claude Code session on the Mac. The first hook fires within a few seconds and a session card appears on the watch. Index badge shows `1/1`.

## Testing state transitions

While looking at the watch:

- Start typing a prompt → `待機` (eyes closed).
- Let Claude invoke a tool → `作業中` (squinting eyes, timer runs).
- Let Claude ask a question / request approval → `入力待ち` (eyes look around).
- Let Claude finish → `完了` (eyes go from wide to small).

With haptics enabled (default), each transition vibrates a distinct pattern. With audio enabled, you get 800 Hz / 2 kHz / ascending / descending chirps matching the M5Stack designs.

## Unit tests

```sh
xcodebuild -scheme "FiddlerWAIch Watch App" \
  -destination "platform=watchOS Simulator,name=Apple Watch Series 9 (45mm),OS=10.2" \
  test
```

## Regenerating CAF chirps

```sh
./scripts/generate-sounds.sh
```

Outputs 7 CAF files into `FiddlerWAIch Watch App/Resources/Sounds/`. Needs `sox`.

## Bundle ID

The default `com.example.fiddlerwaich.watchkitapp` is a placeholder. Change in `project.yml` (`PRODUCT_BUNDLE_IDENTIFIER`) and re-run `xcodegen`, or override in Xcode's signing tab.

## Roadmap

Phase I shipped here: read-only dashboard. Phase II ideas documented in `docs/FiddlerWAIch-plan.md`:

- Approve/deny/cancel commands from the watch (requires a return-channel responder daemon on the dev machine).
- Private broker support with per-user auth (commands refused on public broker).
- Optional iOS companion app for settings-only pairing.
- APNs relay for locked-screen notifications.

## Known issues

### watchOS third-party networking limitation

**This is the main blocker.** On the test hardware (Apple Watch Series 5, watchOS 10.x), the app can't connect to any MQTT broker because third-party network APIs that require a "real" internet path are denied, while plain HTTP (that runs through Apple's URLSession HTTP stack) is allowed. Our diagnostic evidence:

| Test | Result | What this means |
|---|---|---|
| `URLSession.shared.data(for:)` to `http://captive.apple.com` | **200 OK** | Plain HTTP works |
| `URLSession.shared.data(for:)` to `http://broker.hivemq.com:8000/mqtt` | **400** | Broker reachable on TCP/8000 via HTTP |
| `URLSession.shared.data(for:)` to `https://www.google.com/generate_204` | **204** | Plain HTTPS works |
| `URLSessionWebSocketTask` to `ws://broker.hivemq.com:8000/mqtt` | **-1009 NotConnectedToInternet** | WebSocket task denied |
| `NWConnection` (TCP) to `broker.hivemq.com:1883` | **ENETDOWN (errno 50)** | Raw TCP denied |
| `NWPathMonitor.pathUpdateHandler` | `path=NO NET, iface=other` | No "real" path reported |

Meanwhile the watch has a valid Wi-Fi DHCP lease (192.168.0.x), Apple's own apps (Weather, Stocks, News) work fine, and the iPhone Watch app shows the watch as connected.

**Observations:**

- `iface=other` in `NWPathMonitor` is the unusual signal. Healthy direct Wi-Fi normally reports `iface=wifi`. On this watch, despite having a LAN IP, the reported interface is `other` — likely some flavor of Companion Link / Personal Hotspot hybrid routing.
- Apple's first-party apps get privileged pass-through of URLSession and other APIs through this routing. Third-party apps only get URLSession's HTTP `dataTask` — not `webSocketTask`, and not `NWConnection`.
- This is not a code issue — **the same app connects successfully from the watchOS Simulator** using an identical URL scheme, protocols, and server (the simulator borrows the Mac's network, which reports `iface=wifi`).
- It is also not an entitlement, capability, or provisioning issue. Adding `NSAppTransportSecurity` / `NSLocalNetworkUsageDescription` / `NSBonjourServices` made no difference.
- It may be specific to this particular watch / watchOS version / network setup. Newer Apple Watches or cleaner network configurations may not exhibit it.

**References to the code paths that failed:**
- `FiddlerWAIch Watch App/Networking/MQTTClient.swift` — URLSessionWebSocketTask path, default transport.
- `FiddlerWAIch Watch App/Networking/MQTTClientTCP.swift` — NWConnection TCP path, alternative transport.
- `FiddlerWAIch Watch App/Networking/MQTTPacket.swift` — shared MQTT 3.1.1 packet encode/decode.
- `FiddlerWAIch Watch App/Networking/NetworkMonitor.swift` — NWPathMonitor status reporting.
- `FiddlerWAIch Watch App/Views/SettingsView.swift` — in-app probe + log viewer used for diagnosis.

### Paths forward

If this project is resumed, pick one:

1. **Try on different / newer Apple Watch hardware.** Simplest. The code is ready; hardware may just work. Simulator already verifies the logic.
2. **Try factory-reset + re-pair** of the existing watch. Sometimes clears strange routing state. 10 minutes of effort, possible to fix.
3. **Build an HTTP relay.** A tiny Python/Go server on the dev Mac subscribes to MQTT topics on the watch's behalf and serves messages over plain HTTP long-poll. The watch uses `URLSession.dataTask` (which is proven to work) to poll this relay. ~50–100 lines of relay code. Switches `MQTTClient.swift` from WebSocket to HTTP polling. Works around the limitation completely.
4. **Vendor a different WebSocket library** that uses an older networking stack (e.g. Starscream 3.x via NSStream) that may not hit the same connectivity check. Unproven; possibly works on affected hardware.
5. **Use URLSessionStreamTask** (deprecated but may still function on watchOS 10) — opens a raw TCP stream through URLSession, potentially bypassing the WebSocket-specific check. Unproven.

### Apple Watch deployment from Xcode is flaky

Apple Watches have no data cable — all Xcode installs route through the paired iPhone. The iPhone must be on the same Wi-Fi, plugged into the Mac, unlocked, and Developer Mode must be enabled on *both* iPhone and watch. First-ever install typically takes 2-8 minutes and multiple retries. See https://www.fplanque.com/tech/dev/apple-watch-xcode-connection/ for a longer walkthrough.

Common failure modes and fixes:

- **"OS version lower than deployment target"** — device is on an older watchOS than the project targets. Lower `watchOS` in `project.yml` or update the watch.
- **"Control channel connection timed out while in state preparing"** — Xcode's dev pairing bridge is wedged. Reboot Mac + iPhone + watch; re-pair via Xcode → Window → Devices and Simulators → right-click iPhone → Unpair Device → plug back in.
- **Xcode too old for new iPhone iOS** — update Xcode from the Mac App Store. Xcode must support the iPhone's iOS version to pair at all, which blocks watch pairing too.
- **"No eligible devices" with iPhone visible** — iPhone pair succeeded but watch prep didn't. Put watch on charger, keep screen awake, wait up to 20 min for "Preparing" to finish the first time.
- **Free Apple ID certs expire every 7 days** — the app stops launching. Re-run from Xcode to reinstall.

### Audio init is deferred

`AudioManager` sets up `AVAudioSession` lazily on first playback, not at app launch. This was moved out of `init()` because synchronous audio session setup could hang the watch's launch. If you change feedback mode to "音" (audio) and the first sound doesn't play, the CAF files may be missing — re-run `scripts/generate-sounds.sh`.

### mac4 entry is slightly painful on the watch

Scribble works for 4 hex chars, but is tedious. Alternatives:
- Dictation: say "bee eff aye see" clearly.
- On-screen keyboard (Series 7+): tap the field, peck letters.
- Paired iPhone keyboard push: on newer watchOS, tapping a text field may offer an iPhone input handoff.

Phase II may add an iPhone companion app for settings-only pairing to bypass this.
