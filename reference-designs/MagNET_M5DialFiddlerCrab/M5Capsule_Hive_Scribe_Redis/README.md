# M5Capsule Hive Scribe + Redis (Role 4 variant)

Same hardware as `M5Capsule_Hive_Scribe`, plus a RESP2-compatible TCP server so a laptop `redis-cli` (or any Redis client library) can read/write a key-value store backed by the same NVS partition the scribe already manages.

The Redis server is a sidecar — every existing scribe behavior (BLE provisioning, hive node join, role-bundle install, MQTT bridge) still works. The Redis surface is **off by default** at boot; bring it up explicitly with `redis-on`.

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
