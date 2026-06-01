# Video Streaming in XR Dataspaces — Approach Analysis

Status: Research · Date: 2026-04-19

---

## Goal

A camera (ESP32-CAM, IP camera, or test stream) appears as a service in a dataspace manifest. The user joins the dataspace in XR, sees a live video feed as a spatial panel, can reposition/resize it, and interact alongside other data marks (charts, trees, force graphs).

---

## 1. Approaches compared

### 1A. HLS (HTTP Live Streaming) via `<video>` → three.js `VideoTexture`

**How it works:**
- Create a hidden `<video>` element with an HLS source (`.m3u8` playlist)
- Use `hls.js` to polyfill HLS on browsers that don't support it natively
- Create a `THREE.VideoTexture(videoElement)` that samples the video each frame
- Apply the texture to a `THREE.PlaneGeometry` in the scene
- The plane is a spatial panel — positionable, rotatable, hoverable

**Pros:**
- Works in any browser with MediaSource Extensions (Quest's Meta Browser ✓)
- No WebRTC complexity — just HTTP
- Many free test streams available (no hardware needed to start)
- `hls.js` is battle-tested (30 KB gzipped)
- Latency: 2-10 seconds (acceptable for monitoring, not for control)

**Cons:**
- HLS latency is high (segment-based, typically 3-6s)
- Requires an HLS-capable server or CDN
- ESP32-CAM doesn't natively output HLS (needs a transcoding proxy)

**Test streams (no hardware needed):**
- Apple's test stream: `https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_4x3/bipbop_4x3_variant.m3u8`
- Akamai test: `https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8`
- These let us validate the XR UX immediately without any device setup

### 1B. MJPEG stream from ESP32-CAM → `ImageBitmapLoader` / `<img>` → `CanvasTexture`

**How it works:**
- ESP32-CAM serves an MJPEG stream at `http://<ip>:81/stream` (built-in, no setup)
- Option A: Use `<img src="http://ip:81/stream">` — browser renders MJPEG natively, capture to canvas each frame, use `THREE.CanvasTexture`
- Option B: Fetch JPEG frames via `fetch()` with ReadableStream, decode with `createImageBitmap()`, update a `THREE.DataTexture`

**Pros:**
- ESP32-CAM outputs MJPEG out of the box (Arduino example sketch, 2 minutes to flash)
- No transcoding server needed
- Very low complexity — the simplest possible camera integration
- Latency: 100-500ms (near real-time)
- Works on local network (no cloud needed)

**Cons:**
- MJPEG is bandwidth-heavy (no inter-frame compression) — 2-5 Mbps at VGA
- `<img>` MJPEG trick doesn't work in all browsers consistently
- Cross-origin issues if camera is on a different host (need CORS headers or proxy)
- Quest's Meta Browser may block mixed content (HTTPS page → HTTP camera stream)
- Resolution limited by ESP32-CAM (typically 640x480 or 800x600)

**The mixed-content problem:** Our XR page runs on HTTPS (required for WebXR). ESP32-CAM serves HTTP. Browsers block HTTP resources from HTTPS pages. Solutions:
1. Proxy through the context engine (HTTPS server fetches from HTTP camera, serves to client)
2. ESP32-CAM with TLS (possible but complex on ESP32)
3. Use the Vite proxy during development (`/api/v1/camera` → `http://esp32-cam-ip:81/stream`)

### 1C. WebRTC via a signaling server

**How it works:**
- ESP32-CAM (or any camera) → WebRTC-capable server (e.g., mediamtx, Janus) → peer connection to browser
- Browser receives `MediaStream` → `THREE.VideoTexture(videoElement)` (same as HLS)

**Pros:**
- Sub-second latency (50-200ms)
- Bidirectional — could add audio, send commands back
- Modern approach, good browser support

**Cons:**
- Heavy setup: needs a signaling server + STUN/TURN
- ESP32-CAM doesn't speak WebRTC natively (needs a bridge like mediamtx or go2rtc)
- More moving parts to deploy
- Overkill for a monitoring camera

### 1D. WebSocket binary frames

**How it works:**
- ESP32-CAM captures JPEG frames, sends via WebSocket to the context engine
- Context engine relays via WSS to the browser
- Browser decodes JPEG → `createImageBitmap()` → `THREE.CanvasTexture` update each frame

**Pros:**
- Uses the same WSS transport as live data (manifest `source: "url"` with `wss://`)
- Integrates naturally with the existing manifest schema
- Latency: 100-300ms
- No mixed-content issues (everything over WSS through the context engine)

**Cons:**
- Custom protocol (not standard streaming)
- Need to manage frame rate and backpressure
- JPEG decode on the main thread could block at high frame rates (use `createImageBitmap` off-thread)

---

## 2. Recommended approach: start with HLS test stream, then add MJPEG/WSS for ESP32-CAM

### Phase 1: HLS test stream (validate XR UX — no hardware needed)

1. Install `hls.js`:
   ```bash
   npm install hls.js
   ```

2. Create `src/viz/videoPanel.ts`:
   ```typescript
   export interface VideoPanelOptions {
     url: string;              // HLS .m3u8 URL or MJPEG URL
     type?: 'hls' | 'mjpeg' | 'wss';
     width?: number;           // panel width in meters (default 0.4)
     aspectRatio?: number;     // default 16/9
   }
   
   export interface VideoPanelViz {
     group: THREE.Group;
     mesh: THREE.Mesh;         // the video plane
     tick(): void;              // update texture each frame
     play(): void;
     pause(): void;
     dispose(): void;
   }
   
   export function buildVideoPanel(opts: VideoPanelOptions): VideoPanelViz { ... }
   ```

3. Implementation:
   - Create a hidden `<video>` element
   - If HLS: attach `hls.js` to it, load the `.m3u8` URL
   - If MJPEG: set `<img>` src, capture to canvas each frame
   - `THREE.VideoTexture(video)` on a `PlaneGeometry(width, width/aspectRatio)`
   - `MeshBasicMaterial({ map: videoTexture })` — no lighting needed for video
   - Add a thin border frame (wireframe box) around the panel
   - Add a title label above the panel (troika text)
   - `tick()`: `videoTexture.needsUpdate = true` (three.js handles the rest)

4. Register as a mark type in the manifest:
   ```json
   {
     "id": "front-door-cam",
     "type": "video",
     "title": "Front door",
     "data": {
       "source": "url",
       "url": "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8",
       "shape": "video"
     },
     "config": { "width": 0.4, "aspectRatio": 1.777 }
   }
   ```

5. Test with a public HLS stream to validate:
   - Video renders as a spatial panel in XR
   - User can hover the panel (highlight border)
   - User can pinch to select/pin the panel
   - User can drag the panel to reposition it
   - Video plays at acceptable frame rate in Quest's browser

### Phase 2: ESP32-CAM via context engine (real device)

1. ESP32-CAM serves MJPEG at `http://<ip>:81/stream`
2. Context engine (the join server, extended) proxies:
   - Receives MJPEG frames from ESP32-CAM
   - Strips JPEG boundaries, re-serves as WebSocket binary messages over WSS
   - Or: re-encodes as HLS segments (heavier, but uniform transport)
3. Manifest specifies:
   ```json
   {
     "type": "video",
     "data": {
       "source": "url",
       "url": "wss://hlxr.org/ds/room/camera/front-door",
       "shape": "video"
     }
   }
   ```
4. `videoPanel.ts` handles the WSS path:
   - Connect to WSS URL
   - On each binary message: `createImageBitmap(blob)` → update `CanvasTexture`
   - Target 10-15 fps (ESP32-CAM typical at VGA)

### Phase 3: Multiple cameras as services

- Each camera is a mark in the manifest
- Room dataspace has: device tree + sensor charts + camera panels
- User sees a spatial dashboard with live video feeds alongside data visualizations
- Pinch a camera panel → it expands to fill more space
- Two cameras side-by-side for comparing angles

---

## 3. Manifest schema extension

Add `'video'` to the `MarkType` union:
```typescript
export type MarkType =
  | 'line' | 'bar' | 'scatter' | 'arc'
  | 'tree' | 'treemap' | 'sunburst' | 'pack'
  | 'force' | 'ridgeline' | 'sankey'
  | 'parallel' | 'tangled-tree' | 'edge-bundle' | 'hexbin'
  | 'video';  // NEW
```

Data shape for video:
```typescript
export interface InlineData {
  // ... existing fields ...
  /** For video marks: not used (video is always URL-sourced) */
}

// UrlData already supports any URL; shape: 'video' is the discriminant
```

Config for video marks:
```typescript
// In MarkSpec.config:
{
  width?: number;         // panel width in meters (default 0.4)
  aspectRatio?: number;   // width/height (default 16/9)
  autoplay?: boolean;     // start playing on load (default true)
  muted?: boolean;        // mute audio (default true for cameras)
  type?: 'hls' | 'mjpeg' | 'wss';  // transport hint (auto-detected if omitted)
}
```

---

## 4. Quick-start: verify in 30 minutes

**Without any hardware:**

```bash
# 1. Install hls.js
npm install hls.js

# 2. Create src/viz/videoPanel.ts (the mark builder)

# 3. Register the 'video' builder in src/manifest/builders.ts

# 4. Add a video mark to examples/room-dataspace.json using a public HLS test stream

# 5. npm run dev, open ?scene=demo, see the video panel in the gallery
```

Public HLS test streams that work without auth:
- Mux test: `https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8`
- Apple bipbop: `https://devstreaming-cdn.apple.com/videos/streaming/examples/bipbop_4x3/bipbop_4x3_variant.m3u8`
- Akamai live: `https://cph-p2p-msl.akamaized.net/hls/live/2000341/test/master.m3u8`

**With ESP32-CAM (needs the device):**
- Flash the CameraWebServer example sketch
- Note the IP from serial monitor
- Add Vite proxy: `'/api/v1/camera': { target: 'http://<esp32-ip>:81' }`
- Use URL `/api/v1/camera/stream` in the manifest

---

## 5. XR-specific considerations

### Video panel placement
- Default: floating panel at the same height as other marks in the gallery grid
- In room-scale (UC2): anchor to real-world position where the camera physically is
- Size: 40cm wide default (comfortable at 1.2m viewing distance)

### Performance on Quest
- HLS at 720p: ≤30 fps is fine (Quest's browser handles this)
- Video decode is hardware-accelerated on Quest (Snapdragon XR2)
- Multiple simultaneous streams: test with 2-3; likely a 4-stream limit before frame drops
- `VideoTexture` updates each frame but only if the video has a new frame (three.js checks internally)

### Interaction
- Hover: border highlight (same as chart marks)
- Pinch: select/pin (same latched behavior)
- Drag: reposition the panel in 3D space (same as force-node drag but for the whole panel)
- Pinch-and-stretch (two hands): resize the panel
- Double-pinch: toggle fullscreen (panel scales to 1m wide, other marks dim)

### Mixed content
- WebXR requires HTTPS
- Camera streams over HTTP get blocked
- **Solution for dev:** Vite proxy (already configured)
- **Solution for production:** context engine proxies camera streams over WSS (same-origin, no mixed content)

---

## 6. Decision for you

- [ ] Start with HLS test stream to validate XR video panel UX? (Fastest — 30 min, no hardware)
- [ ] Jump straight to ESP32-CAM? (Needs device + flash + network setup)
- [ ] Both in sequence? (HLS first to nail the UX, then swap to real camera)

I recommend "both in sequence" — validate the panel UX with a test stream today, then swap in the real camera when your devices are ready. The `videoPanel.ts` builder handles both transports.
