#!/usr/bin/env python3
"""Put WiFi credentials on a Sentinel over USB, without an interactive monitor.

Run it in your own terminal. The password is typed locally (not echoed) and
goes only to the device:

    ~/.platformio/penv/bin/python tools/sentinel_wifi.py --port /dev/cu.usbserial-XXXXXXXX

Drives the Forth console: wifi-ssid / wifi-pass / wifi-connect, then waits
for "[MQTT] connected" and prints sen-stat. Opening the port resets the Stick,
so it waits for the boot banner first. Credentials are saved in NVS; the
Stick reconnects on every boot after this.
"""

import argparse
import getpass
import sys
import time

import serial


def read_until(s, marker, timeout, echo=True):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = s.read(512)
        if chunk:
            buf += chunk
            if echo:
                sys.stdout.write(chunk.decode(errors="replace"))
                sys.stdout.flush()
            if marker.encode() in buf:
                return True
    return False


def send_line(s, text):
    time.sleep(0.3)                 # read_line drains stale input for 50 ms first
    s.write(text.encode() + b"\r")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True)
    args = ap.parse_args()

    ssid = input("WiFi network name (SSID): ").strip()
    if not ssid:
        sys.exit("no SSID given")
    pw = getpass.getpass("WiFi password (Enter for an open network): ")

    print(f"# opening {args.port}")
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, 115200, 0.1
    s.dtr = s.rts = False           # asserted lines hold this board in reset
    s.open()
    # Opening the port may or may not reset the Stick. If boot output starts
    # flowing, wait for the banner to finish; otherwise it is already running.
    if read_until(s, "\n", 1.5, echo=False):
        print("# waiting for the Stick to boot...")
        read_until(s, "power key hold = off", 15, echo=False)
        time.sleep(1)
    s.reset_input_buffer()

    print("# sending network name")
    s.write(b"wifi-ssid\r")
    read_until(s, "SSID:", 5, echo=False)
    send_line(s, ssid)
    read_until(s, "wifi-pass", 5, echo=False)

    print("# sending password")
    s.write(b"wifi-pass\r")
    read_until(s, "Password", 5, echo=False)
    # A lone first Enter is skipped by the console, so an empty password needs two.
    send_line(s, pw if pw else "\r")
    read_until(s, "set", 5, echo=False) or read_until(s, "open network", 1, echo=False)

    s.write(b"wifi-connect\r")
    print(f"# connecting to '{ssid}'...")
    ok = read_until(s, "[MQTT] connected", 45)
    print()
    s.write(b"sen-stat\r")
    read_until(s, "heap", 5)
    print()
    print("# MQTT connected — done." if ok else
          "# MQTT did not connect within 45 s. Check the SSID/password (2.4 GHz only), "
          "then run this again.")
    s.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
