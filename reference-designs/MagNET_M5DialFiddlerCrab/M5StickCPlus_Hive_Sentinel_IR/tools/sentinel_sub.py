#!/usr/bin/env python3
"""Watch Sentinel IR presence events from anywhere.

The Sentinel publishes to the usual MagNET broker, so this works from any
network — no LAN access to the Stick needed:

    pip install -r tools/requirements.txt
    python3 tools/sentinel_sub.py                 # every sentinel
    python3 tools/sentinel_sub.py --id a1b2       # one device (MAC suffix)
    python3 tools/sentinel_sub.py --raw           # print the JSON as-is

Topics (base = iotj/magnet/sentinel/<id>):
    <base>/state   retained: current state + stats ("offline" = last will)
    <base>/event   enter | present | clear | rollover | reset
    <base>/activity  retained: motion now, clear countdown, last 5 min
    <base>/history   retained: occupied s per 15 min, last 24 h
"""

import argparse
import json
import sys
import time
from urllib.parse import urlparse

import paho.mqtt.client as mqtt

DEFAULT_BROKER = "mqtt://broker.hivemq.com:1883"   # craw_nvs CRAW_MQTT_DEFAULT_BROKER


def hm(secs):
    secs = int(secs or 0)
    return f"{secs // 3600}:{secs // 60 % 60:02d}"


def stats_line(st):
    if not st:
        return ""
    return "  ".join(f"{p} {st[p]['entries']} in/{hm(st[p]['occ_s'])}"
                     for p in ("hour", "day", "week", "month") if p in st)


def describe(m):
    if "bins_pct" in m:                       # <base>/activity
        if m.get("active"):
            return "activity motion now"
        ci = m.get("clear_in_s")
        lm = m.get("last_motion_s")
        tail = f", clears in {ci} s" if ci is not None else ""
        return f"activity last motion {lm} s ago{tail}" if lm is not None else "activity quiet"
    if "bins_s" in m:                         # <base>/history
        return f"history  last 24 h: {sum(m['bins_s']) // 60} min occupied"
    ev = m.get("ev")
    if ev == "state":
        if not m.get("online", True):
            return "OFFLINE (last will)"
        return f"state {m.get('state', '?').upper()} for {hm(m.get('since_s'))}   {stats_line(m.get('stats'))}"
    if ev == "enter":
        return f"ENTER    {stats_line(m.get('stats'))}"
    if ev == "present":
        return f"present  still here after {hm(m.get('visit_s'))}"
    if ev == "clear":
        return f"CLEAR    visit lasted {hm(m.get('visit_s'))}"
    if ev == "rollover":
        return (f"rollover {m.get('period')} {m.get('label')}: "
                f"{m.get('entries')} in, {hm(m.get('occ_s'))} occupied")
    if ev == "reset":
        return f"reset    {', '.join(m.get('periods', []))} (from the device)"
    return json.dumps(m)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--broker", default=DEFAULT_BROKER, help=f"default {DEFAULT_BROKER}")
    ap.add_argument("--base", default="iotj/magnet/sentinel", help="topic prefix before <id>")
    ap.add_argument("--id", default="+", help="device MAC suffix, e.g. a1b2 (default: all)")
    ap.add_argument("--raw", action="store_true", help="print raw JSON")
    ap.add_argument("--count", type=int, default=0, help="exit after N messages (0 = forever)")
    ap.add_argument("--timeout", type=float, default=0, help="exit after N seconds (0 = forever)")
    args = ap.parse_args()

    u = urlparse(args.broker if "://" in args.broker else "mqtt://" + args.broker)
    topic = f"{args.base}/{args.id}/#"
    seen = 0

    def on_connect(client, userdata, flags, rc, props=None):
        if rc != 0:
            print(f"connect failed: {rc}", file=sys.stderr)
            return
        client.subscribe(topic, qos=1)
        print(f"# {u.hostname}:{u.port or 1883}  subscribed {topic}", flush=True)

    def on_message(client, userdata, msg):
        nonlocal seen
        dev = msg.topic[len(args.base) + 1:].split("/")[0]
        try:
            m = json.loads(msg.payload)
        except ValueError:
            print(f"{dev}  (not JSON) {msg.payload[:120]!r}", flush=True)
            return
        when = m.get("local") or time.strftime("%Y-%m-%dT%H:%M:%S") + " (rx)"
        tag = " retained" if msg.retain else ""
        if args.raw:
            print(f"{msg.topic}{tag}  {msg.payload.decode()}", flush=True)
        else:
            print(f"{when[11:19] if 'T' in when else when}  {dev}  {describe(m)}{tag}", flush=True)
        seen += 1
        if args.count and seen >= args.count:
            client.disconnect()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2) if hasattr(mqtt, "CallbackAPIVersion") else mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(u.hostname, u.port or 1883, keepalive=30)
    if args.timeout:
        client.loop_start()
        end = time.time() + args.timeout
        while time.time() < end and not (args.count and seen >= args.count):
            time.sleep(0.1)
        client.loop_stop()
    else:
        try:
            client.loop_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
