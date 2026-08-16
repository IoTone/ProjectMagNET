#!/usr/bin/env python3
"""catbot_photoloop — periodic photo turns for a catbot v2 run.

The bot's node sends a ~48 KB noise PNG over a Type 6 extended transfer to
a rotating peer: the transfer path exercised under live chat load, progress
visible over the bridge's SSE (and accounted in the catbot log). First photo
~15 min in, then every --hours-between (default 2). Stdlib only.

    catbot_photoloop.py [--url http://127.0.0.1:8642] [--bot neko]
                        [--hours-between 2]
"""
import argparse
import base64
import json
import os
import random
import struct
import time
import urllib.request
import zlib

from catbot_traffic import find_bot, post   # same dir; shared discovery


def noise_png(w=128, h=128):
    """A valid RGB PNG of random noise — incompressible, so ~48 KB: big
    enough for a real multi-window transfer, small enough to finish in ~10 s."""
    raw = b''.join(b'\x00' + os.urandom(w * 3) for _ in range(h))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(raw, 1)) + chunk(b'IEND', b''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--url', default='http://127.0.0.1:8642')
    ap.add_argument('--bot', default='neko')
    ap.add_argument('--hours-between', type=float, default=2.0)
    a = ap.parse_args()
    bot, dests = find_bot(a.url, a.bot)
    print(f'photoloop: bot={a.bot}@{bot}, receivers={dests}', flush=True)
    n = 0
    while True:
        time.sleep(a.hours_between * 3600 if n else 900)
        n += 1
        try:
            post(a.url, '/sendphoto', {
                'node': bot, 'to': random.choice(dests),
                'name': f'{a.bot}-selfie-{n}.png',
                'data': base64.b64encode(noise_png()).decode()})
            print(f'photo {n} launched', flush=True)
        except Exception as e:
            print('photo err:', e, flush=True)


if __name__ == '__main__':
    main()
