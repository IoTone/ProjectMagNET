#!/usr/bin/env python3
"""magnet_cli — terminal chat client for a MagNET node (reference SDK demo).

    python3 magnet_cli.py                    # auto-discover a node
    python3 magnet_cli.py /dev/cu.usbmodem101 --name alice

Type to chat. Slash commands:
    /status /peers /whoami /channel /stats /selftest
    /name <n>      set display name
    /join <cred>   join a channel (passphrase, 12+ word seed phrase, or qr:...)
    /dm <ipv6> <t> direct message
    /raw <line>    send any HCP line verbatim
    /quit
"""
import argparse
import sys
import threading

from magnet_hcp import HCPError, MagnetNode, SerialTransport


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", help="serial port (default: auto-discover)")
    ap.add_argument("--name", help="set display name on connect")
    args = ap.parse_args()

    port = args.port
    if not port:
        ports = SerialTransport.discover()
        if not ports:
            print("no MagNET node found", file=sys.stderr)
            return 1
        port = ports[0]
        print(f"# using {port}")

    node = MagnetNode(port)
    node.on("chat", lambda e: print(f"\r<{e.fields[2] if len(e.fields) > 2 else '?'}> {e.text}"))
    node.on("dm", lambda e: print(f"\r*{e.fields[1] if len(e.fields) > 1 else '?'}* {e.text}"))
    node.on("peer_join", lambda e: print(f"\r# peer joined: {' '.join(e.fields)}"))
    node.on("peer_leave", lambda e: print(f"\r# peer left: {' '.join(e.fields)}"))
    node.on("role", lambda e: print(f"\r# role -> {' '.join(e.fields)}"))
    node.on("state", lambda e: print(f"\r# state -> {' '.join(e.fields)}"))
    node.on("warn", lambda e: print(f"\r! warning: {e.text or ' '.join(e.fields)}"))

    try:
        st = node.status()
        print(f"# connected: {st}")
        if args.name:
            node.set_name(args.name)
            print(f"# name set to {args.name}")
        if st.get("state") != "READY":
            print("# node is not READY yet — chat will fail until it attaches")
    except Exception as e:
        print(f"# could not query node: {e}", file=sys.stderr)
        node.close()
        return 1

    print("# type to chat, /help for commands, /quit to exit")
    try:
        for line in iter(sys.stdin.readline, ""):
            line = line.rstrip("\n")
            if not line:
                continue
            try:
                if not line.startswith("/"):
                    node.chat(line)
                    continue
                cmd, _, rest = line[1:].partition(" ")
                cmd = cmd.lower()
                if cmd in ("quit", "exit"):
                    break
                elif cmd == "help":
                    print(__doc__)
                elif cmd == "status":
                    print(node.status())
                elif cmd == "peers":
                    for p in node.peers():
                        print(f"  {p['id']}  {p['name']:<12} {p['ipv6']}")
                elif cmd == "whoami":
                    print(node.whoami())
                elif cmd == "channel":
                    print(node.channel())
                elif cmd == "stats":
                    print(node.stats())
                elif cmd == "selftest":
                    print("PASS" if node.selftest() else "FAIL")
                elif cmd == "name":
                    node.set_name(rest.strip())
                elif cmd == "join":
                    print("# deriving key, this can take a few seconds...")
                    print(node.set_channel(rest.strip()))
                elif cmd == "dm":
                    peer, _, text = rest.partition(" ")
                    node.dm(peer, text)
                elif cmd == "raw":
                    print(node.command(rest))
                else:
                    print(f"# unknown command /{cmd}")
            except HCPError as e:
                print(f"! {e.code}: {e.message}")
            except Exception as e:
                print(f"! {e}")
    except KeyboardInterrupt:
        pass
    finally:
        node.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
