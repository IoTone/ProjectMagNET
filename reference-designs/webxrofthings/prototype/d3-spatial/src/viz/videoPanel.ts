import * as THREE from 'three';
import { Text } from 'troika-three-text';
import { TEXT, EDGE } from '../ui/palette';

export interface VideoPanelOptions {
  url: string;
  type?: 'hls' | 'mjpeg' | 'frames';
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

  // Texture: VideoTexture for HLS, plain Texture from img for MJPEG/frames
  const videoTexture = (type !== 'mjpeg' && type !== 'frames')
    ? new THREE.VideoTexture(video)
    : null;
  if (videoTexture) {
    videoTexture.minFilter = THREE.LinearFilter;
    videoTexture.magFilter = THREE.LinearFilter;
    videoTexture.colorSpace = THREE.SRGBColorSpace;
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
    map: ((type === 'mjpeg' || type === 'frames') ? imgTexture : videoTexture) ?? undefined,
    side: THREE.FrontSide,
  });
  const mesh = new THREE.Mesh(panelGeo, panelMat);
  group.add(mesh);

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
      if ((type === 'mjpeg' || type === 'frames') && mjpegImg && imgTexture) {
        if (mjpegImg.complete && mjpegImg.naturalWidth > 0) {
          imgTexture.needsUpdate = true;
        }
      } else if (playing && video.readyState >= video.HAVE_CURRENT_DATA && videoTexture) {
        videoTexture.needsUpdate = true;
      }
    },
    play() { doPlay(); },
    pause() {
      if (type === 'mjpeg' && mjpegImg) { mjpegImg.src = ''; playing = false; }
      else { video.pause(); playing = false; }
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
      videoTexture?.dispose();
      imgTexture?.dispose();
      panelGeo.dispose();
      panelMat.dispose();
      borderGeo.dispose();
    },
  };
}

async function initHLS(video: HTMLVideoElement, url: string): Promise<void> {
  // Dynamic import so hls.js isn't in the critical path
  const Hls = (await import('hls.js')).default;
  if (Hls.isSupported()) {
    const hls = new Hls({ enableWorker: true, lowLatencyMode: false });
    hls.loadSource(url);
    hls.attachMedia(video);
    hls.on(Hls.Events.MANIFEST_PARSED, () => {
      console.log('[video] HLS manifest parsed, ready to play');
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
