#!/usr/bin/env python3
"""ble_hcp.py — drive a Hanasu node's BLE-GATT HCP binding (§11.2.1).

    python3 tools/ble_hcp.py --id 959f2e62 [--pair] [--wait S] "CAPS" "STATUS" ...

--id is REQUIRED: several bench nodes advertise the same service, and a host
that is already bonded to one of them gets an encrypted link silently — so an
unguarded privileged verb lands on whichever node answers first (it renamed
the companion node `probe` once). Each candidate is asked WHOAMI and skipped
unless its device id matches.

Finds the node by SERVICE UUID (hosts must never match on the GAP name —
platforms cache it; docs/BLE-PAIRING.md), subscribes to HCP-EVT, writes each
argument as one HCP line to HCP-CMD, and prints every line that comes back.
--pair reads HCP-AUTH first: an encryption-required read is what makes a
central pair (a peripheral's Security Request alone is ignored).
"""
import argparse
import asyncio

from bleak import BleakClient, BleakScanner

SVC = "6d61676e-2d68-6370-0001-000000000000"
CMD = "6d61676e-2d68-6370-0001-000000000001"
EVT = "6d61676e-2d68-6370-0001-000000000002"
AUTH = "6d61676e-2d68-6370-0001-000000000003"


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lines", nargs="*")
    ap.add_argument("--pair", action="store_true")
    ap.add_argument("--wait", type=float, default=2.0, help="seconds per command")
    ap.add_argument("--scan", type=float, default=8.0)
    ap.add_argument("--id", required=True, help="target device id (8 hex), from WHOAMI")
    a = ap.parse_args()

    found = await BleakScanner.discover(timeout=a.scan, return_adv=True)
    cands = [d for d, adv in found.values()
             if SVC in [u.lower() for u in adv.service_uuids]]
    if not cands:
        print("! no node advertising the HCP service")
        return 1
    print(f"# {len(cands)} HCP node(s) advertising; looking for id={a.id}")

    buf = bytearray()
    seen = []

    def on_evt(_, data: bytearray):
        buf.extend(data)
        while b"\n" in buf:
            line, _, rest = bytes(buf).partition(b"\n")
            buf[:] = rest
            text = line.decode(errors="replace")
            seen.append(text)
            print("< " + text)

    for dev in cands:
        async with BleakClient(dev) as c:
            await c.start_notify(EVT, on_evt)
            await c.write_gatt_char(CMD, b"WHOAMI", response=True)
            for _ in range(30):             # a bonded reconnect re-encrypts first
                if any(l.startswith("+OK id=") for l in seen):
                    break
                await asyncio.sleep(0.1)
            ident = next((l for l in seen if l.startswith("+OK id=")), "")
            seen.clear()
            if f"id={a.id}" not in ident:
                print(f"# skip {dev.address}: {ident or 'no WHOAMI'}")
                continue
            print(f"# target {dev.address}: {ident}  mtu={c.mtu_size}")
            await run(c, a)
            return 0
    print(f"! id={a.id} not found among {len(cands)} node(s)")
    return 1


async def run(c, a):
    if a.pair:
        try:
            v = await c.read_gatt_char(AUTH)
            print(f"# HCP-AUTH read ok: {v.decode(errors='replace')!r} (link encrypted)")
        except Exception as e:                     # noqa: BLE001
            print(f"! HCP-AUTH read failed: {e}")
    for line in a.lines:
        print("> " + line)
        await c.write_gatt_char(CMD, line.encode(), response=True)
        await asyncio.sleep(a.wait)


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
