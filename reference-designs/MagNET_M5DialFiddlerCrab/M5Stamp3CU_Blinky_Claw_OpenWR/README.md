# Claw OpenWR — Claude Code LED Notifier

An M5Stamp C3U device that shows Claude Code's status as LED patterns. When Claude needs your input, the LED strobes. When a task completes, it pulses green. When there's an error, it flashes red. Works across subnets via MQTT, or locally via HTTP.

## Quick Start

### 1. Flash the device

```bash
cd M5Stamp3CU_Blinky_Claw_OpenWR
pio run -e esp32c3 -t upload -t monitor
```

### 2. Connect to WiFi (one-time, via REPL)

```
ok> wifi-ssid
SSID: YourNetwork
ok> wifi-pass
Password (Enter for open): ********
ok> wifi-connect
Saved. Connecting to 'YourNetwork'...
```

LED goes pink while connecting, green strobe for 5 seconds on success, then off. Credentials are saved and restored on reboot.

### 3. Get your MQTT topic

```
ok> mqtt-topic
iotj/cl/openwr/updates/aaa4
```

Note the last 4 characters (your device's MAC suffix). You'll need this for the hook config.

### 4. Configure Claude Code hooks

Edit `~/.claude/settings.json` (creates it if it doesn't exist):

```json
{
  "hooks": {
    "Stop": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "mosquitto_pub -h broker.hivemq.com -t 'iotj/cl/openwr/updates/aaa4' -m '5' 2>/dev/null || true"
          }
        ]
      }
    ],
    "Notification": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "mosquitto_pub -h broker.hivemq.com -t 'iotj/cl/openwr/updates/aaa4' -m '3' 2>/dev/null || true"
          }
        ]
      }
    ]
  }
}
```

Replace `aaa4` with your device's topic suffix from step 3.

### 5. Install mosquitto (if not already)

```bash
# macOS
brew install mosquitto

# Ubuntu/Debian
sudo apt install mosquitto-clients
```

Only the `mosquitto_pub` client tool is needed, not the broker.

### 6. Test it

```bash
# Send "attention" notification
mosquitto_pub -h broker.hivemq.com -t "iotj/cl/openwr/updates/aaa4" -m "3"

# Send "task complete"
mosquitto_pub -h broker.hivemq.com -t "iotj/cl/openwr/updates/aaa4" -m "5"

# Send "clear" (LED off)
mosquitto_pub -h broker.hivemq.com -t "iotj/cl/openwr/updates/aaa4" -m "0"
```

Now start a Claude Code session — the LED will strobe when Claude needs input and pulse green when it finishes.

## LED Modes

| Mode | Name | LED Pattern | Trigger |
|------|------|-------------|---------|
| 0 | Off | Dark | Idle / clear |
| 1 | Random Flash | Random colors, 200ms | Manual |
| 2 | Slow Breathe | Fade random hue | Manual |
| 3 | **Attention** | Fast strobe random | **Claude needs input** |
| 4 | Rainbow | HSV hue cycle | Manual |
| 5 | **Task Complete** | Green pulse, 1s cycle | **Claude finished** |
| 6 | Network Offline | Solid pink | WiFi disconnected |
| 7 | **Error** | Red strobe | **Claude error** |

## Using HTTP Instead of MQTT (Same Subnet)

If your device and computer are on the same subnet, you can use HTTP instead. Get your device's IP from `wifi-status`, then configure hooks with `curl`:

```json
{
  "hooks": {
    "Stop": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "curl -s -m 2 'http://192.168.1.100/notify?state=5' || true"
          }
        ]
      }
    ],
    "Notification": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "curl -s -m 2 'http://192.168.1.100/notify?state=3' || true"
          }
        ]
      }
    ]
  }
}
```

Replace `192.168.1.100` with your device's actual IP. If mDNS works on your network, use `clawopenwr-XXXX.local` instead.

### HTTP Endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /notify?state=N` | Set LED to mode N (0-7) |
| `GET /status` | JSON: mode, IP, MAC, hostname, uptime |
| `GET /ping` | Returns "pong" (health check) |

## REPL Commands

Connect via USB serial (115200 baud) to access the Forth REPL.

### WiFi

| Command | Description |
|---------|-------------|
| `wifi-ssid` | Set WiFi SSID (interactive prompt) |
| `wifi-pass` | Set WiFi password (Enter for open network) |
| `wifi-connect` | Save credentials to flash and connect |
| `wifi-status` | Show SSID, IP, hostname, connection state |
| `wifi-clear` | Erase stored WiFi credentials |

### MQTT

| Command | Description |
|---------|-------------|
| `mqtt-broker` | View/change broker URI (saved to flash) |
| `mqtt-status` | Show broker, topic, connected state |
| `mqtt-topic` | Print the subscription topic |

### LED Control

| Command | Description |
|---------|-------------|
| `N blinky` | Set mode 0-7 |
| `modes` | List all modes |
| `mode?` | Show current mode |
| `R G B led-rgb` | Set LED to exact color (stops pattern) |
| `led-off` | Turn off LED |

### Examples

```
ok> wifi-status
SSID:      'MyNetwork'
Connected: yes
IP:        192.168.1.100
Hostname:  clawopenwr-aaa4.local
Mode:      0 (Off)
Free heap: 80124 bytes

ok> mqtt-status
Broker:    mqtt://broker.hivemq.com:1883
Topic:     iotj/cl/openwr/updates/aaa4
Connected: yes

ok> 3 blinky
Mode 3: Attention (Fast Strobe)

ok> 0 blinky
Mode 0: Off
```

## Battery / Headless Operation

Once WiFi is configured, the device runs fully unattended on battery:

1. Power on -> auto-connects WiFi -> connects to MQTT broker -> starts HTTP server
2. Receives notifications via MQTT (cross-subnet) or HTTP (local)
3. No USB connection required

**To reset WiFi**: hold the button for 5 seconds to clear credentials and reboot.

## Hardware

- **Board**: M5Stamp C3U (ESP32-C3, RISC-V, 4MB flash)
- **LED**: SK6812 NeoPixel on GPIO 2
- **Button**: GPIO 9 (press to cycle modes 0-4, hold 5s to reset WiFi)
- **Console**: USB-C (USB-serial-JTAG, 115200 baud)

## Changing the MQTT Broker

The default public broker (`broker.hivemq.com`) requires no setup. To use a private broker:

```
ok> mqtt-broker
Current: mqtt://broker.hivemq.com:1883
New broker URI (Enter to keep): mqtt://my-broker.example.com:1883
Broker set to: mqtt://my-broker.example.com:1883
```

Then update your Claude Code hooks to publish to the same broker.

## Memory Usage

| Metric | Value |
|--------|-------|
| Flash | 935 KB (89% of 1 MB) |
| RAM | 89 KB (27% of 328 KB) |
| Forth heap | 48 KB |
| Free heap | ~80 KB |
