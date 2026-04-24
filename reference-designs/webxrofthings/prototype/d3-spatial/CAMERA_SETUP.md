# ESP32-CAM Setup & Streaming

Status: Draft · Last updated: 2026-04-24

---

## Working setup (localhost dev, Mac Chrome)

This is the tested working path.

### Prerequisites

- **ESP32-CAM** flashed with CameraWebServer example, modified to send CORS headers:
  ```c
  // In app_httpd.cpp stream_handler, before streaming:
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, OPTIONS");
  ```
  Plus an OPTIONS handler for the preflight (see `CAMERA_FIRMWARE.md` if we create one).

- **Mac on the same Wi-Fi network** as the camera. Camera at e.g. `http://10.0.0.185/stream`.

- **macOS Local Network permission** granted to Terminal:
  - System Settings → Privacy & Security → Local Network → enable Terminal
  - Without this, curl/Node/cloudflared all fail with "No route to host" even though Chrome works
  - This is the single most common stumbling block on macOS 15+

### Run

```bash
VITE_CAMERA_URL=http://10.0.0.185/stream npm run dev
# Open http://localhost:5173/?scene=demo
```

The video panel in the bottom row shows the live MJPEG feed.

### Verify

```bash
# From Mac terminal (with Local Network permission granted):
curl -v http://10.0.0.185/stream --max-time 5 2>&1 | head -10
# Should see: HTTP/1.1 200 OK, Content-Type: multipart/x-mixed-replace
```

---

## Known failure mode: Quest / Spectacles via cloudflared

**Symptom:** Works on localhost. Fails to load the video panel on Quest 3 or Snap Spectacles when accessing via cloudflared tunnel. Other marks render; only the video cell shows "camera offline."

### Possible causes (ordered by likelihood)

#### 1. Cloudflared buffers `multipart/x-mixed-replace`

**Most likely.** Cloudflare's edge proxy buffers HTTP responses to inspect them for routing, caching, and WAF rules. Persistent streaming responses like MJPEG can hit buffer timeouts or have boundary markers stripped.

**Symptom:** Browser sees the request hang, time out, or connect but never show frames.

**Workaround:**
- Use cloudflared with `--protocol http2` to stream without buffering: `cloudflared tunnel --url http://10.0.0.185 --protocol http2`
- Or: switch to the "frame loop" approach (§VIDEO_STREAMING_ANALYSIS.md) — periodic JPEG snapshots fetched individually, which Cloudflare handles as regular HTTP requests
- Or: use a different tunnel service that supports long-lived HTTP streaming (ngrok, localtunnel, direct port forwarding)

#### 2. Mixed content

**If the camera URL is still `http://` when served from HTTPS tunnel**, the browser blocks it.

**Symptom:** Console error "Mixed Content: blocked" or the request simply doesn't fire.

**Workaround:**
- Tunnel the camera too: `cloudflared tunnel --url http://10.0.0.185`
- Use the resulting `https://xxx.trycloudflare.com` URL in `VITE_CAMERA_URL`
- Both the app and the camera are now HTTPS — no mixed content

#### 3. CORS mismatch over tunnel

Even if the camera sends `Access-Control-Allow-Origin: *` on GET, the browser may send a **preflight OPTIONS request** before the GET. ESP32-CAM typically doesn't respond to OPTIONS, so the preflight fails and the GET never happens.

**Symptom:** DevTools network tab shows an OPTIONS request to the stream URL with no response or 404.

**Workaround:**
- Add an OPTIONS handler to the ESP32-CAM firmware that returns the CORS headers and `204 No Content`
- Or: drop `crossOrigin = 'anonymous'` from the `<img>` and accept that the texture is cross-origin. WebGL may still refuse to upload it.

#### 4. Quest / Spectacles MJPEG support

Quest's Meta Browser is Chromium-based and should handle `multipart/x-mixed-replace` in `<img>`. Spectacles' browser is more limited — MJPEG in `<img>` may not work at all.

**Symptom:** Works on Quest, fails only on Spectacles. Or connection succeeds but no frames render.

**Workaround:**
- Switch to the frame-loop approach (periodic HTTP GETs for individual JPEGs, not a persistent stream)
- Or: use a context-engine transcoder that converts MJPEG → HLS, served over WSS

#### 5. Cloudflared DNS resolution timing

Cloudflared tunnels sometimes take 30-60 seconds to propagate DNS. If the Quest tries to fetch the camera URL within that window, it fails with DNS error.

**Symptom:** First attempt fails, reload after a minute works.

**Workaround:** wait before testing; use a persistent named tunnel instead of a throwaway `--url` tunnel.

### How to diagnose from Quest

Plug the Quest into a desktop via USB, enable Developer Mode, open `chrome://inspect` in desktop Chromium. Click "Inspect" on the Quest browser tab. The DevTools Network panel shows exactly what the Quest browser sees:

- Is the camera URL HTTPS or HTTP? (mixed content check)
- Is there a preflight OPTIONS request? Does it succeed? (CORS check)
- Does the GET request complete, hang, or time out? (buffering check)
- Is the Content-Type `multipart/x-mixed-replace`? (stream format check)

---

## Full cloudflared-to-Quest pipeline (when buffering works)

After resolving the issues above, the full pipeline:

```bash
# Terminal 1: tunnel the camera directly
cloudflared tunnel --url http://10.0.0.185
# note: https://<camera-tunnel>.trycloudflare.com

# Terminal 2: dev server with the camera URL
VITE_CAMERA_URL=https://<camera-tunnel>.trycloudflare.com/stream npm run dev

# Terminal 3: tunnel the app
cloudflared tunnel --url http://localhost:5173
# note: https://<app-tunnel>.trycloudflare.com

# Terminal 4 (optional): join server for join-code flow
npm run server
```

Open the app-tunnel URL on Quest. The browser fetches:
- The app from `https://<app-tunnel>.trycloudflare.com`
- The camera stream from `https://<camera-tunnel>.trycloudflare.com/stream`

Both are HTTPS, different origins, CORS-enabled on the camera side. Should work — unless cloudflared's buffering breaks the MJPEG stream (issue #1 above).

---

## Fallback: frame-loop mode

If persistent streaming over cloudflared doesn't work reliably, switch to polled frames. The context engine periodically snapshots the camera and serves the N most recent frames as a small bundle. The browser loops the bundle until the next poll arrives. Latency is 3-5 seconds but reliability is much higher.

See `VIDEO_STREAMING_ANALYSIS.md` Phase 2 for the architecture.

---

## macOS Local Network permission — why this matters

Starting with macOS 15 (Sequoia), apps must have explicit "Local Network" permission to access devices on the LAN. The permission is granted per-app:

- **Chrome** gets it automatically when you visit a `http://10.0.0.x` URL (with a permission prompt)
- **Terminal** does NOT get it by default — you must grant it in System Settings
- **Node, cloudflared, Vite** all use Terminal's network context, so they inherit whatever permission Terminal has

**If you don't grant it:** curl, Node fetch, cloudflared — anything run from Terminal — fails with `EHOSTUNREACH` or "No route to host" even though the camera is clearly reachable (ARP resolves, route exists). Chrome works, so it looks like a Node-specific bug. It isn't; it's macOS security.

**Fix:** System Settings → Privacy & Security → Local Network → enable the toggle for Terminal (and iTerm2 if you use it).

This was the single biggest stumbling block in our debugging session. Document it prominently for anyone setting this up fresh on macOS.
