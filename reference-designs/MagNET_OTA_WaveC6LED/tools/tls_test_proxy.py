#!/usr/bin/env python3
"""tls_test_proxy — a TLS terminator for R1 bench verification.

Listens with the given cert/key and forwards plaintext to the robotarme
server. This is BENCH SCAFFOLDING, not deployment advice: it exists so the
device's mbedTLS verification can be exercised against a cert we control,
including the negative case (a cert the pinned CA did not sign MUST be
refused). Usage:

    tls_test_proxy.py <listen-port> <cert.pem> <key.pem> [backend-port]
"""
import socket, ssl, sys, threading

def pump(a, b):
    try:
        while True:
            d = a.recv(4096)
            if not d: break
            b.sendall(d)
    except OSError: pass
    finally:
        for s in (a, b):
            try: s.shutdown(socket.SHUT_RDWR)
            except OSError: pass

def main():
    lport, cert, key = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    bport = int(sys.argv[4]) if len(sys.argv) > 4 else 8000
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", lport)); srv.listen(8)
    print(f"tls:{lport} -> plain:127.0.0.1:{bport}", flush=True)
    while True:
        c, addr = srv.accept()
        try: tls = ctx.wrap_socket(c, server_side=True)
        except ssl.SSLError as e:
            print(f"  handshake from {addr[0]}: {e}", flush=True); c.close(); continue
        print(f"  TLS OK from {addr[0]}", flush=True)
        b = socket.create_connection(("127.0.0.1", bport))
        threading.Thread(target=pump, args=(tls, b), daemon=True).start()
        threading.Thread(target=pump, args=(b, tls), daemon=True).start()

main()
