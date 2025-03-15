/**
  * sketch_insertcodename_esp32thingplus_wifideus.ino
  * 
  * This is based on 
  * sketch_insertcodename_esp32thingplus_wifi.ino created by AC
  * 
  * It includes wifi changes to support a more robust wifi pairing
  * than the hardcoded design.
  * 
  * It still requires some knowledge of the current network to configure
  * the broadcast address, but perhaps that can be fixed to figure that
  * out based on the assigned IP address.
  * 
 **/
///
/// Modified to use WifiManager: https://github.com/tzapu/WiFiManager/blob/master/examples/AutoConnect/AutoConnectWithFeedbackLED/AutoConnectWithFeedbackLED.ino
///
/// AXYZ Modify according to this post conceptually
/// https://community.particle.io/t/multicast-udp-tutorial/19900
///
/// But use ESP32 related headers: https://www.alejandrowurts.com/projects/bb9e-v1-update-7-esp32-bilateral-coms/
///
/// Use Broadcast address (I could not get the multicast working, I think we need all hosts to join
/// a multicast group).
///
/// Testing from PC host: nc -l -u -k 10.0.0.255 50001
/// (use your network's broadcast address)
///
/// We've added in a simulation of how the Particle Boron will operate, wake up once a minute and try to get one good packet
///

#include <HardwareSerial.h>
#include <WiFiUdp.h>
#include "WiFiManager.h"          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
//for LED status
#include <Ticker.h>

#define VERS	"0.6.2"
// Reference Diagram for RXD1, TXD1
// https://cdn.sparkfun.com/assets/learn_tutorials/8/5/2/ESP32ThingPlusV20.pdf

#define RXD1  16
#define TXD1  17
#define UART_BAUDRATE   115200

uint8_t  uartRxBuff[1024];
int  rxPos = 0;
int  cmdLength = 0;
uint8_t  cmdType = 0;
long lastRxReceive = 0;


#ifndef LED_BUILTIN
#define LED_BUILTIN 13 // If your ESP32 DOES NOT DEFINE LED_BUILTIN
#endif

int LED = LED_BUILTIN;
Ticker ticker;

// According to https://quadmeup.com/arduino-esp32-and-3-hardware-serial-ports/
// Serial2 and Serial3 don't work and have to be accessed through hardwareSerial
HardwareSerial MySerial(2);

// According to hmi.c, the string array is 20 elements
const size_t bufferSize = 20;
const int MAX_SERIAL_WAIT_TIME = 10000;

int i;
const int ledPin = 13;
int ret = 0;
char tofStr[bufferSize];
float tofData;

bool useOldRead = false;
const int SLEEP_DURATION = 1000*10;

WiFiUDP  udp;

// char buffer[50];
// 239.1.1.234  239.255.255.250
const IPAddress multicastaddr = IPAddress(239,255,255,250);
// TODO: Change this to your personal network's broadcast address, 192.168.1.255, for example
const IPAddress broadcastaddr = IPAddress(10,0,0,255); // 10.0.0.255
const int mcportno = 50001;

int counter = 0;

void tick()
{
  //toggle state
  digitalWrite(LED, !digitalRead(LED));     // set pin to the opposite state
}

//gets called when WiFiManager enters configuration mode
void configModeCallback (WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
  //entered config mode, make led toggle faster
  ticker.attach(0.2, tick);
}

void setup()
{
    Serial.begin(115200);
    // I'm using Serial2, called MySerial, and defined through hardwareSerial
    MySerial.begin(UART_BAUDRATE, SERIAL_8N1, RXD1, TXD1);
    pinMode(LED, OUTPUT);      // set the LED pin mode
    delay(100); // Why dis?

    // start ticker with 0.5 because we start in AP mode and try to connect
  ticker.attach(0.6, tick);

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wm;
  //reset settings - for testing
  // wm.resetSettings();

  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wm.setAPCallback(configModeCallback);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  if (!wm.autoConnect()) {
    Serial.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(1000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  ticker.detach();
  //keep LED on
  digitalWrite(LED, LOW);

  udp.begin(0);
}

void loop()
{
  byte incomingByte[bufferSize];
  counter++;
  udp.beginPacket(broadcastaddr, mcportno);
  
  delay(SLEEP_DURATION); // Note: This is just to simulate a "long" sleep and then wake up and start reading
  byte bytein;
  int datalen = 0; // make this local not global
  // OLD Methodology for the read
  if (useOldRead) {
  while(MySerial.available() > 0) {
    ticker.attach(0.5, tick);
    for (i = 0; i < bufferSize; i++) {
        incomingByte[i] = MySerial.read(); //  Read incoming string, 20 bytes
        if (incomingByte[i] == '\n') {
          datalen = i;
        }
    }
    ticker.attach(0.5, tick);
  }

  } else {

  // New Methodology for the read
  // Inspired by: https://forum.arduino.cc/t/converting-int-or-byte-to-ascii/523311/14
  unsigned long _time = millis();
  char c = 0;
  i = 0; // Reset the index
  while (!MySerial.available()) {
    if ((millis() - _time) > MAX_SERIAL_WAIT_TIME) {
      Serial.print("WARNING: Max wait time exceeded waiting for UART, is there a connection problem?\n");
      break;
    }
  }
  
  while ((c = MySerial.read()) != '\r') {
    // Toss out everything to here
    Serial.printf("Tossing out '%c'\n",  c);
  }
  Serial.printf("Final toss out '%c'\n",  c);
  
  // Then try to do one complete good read
  while ((c = MySerial.read()) != '\n') {
    
    if (c >= 32 && c <= 127) { // check if ascii else discard
      incomingByte[i] = c;
      i++;
      // We can't exceed the buffer size, so reset our index and toss out our data
      /*
      if (i == bufferSize-1) {
        i = 0;
      }
      */
    }
  }
  
  incomingByte[i] = '\n';
  i++;
  datalen = i;
  }
  
  for (i = 0; i < datalen; i++) {
    tofStr[i] = char(incomingByte[i]); // Convert to characters
    // sprintf(buffer, "i = %d str = %c , ascii = $d\n", counter, tofStr[i], (int) tofStr[i]);
    // Serial.println(buffer);
    // Serial.printf("i = %d str = %c , ascii = $d\n",  counter, tofStr[i], int(incomingByte[i]));
    Serial.printf("i = %d str = %c ascii= ",  counter, tofStr[i]);
    Serial.print(incomingByte[i]);
    Serial.print("\n");
  }
  

  tofData = atof(tofStr); // atof creates a double, not sure if float is the same
  Serial.println(tofData,15); // need many decimals for value in seconds

  udp.printf("%d,", counter);
  // udp.write(incomingByte, datalen);
  udp.printf("%2.15f", tofData);
  udp.printf("\r\n");   // End segment
  // udp.write(incomingByte, datalen);  // print the char
  // udp.printf("\r\n");   // End segment
  if (udp.endPacket() == 0) { // Close communication
    Serial.println("send failed");
  } else {
    // Serial.println("sent");
  }
}
