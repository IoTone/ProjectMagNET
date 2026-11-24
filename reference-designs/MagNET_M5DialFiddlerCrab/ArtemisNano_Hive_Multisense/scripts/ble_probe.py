#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
PC-side check for the Multisensor BLE interface (README 5.1), using the PC's own
Bluetooth adapter via bleak (pip install bleak).

  python scripts/ble_probe.py                 scan 8 s, decode MagNET adverts
  python scripts/ble_probe.py --watch 60      keep scanning, print each new reading
  python scripts/ble_probe.py --hcp WHOAMI CAPS SENSE
                                              connect to the first Multisensor and
                                              run HCP verbs over the GATT service
  --name MagNET-7d8f                          pick a node by name
"""
import argparse
import asyncio
import struct
import sys

from bleak import BleakClient, BleakScanner

COMPANY_TEST = 0xFFFF
SVC = "6d61676e-2d68-6370-0001-000000000000"
CMD = "6d61676e-2d68-6370-0001-000000000001"
EVT = "6d61676e-2d68-6370-0001-000000000002"

CAP_BITS = ["presence", "lux", "color", "time", "battery", "sound", "orientation"]
STABILITY = ["unknown", "on-table", "stationary", "stable", "motion"]


def decode(mfg: bytes) -> dict:
    """Decode the 0xFFFF manufacturer payload (company ID already stripped)."""
    if len(mfg) < 6:
        return {"error": f"short header ({len(mfg)} bytes)"}
    out = {
        "version": mfg[0] >> 4,
        "role": mfg[0] & 0x0F,
        "caps": [n for i, n in enumerate(CAP_BITS) if struct.unpack_from("<H", mfg, 1)[0] & (1 << i)],
        "battery_pct": None if mfg[3] == 0xFF else mfg[3],
        "seq": mfg[4],
        "bundle": mfg[5],
    }
    if len(mfg) >= 14:
        # The host CONCATENATES the advert's manufacturer block with the scan
        # response's rather than replacing it: measured on CoreBluetooth
        # 2026-10-29, 20 bytes carrying the header twice then the readings
        #   1f 5e 00 62 84 00 | 1f 5e 00 62 84 00 | ff ff ff ff ab 00 03 05
        # so the readings are the LAST 8 bytes, which is also offset 6 when
        # only the 14-byte scan response arrives. Decoding at a fixed offset 6
        # reads the repeated header as data (lux 24095 = the second `1f 5e`).
        o = len(mfg) - 8
        lux, cct, yaw = struct.unpack_from("<HHh", mfg, o)
        out["lux"] = None if lux == 0xFFFF else lux
        out["cct"] = None if cct == 0xFFFF else cct
        out["yaw"] = None if yaw == -0x8000 else yaw
        stab = mfg[o + 6]
        out["stability"] = None if stab == 0xFF else STABILITY[stab] if stab < 5 else stab
        out["sig_motion"] = mfg[o + 7]
    return out


def is_magnet(dev, adv) -> bool:
    name = adv.local_name or dev.name or ""
    return name.startswith("MagNET-") or SVC in [u.lower() for u in adv.service_uuids]


async def scan(seconds: float, want: str | None, watch: bool):
    seen = {}
    found = {}

    def on_adv(dev, adv):
        if not is_magnet(dev, adv):
            return
        name = adv.local_name or dev.name or dev.address
        if want and name != want:
            return
        found[name] = dev
        mfg = adv.manufacturer_data.get(COMPANY_TEST)
        info = decode(mfg) if mfg else {"note": "no 0xFFFF data (not a Multisensor?)"}
        key = (name, info.get("seq"), info.get("lux"), info.get("yaw"))
        if watch and seen.get(name) == key:
            return
        if not watch and name in seen:
            return
        seen[name] = key
        print(f"{name}  {dev.address}  rssi {adv.rssi}  {info}")

    async with BleakScanner(on_adv):
        await asyncio.sleep(seconds)
    if not seen:
        print("no MagNET adverts seen")
    return found


async def hcp(dev, verbs):
    buf = bytearray()
    lines = asyncio.Queue()

    def on_evt(_, data: bytearray):
        buf.extend(data)
        while b"\n" in buf:
            line, _, rest = buf.partition(b"\n")
            buf[:] = rest
            lines.put_nowait(line.decode(errors="replace"))

    async with BleakClient(dev) as client:
        print(f"connected to {dev.address}, mtu {client.mtu_size}")
        await client.start_notify(EVT, on_evt)
        for i, verb in enumerate(verbs):
            tag = f"p{i}"
            await client.write_gatt_char(CMD, f"@{tag} {verb}\n".encode(), response=True)
            while True:
                try:
                    line = await asyncio.wait_for(lines.get(), 5)
                except asyncio.TimeoutError:
                    print(f"{verb}: no reply")
                    break
                print(line)
                if line.startswith(f"@{tag} "):
                    break


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--watch", type=float, metavar="SECONDS", help="scan this long, print every change")
    ap.add_argument("--name", help="node name, e.g. MagNET-7d8f")
    ap.add_argument("--hcp", nargs="+", metavar="VERB", help="connect and run these HCP verbs")
    args = ap.parse_args()

    found = await scan(args.watch or args.seconds, args.name, bool(args.watch))
    if args.hcp:
        if not found:
            sys.exit(1)
        await hcp(next(iter(found.values())), args.hcp)


if __name__ == "__main__":
    asyncio.run(main())
