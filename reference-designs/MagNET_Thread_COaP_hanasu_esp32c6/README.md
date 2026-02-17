# Overview

P2P chat use case on top of Thread.  It uses multicast and IPv6.  It attempts to allow all nodes to receive all comms or use direct 1-1 messaging.

## Use Cases

- IoT lighting control
- Private ad-hoc network chat
- Swarm AI intelligence

## References

- https://github.com/espressif/arduino-esp32/tree/release/v3.0.x/libraries/OpenThread/examples/COAP
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-reference/network/esp_openthread.html

## Hardware

- Any ESP32C6 (C5 might work but not tested)
- Recommendations
  - XIAO ESP32C6: https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/
  - M5NanoC6 (preferred): https://docs.m5stack.com/en/core/M5NanoC6

The M5NanoC6 is preferred because of built-in RGBs.  The XIAO ESP32C6 would be the better choice if you have expansion boards you are integrating with (it is a bit more versatile for plugging into a variety of existing hardware).  

## Build

- Depends on Board Manager: ESP32 3.0.6, M5Stack (2.1.2)
- Depends on Library: M5Unified (0.2.5), Adafruit_Neopixel (1.24.4)

## Setup

- Attach at least two nodes to one or more arduino IDEs
- build and install the software
- run the serial logger in the arduino

## Testing

- Use the UART in the arduino IDE to send "chat> some message"
- You can also send raw text, but the clients currently will throw out anything that doesn't match the chat> prefix.
- To send a DM: fd74:9ea0:9184:3064:dc9:2a4b:6777:b3ce chat> some message (replace the IPv6address with the target)
- You could also use python or node.js to connect to the uart and send messages programatically over the uart.
- Original code used this message to send lamp on/off: -> otLampCoapPUT(): coap put ff05::abcd Lamp con 0

## Design Notes

The inspiration for this project was the OLPC Mesh https://wiki.laptop.org/go/Mesh_Network_Details  which was a $100 laptop that included a p2p mesh networking feature.  For places where infrastructure was lacking, this was super interesting.  We have revived the $100 PC concept (probably inflated to $200 now) for Open Hardware for EDU STEM use cases as part of the "PONY" Cyberdeck project.  The mesh networking feature pursued is described and analyzed here: https://github.com/IoTone/PONY-Cyberdeck-25/issues/7

### Milestone 1 - Initial 2 peer PoC

DONE.  See commit ref: #59440363da3e284a7391a0f0b8a766c08f00fe6d

### Milestone 2 - Multipeer (4) PoC

All peers should be able to see all messages along with the sender.

DONE.  See commit ref: #828648daa49961b0641ba3a481792488255d4315

### Milestone 3 - Bug Fixes / Stability

1-1 messaging should work
Fix lighting protocol : ... for now add: lights switch 0/1 and lights color R,G,B
Startup should search for existing networks properly

NOT STARTED


### Milestone 5 - Migration to Platform.io / ESP32-IDF

Need to get this off of Arduino before doing anything at further scale.  Having a clean way to build once and install many times is preferred over the "compile/build" process, and toolchain mess.


NOT STARTED


### Milestone 4 - Scalability Testing

Obtain 50 nodes and stage a 50 node test.  Each node should log all of its data to OTEL.

NOT STARTED

## Known Issues

- The original code is based of the COAP Lamp / COAP Switch examples, and there are design choices made there originally that may affect this design
- LLM was used to help debug multicast issues, and often in fixing one thing, another thing was broken
- DMs using @IPV6 don't work.  This is a parsing bug in how the incoming handling messages are done.  Possibly also a sender issue.  But the messages are addressed properly, but not parsed properly.
- The original "button press" to handle on off is broken.  When the code switched from "con" to "non" it was broken.
- Sometimes no node or all nodes will become the leader
- Messages payload must be < 256 bytes
- There should be link level security, but no real application level security.  Any bad actor could join a network and flood or do something malicious, spoofing a friendly device or person.  
- The current scale of this network isn't known, but expected to be limited to 256 nodes in theory.  
- Sometimes the XIAO ESP32C6 will crash 3 times in a row on boot before starting up
- No scalability testing for a busy communication network (not clear if the current parser will handle this properly if multiple messages are incoming)

## Support

- File bug reports or make PRs
