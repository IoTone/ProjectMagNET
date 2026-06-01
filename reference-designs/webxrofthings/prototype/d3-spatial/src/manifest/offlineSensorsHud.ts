/**
 * offlineSensorsHud — red-outlined "OFFLINE SENSORS" HUD card.
 *
 * Shown when one or more manifest URL marks have been flagged offline
 * by the health monitor. The box stays hidden when everything is
 * healthy so the dataspace doesn't carry chrome the user doesn't need
 * to read.
 *
 * Card chrome:
 *  - Dark fill (matches the standard panel bg), 88% opacity
 *  - Red border (LineSegments around the plane's edges)
 *  - Header line "OFFLINE SENSORS" in slightly-larger text
 *  - Body lines listing each mark id, comma-separated, wrapping
 *
 * Width auto-grows with the number of offline marks (capped) so a
 * single-mark outage stays small and a five-mark outage doesn't
 * overflow.
 *
 * Anchored to a caller-supplied position so renderManifest can park
 * it next to (rather than over) the dataspace title.
 */

import * as THREE from 'three';
import { Text } from 'troika-three-text';

const W              = 0.32;
const PADDING        = 0.012;
const HEADER_SIZE    = 0.014;
const BODY_SIZE      = 0.012;
const BORDER_COLOR   = 0xff4444;     // bright red — reads as warning even on bright passthrough
const FILL_COLOR     = 0x180a0a;     // near-black with a hint of warm red
const HEADER_COLOR   = 0xff6b6b;
const BODY_COLOR     = 0xf3d5d5;

export interface OfflineSensorsHud {
  group: THREE.Group;
  /** Update which mark ids show as offline. Empty array → card hidden. */
  setOfflineIds(ids: string[]): void;
  dispose(): void;
}

export function buildOfflineSensorsHud(): OfflineSensorsHud {
  const group = new THREE.Group();
  group.name  = 'offline-sensors-hud';
  group.visible = false;

  /* Fill plane. Height is recomputed per-update from the body text's
   * synced bbox so the card always hugs its content. */
  const fillMat = new THREE.MeshBasicMaterial({
    color: FILL_COLOR, transparent: true, opacity: 0.88, depthWrite: false,
  });
  const fillMesh = new THREE.Mesh(new THREE.PlaneGeometry(W, 0.08), fillMat);
  fillMesh.position.z = -0.0005;
  group.add(fillMesh);

  /* Red border via line segments — slightly forward so it never
   * z-fights the fill at grazing angles. */
  const borderMat = new THREE.LineBasicMaterial({ color: BORDER_COLOR, linewidth: 2 });
  const borderGeo = new THREE.EdgesGeometry(new THREE.PlaneGeometry(W, 0.08));
  const border    = new THREE.LineSegments(borderGeo, borderMat);
  group.add(border);

  /* Header. */
  const header = new Text();
  header.text       = 'OFFLINE SENSORS';
  header.fontSize   = HEADER_SIZE;
  header.color      = HEADER_COLOR;
  header.anchorX    = 'left';
  header.anchorY    = 'top';
  header.maxWidth   = W - PADDING * 2;
  header.position.set(-W / 2 + PADDING, 0.04 - PADDING, 0.001);
  header.sync();
  group.add(header);

  /* Body. troika auto-wraps within maxWidth. */
  const body = new Text();
  body.text       = '';
  body.fontSize   = BODY_SIZE;
  body.color      = BODY_COLOR;
  body.anchorX    = 'left';
  body.anchorY    = 'top';
  body.maxWidth   = W - PADDING * 2;
  body.position.set(-W / 2 + PADDING, 0.04 - PADDING - HEADER_SIZE - 0.006, 0.001);
  body.sync();
  group.add(body);

  function resizeFrame(h: number) {
    fillMesh.geometry.dispose();
    fillMesh.geometry = new THREE.PlaneGeometry(W, h);
    /* Edges geometry has to be rebuilt to match. */
    borderGeo.dispose();
    const fresh = new THREE.EdgesGeometry(new THREE.PlaneGeometry(W, h));
    border.geometry = fresh;
    /* Re-anchor header + body to the new top edge. */
    header.position.y = h / 2 - PADDING;
    body.position.y   = h / 2 - PADDING - HEADER_SIZE - 0.006;
  }

  function setOfflineIds(ids: string[]) {
    if (ids.length === 0) {
      group.visible = false;
      return;
    }
    group.visible = true;
    /* Bullet-style listing. Comma-separated would feel telegraphic;
     * "•" markers + line breaks read as a check-list of failures. */
    body.text = ids.map(id => `• ${id}`).join('\n');
    body.sync(() => {
      /* sync() callback gives us the laid-out bbox. Add chrome
       * padding + header height for the total card height. */
      const bbox      = body.geometry.boundingBox;
      const bodyH     = bbox ? Math.abs(bbox.max.y - bbox.min.y) : BODY_SIZE * ids.length * 1.25;
      const cardH     = HEADER_SIZE + bodyH + PADDING * 3 + 0.006;
      resizeFrame(Math.max(0.06, cardH));
    });
  }

  function dispose() {
    fillMat.dispose();
    fillMesh.geometry.dispose();
    borderMat.dispose();
    borderGeo.dispose();
    (border.geometry as THREE.BufferGeometry).dispose();
    (header as unknown as { dispose?: () => void }).dispose?.();
    (body   as unknown as { dispose?: () => void }).dispose?.();
  }

  return { group, setOfflineIds, dispose };
}
