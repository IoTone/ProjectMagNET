#!/usr/bin/env node
/**
 * discover-magnet-devices — find every MagNET ESP32 on the local
 * network via Bonjour / mDNS, with NO firmware changes required.
 *
 * Every MagNET device's firmware already registers an mDNS service
 * (`_magnet-node._tcp`, `_magnet-imu._tcp`, or just `_http._tcp` for
 * the older ones) and an mDNS hostname like `magnet-cam-8610.local`.
 * This script wraps macOS's built-in `dns-sd` to browse those
 * services, resolve each hostname to an IPv4, and dump a JSON
 * inventory the Vite proxy can consume in place of `*_HOST` env vars.
 *
 *   node tools/discover-magnet-devices.mjs              # pretty table
 *   node tools/discover-magnet-devices.mjs --json       # JSON to stdout
 *   node tools/discover-magnet-devices.mjs --timeout 5  # browse 5s (def. 3)
 *
 * Caveats:
 *   - macOS only. Linux equivalent is `avahi-browse -arpt _magnet-node._tcp`;
 *     a small fork would handle that, kept out for v1 simplicity.
 *   - macOS Local Network permission must be granted to whichever
 *     terminal / `node` runs this. See docs/macOS-LAN-networking.md.
 *   - iCloud Private Relay routes some LAN traffic through Apple's
 *     proxies; disable it for the dev session.
 *   - dns-sd streams forever; we kill it after `--timeout` seconds.
 *     The "right" number balances "finds slow-to-respond devices"
 *     vs "doesn't block `npm run dev` for 10 s." 3 s is a working
 *     default against the test LAN.
 */

import { spawn } from 'node:child_process';
import { platform } from 'node:os';

/* Service types every MagNET firmware registers, in priority order.
 * `_magnet-node._tcp` is the richest (TXT carries role + caps);
 * `_magnet-imu._tcp` is added lazily by the Capsule when `imu-on`
 * runs; `_http._tcp` is the fallback for devices that haven't been
 * updated to publish a magnet-specific record yet. */
const SERVICE_TYPES = [
  '_magnet-node._tcp',
  '_magnet-imu._tcp',
  '_http._tcp',
];
const DEFAULT_BROWSE_MS  = 3000;
const RESOLVE_TIMEOUT_MS = 1500;

/* Run `dns-sd <args>` for up to `timeoutMs` ms, return the captured
 * stdout+stderr concatenation. dns-sd streams forever by design;
 * we let it stream then SIGTERM it. */
function runDnsSd(args, timeoutMs) {
  return new Promise((resolve) => {
    const child = spawn('dns-sd', args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let buf = '';
    child.stdout.on('data', d => { buf += d.toString(); });
    child.stderr.on('data', d => { buf += d.toString(); });
    const timer = setTimeout(() => { child.kill('SIGTERM'); }, timeoutMs);
    child.on('close', () => { clearTimeout(timer); resolve(buf); });
    child.on('error', () => { clearTimeout(timer); resolve(buf); });
  });
}

/* Parse `dns-sd -Z` output. It's a zonefile-style dump with SRV
 * records giving { port, target hostname } and TXT records carrying
 * the device's role + capabilities. Instance names with spaces are
 * escaped as `\032` per RFC1035. */
function parseZoneFile(output) {
  const devices = new Map();  // keyed by hostname
  const lines = output.split('\n');

  /* SRV match: <instance>.<service-type>  SRV  prio weight port hostname.
   * We only accept SRV rows whose target hostname starts with
   * `magnet-` — that keeps `_http._tcp` from sweeping up every
   * printer + AirPlay receiver on the LAN. */
  const srvRe = /^(\S+?)\.\s*(_[\w-]+\._tcp)\s+SRV\s+\d+\s+\d+\s+(\d+)\s+(magnet-[\w-]+)\.local\.?/i;
  for (const line of lines) {
    const m = line.match(srvRe);
    if (!m) continue;
    const [, instanceRaw, service, portStr, host] = m;
    const instance = instanceRaw.replace(/\\032/g, ' ');
    const hostFull = `${host}.local`;
    if (!devices.has(hostFull)) {
      devices.set(hostFull, {
        hostname: hostFull,
        host:     host,                 // bare, no .local
        instance,
        port:     parseInt(portStr, 10),
        services: [service],
        txt:      {},
      });
    } else {
      const d = devices.get(hostFull);
      if (!d.services.includes(service)) d.services.push(service);
    }
  }

  /* TXT match — pair with the SRV-discovered device by instance name.
   * TXT records can repeat; later entries supersede earlier (TTL doesn't
   * matter to us). */
  const txtRe = /^(\S+?)\.\s*_[\w-]+\._tcp\s+TXT\s+(.*)/;
  for (const line of lines) {
    const m = line.match(txtRe);
    if (!m) continue;
    const [, instanceRaw, txtRaw] = m;
    const instance = instanceRaw.replace(/\\032/g, ' ');
    /* Each key=value pair is wrapped in quotes. Empty TXT shows as `""`
     * which we skip. */
    const pairs = {};
    for (const pm of txtRaw.matchAll(/"([^"]*)"/g)) {
      const body = pm[1];
      if (!body) continue;
      const eq = body.indexOf('=');
      if (eq < 0) continue;
      pairs[body.slice(0, eq)] = body.slice(eq + 1);
    }
    /* Find the device record matching this instance (we keyed by hostname,
     * so iterate). Cheap — <10 devices in practice. */
    for (const dev of devices.values()) {
      if (dev.instance === instance) {
        Object.assign(dev.txt, pairs);
        break;
      }
    }
  }

  return Array.from(devices.values());
}

/* Resolve a `.local` hostname to its IPv4 address via Bonjour. */
async function resolveIPv4(host) {
  const out = await runDnsSd(['-G', 'v4', host], RESOLVE_TIMEOUT_MS);
  for (const line of out.split('\n')) {
    /* "Timestamp  Add  flags  if  hostname  IP  TTL" */
    const m = line.match(/\bAdd\b\s+\S+\s+\S+\s+\S+\s+(\d+\.\d+\.\d+\.\d+)\s+\d+\s*$/);
    if (m) return m[1];
  }
  return null;
}

async function discover(timeoutMs = DEFAULT_BROWSE_MS) {
  const merged = new Map();   // hostname → device record

  for (const svc of SERVICE_TYPES) {
    const out = await runDnsSd(['-Z', svc, 'local.'], timeoutMs);
    const list = parseZoneFile(out);
    for (const dev of list) {
      if (!merged.has(dev.hostname)) {
        merged.set(dev.hostname, dev);
      } else {
        /* Same device on multiple service types — union services + txt. */
        const existing = merged.get(dev.hostname);
        for (const s of dev.services) if (!existing.services.includes(s)) existing.services.push(s);
        Object.assign(existing.txt, dev.txt);
        if (existing.port === 0 && dev.port > 0) existing.port = dev.port;
      }
    }
  }

  /* Resolve IPs in parallel — Bonjour resolution is one RTT each, no need to
   * serialise. */
  await Promise.all(Array.from(merged.values()).map(async dev => {
    dev.ip = await resolveIPv4(dev.hostname);
  }));

  return Array.from(merged.values()).filter(d => d.ip != null);
}

/* ─── CLI ──────────────────────────────────────────────────────── */

if (platform() !== 'darwin') {
  console.error(`[discover] only macOS dns-sd is supported.`);
  console.error(`On Linux:  avahi-browse -arpt _magnet-node._tcp`);
  process.exit(1);
}

const argv = process.argv.slice(2);
const wantJson = argv.includes('--json');
const tIdx = argv.indexOf('--timeout');
const timeoutSec = tIdx >= 0 ? Number.parseFloat(argv[tIdx + 1]) : 3;
const timeoutMs  = Math.max(500, Math.floor(timeoutSec * 1000));

const devices = await discover(timeoutMs);

if (wantJson) {
  console.log(JSON.stringify(devices, null, 2));
} else {
  console.log(`Found ${devices.length} MagNET device(s) in ${timeoutSec}s:\n`);
  for (const d of devices) {
    const portStr = d.port > 0 ? `:${d.port}` : '';
    console.log(`  ${d.hostname.padEnd(34)} → ${d.ip.padEnd(15)}${portStr}`);
    console.log(`    instance:  ${d.instance}`);
    console.log(`    services:  ${d.services.join(', ')}`);
    const txt = Object.entries(d.txt);
    if (txt.length > 0) {
      console.log(`    txt:       ${txt.map(([k, v]) => `${k}=${v}`).join(' · ')}`);
    }
    console.log('');
  }
}
