# Zeroconf discovery over a tunnel, and a tunnel-free local-HTTPS alternative

Status: Draft · Captured: 2026-07-03 · Scope: `prototype/d3-spatial/`

Two related networking problems this doc addresses:

1. **Why mDNS/zeroconf sensor discovery misbehaves when the headset reaches the app through a cloudflared tunnel**, and a concrete approach to fix it.
2. **A tunnel-free way to serve the app over browser-trusted HTTPS** using a local DNS record plus a globally-issued Let's Encrypt certificate — so a Quest 3 gets a secure context (required for WebXR) while talking to LAN sensors *directly*, with no tunnel latency, header mangling, or MJPEG buffering.

The two are a pair: fixing discovery makes late-arriving sensors show up without a restart; going tunnel-free removes the transport that breaks the ESP devices in the first place.

---

## Part 1 — Zeroconf discovery over a tunnel

### 1.1 How discovery works today

Discovery is **server-side only**. It runs on the dev Mac, not in the headset browser.

- `tools/discover-magnet-devices.mjs` wraps macOS `dns-sd`, browses `_magnet-node._tcp`, `_magnet-imu._tcp`, and (filtered) `_http._tcp`, parses SRV+TXT, and resolves each `magnet-*.local` host to an IPv4 address. macOS-only; `--json`, `--timeout` (default 3 s).
- `vite.config.ts` → `discoverHostsSync()` runs that script **once, synchronously, at config-module load** (`execSync`, 8 s hard cap), and maps each device onto a `*_HOST` slot by its TXT `role`/`caps` (`CAMERA_HOST`, `IMU_HOST`, `VITALS_HOST`, `ENV_HOST`, `LIGHTING_HOST`, `BOOMBOX_HOST`).
- `pickHost(envKey, fallback)` resolves each proxy target with precedence **env var → mDNS discovery → hardcoded `magnet-*.local` fallback**, logging the chosen source at startup.
- `server.proxy` maps `/api/v1/...` URL prefixes to those targets globally (most-specific prefix first), each device behind a dedicated `http.Agent` (`family:4, keepAlive:false, maxSockets:1`) that strips noisy/`cf-*`/`x-forwarded-*` headers.

The headset never does discovery. It hits `/api/v1/...` on the origin it loaded from; Vite's proxy bridges those requests to whatever LAN device `pickHost` picked at boot.

### 1.2 Why it "doesn't work" over a tunnel

The tunnel doesn't route mDNS — but discovery is server-side, so that alone isn't the failure. The real causes, in order of how often they bite:

1. **Discovery is one-shot at Vite boot.** `discoverHostsSync()` runs when `vite.config.ts` is first evaluated. The usual tunnel-demo order is *start dev server → start tunnel → power on the ESP boards*. Any board that boots after Vite is never seen; its proxy target stays on the `.local` fallback for the life of the process. There is no periodic rescan and no way to trigger one short of restarting Vite.

2. **The tunnel itself breaks the ESP hop even when the device *was* discovered.** cloudflared injects `Cf-*` / `X-Forwarded-*` headers that overflow the ESP-IDF httpd header buffer (→ `431`, see `CONFIG_HTTPD_MAX_REQ_HDR_LEN=2048` fix in `STATUS.md`), buffers `multipart/x-mixed-replace` MJPEG (camera stalls — see `CAMERA_SETUP.md`), and adds enough latency that overlapping polls trip `httpd_sock_err send:11` on the small chips. So a correctly-discovered device still returns errors through the tunnel.

3. **`.local` fallbacks depend on the Mac's own resolver, which macOS privacy layers silently break.** When discovery misses a device, the target is `http://magnet-vitals.local`. That name is resolved by the Mac's mDNSResponder — fine on-LAN, but Local Network permission, iCloud Private Relay, and Private Wi-Fi MAC randomisation can make the Mac itself unable to reach the device (see `CAMERA_SETUP.md` → "macOS Local Network permission"). The tunnel is often the *only* thing the operator changed, so the failure gets misattributed to the tunnel.

4. **mDNS can never be pushed to the client.** Do not attempt client-side zeroconf as a "fix": mDNS is link-local multicast (`224.0.0.251:5353`) and does not cross a tunnel or route off-LAN. Discovery must stay server-side. The remote headset can only ever reach sensors through the dev-server proxy.

5. **No inventory surface.** Nothing tells the client which devices were actually found. It blindly polls fixed prefixes, so "0 sensors discovered" and "sensor discovered but unreachable" look identical from the headset — both just fall to placeholder/fake data with no signal about which one happened.

### 1.3 Approach to fix it

Goal: late-arriving sensors appear without a Vite restart, and the client can *see* the live inventory. Four changes, smallest-blast-radius first:

1. **Make discovery a live, refreshing background service instead of a boot-time `execSync`.**
   Replace the one-shot `discoverHostsSync()` with a small Vite plugin that (a) runs the first scan asynchronously so it never blocks server start, and (b) re-runs on an interval (30–60 s) and on demand. Keep the results in an in-memory table keyed by role, with `{ role, ip, caps, firstSeen, lastSeen }`. Preserve the existing precedence: an explicit `*_HOST` env var always pins and suppresses rescan for that slot (deterministic demos), mDNS fills the rest.

2. **Resolve proxy targets dynamically, not at boot.**
   Vite's static `server.proxy` map freezes targets at startup, which is the root of the "late device never routes" problem. Swap the per-device entries for a `router` function (http-proxy-middleware supports `router: (req) => liveTable[slotFor(req.url)] ?? fallback`) or a thin Connect middleware that re-reads the live table per request. Now a board that boots five minutes into a session starts routing correctly on its next poll — no restart.

3. **Expose the inventory to the client: `GET /api/v1/_discovery`.**
   Serve the live table (role, ip, caps, lastSeen, and a cheap reachability flag) from the dev server. The headset HUD polls it so it can distinguish "no sensor of this role" from "sensor present but unreachable," show a real device list, and stop showing the DEMO-MODE badge the instant a real device appears. This is the client-visible half of the fix and pairs with the existing `healthMonitor`/offline-sensors HUD.

4. **Health-probe before trusting a target.**
   Before a slot is reported reachable (and before the client wastes a poll on it), do a cheap `HEAD`/short-timeout `GET` against the resolved target. A `.local` name that no longer resolves or a powered-down board is marked unreachable, so the mark degrades to synthesized/placeholder data immediately instead of hanging on a dead socket. Feeds the reachability flag in #3.

None of this changes the on-device firmware or the manifest format; it's all in `vite.config.ts` + a new tiny plugin + one JSON endpoint. Env-var pinning remains the escape hatch when you want zero discovery magic during a demo (`MAGNET_DISCOVERY=0` still disables scanning entirely).

> Note: `STATUS.md` already tracks "Live service discovery via mDNS" as a deferred capability citing `specs/device-self-registration.md`. Part 1 here is the **dev-proxy** version of that (host resolves devices for the client); the spec's version is the **device** self-registering with an engine. They compose — the proxy table can be seeded from either source.

---

## Part 2 — Tunnel-free HTTPS via local DNS + Let's Encrypt

### 2.1 Why we reach for a tunnel at all

WebXR requires a **secure context**: `https://` or `localhost`. On a Quest 3, `http://192.168.1.50:5173` gives you a page but **no immersive session, no passthrough, no XR at all**. cloudflared is used purely because it hands you a browser-trusted `https://<random>.trycloudflare.com` for free. Everything painful about the tunnel (§1.2.2) is collateral damage from needing that one thing: a trusted HTTPS origin.

So the tunnel-free question is really: **how do we serve a browser-trusted HTTPS origin that resolves to a LAN IP, without installing a custom CA on the headset?**

### 2.2 Why not mkcert / a self-signed local CA

`mkcert` (or any private CA) works great on laptops but requires installing its **root CA into every client's trust store**. On a Quest 3 that is awkward-to-impossible (locked-down browser trust store, no easy user cert import, MDM territory), and it's per-device toil for phones too. That per-device CA install is exactly the "enterprise root-CA" pain called out as a limitation in `PROPOSAL.md`. Using a **publicly-trusted** Let's Encrypt cert sidesteps it — the Quest already trusts Let's Encrypt out of the box, so there is nothing to install on the headset.

### 2.3 The approach: DNS-01 wildcard cert on a name that points at the LAN

The trick that makes this work is the **DNS-01 ACME challenge**. Unlike HTTP-01, DNS-01 proves domain control by publishing a `_acme-challenge` **TXT record** — it never requires Let's Encrypt to connect to your server. That means:

> You can obtain a valid, publicly-trusted certificate for a hostname whose A record points at a **private `192.168.x.x` address** and that is never reachable from the public internet.

Pieces:

1. **A domain you control**, e.g. `xrot.example.com`. Use a DNS provider with an API (Cloudflare, Route 53, deSEC — deSEC is free and DNS-01-friendly). You only need one delegated zone.

2. **A wildcard cert** `*.lan.xrot.example.com` from Let's Encrypt via **DNS-01**. Wildcard = you never re-issue when you add a device or a new front-end name. Certs are 90-day; auto-renew at ~60.

3. **A name → LAN-IP mapping.** Two ways, pick per how private you want to be:

   - **(a) Public A record → private IP (simplest).** `dev.lan.xrot.example.com IN A 192.168.1.50` in public DNS. Browsers and Let's Encrypt do not care that it's an RFC-1918 address. Any client on that LAN resolves the name normally and connects directly. Zero local-DNS infrastructure. Trade-off: it publicly discloses your internal IP, and the name only works while the client is actually on that LAN.

   - **(b) Split-horizon / local DNS (private).** Run a local resolver — dnsmasq, Pi-hole, Unbound, or your router/UniFi "local DNS record" feature — that answers `*.lan.xrot.example.com → 192.168.1.50` for clients on the LAN, while public authoritative DNS holds only the `_acme-challenge` TXT (or nothing). Clients must use that resolver (hand it out via DHCP). Keeps internal topology private and lets the same name resolve differently inside vs. outside. dnsmasq one-liner: `address=/lan.xrot.example.com/192.168.1.50`.

4. **Terminate TLS locally with the real cert.** Put a small reverse proxy in front of the existing Vite dev server. **Caddy** is the least-effort option: it performs the DNS-01 ACME itself (with a DNS-provider plugin), auto-renews, and reverse-proxies to Vite. Everything the app already does — the `/api/v1/...` device proxying, header stripping, mDNS discovery from Part 1 — stays exactly as-is behind it; Caddy only adds real TLS at the edge.

   ```
   # Caddyfile — Caddy gets + renews the LE cert via DNS-01, proxies to Vite
   dev.lan.xrot.example.com {
       tls {
           dns cloudflare {env.CF_API_TOKEN}     # DNS-01; no inbound 80/443 needed
       }
       reverse_proxy localhost:5173               # the existing Vite dev server
   }
   ```

   acme.sh + `vite --https` (feed it the issued cert/key) is the no-Caddy equivalent if you'd rather not add a proxy — but then renewal is your cron job, whereas Caddy handles it.

### 2.4 What the headset experiences

The Quest 3 opens `https://dev.lan.xrot.example.com`:

- The name resolves (public A record or local resolver) to `192.168.1.50` — **on the LAN**.
- The cert is a genuine Let's Encrypt cert the Quest already trusts → **secure context → full WebXR**, with **nothing installed on the headset**.
- Traffic goes **straight over the LAN**: no tunnel hop, no added latency, no `Cf-*` header overflow, no MJPEG buffering, POSTs (join, actuators) complete promptly. The join-latency and camera-buffering classes of bug simply don't apply.
- The ESP devices stay plain HTTP on the LAN. There's **no mixed-content problem** because the browser only ever talks HTTPS to Caddy/Vite; the HTTP hop to the ESP is server-side (same model as today), invisible to the page.

### 2.5 Platform compatibility (Quest 3 / Android XR / Snap Spectacles)

The "nothing installed on the headset" guarantee is **platform-agnostic**: it holds on any XR browser that uses its OS's **system trust store**, because the Let's Encrypt roots (ISRG Root X1/X2) ship in every modern trust store. You are not relying on Quest-specific behaviour — you're relying on the same public-CA trust that already makes `*.trycloudflare.com` work today.

| Platform | Cert trust (no install) | WebXR secure context | LAN-direct networking | Verdict |
|---|---|---|---|---|
| **Quest 3** | ✅ Android system store incl. LE | ✅ `immersive-ar/vr` | ✅ normal Android networking | Works as described. |
| **Android XR glasses** | ✅ Chromium + Android system store incl. LE | ✅ full WebXR | ✅ normal Android networking | Same as Quest — high confidence. (App already gates on Android XR.) |
| **Snap Spectacles** | ✅ *expected* (system/public store) — **verify on first connect** | ✅ WebXR (with the renderer quirks below) | ⚠️ **verify** — quirkier net stack; may not honour a locally-served resolver | Should work; two on-device checks below. |

**Android XR glasses** — treat exactly like Quest. Chromium-based browser, Android system CA store (LE included), full WebXR under the standard HTTPS secure-context rule, ordinary Android networking so the name resolves and the LAN IP routes like any Android device.

**Snap Spectacles** — two things to confirm on-device, **neither about cert install**:

1. **Trust store** — Spectacles' browser almost certainly uses a system/public trust store, so a public LE cert is trusted with nothing to import. This isn't personally verified, but there's no architectural reason a public LE leaf would be rejected where a public-CA `trycloudflare.com` cert is accepted today. Confirm on first connect.
2. **LAN reachability + DNS** — the likelier friction point, and unrelated to certs. Spectacles have a quirkier network stack, and **split-horizon DNS depends on the device honouring the DHCP-provided local resolver**. If Spectacles ignore it, the name won't resolve to the LAN IP.

> **Recommendation for XR glasses (Spectacles + Android XR):** prefer the **public A-record → private IP** option (§2.3a) over split-horizon DNS. It resolves through normal public DNS, so it does not depend on the headset honouring a locally-served resolver — it only needs the device on the same LAN for the private IP to route. That removes the one real unknown for Spectacles. (Trade-off unchanged: it discloses the internal IP publicly.)

This transport choice is orthogonal to the known Spectacles **rendering** quirks (framebuffer scale factor / `antialias:false` blanking the scene, head-relative XR reference space, narrow FOV) — those behave the same whether you arrive via tunnel or LAN-direct.

### 2.6 Wiring it into this project

Minimal, additive changes:

- Add the new hostname to `server.allowedHosts` in `vite.config.ts` (alongside the existing `.trycloudflare.com` / `.ngrok*` entries).
- Keep the ESP header-stripping proxy hooks — still cheap insurance even without a tunnel.
- Keep env-var / mDNS discovery for the device hop unchanged; Part 1's live discovery works identically behind Caddy.
- If you use split-horizon DNS, make sure the demo network's DHCP hands out the local resolver, or the headset falls back to public DNS and (with option-b) fails to resolve the name.

**Scripted setup (Nix):** [`../tools/nix-https/`](../tools/nix-https/) is a local flake that pins a Caddy build *with* the Cloudflare DNS-01 plugin and exposes `nix run .#serve` (Caddy edge → Vite, auto-issue + renew), `.#cert` (PEMs only, for `vite --https`), and `.#dns` (optional split-horizon dnsmasq). No NixOS/nix-darwin required. See its README for the one-time vendor-hash step and env vars.

### 2.7 When to still use the tunnel

The Let's Encrypt + local-DNS path requires the headset to be **on the same LAN** as the sensors and dev machine. If you need to demo to someone off-site (client is not on your network), a tunnel — or a real public deployment — is still the answer. For on-prem demos, conference booths, and daily dev on Quest 3, tunnel-free is faster and far more reliable.

### 2.8 Option comparison

| Approach | WebXR secure context | Client CA install | Latency to sensors | Works off-LAN | Setup cost |
|---|---|---|---|---|---|
| cloudflared tunnel | ✅ trusted | none | high (tunnel hop) + header/MJPEG breakage | ✅ | trivial, but fragile |
| mkcert / private CA | ✅ *after* install | **required per device** (hard on Quest) | LAN-direct | ❌ | low on laptops, painful on headsets |
| Public A → private IP + DNS-01 cert | ✅ trusted | none | **LAN-direct** | ❌ | moderate (domain + ACME) |
| Split-horizon DNS + DNS-01 cert | ✅ trusted | none | **LAN-direct** | ❌ | moderate + local resolver |

**Recommended for on-LAN Quest 3 dev/demo:** DNS-01 wildcard cert served by Caddy, names pointed at the LAN via split-horizon DNS (or a public A record if internal-IP disclosure is acceptable). It gives real HTTPS with nothing to install on the headset and removes the entire class of tunnel-induced ESP failures.

**For XR glasses (Snap Spectacles / Android XR):** same DNS-01 cert, but prefer the **public A-record → private IP** mapping over split-horizon — it doesn't depend on the headset honouring a locally-served resolver (see §2.5).

---

## References

- `CAMERA_SETUP.md` — cloudflared MJPEG buffering, `--protocol http2`, macOS Local Network permission.
- `STATUS.md` — `431` header-length fix, deferred "Live service discovery via mDNS," allowed tunnel hosts.
- `DEVELOPER_GUIDE.md` / `XR_UX_BEST_PRACTICES.md` — env→mDNS→`.local` precedence, `MAGNET_DISCOVERY=0` kill-switch, macOS LAN gotchas.
- `PROPOSAL.md` — HTTPS/WSS with self-obtained SSL cert (R-level); root-CA-install as an enterprise limitation this approach avoids.
- `tools/discover-magnet-devices.mjs`, `vite.config.ts` (`discoverHostsSync`, `pickHost`, `server.proxy`) — the current discovery + proxy implementation.
- `tools/nix-https/` — Nix flake that scripts the Caddy + DNS-01 setup (`nix run .#serve` / `.#cert` / `.#dns`).
