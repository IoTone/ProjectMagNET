# UC4 — Airplane (`uc4-airplane.json`)

Simulated in-flight experience: live attitude indicator from a real IMU, plus cabin music, video, and a spatial photo gallery. Four mutually-exclusive content modes selectable from the bottom HUD; only one renders at a time, the others stay loaded in the scene graph but hidden + paused.

Join code: `DEMO04` (resolved by `mock-join-server`). Direct load: `?manifest=/examples/uc4-airplane.json`.

---

## Marks

| Mark                    | Type             | Default | Notes                                                          |
|-------------------------|------------------|---------|----------------------------------------------------------------|
| `airplane-imu`          | `imu`            | visible | Wireframe airplane + Garmin G5-style instrument labels + slowly-rotating globe backdrop. Polls `/api/v1/sensor/imu`. |
| `cabin-boombox`         | `spatial-audio`  | hidden  | Procedural 8-second I-V-vi-IV loop, spatialised via `PositionalAudio`. Pinch to cycle theme. |
| `cabin-display`         | `video`          | hidden  | HLS via hls.js. Mux "Big Buck Bunny" test stream. 720p ABR cap so Spectacles decodes cleanly. |
| `cabin-spatial-gallery` | `splat-gallery`  | hidden  | 3 × 3D Gaussian splats (Kyoto, Banff, Marrakech). ArrowL/R navigates, 30 s auto-advance. |

## HUD switching

```
[ Flight Info ]  [ Music ]  [ Video ]  [ Photos ]  [ Recenter ]  [ Leave ]
```

Each `show-only:<markId>` action makes that mark's cell visible and pauses all the others (audio + video + splat auto-advance), so switching back is instant.

## IMU data source

The `airplane-imu` mark polls `/api/v1/sensor/imu`. Two backends; the URL stays the same and the swap is the Vite proxy target:

### Real hardware: M5Capsule (preferred)

Bosch BMI270 6-DoF IMU on the Capsule's internal I2C bus (SDA = GPIO 8, SCL = GPIO 10, address 0x69). The `craw_imu` component samples at 50 Hz, runs a 6-DoF Madgwick AHRS to fuse roll/pitch/yaw, and serves the JSON over `esp_http_server`.

```bash
# 1. Flash the Capsule firmware (one-time)
cd reference-designs/MagNET_M5DialFiddlerCrab/M5Capsule_Hive_Scribe_Redis
pio run -e m5capsule -t upload

# 2. Bring up the dataspace pointed at the Capsule's IP
cd reference-designs/webxrofthings/prototype/d3-spatial
IMU_HOST=http://<capsule-ip> npm run dev
```

The Capsule runs an auto-boot ritual: ~5 s of buzzer pings (lay-it-flat reminder), then auto-init + auto-zero. After that the HTTP endpoint is live; the dataspace cell will pick it up on the next poll. Manual control on the Capsule console: `imu-on / imu-off / imu-status / imu-zero / imu-scan`.

**No magnetometer.** Yaw is integrated from gyro through Madgwick's IMU-only path and drifts roughly 1–2°/min once thermally stable. `imu-zero` on the Capsule (or re-flash → boot ritual auto-zeros) resets the heading datum.

See `MagNET_M5DialFiddlerCrab/M5Capsule_Hive_Scribe_Redis/README.md` for the firmware details.

### Simulation: mock-join-server

For demos without hardware, the `mock-join-server` serves a synthesised stream at the same URL/shape. Unset `IMU_HOST` to fall back to it via the generic `/api/v1` rule (which proxies to `localhost:3001`).

```bash
# Terminal 1: join server
cd reference-designs/webxrofthings/prototype/d3-spatial
npm run server

# Terminal 2: dataspace WITHOUT IMU_HOST
unset IMU_HOST
npm run dev
```

The synthesised values are time-based sines — the airplane "flies" through a slow procedural sequence.

## Gyro-driven dead reckoning

The BMI270 samples at 50 Hz internally but HTTP polling at 5 Hz nominal becomes 2–3 Hz effective through a cloudflared tunnel (round-trip latency). The cell handles this by integrating the snapshot's `angular_velocity` field at 60 fps between authoritative polls and snapping `targetQuat` to the firmware's fused orientation each time a poll lands. Result: fluid motion regardless of polling cadence, drift-corrected ~3 ×/sec. See `src/demo/liveImuCell.ts:onBeforeRender`.

## Visual scale (M22 — 2026-05-21)

The airplane + globe + instrument labels render 35% larger than the original prototype (manifest `size: 0.162`, was `0.12`). The instrument-label X offset multiplier was widened (1.3 → 1.5) so the bigger wings don't sweep into the KT / HDG labels' bounding boxes during banking, and the globe was pushed out (1.8 → 2.3 of `size`) so the heading text doesn't clip the globe's left hemisphere. The cell's overall extent is ~1 m wide at the new scale, comfortable in a Quest 3 / Spectacles FOV at the usual 1.4 m anchor distance.

## Vite proxy ordering

The `/api/v1/sensor/imu` proxy rule **must** appear before the generic `/api/v1/sensor` rule (which targets the UC2 AHT20 env sensor on the Atom Echo). Without the more-specific rule the IMU poll silently goes to UC2's device — which has `/api/v1/sensor/environment` but no `/api/v1/sensor/imu` — and the airplane instruments stay at `---`.

Vite proxy strips `Cf-*` / `X-Forwarded-*` headers from the cloudflared tunnel before forwarding to the Capsule, and the firmware bumps `CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048` (default 512 is too small for tunnel-augmented requests). Either fix alone resolves HTTP 431s; both is defense-in-depth.

## Loading this manifest

```bash
# Terminal 1: dev server (with IMU_HOST set, see above)
npm run dev

# Terminal 2: mock-join-server (only needed if testing the DEMO04 code flow)
npm run server

# Terminal 3 (optional): cloudflared tunnel for headset access
cloudflared tunnel --url http://localhost:5173
```

Then either type `DEMO04` in the join panel or open `?manifest=/examples/uc4-airplane.json` directly.

## Implementation files

- Manifest: `examples/uc4-airplane.json`
- Cells: `src/demo/liveImuCell.ts`, `src/demo/liveSpatialAudioCell.ts`, `src/viz/videoPanel.ts`, `src/demo/liveSplatGalleryCell.ts`
- HUD action plumbing: `src/main.ts:applyShowOnly`
- Firmware (real IMU): `reference-designs/MagNET_M5DialFiddlerCrab/M5Capsule_Hive_Scribe_Redis/components/craw_imu/`
- Mock IMU service: `server/mock-join-server.ts:617` (`GET /api/v1/sensor/imu`)
- Vite proxy: `vite.config.ts` — `/api/v1/sensor/imu` entry uses `IMU_HOST` env var, falls back to `magnet-scribe.local`
