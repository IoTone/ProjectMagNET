# chat-bench — the bench chatting side-by-side in a browser

A visual test rig for mesh chat **and Type 6 photo transfer**: one SolidJS
page with a chat pane per attached node (any number ≥ 2 — the page asks the
bridge via `GET /nodes` and lays out accordingly), each showing live lifecycle
state, role, channel, encryption info, **per-message end-to-end latency**
(host serial → envelope/AEAD → 802.15.4 → receiver's serial, all timed on the
bridge's single clock), and a **photo** button that sends an image across the
mesh as a Type 6 extended transfer with live progress and an inline preview
on the receiving pane.

```
../../../../.venv/bin/python bridge.py            # /dev/ttyACM0 + /dev/ttyACM1
bridge.py /dev/cu.usbmodem13101 /dev/cu.usbmodem13201 \
          /dev/cu.usbmodem13301 /dev/cu.usbmodem13401   # the whole bench
# → open http://127.0.0.1:8642/
```

With more than two nodes, chat latency is measured **per receiver** (every
receiving pane gets its own ⏱ stamp) and photos — which are unicast — grow a
per-pane receiver picker; `POST /sendphoto` takes `"to": <node index>`
(implied on a two-node bench).

- `bridge.py` — stdlib-only HTTP server wrapping a `MagnetNode` (host-sdk)
  around each port. Events stream to the page over SSE; sends are POSTs.
  Opening the ports **resets both boards** (DTR/RTS) — they re-attach in
  ~30 s and the panes go red → amber → green as they come up.
- `index.html` — the whole frontend. SolidJS via esm.sh (needs internet on
  first load), tagged-template syntax, no build step, no node_modules.

Latency is correlated by message text: pane A's send timestamp (taken by the
bridge just before the serial write) vs. the bridge timestamp of pane B's
`!CHAT` arrival. Both bubbles get the ⏱ stamp; the footer keeps a running
average. System lines (`!STATE`, `!ROLE`, `!WARN`, peer join/leave) appear
inline, dimmed.

Photos: the page downscales the picked image in-browser (canvas, max 1024 px,
JPEG re-encoded toward ≤180 KB — the §11.8 host-side re-encode rule), POSTs it
to `/sendphoto`, and the bridge drives the Type 6 window/`!XFER_NEXT` flow to
the *other* node's ML-EID. Raw `!XFER` chunk lines are consumed in the bridge
(one 448-char base64 line per chunk would swamp the page); the panes get
throttled `{kind:"xfer"}` progress events and one final `{kind:"photo"}` event
carrying the reassembled image as a data URI. At mesh speed (~5.3 KiB/s)
expect ~10–35 s per photo, with the receiving pane counting up in 10 % steps.

Don't run `hcp.py`/`xfer.py` against the same ports while the bridge holds
them — one process per serial port.
