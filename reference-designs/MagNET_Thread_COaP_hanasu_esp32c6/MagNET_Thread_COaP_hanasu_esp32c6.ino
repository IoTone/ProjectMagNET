#define CORE_DEBUG_LEVEL 4
#define LOG_CAL_LEVEL CORE_DEBUG_LEVEL
/**
 * Portions by 2025 IoTone Japan
 */
// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
/*
  Mag*NET Thread CoAP hanasu esp32c6
  The goal is a generic OpenThread based COAP chat server that forms a P2P mesh
  This prototype will just converse autonomously with anyone on the same network channel
  ## Design Goals
  - ZeroConf
  - has a serial UART interface for configuration and communication from a host agent
  - Doesn't require special firmware, partitions or compile time settings on ESP32C6 devices
  (unlike the zigbee code)
  - Able to switch between modes of operation, leader, router, or end device
*/
/**
 *
 * Based on a mix of examples from
 * https://github.com/espressif/arduino-esp32/tree/master/libraries/OpenThread/examples/CLI/COAP
 *
 */
#include "OThreadCLI.h"
#include "OThreadCLI_Util.h"
#include <Adafruit_NeoPixel.h>
#if ( defined(ARDUINO_M5STACK_NANOC6) )
#include <M5Unified.h>
#endif

#include <Regexp.h>
#define SWVERSION "0.0.6"
#define BUTTON 9 // C6/H2 Boot button
#define USER_BUTTON BUTTON
#define OT_CHANNEL "24" // TODO: let the device "agent" choose the channel
#define OT_NETWORK_KEY "00112233445566778899aabbccddeeff" // keccaksum -N 128 somefile
// #define OT_PANID "0xface"
// #define OT_MESH_PREFIX "fdde:ad00:beef:cafe"
                              
#define OT_MCAST_ADDR "ff05::abcd"
#define OT_COAP_RESOURCE_NAME "Lamp"
#define OT_COAP_CHAT_RESOURCE_NAME "chat"  // Unified chat resource for all nodes
// Additional changes for m5nanoc6
#define LED 2
#define NUMPIXELS 1
#define M5NANO_C6_RGB_LED_PWR_PIN 19
#define M5NANO_C6_RGB_LED_DATA_PIN 20
#ifdef LED_BUILTIN
  #define LED_PIN LED_BUILTIN
#else
  #define LED_PIN 13
#endif
#define RGB_BUILTIN LED_PIN
#if ( defined(ARDUINO_M5STACK_NANOC6) )
  Adafruit_NeoPixel pixels(NUMPIXELS, M5NANO_C6_RGB_LED_DATA_PIN, NEO_GRB + NEO_KHZ800);
#else
  Adafruit_NeoPixel pixels(NUMPIXELS, LED, NEO_GRB + NEO_KHZ800);
#endif
static String eui64 = "0x0000";
static bool isLeader = false;
static String currentChatPayload = "";
String getThreadEui64() {
  // Execute CLI command to get EUI-64
  OThreadCLI.println("eui64");
  delay(100); // Brief wait for response
  char resp[64] = {0};
  size_t len = OThreadCLI.readBytesUntil('\n', resp, sizeof(resp) - 1);
  if (len > 0) {
    resp[len] = '\0';
    String s(resp);
    s.trim();
    if (s.length() == 16 && s.indexOf(':') == -1) { // Expect hex like "0011223344556677"
      return s; // Full 16-char hex EUI-64
    }
  }
  return "0000000000000000"; // Fallback
}
String hexToAscii(String hex) {
  String res = "";
  hex.trim();
  for (size_t i = 0; i < hex.length(); i += 2) {
    if (i + 1 < hex.length()) {
      String byteStr = hex.substring(i, i + 2);
      char c = (char)strtol(byteStr.c_str(), NULL, 16);
      res += c;
    }
  }
  return res;
}

String stringToHex(const String& input) {
  String hex = "";
  for (size_t i = 0; i < input.length(); ++i) {
    char buf[3];
    sprintf(buf, "%02X", (unsigned char)input[i]);
    hex += buf;
  }
  return hex;
}

// this function is used by the Lamp mode to listen for CoAP frames from the Switch Node
void otCOAPListen() {
  // waits for the client to send a CoAP request
  char cliResp[256] = {0};
  size_t len = OThreadCLI.readBytesUntil('\n', cliResp, sizeof(cliResp));
  cliResp[len - 1] = '\0';
  if (strlen(cliResp)) {
    String sResp(cliResp);
    Serial.println(cliResp);
    if (sResp.startsWith("coap request from") && sResp.indexOf("PUT") > 0) {
      size_t payloadStart = sResp.indexOf("PUT with payload: ");
      size_t fromStart = sResp.indexOf("from ");
      size_t longID_start = fromStart + 5;
      size_t longID_end = longID_start + sResp.substring(longID_start).indexOf(" PUT ");
      String longID = sResp.substring(longID_start, longID_end);
      String shortID = longID.substring(longID.lastIndexOf(":") + 1);
      if (payloadStart >= 0) {
        payloadStart += 18;
        String hexPayload = sResp.substring(payloadStart);
        hexPayload.trim();

        String textMsg = hexToAscii(hexPayload);

        // First, check if it's a lamp control command (single '0' or '1')
        if (textMsg.length() == 1 && (textMsg[0] == '0' || textMsg[0] == '1')) {
          char payload = textMsg[0];
          log_i("CoAP PUT [%s]\r\n", payload == '0' ? "OFF" : "ON");
          if (isLeader) {  // Only leader controls the RGB LED for lamp
            if (payload == '0') {
              for (int16_t c = 248; c > 16; c -= 8) {
#if ( defined(ARDUINO_M5STACK_NANOC6) )
                pixels.setPixelColor(0, pixels.Color(c, c, c));
                pixels.show();
#else
                pixels.setPixelColor(0, pixels.Color(c, c, c));
                pixels.show();
                // rgbLedWrite(RGB_BUILTIN, c, c, c); // ramp down
#endif
                delay(5);
              }
#if ( defined(ARDUINO_M5STACK_NANOC6) )
              pixels.clear();
              pixels.show();
#else
              pixels.clear();
              pixels.show();
              // rgbLedWrite(RGB_BUILTIN, 0, 0, 0); // Lamp Off
#endif
            } else {
              for (int16_t c = 16; c < 248; c += 8) {
#if ( defined(ARDUINO_M5STACK_NANOC6) )
                pixels.setPixelColor(0, pixels.Color(c, c, c));
                pixels.show();
#else
                pixels.setPixelColor(0, pixels.Color(c, c, c));
                pixels.show();
                // rgbLedWrite(RGB_BUILTIN, c, c, c); // ramp up
#endif
                delay(5);
              }
#if ( defined(ARDUINO_M5STACK_NANOC6) )
              pixels.setPixelColor(0, pixels.Color(255, 255, 255));
              pixels.show();
#else
              pixels.setPixelColor(0, pixels.Color(255, 255, 255));
              pixels.show();
              // rgbLedWrite(RGB_BUILTIN, 255, 255, 255); // Lamp On
#endif
            }
          }
        }
        // Then, check if it's a chat message
        // Format will be 
        // coap put ff05::abcd chat non -x 636861743E2069732074686520636174
        // so look for -x
        // above is string: chat> is the cat
        Serial.println("textMsg=" + textMsg);
        size_t chatidx = textMsg.indexOf("-x");
        if (chatidx == 0) {
          String chatmsg = hexToAscii(textMsg.substring(chatidx + 2)).substring(5); // 2: -x 5: chat>
          currentChatPayload = longID + "," + shortID + "," + chatmsg;
          Serial.print(currentChatPayload);
          Serial.println("");
          // Human-readable chat output
          Serial.println("Chat from " + shortID + " (" + longID + "): " + chatmsg);
          // Blue flash for received chat
#if ( defined(ARDUINO_M5STACK_NANOC6) )
          pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          pixels.show();
          delay(100);
          if (isLeader) {
            pixels.setPixelColor(0, pixels.Color(0, 255, 0));
          } else {
            pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          }
          pixels.show();
#else
          /* rgbLedWrite(RGB_BUILTIN, 0, 0, 255);
          delay(100);
          if (isLeader) {
            rgbLedWrite(RGB_BUILTIN, 0, 64, 8);
          } else {
            rgbLedWrite(RGB_BUILTIN, 0, 0, 64);
          } */
          pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          pixels.show();
          delay(100);
          if (isLeader) {
            pixels.setPixelColor(0, pixels.Color(0, 255, 0));
          } else {
            pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          }
          pixels.show();
#endif
        } else if (textMsg.length() != 1 || (textMsg[0] != '0' && textMsg[0] != '1')) {
          Serial.println("Ignoring non-chat/non-lamp: " + textMsg + " of length " + textMsg.length());
        }
      }
    } else {
      Serial.print("Received unexpected message: ");
      Serial.println(sResp);
    }
  }
}
bool otExecCommandMulti(const char* fullcmd) {
  Serial.print("otExecCommandMulti: ");
  Serial.println(fullcmd);
  OThreadCLI.println(fullcmd);
  char resp[256];
  unsigned long start = millis();
  while (millis() - start < 2000) { // 2s timeout
    if (OThreadCLI.available()) {
      size_t len = OThreadCLI.readBytesUntil('\n', resp, sizeof(resp) - 1);
      if (len > 0) {
        resp[len] = '\0';
        String s(resp);
        s.trim();
        if (s == "Done") {
          Serial.println("Success");
          return true;
        }
        if (s.startsWith("Error")) {
          Serial.println("Command error: " + s);
          return false;
        }
      }
    }
    delay(10);
  }
  Serial.println("Command timeout");
  return false;
}
const char *otSetupChildNode[] = {
  // -- clear/disable all
  // stop CoAP
  "coap", "stop",
  // stop Thread
  "thread", "stop",
  // stop the interface
  "ifconfig", "down",
  // clear the dataset
  "dataset", "clear",
  // -- set dataset
  // set the channel
  "dataset channel", OT_CHANNEL,
  // set the network key
  "dataset networkkey", OT_NETWORK_KEY,
  // commit the dataset
  "dataset", "commit active",
  // -- network start
  // start the interface
  "ifconfig", "up",
  // start the Thread network
  "thread", "start"
};
const char *otSetupLeader[] = {
  // -- clear/disable all
  // stop CoAP
  "coap", "stop",
  // stop Thread
  "thread", "stop",
  // stop the interface
  "ifconfig", "down",
  // clear the dataset
  "dataset", "clear",
  // -- set dataset
  // create a new complete dataset with random data
  "dataset", "init new",
  // set the channel
  "dataset channel", OT_CHANNEL,
  // set the network key
  "dataset networkkey", OT_NETWORK_KEY,
  // commit the dataset
  "dataset", "commit active",
  // -- network start
  // start the interface
  "ifconfig", "up",
  // start the Thread network
  "thread", "start"
};
const char *otCoapSwitch[] = {
  // -- create a multicast IPv6 Address for this device
  "ipmaddr add", OT_MCAST_ADDR,
  // -- start CoAP as client
  "coap", "start",
  // create a CoAP resource (unified chat for P2P)
  "coap resource", OT_COAP_CHAT_RESOURCE_NAME
};
const char *otCoapLamp[] = {
  // -- create a multicast IPv6 Address for this device
  "ipmaddr add", OT_MCAST_ADDR,
  // -- start and create a CoAP resource
  // start CoAP as server
  "coap", "start",
  // create Lamp resource (for control)
  "coap resource", OT_COAP_RESOURCE_NAME,
  // create unified chat resource (for P2P chat reception)
  "coap resource", OT_COAP_CHAT_RESOURCE_NAME
};
bool otDeviceSetup(
  const char **otSetupCmds, uint8_t nCmds1, const char **otCoapCmds, uint8_t nCmds2, ot_device_role_t expectedRole1, ot_device_role_t expectedRole2
) {
  Serial.println("Starting OpenThread.");
  isLeader = (expectedRole1 == expectedRole2);
  if (isLeader) {
    Serial.println("Running as Lamp (RGB LED) - use the other C6/H2 as a Switch");
  } else {
    Serial.println("Running as Switch - use the BOOT button to toggle the other C6/H2 as a Lamp");
  }
  uint8_t i;
  for (i = 0; i < nCmds1; i++) {
    if (!otExecCommand(otSetupCmds[i * 2], otSetupCmds[i * 2 + 1])) {
      break;
    }
  }
  if (i != nCmds1) {
    log_e("Sorry, OpenThread Network setup failed!");
#if ( defined(ARDUINO_M5STACK_NANOC6) )
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
#else
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
    // rgbLedWrite(RGB_BUILTIN, 255, 0, 0); // RED ... failed!
#endif
    return false;
  }
  Serial.println("OpenThread started.\r\nWaiting for activating correct Device Role.");
  uint8_t tries = 24; // 24 x 2.5 sec = 1 min
  if (isLeader) {
    while (tries && otGetDeviceRole() != expectedRole1) {
      Serial.print(".");
      delay(2500);
      tries--;
    }
  } else {
    while (tries && otGetDeviceRole() != expectedRole1 && otGetDeviceRole() != expectedRole2) {
      Serial.print(".");
      delay(2500);
      tries--;
    }
  }
  Serial.println();
  if (!tries) {
    log_e("Sorry, Device Role failed by timeout! Current Role: %s.", otGetStringDeviceRole());
#if ( defined(ARDUINO_M5STACK_NANOC6) )
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
#else
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
    // rgbLedWrite(RGB_BUILTIN, 255, 0, 0); // RED ... failed!
#endif
    return false;
  }
  // Enable router eligibility on non-leader nodes for better multicast forwarding/reception
  if (!isLeader) {
    Serial.println("Enabling router eligibility (REED) for improved multicast support.");
    otExecCommandMulti("routereligible enable");
  }
  Serial.printf("Device is %s.\r\n", otGetStringDeviceRole());
  for (i = 0; i < nCmds2; i++) {
    if (!otExecCommand(otCoapCmds[i * 2], otCoapCmds[i * 2 + 1])) {
      break;
    }
  }
  if (i != nCmds2) {
    log_e("Sorry, OpenThread CoAP setup failed!");
#if ( defined(ARDUINO_M5STACK_NANOC6) )
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
#else
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
    // rgbLedWrite(RGB_BUILTIN, 255, 0, 0); // RED ... failed!
#endif
    return false;
  }
  Serial.println("OpenThread setup done. Node is ready.");
  if (isLeader) {
#if ( defined(ARDUINO_M5STACK_NANOC6) )
    pixels.setPixelColor(0, pixels.Color(0, 255, 0));
    pixels.show();
#else
    pixels.setPixelColor(0, pixels.Color(0, 255, 0));
    pixels.show();
    // rgbLedWrite(RGB_BUILTIN, 0, 64, 8); // GREEN ... Lamp is ready!
#endif
  } else {
#if ( defined(ARDUINO_M5STACK_NANOC6) )
    pixels.setPixelColor(0, pixels.Color(0, 0, 255));
    pixels.show();
#else
    pixels.setPixelColor(0, pixels.Color(0, 0, 255));
    pixels.show();
    // rgbLedWrite(RGB_BUILTIN, 0, 0, 64); // BLUE ... Switch is ready!
#endif
  }
  return true;
}
void setupChildNode() {
  bool startedCorrectly = false;
  while (!startedCorrectly) {
    startedCorrectly |= otDeviceSetup(
      otSetupChildNode, sizeof(otSetupChildNode) / sizeof(char *) / 2, otCoapSwitch, sizeof(otCoapSwitch) / sizeof(char *) / 2, OT_ROLE_CHILD, OT_ROLE_ROUTER
    );
    if (!startedCorrectly) {
      Serial.println("Setup Failed...\r\nTrying again...");
    }
  }
}
void setupLeaderNode() {
  bool startedCorrectly = false;
  while (!startedCorrectly) {
    startedCorrectly |=
      otDeviceSetup(otSetupLeader, sizeof(otSetupLeader) / sizeof(char *) / 2, otCoapLamp, sizeof(otCoapLamp) / sizeof(char *) / 2, OT_ROLE_LEADER, OT_ROLE_LEADER);
    if (!startedCorrectly) {
      Serial.println("Setup Failed...\r\nTrying again...");
    }
  }
}
// Sends the CoAP frame to the Lamp node
bool otLampCoapPUT(bool lampState) {
  bool gotDone = false, gotConfirmation = false;
  String coapMsg = "coap put ";
  coapMsg += OT_MCAST_ADDR;
  coapMsg += " ";
  coapMsg += OT_COAP_RESOURCE_NAME;
  coapMsg += " con ";
  coapMsg += lampState ? "1" : "0";
  Serial.print("otLampCoapPUT(): ");
  Serial.println(coapMsg);
  OThreadCLI.println(coapMsg.c_str());
  log_d("Send CLI CMD:[%s]", coapMsg.c_str());
  char cliResp[256];
  uint8_t tries = 5;
  *cliResp = '\0';
  while (tries && !(gotDone && gotConfirmation)) {
    size_t len = OThreadCLI.readBytesUntil('\n', cliResp, sizeof(cliResp));
    cliResp[len - 1] = '\0';
    log_d("Try[%d]::MSG[%s]", tries, cliResp);
    if (strlen(cliResp)) {
      if (!strncmp(cliResp, "coap response from", 18)) {
        gotConfirmation = true;
      }
      if (!strncmp(cliResp, "Done", 4)) {
        gotDone = true;
      }
    }
    tries--;
  }
  if (gotDone && gotConfirmation) {
    return true;
  }
  return false;
}
// this function is used by the Switch mode to check the BOOT Button and send the user action to the Lamp node
void checkUserButton() {
  static long unsigned int lastPress = 0;
  const long unsigned int debounceTime = 500;
  static bool lastLampState = true;
#if ( defined(ARDUINO_M5STACK_NANOC6) )
  if (M5.BtnA.wasReleased()) {
    Serial.println("Button Released");
#else
  pinMode(USER_BUTTON, INPUT_PULLUP);
  if (millis() > lastPress + debounceTime && digitalRead(USER_BUTTON) == LOW) {
#endif
    lastLampState = !lastLampState;
    if (!otLampCoapPUT(lastLampState)) {
#if ( defined(ARDUINO_M5STACK_NANOC6) )
      pixels.setPixelColor(0, pixels.Color(255, 0, 0));
      pixels.show();
#else
      pixels.setPixelColor(0, pixels.Color(255, 0, 0));
      pixels.show();
      // rgbLedWrite(RGB_BUILTIN, 255, 0, 0);
#endif
      Serial.println("problem sending lamp command");
      // XXX This is wrong way to handle this
      /* 
      Serial.println("Resetting the Node as Switch... wait.");
      setupChildNode();
      */
    }
    lastPress = millis();
  }
}

String getMyMeshIpv6() {
  OThreadCLI.println("ipaddr");
  delay(300);  // Allow time for multi-line response
  String addresses = "";
  char buf[128];
  while (OThreadCLI.available()) {
    size_t len = OThreadCLI.readBytesUntil('\n', buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = '\0';
      String line(buf);
      line.trim();
      if (line.length() > 0 && line.startsWith("fd")) {  // Mesh-local addresses start with "fd"
        addresses += line + "\n";
      }
    }
  }
  return addresses;
}

void setup() {
  Serial.begin(115200);
#if ( defined(ARDUINO_M5STACK_NANOC6) )
  pinMode(M5NANO_C6_RGB_LED_PWR_PIN, OUTPUT);
  digitalWrite(M5NANO_C6_RGB_LED_PWR_PIN, HIGH);
  auto cfg = M5.config();
  M5.begin(cfg);
  pixels.begin();
  pinMode(BUTTON, INPUT_PULLUP);
  delay(200);
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();
#else
pixels.begin();
  pinMode(BUTTON, INPUT_PULLUP);
  delay(200);
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();
  // rgbLedWrite(RGB_BUILTIN, 64, 0, 0);
#endif
  OThreadCLI.begin(false);
  OThreadCLI.setTimeout(250);
  int count = 0;
  while(count < 2) {
    char respBuf[1024];
    memset(respBuf, 0, sizeof(respBuf));
    int ret = otGetRespCmd("scan", respBuf, 5000);
    if (ret == 1) {
      Serial.println("Scan results:");
      Serial.println(respBuf);
      char *line = strtok(respBuf, "\r\n");
      while (line != NULL) {
        MatchState ms;
        ms.Target(line);
        char result[32];
        const char *pattern = "^|%s*(%x+)%s*|%s*(%x+)%s*|%s*(-?%d+)%s*|%s*(-?%d+)%s*|%s*(-?%d+)%s*|%s*$";
        if (ms.Match(pattern) == REGEXP_MATCHED) {
          ms.GetCapture(result, 2);
          if (strcmp(result, OT_CHANNEL) == 0) {
            Serial.print("Found target channel: ");
            Serial.println(result);
            count = -1;
            break;
          }
        } else {
          Serial.println("No existing PAN match found");
        }
        line = strtok(NULL, "\r\n");
      }
    } else {
      Serial.printf("Scan failed, error=%d\n", ret);
    }
    if (count >= 0) {
      delay(5000);
      count++;
    } else {
      break;
    }
  }
  if (count >= 0) {
    Serial.println("Setting up a leader node");
    setupLeaderNode();
  } else {
    Serial.println("Setting up a child node");
    setupChildNode();
  }
  eui64 = getThreadEui64();
  String startup = "Starting MagNET Thread CoAP Hanasu " + String(SWVERSION) + " Thread EUI64: " + eui64;
  Serial.println(startup);
  String myIpv6 = getMyMeshIpv6();
  if (myIpv6.length() > 0) {
    Serial.println("My Mesh-Local IPv6 Address(es):");
    Serial.println(myIpv6);
  } else {
    Serial.println("No IPv6 addresses found yet (try again after attach).");
  }
  Serial.println("P2P CoAP Chat ready!");
  Serial.println("Type messages to send multicast to all nodes (chat).");
  Serial.println("Or type @IPv6_address message to send directly to a peer.");
  Serial.println("Example: @fdde:ad00:beef::1 chat> Hello Bob!");
}

void loop() {
  checkUserButton();
  otCOAPListen();
  if (Serial.available()) {
    Serial.println("Sending chat message");
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      bool isDirect = input.startsWith("@");
      String addr, payload, confirmType;
      String displayMsg = input;  // Full user input for echo/debug
      confirmType = isDirect ? "con" : "non";
      if (isDirect) {
        size_t spacePos = input.indexOf(' ', 1);  // Space after @addr
        if (spacePos != -1) {
          addr = input.substring(1, spacePos);
          payload = input.substring(spacePos + 1);  // Everything after space (prefix + message)
          payload.trim();
        } else {
          Serial.println("Invalid direct format. Use @addr prefixed_message (space after address)");
          return;
        }
      } else {
        addr = String(OT_MCAST_ADDR);
        payload = input;  // Full input (user provides prefix like chat>)
      }
      if (payload.length() > 0) {
        String fullcmd;
        if (payload.startsWith("chat>")) {
          // Use hex encoding ONLY for chat> prefixed messages (preserves spaces/special chars reliably)
          String hexPayload = stringToHex(payload);
          fullcmd = "coap put " + addr + " " + OT_COAP_CHAT_RESOURCE_NAME + " " + confirmType + " -x" + hexPayload;
        } else {
          // For future non-chat prefixed commands (e.g., short "lamp>1"), use quoted plain text
          String quotedPayload = "\"" + payload + "\"";
          fullcmd = "coap put " + addr + " " + OT_COAP_CHAT_RESOURCE_NAME + " " + confirmType + " " + quotedPayload;
        }
        Serial.print("Sending command: ");
        Serial.println(fullcmd);  // Debug: exact CLI command
        if (otExecCommandMulti(fullcmd.c_str())) {
          Serial.println("Sent to " + (isDirect ? addr : "multicast") + ": " + displayMsg);
          Serial.println("Me [" + eui64 + "]: " + displayMsg);
          // Blue flash on send
#if ( defined(ARDUINO_M5STACK_NANOC6) )
          pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          pixels.show();
          delay(100);
          if (isLeader) {
            pixels.setPixelColor(0, pixels.Color(0, 255, 0));
          } else {
            pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          }
          pixels.show();
#else
          pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          pixels.show();
          delay(100);
          if (isLeader) {
            pixels.setPixelColor(0, pixels.Color(0, 255, 0));
          } else {
            pixels.setPixelColor(0, pixels.Color(0, 0, 255));
          }
          pixels.show();
#endif
        } else {
          Serial.println("Failed to send message.");
        }
      }
    }
  }
  delay(10);
#if ( defined(ARDUINO_M5STACK_NANOC6) )
  M5.update();
#endif
}