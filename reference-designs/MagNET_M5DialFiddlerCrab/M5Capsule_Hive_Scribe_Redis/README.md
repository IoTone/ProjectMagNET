# M5Capsule Hive Scribe + Redis (Role 4 variant)

Same hardware as `M5Capsule_Hive_Scribe`, plus a RESP2-compatible TCP server so a laptop `redis-cli` (or any Redis client library) can read/write a key-value store backed by the same NVS partition the scribe already manages. **Also**: a BMI270-backed IMU service (`craw_imu` component) that exposes a Madgwick-fused orientation + raw accel/gyro at `GET /api/v1/sensor/imu`, used by the d3-spatial UC4 airplane dataspace as the real-hardware swap for the mock-join-server's simulated stream.

The Redis server and the IMU service are both sidecars — every existing scribe behavior (BLE provisioning, hive node join, role-bundle install, MQTT bridge) still works. **Both surfaces are off by default at boot**; bring them up explicitly with `redis-on` / `imu-on`.

## Hardware

Identical to `M5Capsule_Hive_Scribe`. See that project's README for the pin map and provisioning flow.

## Wire format

RESP2. Both inline (`PING\r\n`) and array (`*1\r\n$4\r\nPING\r\n`) command forms are accepted so telnet and standard clients are equally happy. Frames cap at 16 KB. Up to 4 concurrent clients (per-client task pattern, mirrors the hive ruler).

## Commands implemented

```
PING [msg]                     SET k v
ECHO msg                       GET k
QUIT                           DEL k [k...]
SELECT 0                       EXISTS k [k...]
AUTH pw       (no-op stub)     TYPE k
CLIENT GETNAME / SETNAME / ID  KEYS pat            (* and ?)
COMMAND [COUNT|DOCS]           DBSIZE
INFO [section]                 FLUSHDB / FLUSHALL

LPUSH k v [v...]               LPOP k
RPUSH k v [v...]               RPOP k
LLEN k                         LINDEX k idx        (negative idx ok)
LRANGE k start stop            (negative idx ok)
```

Anything else returns `-ERR unknown command 'X'`. Operating on a string key with a list command (or vice versa) returns `-WRONGTYPE Operation against a key holding the wrong kind of value`.

## Storage limits (NVS backend, v1)

| Limit                    | Value      |
|--------------------------|------------|
| User key length          | 14 chars   |
| String value             | 1024 bytes |
| List total per key       | 4 KB       |
| List entries per key     | 64         |
| List entry length        | 1024 bytes |
| Concurrent clients       | 4          |

These ride on top of ESP-IDF's NVS-blob limits. The future SD-FAT backend (`M5Cam_Redis_SD/`) lifts each by ~1000×.

## Configuration profiles

```
0 redis-profile!   →  local   bind=127.0.0.1   port=6379
1 redis-profile!   →  lan     bind=0.0.0.0     port=6379    (default)
2 redis-profile!   →  quiet   bind=0.0.0.0     port=16379
3 redis-profile!   →  custom  (preserved across reboots)
```

`redis-bind <addr>` and `N redis-port` switch the active profile to `custom` automatically. Profile + custom overrides live in NVS namespace `scribe_redis`.

## Forth surface

```
redis-on                   start the listener on the active config
redis-off                  stop, close all clients, clear redis:status
redis-status               state + bind/port + clients + cmd counter

redis-profile              print active profile + bind + port
N redis-profile!           apply preset (0..3) and persist
redis-bind                 prompt for bind addr → switches to custom
N redis-port               set port → switches to custom

redis-flush                FLUSHDB on the local store
redis-do                   prompt for a command, send to localhost, print reply
```

## Built-in client (`redis-do`)

Connects to `127.0.0.1:<active-port>` from the Capsule itself, sends a single command (whitespace-tokenized into a RESP array), drains the reply, pretty-prints with the standard `redis-cli` indentation:

```
redis> SET foo bar
+OK

redis> GET foo
"bar"

redis> LPUSH log first second
(integer) 2

redis> LRANGE log 0 -1
1) "second"
2) "first"
```

## Hive integration — Dial indicator dot

While the server is up and the scribe is joined to the hive, a 5 s heartbeat publishes `redis:status` → `<self-id>:<port>:<clients>` to the hive KV table via `craw_hive_node_kv_put`. The Dial reads that key and renders a small orange dot (next to the purple MQTT-bridge dot) so you can confirm the server is reachable from across the room. Dot goes dark when `redis-off` clears the value.

## Security model — v1

Cleartext TCP, dev-only. Profile defaults to `lan` (0.0.0.0). Every server start logs:

```
[redis] up on 0.0.0.0:6379
[WARN] exposed on LAN, no encryption
```

`AUTH` is parsed and replied `+OK` so `redis-cli -a foo` doesn't error, but the password is **not** checked. Real auth lands in v2.

## Smoke test

1. Flash; reboot; provision WiFi; confirm hive join via `hive-status`.
2. `redis-on` — boot banner shows `[redis] up on 0.0.0.0:6379`.
3. From the laptop: `redis-cli -h <capsule-ip> ping` → `PONG`.
4. `redis-cli -h <capsule-ip> SET foo bar`, then `GET foo` → `"bar"`.
5. `LPUSH log a`, `LPUSH log b`, `LRANGE log 0 -1` → `["b", "a"]`.
6. `DBSIZE`, `KEYS *`, `INFO`.
7. Reboot. `redis-on`. `GET foo` should still return `bar` (NVS persistence).
8. On the Dial: `ruler-status` lists this Capsule with `bridge:status` / `redis:status` round-tripped. The Dial's main display shows the orange Redis dot next to the bridge dot.
9. `redis-do` from the Capsule REPL — confirms loopback path works without needing the laptop.

## IMU (`craw_imu`) — UC4 airplane data source

The Capsule carries a Bosch BMI270 6-DoF IMU (3-axis accel + 3-axis gyro) on the internal I2C bus (G8=SDA, G40=SCL). The `craw_imu` component samples it at 50 Hz, runs a 6-DoF Madgwick AHRS to fuse roll/pitch/yaw, and exposes the result over HTTP for the d3-spatial UC4 airplane dataspace.

### Why the BMI270 driver is vendored, not managed

The Espressif-maintained `espressif/bmi270` registry component transitively depends on `espressif/sensor_hub` → `espressif/i2c_bus`. The latter's v2 backend (`i2c_bus_v2.c`) calls `i2c_master_get_bus_handle()` which was added in ESP-IDF **v5.4**. Our platform pin `espressif32@6.9.0` ships ESP-IDF v5.3.1, so the managed-component build fails with `implicit declaration of function 'i2c_master_get_bus_handle'`. The bmi270 driver itself uses only v5.3-compatible I2C APIs and has no internal i2c_bus dep, so we vendor it under `components/craw_imu/vendor/bmi270/` (Apache-2.0, original from `espressif/bmi270` 1.0.1~1) and skip the transitive entirely. If the platform bumps to `espressif32@6.10+`, the vendor copy can be deleted and the managed dep restored — see `components/craw_imu/idf_component.yml` for the restore recipe.

### What's served

```
GET /api/v1/sensor/imu
{
  "orientation":      { "roll_rad": ..., "pitch_rad": ..., "yaw_rad": ... },
  "angular_velocity": { "x": ..., "y": ..., "z": ... },   // rad/s
  "acceleration":     { "x": ..., "y": ..., "z": ... },   // m/s² (gravity included)
  "timestamp_us":     ...
}
```

Same shape as the mock-join-server's simulated stream — swap is via the dataspace's Vite proxy (`/api/v1/sensor/imu` → `IMU_HOST=...` env var).

### Heading caveat — no magnetometer

The BMI270 does **not** include a magnetometer. Yaw is integrated from the gyro's Z-axis through Madgwick's IMU-only path and drifts at roughly 1–2°/min once the sensor is thermally stable. There is no "north" — only "where you said zero was."

`imu-zero` snaps the current yaw as the heading 0 datum, which is the user-controlled equivalent of magnetic-north calibration. The exposed `yaw_rad` is always `wrap_pi(raw_yaw - zero_datum)`.

### Forth surface

```
imu-on                start I2C + BMI270 + sample task + httpd + mDNS
imu-off               stop httpd + mDNS + sample task (driver stays alive)
imu-status            sampler state, sample count, current orientation + raw
imu-zero              set current yaw as heading 0 datum
sda scl imu-scan      i2cdetect-style probe of the bus (sda+scl as ints)
```

### Auto-boot ritual

At boot the firmware runs a 5-step ritual automatically — no need to type `imu-on` after every flash:

1. Wait ~2 s for early-boot logs to settle.
2. Five rising-pitch reminder pings, 1 s apart (1200 → 2200 Hz). User reads this as a countdown to "lay the device flat now."
3. Init I2C, bring up the BMI270 + Madgwick sampler + HTTP endpoint.
4. Wait 1.5 s for Madgwick to converge.
5. Auto-zero the heading datum (sets the user's current pose as HDG 000°), and chime a two-ping success tone.

If IMU init fails, a triple low buzz (400 Hz × 200 ms × 3) plays and the user can retry with `imu-on` manually.

### WiFi-down chirp

A monitor task polls `craw_wifi_is_connected()` every 10 s. While WiFi is down (unconfigured, AP unreachable, or just disconnected), a single quiet low-pitch chirp (600 Hz × 120 ms at half default volume) plays so the user knows what's wrong without checking the serial console. Silent while connected.

### Buzzer volume

The piezo buzzer is driven by 8-bit LEDC PWM (0..255 duty). Volume scales roughly linearly with duty from ~5 % to 50 % (= 128) — past 50 % the wave inverts and gets quieter again, so the firmware caps duty at 128.

```
hz dur buzz                  one-shot tone at default volume
hz dur duty buzz-v           one-shot at explicit volume (0..128)
duty buzz-vol                set default volume for `buzz` + boot ritual
```

Default duty is 64 (50 % of max, audible at arm's length but not abusive in a quiet room).

### Smoke test

1. Provision WiFi via BLE (same flow as the base scribe).
2. `imu-on` → boot banner shows `[imu] up on http://magnet-scribe-XXXX.local/api/v1/sensor/imu`.
3. From a laptop on the same LAN: `curl http://magnet-scribe-XXXX.local/api/v1/sensor/imu | jq .`
4. Lay the Capsule flat panel-up — `accel.z` should read ~+9.8 m/s², `roll`/`pitch` ~0°.
5. Tilt it 30° on the long axis — `roll` should track within ~1° of the eyeball estimate.
6. Spin it on a turntable — `yaw_rad` should rotate linearly, but expect drift over minutes (no magnetometer).
7. `imu-zero` to reset the datum, then the UC4 airplane in the d3-spatial dataspace should read HDG 000° regardless of physical pose.

### Wiring d3-spatial UC4 to this device

The dataspace's proxy table catches `/api/v1/sensor/imu` BEFORE the generic `/api/v1/sensor` rule (which targets the UC2 AHT20 env sensor — without the more-specific rule, UC4 polls UC2's Atom Echo and the airplane labels stay at `---`):

```bash
IMU_HOST=http://magnet-scribe-XXXX.local  npm run dev
# or by IP
IMU_HOST=http://10.0.0.77  npm run dev
```

The UC4 manifest's `airplane-imu-001` device entry already references this Capsule (see `examples/uc4-airplane.json`).

## Related work

- **[DankJugal/ESP32RedisClient](https://github.com/DankJugal/ESP32RedisClient)** — Arduino-flavored ESP32 Redis client. Reference for RESP framing edge cases; pairs naturally with this server when wiring an ESP32Cam-as-client / Capsule-as-server demo.
- **redis-cli (upstream)** — the canonical RESP2 reference for `INFO` field shapes and `KEYS` glob semantics.
- **`M5Capsule_Hive_Scribe/`** — the original scribe; this project's MQTT bridge, Forth-word patterns, and bridge-status dot all come from there.
- **Future**: `M5Cam_Redis_SD/` (planned, separate project) — same `craw_redis` server with a FAT-on-SD storage backend for keys/values that don't fit in NVS.

## Known limitations / v2 candidates

- AUTH not enforced (parsed but ignored).
- No PUB/SUB — would need a separate task per subscriber.
- No `EXPIRE` / TTL — every key is permanent until `DEL` or `FLUSHDB`.
- No `RENAME`, `LSET`, `LREM`, `LTRIM`, `BLPOP` — push-pop covers the demo cases.
- 14-char key limit ride-along from NVS; SD backend lifts.
- Iteration loads each key one-by-one — `KEYS *` on a thousand-entry DB is slow (acceptable; we cap at 64 returned).
