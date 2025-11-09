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

//     http://www.apache.org/licenses/LICENSE-2.0
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


#define SWVERSION "0.0.3"

#define BUTTON      9   // C6/H2 Boot button
#define USER_BUTTON           BUTTON
#define OT_CHANNEL            "24" // TODO: let the device "agent" choose the channel
#define OT_NETWORK_KEY        "00112233445566778899aabbccddeeff" // keccaksum -N 128 somefile
// #define OT_PANID              "0xface"
// #define OT_MESH_PREFIX        "fdde:ad00:beef:cafe"
                               
#define OT_MCAST_ADDR         "ff05::abcd"
#define OT_COAP_RESOURCE_NAME "Lamp"

// Additional changes for m5nanoc6
#define LED         2
#define NUMPIXELS   1
#define M5NANO_C6_RGB_LED_PWR_PIN  19
#define M5NANO_C6_RGB_LED_DATA_PIN 20

#ifdef LED_BUILTIN
  #define LED_PIN     LED_BUILTIN
#else
  #define LED_PIN     13
#endif
#define RGB_BUILTIN LED_PIN

#if ( defined(ARDUINO_M5STACK_NANOC6) )
  Adafruit_NeoPixel pixels(NUMPIXELS, M5NANO_C6_RGB_LED_DATA_PIN, NEO_GRB + NEO_KHZ800);
#else
  Adafruit_NeoPixel pixels(NUMPIXELS, LED, NEO_GRB + NEO_KHZ800);
#endif

static String eui64 = "0x0000";

String getThreadEui64() {
  // Execute CLI command to get EUI-64
  OThreadCLI.println("eui64");
  delay(100);  // Brief wait for response
  char resp[64] = {0};
  size_t len = OThreadCLI.readBytesUntil('\n', resp, sizeof(resp) - 1);
  if (len > 0) {
    resp[len] = '\0';
    String s(resp);
    s.trim();
    if (s.length() == 16 && s.indexOf(':') == -1) {  // Expect hex like "0011223344556677"
      return s;  // Full 16-char hex EUI-64
    }
  }
  return "0000000000000000";  // Fallback
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

// this function is used by the Lamp mode to listen for CoAP frames from the Switch Node
void otCOAPListen() {
  // waits for the client to send a CoAP request
  char cliResp[256] = {0};
  size_t len = OThreadCLI.readBytesUntil('\n', cliResp, sizeof(cliResp));
  cliResp[len - 1] = '\0';
  if (strlen(cliResp)) {
    String sResp(cliResp);
    // cliResp shall be something like:
    // "coap request from fd0c:94df:f1ae:b39a:ec47:ec6d:15e8:804a PUT with payload: 30"
    // payload may be 30 or 31 (HEX) '0' or '1' (ASCII)
    log_d("Msg[%s]", cliResp);
    if (sResp.startsWith("coap request from") && sResp.indexOf("PUT") > 0) {
      char payload = sResp.charAt(sResp.length() - 1);  //  last character in the payload
      log_i("CoAP PUT [%s]\r\n", payload == '0' ? "OFF" : "ON");
      if (payload == '0') {
        for (int16_t c = 248; c > 16; c -= 8) {
        #if ( defined(ARDUINO_M5STACK_NANOC6) )
          // RED
          pixels.setPixelColor(0, pixels.Color(c, c, c));
          pixels.show();
        #else
          rgbLedWrite(RGB_BUILTIN, c, c, c);  // ramp down
        #endif
          delay(5);
        }
        #if ( defined(ARDUINO_M5STACK_NANOC6) )
          pixels.clear();
          pixels.show();
        #else
          rgbLedWrite(RGB_BUILTIN, 0, 0, 0);  // Lamp Off
        #endif
      } else {
        for (int16_t c = 16; c < 248; c += 8) {
        #if ( defined(ARDUINO_M5STACK_NANOC6) )
          // RED
          pixels.setPixelColor(0, pixels.Color(c, c, c));
          pixels.show();
        #else
          rgbLedWrite(RGB_BUILTIN, c, c, c);  // ramp up
        #endif
          delay(5);
        }
      #if ( defined(ARDUINO_M5STACK_NANOC6) )
        // RED
        pixels.setPixelColor(0, pixels.Color(255, 255, 255));
        pixels.show();
      #else
        rgbLedWrite(RGB_BUILTIN, 255, 255, 255);  // Lamp On
      #endif
      }
    } else {
      Serial.println("Received unexpected message: ");
    }
  }
}

// this function is used to listen for CoAP chat messages
void otChatListen() {
  // waits for the client to send a CoAP request
  char cliResp[512] = {0};
  size_t len = OThreadCLI.readBytesUntil('\n', cliResp, sizeof(cliResp));
  cliResp[len] = '\0';  // ensure null-terminated
  if (strlen(cliResp)) {
    String sResp(cliResp);
    sResp.trim();
    log_d("CLI[%s]", cliResp);
    if (sResp.startsWith("coap request from")) {
      // Parse: coap request from [addr] METHOD with payload: hex
      size_t fromPos = sResp.indexOf("from ") + 5;
      size_t endBracket = sResp.indexOf(']', fromPos);
      if (endBracket != -1) {
        String fromAddr = sResp.substring(fromPos, endBracket);
        size_t methodStart = endBracket + 1;
        while (methodStart < sResp.length() && sResp[methodStart] == ' ') methodStart++;
        size_t methodEnd = sResp.indexOf(' ', methodStart);
        if (methodEnd == -1) methodEnd = sResp.length();
        String method = sResp.substring(methodStart, methodEnd);
        size_t payloadStart = sResp.indexOf("payload: ", methodEnd);
        if (payloadStart != -1) {
          payloadStart += 9;
          String hexPayload = sResp.substring(payloadStart);
          hexPayload.trim();
          String textMsg = hexToAscii(hexPayload);
          Serial.println("Received from " + fromAddr + " (" + method + "): " + textMsg);
          // Optional: blink LED to indicate message received
          rgbLedWrite(RGB_BUILTIN, 0, 0, 255);  // Blue flash
          delay(100);
          rgbLedWrite(RGB_BUILTIN, 0, 64, 8);  // Back to green
        }
      }
    }
  }
}

bool otExecCommandMulti(const char* fullcmd) {
  OThreadCLI.println(fullcmd);
  char resp[256];
  unsigned long start = millis();
  while (millis() - start < 2000) {  // 2s timeout
    if (OThreadCLI.available()) {
      size_t len = OThreadCLI.readBytesUntil('\n', resp, sizeof(resp) - 1);
      if (len > 0) {
        resp[len] = '\0';
        String s(resp);
        s.trim();
        if (s == "Done") {
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
  // -- start CoAP as client
  "coap", "start"
};

const char *otCoapLamp[] = {
  // -- create a multicast IPv6 Address for this device
  "ipmaddr add", OT_MCAST_ADDR,
  // -- start and create a CoAP resource
  // start CoAP as server
  "coap", "start",
  // create a CoAP resource
  "coap resource", OT_COAP_RESOURCE_NAME,
  // set the CoAP resource initial value
  "coap set", "0"
};

bool otDeviceSetup(
  const char **otSetupCmds, uint8_t nCmds1, const char **otCoapCmds, uint8_t nCmds2, ot_device_role_t expectedRole1, ot_device_role_t expectedRole2
) {
  Serial.println("Starting OpenThread.");
  bool isLeader = (expectedRole1 == expectedRole2);

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
    // RED
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
  #else
    rgbLedWrite(RGB_BUILTIN, 255, 0, 0);  // RED ... failed!
  #endif
    return false;
  }
  Serial.println("OpenThread started.\r\nWaiting for activating correct Device Role.");
  // wait for the expected Device Role to start
  uint8_t tries = 24;  // 24 x 2.5 sec = 1 min
  if (isLeader) {
    while (tries && otGetDeviceRole() != expectedRole1) {
      Serial.print(".");
      delay(2500);
      tries--;
    }
  } else {
    while (tries && otGetDeviceRole() != expectedRole1 && otGetDeviceRole() != expectedRole2) {
      Serial.print(".");
      // Serial.print(otGetDeviceRole());
      delay(2500);
      tries--;
    }
  }
  Serial.println();
  if (!tries) {
    log_e("Sorry, Device Role failed by timeout! Current Role: %s.", otGetStringDeviceRole());
  #if ( defined(ARDUINO_M5STACK_NANOC6) )
    // RED
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
  #else
    rgbLedWrite(RGB_BUILTIN, 255, 0, 0);  // RED ... failed!
  #endif
    return false;
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
    // RED
    pixels.setPixelColor(0, pixels.Color(255, 0, 0));
    pixels.show();
  #else
    rgbLedWrite(RGB_BUILTIN, 255, 0, 0);  // RED ... failed!
  #endif
    return false;
  }
  Serial.println("OpenThread setup done. Node is ready.");

  if (isLeader) {
    // all fine! LED goes Green
  #if ( defined(ARDUINO_M5STACK_NANOC6) )
    // GREEN
    pixels.setPixelColor(0, pixels.Color(0, 255, 0));
    pixels.show();
  #else
    rgbLedWrite(RGB_BUILTIN, 0, 64, 8);  // GREEN ... Lamp is ready!
  #endif
  } else {
    // all fine! LED goes and stays Blue
  #if ( defined(ARDUINO_M5STACK_NANOC6) )
    // BLUE
    pixels.setPixelColor(0, pixels.Color(0, 0, 255));
    pixels.show();
  #else
    rgbLedWrite(RGB_BUILTIN, 0, 0, 64);  // BLUE ... Switch is ready!
  #endif
  }
  return true;
}

void setupChildNode() {
  // tries to set the Thread Network node and only returns when succeeded
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
  // tries to set the Thread Network node and only returns when succeeded
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
bool otCoapPUT(bool lampState) {
  bool gotDone = false, gotConfirmation = false;
  String coapMsg = "coap put ";
  coapMsg += OT_MCAST_ADDR;
  coapMsg += " ";
  coapMsg += OT_COAP_RESOURCE_NAME;
  coapMsg += " con 0";

  // final command is "coap put ff05::abcd Lamp con 1" or "coap put ff05::abcd Lamp con 0"
  if (lampState) {
    coapMsg[coapMsg.length() - 1] = '1';
  }
  OThreadCLI.println(coapMsg.c_str());
  log_d("Send CLI CMD:[%s]", coapMsg.c_str());

  char cliResp[256];
  // waits for the CoAP confirmation and Done message for about 1.25 seconds
  // timeout is based on Stream::setTimeout()
  // Example of the expected confirmation response: "coap response from fdae:3289:1783:5c3f:fd84:c714:7e83:6122"
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
  static bool lastLampState = true;  // first button press will turn the Lamp OFF from initial Green

#if ( defined(ARDUINO_M5STACK_NANOC6) )
  if (M5.BtnA.wasReleased()) {
    Serial.println("Button Released");
#else
  pinMode(USER_BUTTON, INPUT_PULLUP);  // C6/H2 User Button
  if (millis() > lastPress + debounceTime && digitalRead(USER_BUTTON) == LOW) {
#endif
    lastLampState = !lastLampState;
    if (!otCoapPUT(lastLampState)) {  // failed: Lamp Node is not responding due to be off or unreachable
      // timeout from the CoAP PUT message... restart the node.
    #if ( defined(ARDUINO_M5STACK_NANOC6) )
      // RED
      pixels.setPixelColor(0, pixels.Color(255, 0, 0));
      pixels.show();
    #else
      rgbLedWrite(RGB_BUILTIN, 255, 0, 0);  // RED ... something failed!
    #endif
      Serial.println("Resetting the Node as Switch... wait.");
      // start over...
      setupChildNode();
    }
    lastPress = millis();
  }
}

void setup() {
  Serial.begin(115200);
  // LED starts RED, indicating not connected to Thread network.
#if ( defined(ARDUINO_M5STACK_NANOC6) )
  pinMode(M5NANO_C6_RGB_LED_PWR_PIN, OUTPUT);
  digitalWrite(M5NANO_C6_RGB_LED_PWR_PIN, HIGH);
  auto cfg = M5.config();
  M5.begin(cfg);

  pixels.begin();
  pinMode(BUTTON, INPUT_PULLUP);

  delay(200);

  // RED
  pixels.setPixelColor(0, pixels.Color(255, 0, 0));
  pixels.show();
#else
  rgbLedWrite(RGB_BUILTIN, 64, 0, 0);
#endif
  OThreadCLI.begin(false);     // No AutoStart is necessary
  OThreadCLI.setTimeout(250);  // waits 250ms for the OpenThread CLI response


  // Perform up to 5 scans to find a match for channel 24 and our sessionkey
  // If we don't find it, it's ok to go ahead and elect ourselves leader
  int count = 0;
  // while(otGetDeviceRole() < OT_ROLE_CHILD) {
  while(count < 2) {
    // bool otGetRespCmd(const char *cmd, char *resp = NULL, uint32_t respTimeout = 5000);
    /* if (!otPrintRespCLI("scan", Serial, 3000)) {
      Serial.println("Scan Failed...");
    }
    */
    // Buffer to hold CLI response
    char respBuf[1024];  
    memset(respBuf, 0, sizeof(respBuf));

    // Run an active scan
    int ret = otGetRespCmd("scan", respBuf, /* sizeof(respBuf), */ 5000);

    if (ret == 1) {
      Serial.println("Scan results:");
      Serial.println(respBuf);

      // Example parsing: each line looks like
      // " | Channel | PAN ID | Ext PAN ID | RSSI | LQI |"
      char *line = strtok(respBuf, "\r\n");

      while (line != NULL) {
        // Serial.print("Found: ");
        // Serial.println(line);

        //
        // Parse the scan for an existin channel that matches ours
        //
        MatchState ms;
        // ms.Target((char*)line);
        ms.Target(line);
        char result[32];

        
        // Regex pattern
        const char *pattern =  "^|%s*(%x+)%s*|%s*(%x+)%s*|%s*(-?%d+)%s*|%s*(-?%d+)%s*|%s*(-?%d+)%s*|%s*$";

        if (ms.Match(pattern) == REGEXP_MATCHED) {
          ms.GetCapture(result, 2);
          // int chan = atoi(result);
          if (strcmp(result, OT_CHANNEL) == 0) {
            Serial.print("Found target channel: ");
            Serial.print(result);
            Serial.print("\n");
            //
            // We can break now and setup ourselves as a child
            count = -1;
            break;
          }
          /*
          ms.GetCapture(result, 0); Serial.print("PAN: "); Serial.println(result);
          ms.GetCapture(result, 1); Serial.print("MAC: "); Serial.println(result);
          ms.GetCapture(result, 2); Serial.print("Channel: "); Serial.println(result);
          ms.GetCapture(result, 3); Serial.print("dBm: "); Serial.println(result);
          ms.GetCapture(result, 4); Serial.print("LQI: "); Serial.println(result);
          */
          
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
  // LED goes and keeps Blue when all is ready and Red when failed.
  eui64 = getThreadEui64();
  String startup = "Starting MagNET Thread CoAP Hanasu " + String(SWVERSION) + String(" Thread EUI64: ") + eui64;
  Serial.println(startup);
  // Serial.println(std::string("Starting MagNET Thread CoAP Hanasu " + std::string(SWVERSION)).c_str() + std::string(" EUI64:").c_str());
}

void loop() {
  checkUserButton();
  otCOAPListen();
  // otChatListen();
  if (Serial.available()) {
    Serial.println("Sending chat message");
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      bool isDirect = input.startsWith("@");
      String addr, msg;
      if (isDirect) {
        size_t colonPos = input.indexOf(':', 1);
        if (colonPos != -1) {
          addr = input.substring(1, colonPos);
          msg = input.substring(colonPos + 1);
          msg.trim();
        } else {
          Serial.println("Invalid direct format. Use @addr: message");
          return;
        }
      } else {
        addr = String(OT_MCAST_ADDR);
        msg = input;
      }
      if (msg.length() > 0) {
        String fullcmd = "coap post " + addr + " " + OT_COAP_RESOURCE_NAME + " con " + msg;
        if (otExecCommandMulti(fullcmd.c_str())) {
          Serial.println("Sent to " + (isDirect ? addr : "multicast") + ": " + msg);
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
