# UC3 — XRt Exhibit (`uc3-poster.json`)

Curated mini-exhibit dataspace built around generative-art / data-art marks. Originally drafted as a conference-poster placeholder (per `webxrofthings/PROPOSAL.md §UC3`); rebranded to a small "XRt Exhibit" so DEMO03 has real content instead of stubbed placeholders while a full poster-session toolkit is still being scoped.

Join code: `DEMO03` (resolved by `mock-join-server`). Direct load: `?manifest=/examples/uc3-poster.json`.

---

## What you see

| Mark                | Type                | Position                                            | Hoverable |
|---------------------|---------------------|-----------------------------------------------------|-----------|
| `exhibit-stippling` | `voronoi-stippling` | Grid (centre)                                       | No        |
| `exhibit-moons`     | `moon-phases-arc`   | 300° arc, radius 2.6 m, height 1.6 m                | No        |
| `exhibit-force-tree`| `force-tree-3d`     | Self-positioned, 0.80 m above grid centre           | Yes (grab)|
| `exhibit-owls`      | `owls-to-the-max`   | Self-positioned ceiling tile, world Y 3.2 m         | No        |

Three of the four marks are self-positioned (they author their own world placement and bypass the cell grid). The stippling is grid-placed because it reads as a panel that wants the standard cell chrome.

## Source assets

Both live under `public/spatial/` (gitignored — copy them in before running).

| File                                  | Used by              | Notes                                          |
|---------------------------------------|----------------------|------------------------------------------------|
| `_DSC7796.jpeg`                       | `exhibit-stippling`  | Source image. Stippled on the front face; the raw photo shows on the back when `mirrorBack: true`. |
| `268667__depwl9992__owls.mp3`         | `exhibit-owls`       | Field recording of multiple owls, ~30 s. Random 1.0–2.5 s chops are played at random spatial positions. |

Both are fetched relative to the site root (`/spatial/...`) so the same paths work under `npm run dev` and behind a tunnel.

## Mark notes

### Voronoi stippling

After Bostock (https://observablehq.com/@mbostock/voronoi-stippling). Weighted Lloyd-relaxation against the source image's luminance grid.

- `invert: true` (default) means *light* pixels weight more — for a portrait, the face renders dense white on the dark panel, which reads better with white stipples than the canonical ink-on-paper interpretation.
- `maxGridDim: 256` downsamples the per-iteration pixel walk. Without this a 1920 × 1280 source pegged the main thread at 8 FPS for ~10 s while the pattern converged. Visual output at 256 is indistinguishable from native res because the stipple count (thousands) is orders of magnitude smaller than the grid (~65k pixels) either way.
- `mirrorBack: true` adds a back-facing plane at `z = -0.003`, rotated π around Y with `scale.x = -1` so the rotation-induced texture flip cancels and the back image reads correctly oriented from behind. Stippled art on one side, photograph on the other — meant to be walked around.

### Moon-phases arc

After Bostock (https://observablehq.com/@mbostock/phases-of-the-moon). 29 procedural moons wrapping 300° around the user at radius 2.6 m.

- Custom shader: each moon is a SphereGeometry; the fragment shader rotates a sun-direction vector around the moon's local Y axis by `uPhase` and runs a smoothstep terminator. A faint rim glow on the dark limb keeps the silhouette readable against a dark scene.
- Self-positioned: 300° (not 360°) leaves a small "stage exit" gap directly behind the user.

### Force-directed tree

After Bostock's `@d3/force-directed-tree`. 1 root + 5 clusters + 25 leaves, organic cluster blob (no depth-Y bias).

- Same shape as the gallery's `force.ts` (`nodeMesh`, `nodes`, `tick`, `pinNode`, `unpinNode`, `reheat`). The manifest pipeline's nodeMesh registration wires `onDragStart`/`onDragMove`/`onDragEnd` automatically for any viz that exposes `pinNode` + `unpinNode`, so controller-pinch and hand-tracking both grab nodes the same way the gallery force-graph does.
- Self-ticks via `nodeMesh.onBeforeRender`, alpha-gated — a settled tree costs nothing per frame; a grabbed tree reheats and the rest of the tree follows the spring forces.
- The geometry is baked at `radius` directly (not unit-sphere + per-instance scale) so the InstancedMesh first-pass bounding-sphere cull stays tight. `frustumCulled = false` + an inflated boundingSphere defang the cull entirely so raycasts always reach per-instance hit-tests.

### Owls-to-the-max

After Bostock (https://observablehq.com/@mbostock/owls). Cartoon owl grid drawn into a transparent canvas, mapped to a horizontal plane mounted as a ceiling tile.

- Per-cell deterministic offsets (hue, blink phase, bob phase, size jitter) so each owl is recognisably its own bird without an external Random dep.
- Transparent canvas (`alpha: true` + `clearRect` + `transparent: true` material with `alphaTest: 0.02`) so the owls hover as cut-outs against the scene sky rather than as a dark rectangle.
- One-shot anchor compensation: the cell may be parented to `vizAnchor` (whose world Y varies per session, ≈ `cameraY - 0.20`). On first render the parent's world Y is subtracted from the configured `floorY` so the plane lands at the intended world height regardless of the anchor's position.
- **Spatial hoots**: the cell fetches `268667__depwl9992__owls.mp3` and decodes it once via the shared `AudioListener.context`. A pool of 4 `THREE.PositionalAudio` emitters is attached to the cell group. Every 2.0–5.0 s a random emitter is repositioned to a random azimuth at 3–5 m radius (±0.7 m height jitter), and one PositionalAudio plays a random 1.0–2.5 s chop of the recording (`audio.offset` + `audio.duration` passed through to `AudioBufferSourceNode.start`). Light playback-rate jitter (±12%) so repeated chops don't feel robotic.

## Loading this manifest

```bash
# Terminal 1: dev server
npm run dev

# Terminal 2: join server (for the DEMO03 code path)
npm run server

# Open and either type DEMO03 in the join panel, or load directly:
#   http://localhost:5173/?manifest=/examples/uc3-poster.json
```

For headset testing:

```bash
# Terminal 3: cloudflared tunnel
cloudflared tunnel --url http://localhost:5173
# Open the tunnel URL on Quest / Spectacles
```

## Per-mark interaction

- **Tree**: point-and-pinch on any node to grab. Pulling drags that node through 3D space; the rest of the tree relaxes around it via the spring forces. Release to unpin. Hover shows a halo + label.
- **Stippling, moons, owls**: visual-only, no raycast targets (`hoverable: false`).

## Implementation files

- Manifest: `examples/uc3-poster.json`
- Mark builders: `src/manifest/builders.ts` (`voronoi-stippling`, `moon-phases-arc`, `force-tree-3d`, `owls-to-the-max` entries)
- Cells:
  - `src/viz/voronoiStippling.ts`
  - `src/viz/moonPhasesArc.ts`
  - `src/viz/forceTree3d.ts`
  - `src/viz/owlsToTheMax.ts`
- Self-positioning registered in `src/manifest/renderManifest.ts` (`SELF_POSITIONED` set)
