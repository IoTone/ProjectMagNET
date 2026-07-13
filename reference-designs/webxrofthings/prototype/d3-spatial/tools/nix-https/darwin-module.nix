# nix-darwin module — run the XrOT local-HTTPS Caddy edge as a launchd service.
#
# Turns the foreground `nix run .#serve` into a background service that starts
# at login and restarts on crash, so the browser-trusted HTTPS front for WebXR
# is always up without a spare terminal. See ./README.md for usage and
# ../../docs/discovery-and-tunnel-free-https.md for the why.
#
# Import into your darwin configuration and set `services.xrot-https.*`.
{ config, lib, pkgs, ... }:

let
  cfg = config.services.xrot-https;

  # Caddy with the Cloudflare DNS-01 plugin. `hash` is the vendored Go-module
  # hash for (caddy 2.11.4 + cloudflare v0.2.4), resolved 2026-07-06. If you bump
  # either, set `hash = lib.fakeHash`, `darwin-rebuild switch`, copy the
  # `got: sha256-…` value back — or pass the flake's already-built package via
  # `services.xrot-https.caddyPackage` to keep one source of truth.
  defaultCaddy = pkgs.caddy.withPlugins {
    plugins = [ "github.com/caddy-dns/cloudflare@v0.2.4" ];
    hash = "sha256-hEHgAG0F0ozHRAPuxEqLyTATBrE+pajeXDiSNwniorg=";
  };

  caddyfile = pkgs.writeText "xrot-Caddyfile" ''
    {
      email ${cfg.email}
    }
    https://${cfg.domain}:${toString cfg.httpsPort} {
      tls {
        dns cloudflare {env.CF_API_TOKEN}
      }
      reverse_proxy 127.0.0.1:${toString cfg.vitePort}
    }
  '';

  # Wrapper loads the secret token from a file OUTSIDE the Nix store at runtime,
  # so it never lands in a world-readable /nix/store path. The Caddyfile itself
  # holds no secret (Caddy reads {env.CF_API_TOKEN} from the process env).
  start = pkgs.writeShellScript "xrot-https-start" ''
    set -euo pipefail
    export CF_API_TOKEN="$(cat ${cfg.tokenFile})"
    export HOME=${cfg.dataDir}
    exec ${cfg.caddyPackage}/bin/caddy run --config ${caddyfile} --adapter caddyfile
  '';
in {
  options.services.xrot-https = {
    enable = lib.mkEnableOption "XrOT tunnel-free local HTTPS (Caddy + Let's Encrypt DNS-01)";

    domain = lib.mkOption {
      type = lib.types.str;
      example = "dev.lan.xrot.example.com";
      description = "Hostname the headset opens. Must resolve to the LAN IP (public A-record or split-horizon).";
    };

    email = lib.mkOption {
      type = lib.types.str;
      description = "ACME account email for Let's Encrypt.";
    };

    tokenFile = lib.mkOption {
      type = lib.types.path;
      example = "/Users/you/.config/xrot-https/cf_token";
      description = ''
        Path to a file (OUTSIDE the Nix store) containing the Cloudflare API
        token with Zone:DNS:Edit scope. `chmod 600`, owned by the run user.
      '';
    };

    vitePort = lib.mkOption {
      type = lib.types.port;
      default = 5173;
      description = "Local Vite dev-server port to reverse-proxy to.";
    };

    httpsPort = lib.mkOption {
      type = lib.types.port;
      default = 8443;
      description = ''
        HTTPS listen port. Default 8443 works as a per-user launchd agent with
        no root. For :443 you must use a system daemon (runAsDaemon = true),
        since ports < 1024 need root on macOS.
      '';
    };

    dataDir = lib.mkOption {
      type = lib.types.str;
      default = "/Users/Shared/xrot-caddy";
      description = ''
        Writable dir used as HOME for Caddy's ACME cert storage. Persist it —
        re-issuing on every reboot can hit Let's Encrypt rate limits.
      '';
    };

    caddyPackage = lib.mkOption {
      type = lib.types.package;
      default = defaultCaddy;
      defaultText = lib.literalExpression "pkgs.caddy.withPlugins { … cloudflare … }";
      description = "Caddy build to use. Override with the flake's `packages.caddy` to share one resolved hash.";
    };

    runAsDaemon = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = "Run as a system daemon (root) instead of a per-user agent. Required for httpsPort < 1024.";
    };
  };

  config = lib.mkIf cfg.enable (
    let
      serviceConfig = {
        ProgramArguments = [ "${start}" ];
        RunAtLoad = true;
        KeepAlive = true;
        StandardOutPath = "${cfg.dataDir}/caddy.log";
        StandardErrorPath = "${cfg.dataDir}/caddy.err.log";
      };
    in
    if cfg.runAsDaemon
    then { launchd.daemons.xrot-https = { inherit serviceConfig; }; }
    else { launchd.user.agents.xrot-https = { inherit serviceConfig; }; }
  );
}
