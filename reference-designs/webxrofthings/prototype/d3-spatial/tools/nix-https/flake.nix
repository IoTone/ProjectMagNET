{
  description =
    "Tunnel-free local HTTPS for the XrOT prototype: Caddy + Let's Encrypt DNS-01 (Cloudflare), so a Quest 3 / Android XR / Spectacles gets a browser-trusted cert with nothing installed on the headset. See ../../docs/discovery-and-tunnel-free-https.md.";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        lib = pkgs.lib;

        # ---------------------------------------------------------------
        # Caddy built WITH the Cloudflare DNS-01 plugin baked in.
        #
        # `caddy.withPlugins` needs a recent nixpkgs (>= 24.11 / unstable).
        # The `hash` is the vendored Go-module hash for this exact
        # (caddy version + plugin set). If you bump either, set
        # `hash = lib.fakeHash;`, run `nix build .#caddy`, and copy the
        # `got: sha256-...` value from the error back here.
        # Resolved 2026-07-06 for caddy 2.11.4 + cloudflare v0.2.4.
        #
        # Using a different DNS provider? Swap the plugin (e.g.
        # github.com/caddy-dns/route53, /desec, /googleclouddns) and the
        # `dns <provider>` line in the Caddyfile below (then re-resolve the hash).
        # ---------------------------------------------------------------
        caddyWithDns = pkgs.caddy.withPlugins {
          plugins = [ "github.com/caddy-dns/cloudflare@v0.2.4" ];
          hash = "sha256-hEHgAG0F0ozHRAPuxEqLyTATBrE+pajeXDiSNwniorg=";
        };

        # nix run .#serve  — real HTTPS at the edge, reverse-proxied to Vite.
        # Caddy does the DNS-01 challenge + 60-day auto-renewal itself; it
        # never needs inbound 80/443, so this works behind NAT / on a LAN.
        # Defaults to :8443 so it needs NO sudo (WebXR is happy on any port).
        serve = pkgs.writeShellScriptBin "xrot-https-serve" ''
          set -euo pipefail
          DOMAIN="''${DOMAIN:?set DOMAIN, e.g. dev.lan.xrot.example.com}"
          : "''${CF_API_TOKEN:?set CF_API_TOKEN (Cloudflare API token, Zone:DNS:Edit scope)}"
          VITE_PORT="''${VITE_PORT:-5173}"
          HTTPS_PORT="''${HTTPS_PORT:-8443}"
          ACME_EMAIL="''${ACME_EMAIL:-admin@$DOMAIN}"

          workdir="$(mktemp -d)"
          trap 'rm -rf "$workdir"' EXIT
          # The token is read by Caddy from the environment at runtime via
          # {env.CF_API_TOKEN} — it is NEVER written to the Nix store.
          cat > "$workdir/Caddyfile" <<EOF
          {
            email $ACME_EMAIL
          }
          https://$DOMAIN:$HTTPS_PORT {
            tls {
              dns cloudflare {env.CF_API_TOKEN}
            }
            reverse_proxy 127.0.0.1:$VITE_PORT
          }
          EOF

          echo "[xrot-https] https://$DOMAIN:$HTTPS_PORT  ->  127.0.0.1:$VITE_PORT"
          echo "[xrot-https] point the headset at https://$DOMAIN:$HTTPS_PORT (must resolve to the LAN IP)"
          exec ${caddyWithDns}/bin/caddy run --config "$workdir/Caddyfile" --adapter caddyfile
        '';

        # nix run .#cert  — issue ONLY the wildcard PEMs (no Caddy), for the
        # `vite --https` path. Certs land in $OUT (default ./.certs). Renew by
        # re-running (add to cron/launchd; certs are 90-day).
        cert = pkgs.writeShellScriptBin "xrot-https-cert" ''
          set -euo pipefail
          WILDCARD="''${WILDCARD:?set WILDCARD, e.g. '*.lan.xrot.example.com'}"
          ACME_EMAIL="''${ACME_EMAIL:?set ACME_EMAIL}"
          : "''${CF_DNS_API_TOKEN:?set CF_DNS_API_TOKEN (Cloudflare token, Zone:DNS:Edit scope)}"
          OUT="''${OUT:-$PWD/.certs}"
          echo "[xrot-https] issuing $WILDCARD via DNS-01 (Cloudflare) -> $OUT"
          exec ${pkgs.lego}/bin/lego \
            --accept-tos --email "$ACME_EMAIL" \
            --dns cloudflare --domains "$WILDCARD" --path "$OUT" run
        '';

        # nix run .#dns  — OPTIONAL split-horizon resolver for the LAN.
        # Needs :53 -> run with sudo. Prefer a public A-record -> private IP
        # for XR glasses (see the doc); this is only if you want split-horizon.
        dns = pkgs.writeShellScriptBin "xrot-https-dns" ''
          set -euo pipefail
          DOMAIN_SUFFIX="''${DOMAIN_SUFFIX:?set DOMAIN_SUFFIX, e.g. lan.xrot.example.com}"
          LAN_IP="''${LAN_IP:?set LAN_IP, e.g. 192.168.1.50}"
          echo "[xrot-https] dnsmasq: *.$DOMAIN_SUFFIX -> $LAN_IP  (needs sudo for :53)"
          echo "[xrot-https] hand this resolver out via DHCP, or the headset won't use it"
          exec ${pkgs.dnsmasq}/bin/dnsmasq --no-daemon \
            --address="/$DOMAIN_SUFFIX/$LAN_IP" --port=53
        '';
      in {
        packages = {
          caddy = caddyWithDns;
          serve = serve;
          cert = cert;
          dns = dns;
        };

        apps = {
          serve = { type = "app"; program = "${serve}/bin/xrot-https-serve"; };
          cert = { type = "app"; program = "${cert}/bin/xrot-https-cert"; };
          dns = { type = "app"; program = "${dns}/bin/xrot-https-dns"; };
          default = { type = "app"; program = "${serve}/bin/xrot-https-serve"; };
        };

        # `nix develop` — all the CLI tools on PATH for manual poking.
        devShells.default = pkgs.mkShell {
          packages = [ caddyWithDns pkgs.lego pkgs.dnsmasq pkgs.jq pkgs.cloudflared ];
          shellHook = ''
            echo "xrot-https devshell — caddy (with cloudflare DNS), lego, dnsmasq, cloudflared"
            echo "  export DOMAIN=dev.lan.xrot.example.com CF_API_TOKEN=... && nix run .#serve"
          '';
        };
      });
}
