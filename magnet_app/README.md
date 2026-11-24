# MagNET

BLE scan, provisioning and control app for **MagNET Hanasu** mesh nodes.

Rebuilt (2026-07-31) on
[flutter-responsive-mobile-app-starter-iotj](https://github.com/IoTone/flutter-responsive-mobile-app-starter-iotj),
which supplies the responsive shell, radar/device scanner, permissions flow,
theming, and EN/JA localization. The MagNET-specific layer is:

| Path | What it is |
|------|-----------|
| `lib/magnet/hcp.dart` | Host Control Protocol client — framing, `@tag` correlation, typed verbs. Dart sibling of `reference-designs/…/host-sdk/python/magnet_hcp.py`. |
| `lib/magnet/magnet_ble.dart` | GATT transport for the HCP service (§11.2.1) + node detection. |
| `lib/screens/magnet_node_screen.dart` | Connect, inspect, name, and provision a node. |

> Package: `magnet_app` · bundle id `io.iotone.magnet` · MIT licensed.

## Sentinels (room presence over MQTT)

**Dashboard → Presence sentinels** lists every
[Sentinel IR](../reference-designs/MagNET_M5DialFiddlerCrab/M5StickCPlus_Hive_Sentinel_IR)
node. Sentinels are PIR sensors on WiFi that publish to the MagNET MQTT
broker, so they show up from any network; no BLE is involved.

- **Each card shows:**
  - **State:** ACTIVE (motion right now), PRESENT, CLEAR or OFFLINE.
  - **Countdown:** last motion and the time left to clear. It restarts at
    every movement.
  - **Counts:** entries and occupied time for this hour, today, this week and
    this month.
  - **Last 5 minutes:** a chart of motion in 10-s bars.
  - **Last 24 hours:** a chart of occupied minutes in 15-min bars. Tap or drag
    to read a bar.
  - **Recent activity:** enter, still present, clear, period rollovers and
    resets.
  - **Check-sensor warning:** shown when the PIR has seen no motion for its
    limit (12 h by default). A loose wire looks like an empty room. The
    Dashboard card shows a warning icon too.
- **Chart code:** `lib/screens/sentinel_charts.dart` is a plain CustomPainter,
  with no chart package.
- **Code:** `lib/magnet/sentinel_feed.dart` (feed + payload parser, tested in
  `test/magnet/sentinel_feed_test.dart`) and `lib/screens/sentinel_screen.dart`.
- **When it connects:** the feed connects the first time the screen opens, and
  then stays up while the app runs.
- **Broker:** the default is `mqtt://broker.hivemq.com:1883`. Change it with
  the server icon in the screen's app bar. Web builds use the broker's secure
  WebSocket port (`wss://host:8884/mqtt`).
- **Permissions:** this added `network.client` to the macOS entitlements and
  `INTERNET` to the Android manifest.

## The provisioning model

A node advertises its HCP service **only while unprovisioned**. The moment it
receives a channel credential it derives its keys, tears the BLE stack down,
and hands the antenna to Thread — so it vanishes from the app on purpose. That
is why the flow is one-directional: find → configure → gone.

Practically:

1. Scan. A node shows in the device list with a **SET UP** badge (matched by
   the `MagNET-` name prefix or the advertised service UUID — iOS often
   withholds service UUIDs until connect, hence both).
2. Tap it. The app connects, discovers the service, subscribes to
   notifications, and reads `STATUS` / `WHOAMI` / `CHANNEL SHOW`.
3. Optionally set a display name (it announces on-mesh, so peers see a name
   rather than a hex id).
4. Enter a credential and **Provision**. Accepts a passphrase, a 12–24 word
   seed phrase, or a `qr:` secret — the node auto-detects which and derives
   accordingly (§11.1.2). A typed passphrase runs PBKDF2 on-device and takes
   a few seconds.

A node still on the well-known `magnet` channel is flagged in the status card:
its traffic is readable by anyone nearby.

## Why HCP looks the way it does

Every line is classed by its first character — `+`/`-` is the single terminal
response to your command, `!` is an unsolicited event, `#` is ignorable
commentary. `HcpClient` turns that into the guarantee a UI needs: an awaited
command can never be handed someone else's chat message.
`test/magnet/hcp_test.dart` pins that behaviour, including the
interleaved-event case.

The same grammar runs over USB-CDC, so this client and the Python SDK are two
faces of one protocol; only the byte pipe differs.

## Brand mark

The icon is the field of a bar magnet — the loops are the real dipole solution
`r = L·sin²θ`, stretched on the axis so the mark fills a square. That is why it
reads as *magnetic* rather than as generic concentric rings. Cyan at the poles,
magenta at the equator, on deep space.

It is generated, not hand-drawn:

Run in this order — the last step is not optional, it repairs what the
generators flatten:

```bash
python3 tool/gen_icon.py               # 1. master art + splash source
dart run flutter_launcher_icons        # 2. all platforms from the master
dart run flutter_native_splash:create  # 3. launch screens
python3 tool/gen_icon.py               # 4. restore per-size + macOS squircle
```

Notes worth keeping:

- **Detail varies by size.** The full field is four loop pairs; below 64 px
  they merge into a smear, so the small icon slots get a reduced two-loop set.
  The appiconset carries distinct art per slot, so the large sizes never have
  to compromise for the small ones — `gen_icon.py` writes both the iOS and
  macOS sets itself to do this (and the Android mipmaps, where mdpi is 48 px).
- The widest loop stays inside 80% of the canvas so the rounded-corner mask
  never clips it.
- iOS art is flattened to RGB; the App Store rejects an alpha channel.
- macOS art is a squircle floating on transparency at Apple's ~80% / 22.5%
  radius grid — `flutter_launcher_icons` would otherwise emit a full-bleed
  square, which looks foreign in the Dock. `tool/gen_icon.py` writes that
  appiconset itself — which is why it runs both first and last.
- The in-app splash paints the same dipole in Dart (`branded_splash_screen`)
  rather than loading the asset, so it stays crisp at any size and can animate
  — the field pulses outward instead of the logo spinning.

## Inherited from the starter

Dashboard with live scan summary, BLE + WiFi scanning, signal radar, device
detail sheets, diagnostics, first-run permissions flow, light/dark theming,
EN/JA localization, go_router navigation, provider state.

## Develop

```bash
flutter pub get
flutter analyze     # clean
flutter test        # 27 tests
flutter run
```

Requires a physical device for BLE — simulators have no radio.

## Status

- ✅ HCP client + framing, unit-tested (9 tests)
- ✅ BLE transport, node detection, provisioning screen
- ⏳ End-to-end against real hardware — pending a phone-in-hand pass
- ⏳ Mesh chat view. Needs a node that keeps a host link after provisioning;
  today BLE is provisioning-only by design, so live chat belongs on the
  USB/serial binding or a future resident-BLE build.

## History

The previous app (custom `flutter_reactive_ble` provider, neumorphic UI) was
replaced wholesale in this rebuild. It remains in git history prior to the
2026-07-31 commit if anything needs recovering.
