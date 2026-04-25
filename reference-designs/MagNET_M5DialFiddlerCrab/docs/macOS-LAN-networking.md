# macOS LAN networking gotchas

Any MagNET workflow that touches the local network from macOS (running `fake_ruler.py`, pinging the camera, hitting `magnet-cam-<mac4>.local` from a browser, `dns-sd -B` for mDNS discovery) can hit one or more of the privacy / security filters Apple has added since macOS 14 Sonoma. Symptoms are usually silent failures — the packet just never leaves or never returns.

## Check these first, in order

### 1. Local Network permission (most common)

Since macOS 14 Sonoma, any app that wants to talk to non-Apple devices on your LAN must be explicitly granted "Local Network" access. Terminal, Python, and custom apps don't auto-prompt — they silently get zero packets.

```
System Settings → Privacy & Security → Local Network
```

Look for **Terminal**, **iTerm**, **Python** (any version installed), and any browser you're using. Toggle them **on**. If an app isn't listed, it hasn't triggered the prompt yet — launch it, try to hit a local device once, and it should appear.

Verify a Python script has permission:
```bash
# This will fail silently if Python lacks Local Network permission.
python3 -c "from zeroconf import Zeroconf, ServiceBrowser; import time; zc = Zeroconf(); \
  ServiceBrowser(zc, '_magnet-ruler._tcp.local.', handlers=[lambda zc, t, n, s: print(n, s)]); \
  time.sleep(3)"
```

If you get zero output and you know a ruler is advertising, it's the Local Network toggle.

### 2. Private Wi-Fi Address (rotating MAC)

Under `System Settings → Wi-Fi → [your network] → Details… → Private Wi-Fi address`, the default is **Rotating**. When the MAC rotates, any DHCP reservations or `.local` bindings from prior sessions go stale and mDNS responses may not route back to your laptop.

For a dev LAN, set to **Off** (fixed MAC). Reconnect to the network to take effect. Router DHCP will see the real MAC and reservations will stick.

### 3. iCloud Private Relay

`System Settings → Apple ID → iCloud → Private Relay`. When on, some apps tunnel DNS and HTTP through Apple's relay, which **cannot resolve `.local` hostnames**. Safari is the usual offender.

Workarounds:
- Disable Private Relay for your Wi-Fi network (same pane → "Use Country & Time Zone" option or the per-network toggle)
- Or hit the device by IP instead of hostname (`http://10.0.0.185/stream` vs `http://magnet-cam-a1b2.local/stream`)

### 4. macOS Firewall

`System Settings → Network → Firewall`. If enabled, by default it blocks **incoming** connections. The fake-ruler script listens on TCP 7447; incoming HELLO frames from an ESP32 node will be dropped silently until you either:

- Turn off the firewall for the dev network, or
- Add Python (or your terminal shell) to "Allowed apps" explicitly

The macOS firewall is application-based, not port-based — you authorize the process, not the port.

### 5. Content filter / VPN / pf rules

Any VPN (Tailscale, WireGuard, corporate VPN) or content filter intercepts traffic. If connected, LAN devices may be invisible even with everything else right.

- Disconnect VPN for dev work, or
- Add an exclusion for your LAN subnet (`192.168.0.0/16`, `10.0.0.0/8`)

PF firewall rules can be inspected with `sudo pfctl -s rules` if you suspect something is blocking port 5353 (mDNS) or 7447 (hive).

## Symptom → cause quick lookup

| Symptom | Likely cause |
|---|---|
| `ping magnet-cam-a1b2.local` → `cannot resolve` but browser works | Private Relay / DNS-over-HTTPS; hit by IP |
| `ping <ip>` times out but `/stream` in browser works | ESP32 WiFi power-save drops ICMP (see `M5_Hive_Camera/README.md` troubleshooting), or client isolation enabled on the router |
| `fake_ruler.py` shows no incoming connections from ESP32 | Local Network permission for Python, or macOS firewall blocking incoming |
| `dns-sd -B _magnet-ruler._tcp local.` returns nothing | Ruler isn't advertising, or Local Network permission, or WiFi interface lost multicast |
| Works on phone/Linux but not on Mac | One of 1–5 above, usually Local Network permission |

## Quick sanity test

With a ruler advertising (Dial flashed, or `fake_ruler.py` running on this same Mac), try:

```bash
# 1. mDNS discovery
dns-sd -B _magnet-ruler._tcp local.
# expect: lines with "MagNET-ruler-..." within ~1 second

# 2. Hostname resolution
dscacheutil -q host -a name magnet-cam-a1b2.local
# expect: ip_address lines

# 3. Direct HTTP
curl -s -o /dev/null -w "%{http_code}\n" http://magnet-cam-a1b2.local/status
# expect: 200
```

If any of these fail but the others pass, the symptom lookup above usually points to which step needs attention.

## Why this document exists

macOS is the most common dev laptop in this project, and each of the filters above has bitten bringup at least once. Linux dev works almost always (fewer filters). iOS is unaffected for browsers. Android tends to "just work" for nRF Connect. Windows varies — Defender firewall is the equivalent of item 4. This guide is macOS-specific because the accumulation of overlapping privacy layers is what makes it non-obvious.
