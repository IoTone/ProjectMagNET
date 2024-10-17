/**
 * 2024 IoTone, Inc.
 */
#include "M5Atom_DigDug_Camera_Button.h"
/*
  Mag*NET Button is an M5Stack ESP32 based button.
  It should be configurable to act as a button for anything.  Our example will be used
  to implement the standard servcies.  Ideally this button can fire a camera shutter or
  do something like activate a microphone.

  ## Design Goals

  On a phone, the UI generated is a traditional button in the style of a stateful widget.
  In stealing a concept from social media, we follow specific things.  And we like (true)
  certain things.  So devices could like each other, forming a relationship, and then a
  camera shutter could subscribe to a button press.  No technical terms.  

  In spacial AR, this button could be linked to a camera shutter via a visual interface.
  Drag and drop an arrow between them.

  In a pure machine terms, an AI could orchestrate this relationship without human
  intervention.  Ideally though, there is something like OAuth (but not Oauth) in terms
  of letting a user intervene in approving relationships between devices.

  This only covers a BLE use case.

  ## Background on physical buttons

  The originator of a button that does something when pressed is probably a few hundred
  years ago.  The notion of wearing buttons probably showed up commonly in Star Wars.
  Surprisingly nobody has made a wearable button trigger a commonplace thing.  Due to 
  latency of voice capture / wakeword / ai along with energy cost, it makes sense to have
  a human push a button to trigger something.

  Based on DigDug BLE PoC 3

  Goal of PoC 3 is to wire an external button that will easily be used as a wearable
  and get this to advertise as

  XevIOT_Poc3

  Any button presses update tte characteristic

  =================
  
  Blink + BLE Example

``BLE Example: https://wiki.seeedstudio.com/XIAO-BLE-Sense-Bluetooth-Usage/

  Turns an LED on for one second, then off for one second, repeatedly.

  Most Arduinos have an on-board LED you can control. On the UNO, MEGA and ZERO
  it is attached to digital pin 13, on MKR1000 on pin 6. LED_BUILTIN is set to
  the correct LED pin independent of which board is used.
  If you want to know what pin the on-board LED is connected to on your Arduino
  model, check the Technical Specs of your board at:
  https://www.arduino.cc/en/Main/Products

  modified 8 May 2014
  by Scott Fitzgerald
  modified 2 Sep 2016
  by Arturo Guadalupi
  modified 8 Sep 2016
  by Colby Newman

  This example code is in the public domain.

  http://www.arduino.cc/en/Tutorial/Blink
*/

#include "M5Atom.h"
//#include <ArduinoBLE.h>                           		//-- moved to arduinoGlue.h

	//-- moved to arduinoGlue.h // #define VERSION "0.0.4"

#ifndef LED_BUILTIN
	//-- moved to arduinoGlue.h // #define LED_BUILTIN 13  // If your ESP32 DOES NOT DEFINE LED_BUILTIN
#endif

BLEService ledService("7d841d1c-869e-48b6-b882-924fd32766ef"); // Bluetooth® Low Energy LED Service

// Bluetooth® Low Energy LED Switch Characteristic - custom 128-bit UUID, read and writable by central
BLEByteCharacteristic switchCharacteristic("7d841d1c-869e-48b6-b882-924fd32766ee", BLERead | BLEWrite);
BLEByteCharacteristic buttonCharacteristic("7d841d1c-869e-48b6-b882-924fd32766ed", BLERead | BLENotify);

/*				*** enum moved to arduinoGlue.h ***
enum ButtonState {
  B_On,             // Camera Shooting
  B_Off,            // Camera Not Shooting
  B_Pwr             // Camera Off
};
*/
static ButtonState g_btn_state = B_Off;

const int ledPin = 19; // LED_BUILTIN; // pin to use for the LED

const int BUTTON_PIN = 0; // the number of the pushbutton pin
int lastState = HIGH; // the previous state from the input pin
int currentState;    // the current reading from the input pin

void setup() {
  Serial.begin(115200);
  //Initialize LED PIN
  M5.begin(true, false, true);

  while (!Serial);

  // set LED pin to output mode
  pinMode(ledPin, OUTPUT);

  // initialize digital pin LED_BUILTIN as an output.
  
  pinMode(ledPin, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // begin initialization
  if (!BLE.begin()) {
    Serial.println("starting Bluetooth® Low Energy module failed!");

    while (1);
  }

  // set advertised local name and service UUID:
  BLE.setLocalName("DigDug_Btn_Poc3");
  BLE.setAdvertisedService(ledService);

  // add the characteristic to the service
  ledService.addCharacteristic(switchCharacteristic);
  ledService.addCharacteristic(buttonCharacteristic);
  // add service
  BLE.addService(ledService);

  // set the initial value for the characeristic:
  switchCharacteristic.writeValue(0);
  buttonCharacteristic.writeValue(lastState);
  
  // start advertising
  BLE.advertise();

  Serial.println("BLE LED Peripheral");

  M5.dis.clear();
}

void loop() {
  // listen for Bluetooth® Low Energy peripherals to connect:
  BLEDevice central = BLE.central();

  // if a central is connected to peripheral:
  if (central) {
    Serial.print("Connected to central: ");
    // print the central's MAC address:
    Serial.println(central.address());
    // while the central is still connected to peripheral:
  while (central.connected()) {
      if (switchCharacteristic.written()) {
        if (switchCharacteristic.value()) {   
          Serial.println("LED on");
          digitalWrite(ledPin, LOW); // changed from HIGH to LOW       
        } else {                              
          Serial.println(F("LED off"));
          digitalWrite(ledPin, HIGH); // changed from LOW to HIGH     
        }
      }

      // read the state of the switch/button:
      currentState = digitalRead(BUTTON_PIN);
    
      if(lastState == LOW && currentState == HIGH) {
        Serial.println("The state changed from LOW to HIGH");
        
      }
        
      // save the last state
      if (currentState != lastState) {
        buttonCharacteristic.writeValue(lastState);
      }
      lastState = currentState;
   }

    // when the central disconnects, print it out:
    Serial.print(F("Disconnected from central: "));
    Serial.println(central.address());
  }

  if (M5.Btn.wasPressed()) {
    M5.dis.drawpix(0,0x00C0FF);
    if (g_btn_state == B_Off) {
      g_btn_state = B_On;
    } else {
      g_btn_state = B_Off;
    }
    digitalWrite(ledPin, HIGH);  // turn the LED on (HIGH is the voltage level)
    delay(100);                      // wait for a second
    digitalWrite(ledPin, LOW);   // turn the LED off by making the voltage LOW
    // delay(100);                      // wait for a second
  } /* else if (M5.Btn.isReleased()) {
    M5.dis.drawpix(0,0xF08000);
  } */

  if (g_btn_state == B_Off) {
    // M5.dis.drawpix(0,0xF08000);
    // Serial.println("Button is OFF");
    M5.dis.drawpix(0,0xFF00FF); /* PURPLE */
  } else {
    // Serial.println("Button is On");
    M5.dis.drawpix(0,0x008000);
  }
  // read the state of the switch/button:
  currentState = digitalRead(BUTTON_PIN);

  if(lastState == LOW && currentState == HIGH)
    Serial.println("The state changed from LOW to HIGH");

  // save the last state
  lastState = currentState;

  M5.update();
}