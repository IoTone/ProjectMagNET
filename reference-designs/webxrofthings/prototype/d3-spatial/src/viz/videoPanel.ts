import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT, EDGE } from '../ui/palette';

export interface VideoPanelOptions {
  url: string;
  // 'mp4' = a plain progressive file played by the browser's native
  // <video> decoder (no MSE). Required for Snap Spectacles, whose
  // browser decodes HLS *audio* but won't upload an MSE-fed <video> to
  // a WebGL texture (black panel, audio only). A direct .mp4 textures
  // fine there. 'hls' stays the default for desktop/Quest.
  type?: 'hls' | 'mjpeg' | 'frames' | 'mp4';
  width?: number;
  aspectRatio?: number;
  title?: string;
  autoplay?: boolean;
  muted?: boolean;
  /** For 'frames' mode: ms between captures. Default 300. */
  frameIntervalMs?: number;
}

export interface VideoPanelViz {
  group: THREE.Group;
  mesh: THREE.Mesh;
  tick(): void;
  play(): void;
  pause(): void;
  isPlaying(): boolean;
  dispose(): void;
}

export function buildVideoPanel(opts: VideoPanelOptions): VideoPanelViz {
  const {
    url,
    type = 'hls',
    width = 0.4,
    aspectRatio = 16 / 9,
    title,
    autoplay = true,
    muted = true,
    frameIntervalMs = 300,
  } = opts;

  // Build marker — confirms in the (in-headset) debug console exactly
  // which video pipeline is live, so "did it even update?" is never a
  // guess. Bump the tag when the video path changes materially.
  console.info(
    `[video] build P20 · type=${type} · canvas-blit ` +
    `(HLS cap ${HLS_MAX_HEIGHT}p, blit ${Math.round(1000 / BLIT_INTERVAL_MS)}Hz, opaque fixed 640×360)`,
  );

  const height = width / aspectRatio;
  const group = new THREE.Group();
  group.name = 'video-panel';

  // Video element (hidden, used as texture source)
  const video = document.createElement('video');
  video.crossOrigin = 'anonymous';
  video.playsInline = true;
  video.muted = muted;
  video.loop = true;
  video.style.display = 'none';
  document.body.appendChild(video);

  let hlsInstance: any = null;
  let playing = false;

  // MJPEG uses <img> directly as a texture source (no canvas — avoids tainted-canvas WebGL issue)
  let mjpegImg: HTMLImageElement | null = null;
  let imgTexture: THREE.Texture | null = null;

  // Frames mode: poll a single-JPEG endpoint periodically
  let framesTimer: number | null = null;

  function startFramesPolling() {
    if (!mjpegImg) return;
    const tick = () => {
      if (!mjpegImg) return;
      // Cache-buster so the browser actually re-fetches
      mjpegImg.src = `${url}${url.includes('?') ? '&' : '?'}t=${Date.now()}`;
    };
    tick();
    framesTimer = window.setInterval(tick, frameIntervalMs);
  }

  // Initialize based on type
  if (type === 'hls') {
    initHLS(video, url).then(() => { if (autoplay) doPlay(); });
  } else if (type === 'mp4') {
    // Native progressive playback — no hls.js, no MSE. The browser's
    // built-in decoder feeds the <video>, which THREE.VideoTexture can
    // upload to GL on every platform including Spectacles.
    video.src = url;
    video.load();
    video.addEventListener('loadeddata', () => {
      updateStatus('▶ playing', TEXT.accent);
    }, { once: true });
    video.addEventListener('error', () => {
      updateStatus('video error', TEXT.error);
    }, { once: true });
    if (autoplay) doPlay();
  } else if (type === 'mjpeg' || type === 'frames') {
    mjpegImg = document.createElement('img');
    mjpegImg.crossOrigin = 'anonymous';
    mjpegImg.style.display = 'none';
    document.body.appendChild(mjpegImg);
    mjpegImg.onload = () => {
      playing = true;
      updateStatus(type === 'frames' ? '▶ polling' : '▶ streaming', TEXT.accent);
    };
    mjpegImg.onerror = () => {
      updateStatus('camera offline', TEXT.error);
    };
    if (type === 'frames') {
      startFramesPolling();
    } else {
      mjpegImg.src = url;
    }
  }

  // ─── Texture strategy ──────────────────────────────────────────────
  //
  // For the video (HLS/MP4) path we deliberately DON'T use
  // THREE.VideoTexture. On Snap Spectacles `texImage2D(HTMLVideoElement)`
  // returns blank — the video decodes (audio plays) but the frame never
  // reaches the GL texture, so the panel stays black. Proven: the same
  // clip plays fine in a normal on-screen <video> on Spectacles, just
  // not as a WebGL texture; no source/codec/HLS-cap change moved it.
  //
  // Workaround: blit the <video> into a 2D <canvas> each frame and
  // upload the *canvas* (texImage2D(HTMLCanvasElement) works on
  // Spectacles where the video variant doesn't). Done universally —
  // it also works on Quest/desktop and removes a fragile pre-XR
  // platform check; the per-frame drawImage of one panel is negligible.
  // The blit is driven from mesh.onBeforeRender so it ticks even in the
  // manifest pipeline (which never calls the cell's tick()) — that
  // missing tick() was itself why VideoTexture.needsUpdate was never
  // being set on the UC4 path.
  let blitCanvas: HTMLCanvasElement | null = null;
  let blitCtx: CanvasRenderingContext2D | null = null;
  let canvasTexture: THREE.CanvasTexture | null = null;

  if (type !== 'mjpeg' && type !== 'frames') {
    blitCanvas = document.createElement('canvas');
    // FIXED size, matched to the 360p/16:9 cap (640×360 ≈ 1.7777). Never
    // resized at runtime: assigning canvas.width/height resets the
    // backing store to transparent black, which — with the 2D context's
    // default alpha:true — uploaded as the black glitchy flicker seen on
    // both devices. A constant-size, opaque canvas with the video
    // scaled into it removes that entirely. Smaller canvas = fewer
    // pixels per texImage2D, the dominant Spectacles cost.
    blitCanvas.width = 640;
    blitCanvas.height = 360;
    // alpha:false → fully opaque canvas (no transparent pixels to
    // composite as black). desynchronized:true lets the browser skip a
    // present-sync on each draw, cheaper for a video blit.
    blitCtx = blitCanvas.getContext('2d', { alpha: false, desynchronized: true })
      ?? blitCanvas.getContext('2d');
    if (blitCtx) {
      blitCtx.fillStyle = '#000';
      blitCtx.fillRect(0, 0, blitCanvas.width, blitCanvas.height);
    }
    canvasTexture = new THREE.CanvasTexture(blitCanvas);
    canvasTexture.minFilter = THREE.LinearFilter;
    canvasTexture.magFilter = THREE.LinearFilter;
    canvasTexture.generateMipmaps = false;
    canvasTexture.colorSpace = THREE.SRGBColorSpace;
  }
  if ((type === 'mjpeg' || type === 'frames') && mjpegImg) {
    imgTexture = new THREE.Texture(mjpegImg);
    imgTexture.minFilter = THREE.LinearFilter;
    imgTexture.magFilter = THREE.LinearFilter;
    imgTexture.colorSpace = THREE.SRGBColorSpace;
  }

  // Panel mesh
  const panelGeo = new THREE.PlaneGeometry(width, height);
  const panelMat = new THREE.MeshBasicMaterial({
    map: ((type === 'mjpeg' || type === 'frames') ? imgTexture : canvasTexture) ?? undefined,
    side: THREE.FrontSide,
  });
  const mesh = new THREE.Mesh(panelGeo, panelMat);
  group.add(mesh);

  // Per-frame video→canvas blit. Fires whenever the panel is rendered
  // (covers both the manifest pipeline and the demo gallery, neither of
  // which reliably calls tick()). Cheap early-outs keep it free when
  // there's no new frame.
  if (canvasTexture) {
    let lastBlitMs = 0;
    mesh.onBeforeRender = () => {
      if (!blitCtx || !blitCanvas || !canvasTexture) return;
      if (video.readyState < video.HAVE_CURRENT_DATA) return;
      if (video.videoWidth === 0 || video.videoHeight === 0) return;
      // Throttle to BLIT_INTERVAL_MS — the expensive drawImage +
      // texImage2D shouldn't run faster than the video's frame rate.
      const now = performance.now();
      if (now - lastBlitMs < BLIT_INTERVAL_MS) return;
      lastBlitMs = now;
      // Scale the source frame into the FIXED canvas (no resize → no
      // black reset). 640×360 matches the panel's 16:9 so there's no
      // visible distortion.
      blitCtx.drawImage(video, 0, 0, blitCanvas.width, blitCanvas.height);
      canvasTexture.needsUpdate = true;
    };
  }

  // Border frame
  const borderGeo = new THREE.EdgesGeometry(new THREE.BoxGeometry(width + 0.006, height + 0.006, 0.002));
  const border = new THREE.LineSegments(
    borderGeo,
    new THREE.LineBasicMaterial({ color: EDGE.link, transparent: true, opacity: 0.7 }),
  );
  border.renderOrder = 995;
  group.add(border);

  // Title label
  if (title) {
    const label = new Text();
    label.text = title;
    label.fontSize = 0.016;
    label.color = TEXT.primary;
    label.anchorX = 'center';
    label.anchorY = 'bottom';
    label.position.set(0, height / 2 + 0.012, 0.005);
    label.sync();
    group.add(label);
  }

  // Status badge (shows loading / playing / error)
  const statusText = new Text();
  statusText.text = 'loading...';
  statusText.fontSize = 0.01;
  statusText.color = TEXT.warn;
  statusText.anchorX = 'center';
  statusText.anchorY = 'top';
  statusText.position.set(0, -height / 2 - 0.008, 0.005);
  statusText.sync();
  group.add(statusText);

  function updateStatus(text: string, color: number) {
    statusText.text = text;
    statusText.color = color;
    statusText.sync();
  }

  function doPlay() {
    const p = video.play();
    if (p) {
      p.then(() => {
        playing = true;
        updateStatus('▶ playing', TEXT.accent);
      }).catch((e: Error) => {
        updateStatus('tap to play', TEXT.warn);
        // Retry on next user gesture
        const retryPlay = () => {
          video.play().then(() => {
            playing = true;
            updateStatus('▶ playing', TEXT.accent);
          }).catch(() => {});
          document.removeEventListener('pointerdown', retryPlay);
        };
        document.addEventListener('pointerdown', retryPlay, { once: true });
      });
    }
  }

  return {
    group,
    mesh,
    tick() {
      // Video frames now update via mesh.onBeforeRender (the canvas
      // blit) so this only needs to poke the mjpeg/frames <img> path.
      if ((type === 'mjpeg' || type === 'frames') && mjpegImg && imgTexture) {
        if (mjpegImg.complete && mjpegImg.naturalWidth > 0) {
          imgTexture.needsUpdate = true;
        }
      }
    },
    play() { doPlay(); },
    pause() {
      // The 'frames' branch needs to clear its own polling timer — without
      // this, hiding the demo gallery (e.g. on UC4 entry via `?manifest=`)
      // leaves the cell quietly hammering `/camera/capture?t=…` once per
      // second and spamming the console with 500s when no ESP32-CAM is
      // on the LAN. mjpeg streams stop themselves when src is cleared.
      if (type === 'frames') {
        if (framesTimer !== null) { clearInterval(framesTimer); framesTimer = null; }
        if (mjpegImg) mjpegImg.src = '';
        playing = false;
      } else if (type === 'mjpeg' && mjpegImg) {
        mjpegImg.src = ''; playing = false;
      } else {
        video.pause(); playing = false;
      }
      updateStatus('⏸ paused', TEXT.muted);
    },
    isPlaying() { return playing; },
    dispose() {
      if (framesTimer !== null) { clearInterval(framesTimer); framesTimer = null; }
      video.pause();
      video.src = '';
      if (hlsInstance) { hlsInstance.destroy(); hlsInstance = null; }
      if (video.parentElement) document.body.removeChild(video);
      if (mjpegImg?.parentElement) document.body.removeChild(mjpegImg);
      mesh.onBeforeRender = () => {};
      canvasTexture?.dispose();
      imgTexture?.dispose();
      panelGeo.dispose();
      panelMat.dispose();
      borderGeo.dispose();
    },
  };
}

/**
 * Highest rendition height we let hls.js select. Progression on
 * Spectacles: 720p → ~3 fps + stall; 480p → steady but only ~6 fps.
 * The per-frame canvas drawImage + texImage2D scales with pixel count
 * and is the dominant cost at this point, so step down again: 360p
 * (640×360) is ~56% of 480p's pixels and ~25% of 720p's. Still
 * acceptable on a ~0.6 m panel; goal is 8–10+ fps.
 */
const HLS_MAX_HEIGHT = 360;

/**
 * Cap texture uploads to ~15 Hz regardless of render rate. The clip is
 * 24 fps film; uploading the canvas at the headset's 72–90 Hz render
 * rate was doing 4–6× redundant `texImage2D`s of a big image — the
 * dominant cost in the fps collapse. 15 Hz looks smooth for video and
 * cuts the upload work ~5×.
 */
const BLIT_INTERVAL_MS = 1000 / 15;

async function initHLS(video: HTMLVideoElement, url: string): Promise<void> {
  // Dynamic import so hls.js isn't in the critical path
  const Hls = (await import('hls.js')).default;
  if (Hls.isSupported()) {
    const hls = new Hls({
      enableWorker: true,
      lowLatencyMode: false,
      // Start at the lowest rendition so playback begins immediately with
      // a guaranteed-decodable segment, then ABR climbs (but stays capped
      // — see autoLevelCapping below).
      startLevel: 0,
      capLevelToPlayerSize: false,   // we render to a WebGL texture, no DOM box
    });
    hls.loadSource(url);
    hls.attachMedia(video);
    hls.on(Hls.Events.MANIFEST_PARSED, () => {
      // Cap the ABR ladder at the highest level ≤ HLS_MAX_HEIGHT so the
      // player never switches up into a rendition Spectacles can't decode.
      const levels: Array<{ height: number }> = hls.levels ?? [];
      let cap = -1;
      let capH = -1;
      levels.forEach((lvl, i) => {
        if (lvl.height <= HLS_MAX_HEIGHT && lvl.height > capH) { cap = i; capH = lvl.height; }
      });
      if (cap < 0 && levels.length > 0) cap = 0;   // all too tall → lowest
      if (cap >= 0) hls.autoLevelCapping = cap;
      console.log(
        `[video] HLS manifest parsed — ${levels.length} levels, ` +
        `capped at level ${cap} (${capH > 0 ? capH : '?'}p)`,
      );
    });
    hls.on(Hls.Events.ERROR, (_: any, data: any) => {
      if (data.fatal) console.error('[video] HLS fatal error:', data.type, data.details);
    });
    (video as any).__hlsInstance = hls;
  } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
    // Safari native HLS
    video.src = url;
  } else {
    console.error('[video] HLS not supported in this browser');
  }
}
