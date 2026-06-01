# UC2 — Room Dataspace (kords-livingroom)

Manifest: `examples/uc2-room.json`

---

## Devices

| Device | UDM key | Hostname | Role |
|---|---|---|---|
| ESP32-CAM | `magnet-cam-80e4` | `magnet-cam-80e4.local` | Live video |
| M5StampC3U | `magnet-stamp-c3u-a1b2` | `magnet-stamp-c3u-a1b2.local` | Temperature |

Replace the `a1b2` suffix with your actual M5StampC3U MAC suffix when you flash it.

## Services exposed

### Camera (already working)
- `GET http://magnet-cam-80e4.local/stream` — MJPEG (CORS, header buffer fixes done)
- `GET http://magnet-cam-80e4.local/capture` — single JPEG (used by the manifest)

### Temperature (firmware needed on M5StampC3U)

The manifest expects two endpoints. Both return JSON, both should send `Access-Control-Allow-Origin: *`.

**`GET /temperature` — current reading**
```json
{ "celsius": 24.3, "timestamp": "2026-04-23T16:42:11Z" }
```

**`GET /temperature/history` — last 60 min as a time series**
```json
{
  "samples": [
    { "t": 1750000000000, "v": 23.8 },
    { "t": 1750000060000, "v": 23.9 },
    { "t": 1750000120000, "v": 24.1 }
  ]
}
```

The renderer's existing `line` mark handles this shape directly (`{ t, v }` per sample, ms-since-epoch on `t`).

## ESP32-C3 internal temperature

The ESP32-C3 has a built-in temperature sensor accessed via the ESP-IDF `temperature_sensor` driver. Accuracy is rough (±5–10°C in absolute terms — it's reading the die temperature, not ambient) but it's stable and good enough to demonstrate live data streaming end-to-end. For a real sensor, swap in a DS18B20, BME280, or SHT4x — the manifest stays the same; only the firmware reads a different sensor.

### Minimal sketch outline (Arduino-style)

```cpp
#include <WiFi.h>
#include <WebServer.h>
#include "driver/temperature_sensor.h"

WebServer server(80);
temperature_sensor_handle_t temp_sensor;

// Ring buffer for last 60 min at 1 sample/min
struct Sample { uint64_t t; float v; };
Sample history[60];
int historyCount = 0;
int historyHead = 0;

void setup() {
  // ... WiFi setup, mDNS as magnet-stamp-c3u-XXXX ...

  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  temperature_sensor_install(&cfg, &temp_sensor);
  temperature_sensor_enable(temp_sensor);

  server.on("/temperature", handleCurrent);
  server.on("/temperature/history", handleHistory);
  server.onNotFound(handleNotFound);
  server.begin();
}

float readCelsius() {
  float c;
  temperature_sensor_get_celsius(temp_sensor, &c);
  return c;
}

void sendCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
}

void handleCurrent() {
  sendCors();
  char body[128];
  snprintf(body, sizeof(body),
    "{\"celsius\":%.2f,\"timestamp\":\"%llu\"}",
    readCelsius(), (uint64_t)(time(nullptr)) * 1000ULL);
  server.send(200, "application/json", body);
}

void handleHistory() {
  sendCors();
  String body = "{\"samples\":[";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyHead - historyCount + i + 60) % 60;
    if (i > 0) body += ",";
    body += "{\"t\":" + String((unsigned long long)history[idx].t) + ",\"v\":" + String(history[idx].v, 2) + "}";
  }
  body += "]}";
  server.send(200, "application/json", body);
}

void loop() {
  server.handleClient();
  // Once per minute, push a sample into the ring buffer
  static unsigned long lastSample = 0;
  if (millis() - lastSample > 60000) {
    history[historyHead] = { (uint64_t)time(nullptr) * 1000ULL, readCelsius() };
    historyHead = (historyHead + 1) % 60;
    if (historyCount < 60) historyCount++;
    lastSample = millis();
  }
}
```

For the `OPTIONS` preflight, also add:
```cpp
server.on("/temperature", HTTP_OPTIONS, []() {
  sendCors();
  server.send(204);
});
server.on("/temperature/history", HTTP_OPTIONS, []() {
  sendCors();
  server.send(204);
});
```

## Vite proxy additions

For the manifest to load through the existing proxy pattern, extend `vite.config.ts`:

```typescript
proxy: {
  '/api/v1':   { target: 'http://localhost:3001', changeOrigin: true },
  '/camera':   { target: 'http://magnet-cam-80e4.local', changeOrigin: true,
                 rewrite: p => p.replace(/^\/camera/, ''), agent: cameraAgent },

  // NEW: temperature endpoints
  '/api/v1/sensors/temp/current':
    { target: 'http://magnet-stamp-c3u-a1b2.local', changeOrigin: true,
      rewrite: () => '/temperature', agent: tempAgent },
  '/api/v1/sensors/temp/history':
    { target: 'http://magnet-stamp-c3u-a1b2.local', changeOrigin: true,
      rewrite: () => '/temperature/history', agent: tempAgent },
},
```

Or keep it simple — just expose the M5StampC3U directly as `/temp/*` without renaming.

## Loading this manifest

```bash
# Terminal 1: dev server
npm run dev

# Terminal 2: tunnel for Quest access (camera proxy is local-only via Vite)
cloudflared tunnel --url http://localhost:5173

# Open the tunnel URL on Quest with manifest mode:
#   https://<tunnel>.trycloudflare.com/?manifest=/examples/uc2-room.json
```

The `?manifest=<url>` path skips the join panel and loads the manifest directly — useful for quick testing. Once the join server is wired to serve `uc2-room.json` instead of the demo manifest, the join-code flow lands in this dataspace.

## Notes on UDM/USM use

- `udm_devices` and `usm_services` are metadata only — the renderer doesn't act on them yet.
- They give the inspector card and dataspace HUD richer info to show ("Camera · Magnet Cam · ESP32 OV2640") and let us swap implementations without touching the manifest's marks.
- `marks[].deviceRef` and `marks[].serviceRef` are pointers into these arrays; future renderer work can show device pin glyphs at `udm_spatial_anchor` coordinates, list services in a side panel, etc.
- The `udm_spatial_anchor` field is our addition to UDM (not in the upstream spec yet) — it's how we anchor virtual content to the real-world location of a physical device. Worth proposing back to the IoTone spec as a `udm_spatial_*` extension.

## Next steps after the firmware ships

1. Flash the M5StampC3U with the temperature service
2. Verify the endpoints return CORS-friendly JSON
3. Update `magnet-stamp-c3u-a1b2` placeholder in the manifest with the real MAC suffix
4. Add the Vite proxy entry for the temperature paths
5. Test the dataspace loads end-to-end via `?manifest=/examples/uc2-room.json`
6. Once stable, register `uc2-room.json` as the default manifest in the join server
