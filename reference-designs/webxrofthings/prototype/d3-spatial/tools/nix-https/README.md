# nix-https — tunnel-free local HTTPS for the XrOT prototype

Status: Draft · Captured: 2026-07-06 · Scope: `prototype/d3-spatial/`

A local Nix flake that stands up **browser-trusted HTTPS on the LAN** so a Quest 3 / Android XR / Snap Spectacles gets a secure context for WebXR **with nothing installed on the headset** — no tunnel, no private CA. It's the scripted version of the approach in [`../../docs/discovery-and-tunnel-free-https.md`](../../docs/discovery-and-tunnel-free-https.md) (read that first for the *why*).

What the flake pins for you:

- **Caddy built *with* the Cloudflare DNS-01 plugin** (`caddy.withPlugins`) — otherwise an `xcaddy` build dance. Caddy does the ACME DNS-01 challenge, 60-day auto-renewal, and reverse-proxies to Vite, all in one process. DNS-01 never needs inbound 80/443, so it works behind NAT / on a private LAN IP.
- `lego` — alternative issuer for the `vite --https` path (PEMs only, no Caddy).
- `dnsmasq` — optional split-horizon resolver.

## Prerequisites

- Nix with flakes enabled (`experimental-features = nix-command flakes`). Works with plain Nix on macOS/Linux — no NixOS or nix-darwin required.
- A domain on a DNS provider with an API token. Defaults assume **Cloudflare** (`Zone:DNS:Edit` scope); swap the plugin + `dns` line for Route 53/deSEC/etc.
- The prototype running: `npm run dev` (Vite on `:5173`) + `npm run server` (mock-join-server on `:3001`), in the usual two terminals.

## First build

The vendor hash for **caddy 2.11.4 + cloudflare v0.2.4** is already pinned in `flake.nix` (resolved & built 2026-07-06 — `nix build .#caddy` succeeds and `caddy list-modules` shows `dns.providers.cloudflare`), so it builds directly, no setup step.

Only if you **bump** the plugin or Caddy version: set `hash = lib.fakeHash;`, run `nix build .#caddy`, and copy the `got: sha256-…` value from the error back into `hash`. (Tip: check the latest plugin tag first — `curl -s https://proxy.golang.org/github.com/caddy-dns/cloudflare/@latest`. `v0.1.1` does **not** exist; the current line is `v0.2.x`.)

## Usage

```bash
cd prototype/d3-spatial/tools/nix-https

# 1) Caddy at the edge → Vite. DNS-01 issue + auto-renew, no sudo (:8443).
export DOMAIN=dev.lan.xrot.example.com
export CF_API_TOKEN=<cloudflare-token>       # read at runtime; never enters the store
nix run .#serve
#   optional: VITE_PORT=5173 HTTPS_PORT=8443 ACME_EMAIL=you@example.com

# 2) Point the name at the LAN IP (pick ONE — see the doc §2.3):
#    (a) public A-record  dev.lan.xrot.example.com → 192.168.1.50   (simplest; best for XR glasses)
#    (b) split-horizon via dnsmasq (needs :53 → sudo). Build then run as root
#        (sudo often lacks nix on PATH, so build the binary first):
export DOMAIN_SUFFIX=lan.xrot.example.com LAN_IP=192.168.1.50
nix build .#dns && sudo -E ./result/bin/xrot-https-dns

# On the headset: open https://dev.lan.xrot.example.com:8443
```

Alternative — no Caddy, feed PEMs to `vite --https`:

```bash
export WILDCARD='*.lan.xrot.example.com' ACME_EMAIL=you@example.com CF_DNS_API_TOKEN=<token>
nix run .#cert            # PEMs → ./.certs ; re-run to renew (90-day certs)
```

`nix develop` drops you in a shell with `caddy` (Cloudflare DNS), `lego`, `dnsmasq`, `cloudflared`, `jq` on PATH for manual work.

## Notes

- **`:8443`, not `:443`** — the default avoids `sudo` (macOS needs root for ports <1024). WebXR only requires *HTTPS*, not port 443, so any port is fine. Want `:443`? Set `HTTPS_PORT=443` and run `serve` with `sudo -E`.
- **The token never lands in the Nix store.** Caddy reads `{env.CF_API_TOKEN}` at runtime; the generated Caddyfile lives in a `mktemp -d` dir that's cleaned on exit.
- **Prefer the public A-record mapping for Spectacles / Android XR** — split-horizon depends on the headset honouring your locally-served resolver, which quirky XR net stacks may not (doc §2.5). Use `.#dns` only when you specifically want split-horizon.
- **Add the hostname to `server.allowedHosts`** in `vite.config.ts` (next to the `.trycloudflare.com` entries), or Vite rejects it.
- **Want it as a launchd service** (survives reboots, no terminal)? Use the nix-darwin module below — the manual `nix run .#serve` above is the same thing in the foreground.

## Running as a launchd service (nix-darwin)

[`darwin-module.nix`](./darwin-module.nix) promotes the foreground `serve` into a **launchd service** that starts at login and restarts on crash. Requires [nix-darwin](https://github.com/nix-darwin/nix-darwin) (not plain Nix).

Import it into your darwin configuration and set the options:

```nix
# darwin-configuration.nix (or a flake's darwinConfigurations.<host>.modules)
{
  imports = [ ./prototype/d3-spatial/tools/nix-https/darwin-module.nix ];

  services.xrot-https = {
    enable    = true;
    domain    = "dev.lan.xrot.example.com";
    email     = "you@example.com";
    tokenFile = "/Users/you/.config/xrot-https/cf_token";  # chmod 600, NOT in the store
    # vitePort  = 5173;   # defaults
    # httpsPort = 8443;   # :443 requires runAsDaemon = true (root)
    # dataDir   = "/Users/Shared/xrot-caddy";   # persist for cert reuse
  };
}
```

Then:

```bash
echo -n '<cloudflare-token>' > ~/.config/xrot-https/cf_token && chmod 600 ~/.config/xrot-https/cf_token
darwin-rebuild switch --flake .#<host>
# logs: tail -f /Users/Shared/xrot-caddy/caddy.log
```

What the module does / decisions it encodes:

- **Per-user agent by default** (`launchd.user.agents`) so `:8443` needs no root. Set `runAsDaemon = true` to run as a root **daemon** — required only if you want `httpsPort = 443`.
- **Secret stays out of the store.** The generated Caddyfile holds no token; a wrapper reads `tokenFile` at runtime and exports `CF_API_TOKEN`, which Caddy picks up via `{env.CF_API_TOKEN}`.
- **`RunAtLoad + KeepAlive`** → starts at login, restarts on crash. `StandardOut/ErrorPath` → logs under `dataDir`.
- **Persist `dataDir`** (default `/Users/Shared/xrot-caddy`) — it's Caddy's `HOME` for ACME cert storage; re-issuing every reboot can hit Let's Encrypt rate limits.
- **Vendor hash** — already pinned (same value as `flake.nix`, for caddy 2.11.4 + cloudflare v0.2.4). On a version bump, re-resolve as above, or pass the flake's build via `services.xrot-https.caddyPackage = inputs.self.packages.${system}.caddy;` to keep one source of truth.

> The service only serves the TLS edge → Vite; you still run `npm run dev` + `npm run server` yourself. Managing those as launchd services too is a separate step (add agents that run `npm --prefix <path> run dev`), not covered here.
