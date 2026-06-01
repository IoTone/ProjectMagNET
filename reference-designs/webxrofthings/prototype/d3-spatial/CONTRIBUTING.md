# Contributing to d3-spatial

## Prerequisites

- **Node.js 18+** and **npm**
- A **WebXR device** (Quest 3 recommended) or a desktop browser for non-XR development
- For on-device testing: `cloudflared` (see README for tunnel setup)

## Getting started

```bash
npm install
npm run dev          # Vite dev server with HMR
npm run smoke        # Headless Playwright screenshot tests
```

## Project structure

```
src/
  chart/        Chart layout helpers (axes, scales, grid)
  viz/          Spatial mark renderers (tree, force, sankey, etc.)
  interact/     Input handling: hover, drag, brush, fingertip grab
  ui/           HUD panels, inspector card, breadcrumb, toolbar
  audio/        Spatial audio: positional ticks + ambisonic bed
  manifest/     DataspaceManifest schema and builder registry
  dataspace/    Dataspace session lifecycle and data loading
  util/         Shared math, color, and geometry helpers
  demo/         Sample datasets and the viz gallery entry point
  types/        Shared TypeScript type definitions
  main.ts       Application entry point and XR rig bootstrap
```

## How to add a new mark type

1. **Create `src/viz/mymark.ts`** implementing a `MyMarkViz` interface. Use `tree.ts` or `force.ts` as templates. Your module should export a builder function that takes a D3 data structure and returns a `THREE.Group`.

2. **Add sample data** to `src/demo/sampleHierarchy.ts` if your mark needs a new data shape.

3. **Add the mark to the gallery** in `src/demo/vizGallery.ts` so it appears in the demo scene.

4. **Register with Interact** in `src/main.ts` so hover and select events reach your mark's nodes.

5. **Register a manifest builder** in `src/manifest/builders.ts` so the mark can be instantiated from a `DataspaceManifest` JSON file.

6. **Add smoke shots** in `scripts/smoke.mjs` to capture headless screenshots of the new mark.

7. **Verify everything passes:**
   ```bash
   npm run typecheck
   npm run smoke
   ```

## How to add a UI component

- Create a new file in `src/ui/`.
- Use **three-mesh-ui** `Block` / `Text` for panel-style UI.
- Use **troika-three-text** for standalone floating labels.
- Import colors from `src/ui/palette.ts` — never hard-code color values.

## Interaction model

All interactive objects implement the **Hoverable** interface:

- The system tracks **per-hand state** (left/right controller or hand-tracking).
- **HoverContext** carries the intersected object, hand, point, and distance.
- **`onSelect`** fires on trigger press (click/pinch) while hovering.
- **`onDragStart`** / **`onDragEnd`** fire for sustained grabs (e.g. force graph node repositioning).

See `src/interact/Interact.ts` for the full dispatch loop.

## Testing

| Command | What it does |
|---------|-------------|
| `npm run typecheck` | `tsc --noEmit` — zero errors required before any PR. |
| `npm run smoke` | Headless Playwright screenshots — all shots must match. |

### On-device testing

- Use a **cloudflared tunnel** to expose your local dev server over HTTPS (see README).
- On Quest 3: open `chrome://inspect` on your desktop to attach remote devtools to the headset browser.
- At runtime, `window.__demo` exposes hooks for inspecting scene state, cycling marks, and toggling audio.

## Palette rules

- Use `TEXT.*` tokens from `palette.ts` for all text colors.
- Use `EDGE.*` tokens for connections and links.
- **Never use blue for text or edges on passthrough** — it washes out against real-world backgrounds on Quest 3.

## Code style

- **TypeScript strict mode** — the project uses `strict: true` in `tsconfig.json`.
- No lint config is enforced, but follow existing patterns in the codebase.
- Avoid unnecessary abstractions. Prefer flat, readable functions over deep class hierarchies.

## Platform notes

- **Quest 3** is the primary target. All marks and interactions must work on Quest 3.
- **Snap Spectacles** is the secondary target.
- Test on at least one HMD before submitting a PR. If you only have desktop, note it in the PR description so a reviewer can verify on-device.
