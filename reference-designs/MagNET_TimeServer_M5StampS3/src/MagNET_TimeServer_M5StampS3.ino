/**
 * Copyright 2023-2024 IoTone, Inc.
 *
 *
 * Original 
 * code Using: 
 * https://github.com/sparkfun/SparkFun_DS3234_RTC_Arduino_Library
 *
 * Revised to just use internal clock
 * 
 * Things to improve: 
 * send data as binary encoded uint8_t to improve access to the data
 * send a timestamp
 */
/** NimBLE_Service_Data_Advertiser Demo:
 *
 *  Simple demo of advertising service data that changes every 5 seconds
 * 
 *  Created: on February 7 2021
 *      Author: H2zero
 * 
*/
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLECharacteristic.h>
#include <SPI.h>
#include <ESP32Time.h>
#include <Wire.h>
#include <FastLED.h>

#define VERSION "0.0.5"
#define SERVICE_UUID "e529dbf6-8968-43e3-9930-9e3449e76fe8"

// NOTE: this is depricated , related to use of a XIAO with 
// an ancient sparkfun RTC.
//
// Comment out the line below if you want date printed before month.
// E.g. October 31, 2016: 10/31/16 vs. 31/10/16
#define PRINT_USA_DATE
// #define CONFIGURE_TIME
//////////////////////////////////
// Configurable Pin Definitions //
//////////////////////////////////
// https://github.com/espressif/arduino-esp32/blob/master/variants/XIAO_ESP32C3/pins_arduino.h
// RTC / XIAO ESP32C3
// GND - GND
// VCC - 3v3
// CLK - SCK
// MOSI - MOSI
// MISO - MISO
// SS   - D7 (SS)
// #define DS13074_CS_PIN SS // D7 // 10 // DeadOn RTC Chip-select pin
// #define INTERRUPT_PIN 2 // DeadOn RTC SQW/interrupt pin (optional)

//ESP32Time rtc;
ESP32Time rtc(3600);  // offset in seconds GMT+1

/* LED pin */
#ifndef LED_BUILTIN
#define LED_BUILTIN 13  // If your ESP32 DOES NOT DEFINE LED_BUILTIN
#endif

// M5Stamp Specific
#define PIN_BUTTON 0
#define PIN_LED    21
#define NUM_LEDS   1

#define MAX_TIME_BUFF_SIZE  30

// Serial fix up
// https://github.com/m5stack/STAMP-S3/compare/main...IoTone:STAMP-S3:main
/*
#if ARDUINO_USB_CDC_ON_BOOT
  #define S3USBSerial Serial
#else
  #if ARDUINO_USB_MODE
    #define S3USBSerial USBSerial
  #else
    #error "Please, board settings -> USB CDC On Boot=Enabled"
  #endif
#endif
*/

static NimBLEUUID dataUuid(SERVICE_UUID);
// static NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
static uint32_t count = 0;

static NimBLEServer* pServer;
NimBLECharacteristic *pTimeCharacteristic;
NimBLECharacteristic *pTimeStampCharacteristic;

/**  None of these are required as they will be handled by the library with defaults. **
 **                       Remove as you see fit for your needs                        */
class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
        Serial.println("Client connected");
        Serial.println("Multi-connect support: start advertising");
        NimBLEDevice::startAdvertising();
    };
    /** Alternative onConnect() method to extract details of the connection.
     *  See: src/ble_gap.h for the details of the ble_gap_conn_desc struct.
     */
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
        Serial.print("Client address: ");
        Serial.println(NimBLEAddress(desc->peer_ota_addr).toString().c_str());
        /** We can use the connection handle here to ask for different connection parameters.
         *  Args: connection handle, min connection interval, max connection interval
         *  latency, supervision timeout.
         *  Units; Min/Max Intervals: 1.25 millisecond increments.
         *  Latency: number of intervals allowed to skip.
         *  Timeout: 10 millisecond increments, try for 5x interval time for best results.
         */
        pServer->updateConnParams(desc->conn_handle, 24, 48, 0, 60);
    };
    void onDisconnect(NimBLEServer* pServer) {
        Serial.println("Client disconnected - start advertising");
        NimBLEDevice::startAdvertising();
    };
    void onMTUChange(uint16_t MTU, ble_gap_conn_desc* desc) {
        Serial.printf("MTU updated: %u for connection ID: %u\n", MTU, desc->conn_handle);
    };

/********************* Security handled here **********************
****** Note: these are the same return values as defaults ********/
    uint32_t onPassKeyRequest(){
        Serial.println("Server Passkey Request");
        /** This should return a random 6 digit number for security
         *  or make your own static passkey as done here.
         */
        return 123456;
    };

    bool onConfirmPIN(uint32_t pass_key){
        Serial.print("The passkey YES/NO number: ");Serial.println(pass_key);
        /** Return false if passkeys don't match. */
        return true;
    };

    void onAuthenticationComplete(ble_gap_conn_desc* desc){
        /** Check that encryption was successful, if not we disconnect the client */
        if(!desc->sec_state.encrypted) {
            NimBLEDevice::getServer()->disconnect(desc->conn_handle);
            Serial.println("Encrypt connection failed - disconnecting client");
            return;
        }
        Serial.println("Starting BLE work!");
    };
};

/** Handler class for characteristic actions */
class CharacteristicCallbacks: public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* pCharacteristic){
        Serial.print(pCharacteristic->getUUID().toString().c_str());
        Serial.print(": onRead(), value: ");
        Serial.println(pCharacteristic->getValue().c_str());
    };

    void onWrite(NimBLECharacteristic* pCharacteristic) {
        Serial.print(pCharacteristic->getUUID().toString().c_str());
        Serial.print(": onWrite(), value: ");
        Serial.println(pCharacteristic->getValue().c_str());
    };
    /** Called before notification or indication is sent,
     *  the value can be changed here before sending if desired.
     */
    void onNotify(NimBLECharacteristic* pCharacteristic) {
        Serial.println("Sending notification to clients");
    };


    /** The status returned in status is defined in NimBLECharacteristic.h.
     *  The value returned in code is the NimBLE host return code.
     */
    void onStatus(NimBLECharacteristic* pCharacteristic, Status status, int code) {
        String str = ("Notification/Indication status code: ");
        str += status;
        str += ", return code: ";
        str += code;
        str += ", ";
        str += NimBLEUtils::returnCodeToString(code);
        Serial.println(str);
    };

    void onSubscribe(NimBLECharacteristic* pCharacteristic, ble_gap_conn_desc* desc, uint16_t subValue) {
        String str = "Client ID: ";
        str += desc->conn_handle;
        str += " Address: ";
        str += std::string(NimBLEAddress(desc->peer_ota_addr)).c_str();
        if(subValue == 0) {
            str += " Unsubscribed to ";
        }else if(subValue == 1) {
            str += " Subscribed to notfications for ";
        } else if(subValue == 2) {
            str += " Subscribed to indications for ";
        } else if(subValue == 3) {
            str += " Subscribed to notifications and indications for ";
        }
        str += std::string(pCharacteristic->getUUID()).c_str();

        Serial.println(str);
    };
};

/** Handler class for descriptor actions */
class DescriptorCallbacks : public NimBLEDescriptorCallbacks {
    void onWrite(NimBLEDescriptor* pDescriptor) {
        std::string dscVal = pDescriptor->getValue();
        Serial.print("Descriptor witten value:");
        Serial.println(dscVal.c_str());
    };

    void onRead(NimBLEDescriptor* pDescriptor) {
        Serial.print(pDescriptor->getUUID().toString().c_str());
        Serial.println(" Descriptor read");
    };
};


/** Define callback instances globally to use for multiple Charateristics \ Descriptors */
static DescriptorCallbacks dscCallbacks;
static CharacteristicCallbacks chrCallbacks;

const std::string getDayOfWeek() {
  switch(rtc.getDayofWeek()) {
    case 0: return std::string("Sunday");
    case 1: return std::string("Monday");
    case 2: return std::string("Tuesday");
    case 3: return std::string("Wednesday");
    case 4: return std::string("Thursday");
    case 5: return std::string("Friday");
    case 6: return std::string("Saturday");
    default: return std::string("Unknown");
  }
}
const std::string getTimeStamp() {
  return std::to_string(rtc.getSecond()) + "," + std::to_string(rtc.getMinute()) + "," + std::to_string(rtc.getHour()) + "," + std::to_string(rtc.getDay()) + "," + std::to_string(rtc.getDay()) + "," + std::to_string(rtc.getMonth()) + "," + std::to_string(rtc.getYear());
}

char* getTimeString() {
  // <second>, <minute>, <hour>, <day>, <date>, <month>, <year>
  // Create a static buffer to store the string
  static char buffer[MAX_TIME_BUFF_SIZE];
  sprintf(buffer, "%d,%d,%d,%d,%d,%d", rtc.getSecond(),rtc.getMinute(),rtc.getHour(),rtc.getDay(),rtc.getMonth(),rtc.getYear());
  return buffer;
}

void printTime()
{
  Serial.print(String(rtc.getHour()) + ":"); // Print hour
  if (rtc.getMinute() < 10)
    Serial.print('0'); // Print leading '0' for minute
  Serial.print(String(rtc.getMinute()) + ":"); // Print minute
  if (rtc.getSecond() < 10)
    Serial.print('0'); // Print leading '0' for second
  Serial.print(String(rtc.getSecond())); // Print second
  
  /*
  if (rtc.is12Hour()) // If we're in 12-hour mode
  {
    // Use rtc.pm() to read the AM/PM state of the hour
    if (rtc.pm()) Serial.print(" PM"); // Returns true if PM
    else Serial.print(" AM");
  }
  */
  
  Serial.print(" | ");

  // Few options for printing the day, pick one:
  Serial.print(getDayOfWeek().c_str()); // Print day string
  //Serial.print(rtc.dayC()); // Print day character
  //Serial.print(rtc.day()); // Print day integer (1-7, Sun-Sat)
  Serial.print(" - ");
#ifdef PRINT_USA_DATE
  Serial.print(String(rtc.getMonth()) + "/" +   // Print month
                 String(rtc.getDay()) + "/");  // Print date
#else
  Serial.print(String(rtc.getDay()) + "/" +    // (or) print date
                 String(rtc.getMonth()) + "/"); // Print month
#endif
  Serial.println(String(rtc.getYear()));        // Print year
}

void setup() {
    Serial.begin(115200);
    Serial.println(std::string("Starting NimBLE \"Xevious\" TimeServer " + std::string(VERSION)).c_str());
  
    // Use the serial monitor to view time/date output
    // Serial.begin(9600);
  // #ifdef INTERRUPT_PIN // If using the SQW pin as an interrupt
  //   pinMode(INTERRUPT_PIN, INPUT_PULLUP);
  // #endif
    Serial.println("Starting BLE for MAG*Net");
    
  
    //
    // Time Server set up
    //
    // Call rtc.begin([cs]) to initialize the library
    // The chip-select pin should be sent as the only parameter
    // rtc.begin(DS13074_CS_PIN);
    // TODO: Fetch from NTP
    // 
    /*---------set with NTP---------------*/
    //  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    //  struct tm timeinfo;
    //  if (getLocalTime(&timeinfo)){
    //    rtc.setTimeStruct(timeinfo); 
    //  }

    rtc.setTime(30, 30, 1, 31, 10, 2024);  // 17th Jan 2021 15:24:30
    //rtc.setTime(1609459200);  // 1st Jan 2021 00:00:00
    //rtc.offset = 7200; // change offset value
    // Don't do this more than once, if you want to set the time
    // use DS3234_RTC_Demo.ino
    //
    /*
#ifdef CONFIGURE_TIME
    rtc.autoTime();
#endif
    rtc.set12Hour();
    rtc.update();
    */

    // Turn the alarms off for now
    // rtc.enableAlarmInterrupt();
    // rtc.setAlarm1(rtc.minute() + 1);
    /** sets device name */
    NimBLEDevice::init("Xevious_TimeServer");

    /** Optional: set the transmit power, default is 3db */
#ifdef ESP_PLATFORM
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); /** +9db */
#else
    NimBLEDevice::setPower(9); /** +9db */
#endif

    /** Set the IO capabilities of the device, each option will trigger a different pairing method.
     *  BLE_HS_IO_DISPLAY_ONLY    - Passkey pairing
     *  BLE_HS_IO_DISPLAY_YESNO   - Numeric comparison pairing
     *  BLE_HS_IO_NO_INPUT_OUTPUT - DEFAULT setting - just works pairing
     */
    //NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY); // use passkey
    //NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO); //use numeric comparison

    /** 2 different ways to set security - both calls achieve the same result.
     *  no bonding, no man in the middle protection, secure connections.
     *
     *  These are the default values, only shown here for demonstration.
     */
    //NimBLEDevice::setSecurityAuth(false, false, true);
    // NimBLEDevice::setSecurityAuth(/*BLE_SM_PAIR_AUTHREQ_BOND | BLE_SM_PAIR_AUTHREQ_MITM |*/ BLE_SM_PAIR_AUTHREQ_SC);
    // https://github.com/h2zero/NimBLE-Arduino/issues/588#issuecomment-1732479238
    NimBLEDevice::setSecurityAuth(true, false, true);
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    


    NimBLEService* pBaadService = pServer->createService(SERVICE_UUID);
    pTimeCharacteristic = pBaadService->createCharacteristic(
                                               "F00D",
                                               NIMBLE_PROPERTY::READ |
                                               NIMBLE_PROPERTY::WRITE |
                                               NIMBLE_PROPERTY::NOTIFY
                                              );

    pTimeCharacteristic->setValue(std::to_string(rtc.getSecond()) + "," + std::to_string(rtc.getMinute()) + "," + std::to_string(rtc.getHour()) + "," + getDayOfWeek() + "," + std::to_string(rtc.getDay()) + "," + std::to_string(rtc.getMonth()) + "," + std::to_string(rtc.getYear()));
    pTimeCharacteristic->setCallbacks(&chrCallbacks);

    pTimeStampCharacteristic = pBaadService->createCharacteristic(
                                               "D00F",
                                               NIMBLE_PROPERTY::READ |
                                               NIMBLE_PROPERTY::WRITE |
                                               NIMBLE_PROPERTY::NOTIFY
                                              );

    pTimeStampCharacteristic->setValue(std::to_string(rtc.getSecond()) + "," + std::to_string(rtc.getMinute()) + "," + std::to_string(rtc.getHour()) + "," + getDayOfWeek() + "," + std::to_string(rtc.getDay()) + "," + std::to_string(rtc.getMonth()) + "," + std::to_string(rtc.getYear()));
    pTimeStampCharacteristic->setCallbacks(&chrCallbacks);
    /** Note a 0x2902 descriptor MUST NOT be created as NimBLE will create one automatically
     *  if notification or indication properties are assigned to a characteristic.
     */

    /** Custom descriptor: Arguments are UUID, Properties, max length in bytes of the value */
   
    NimBLEDescriptor* pC01Ddsc = pTimeCharacteristic->createDescriptor(
                                               "C01D",
                                               NIMBLE_PROPERTY::READ |
                                               NIMBLE_PROPERTY::WRITE|
                                               NIMBLE_PROPERTY::WRITE_NR,
                                               512 
                                              );
    pC01Ddsc->setValue("Send it back!");
    pC01Ddsc->setCallbacks(&dscCallbacks);
    
    /** Start the services when finished creating all Characteristics and Descriptors */
    pBaadService->start();

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    /** Add the services to the advertisment data **/
    pAdvertising->addServiceUUID(pBaadService->getUUID());
    /** If your device is battery powered you may consider setting scan response
     *  to false as it will extend battery life at the expense of less data sent.
     */
    pAdvertising->setScanResponse(true);
    pAdvertising->start();

    Serial.println("Advertising Started");
}


void loop() {
  static int8_t lastSecond = -1;
  
  Serial.println(rtc.getTime("%A, %B %d %Y %H:%M:%S"));   // (String) returns time with specified format 
  // formating options  http://www.cplusplus.com/reference/ctime/strftime/


  struct tm timeinfo = rtc.getTimeStruct();

  // Call rtc.update() to update all rtc.seconds(), rtc.minutes(),
  // etc. return functions.
  
  // rtc.update();

  if (rtc.getSecond() != lastSecond) // If the second has changed
  {
    // std::string ts(getTimeString());
    const std::string ts = getTimeStamp();
    // pTimeCharacteristic->setValue(std::to_string(rtc.second()) + "," + std::to_string(rtc.minute()) + "," + std::to_string(rtc.hour()) + "," + std::to_string(rtc.day()) + "," + std::to_string(rtc.date()) + "," + std::to_string(rtc.month()) + "," + std::to_string(rtc.year())); // std::to_string(rtc.second()));
    // pTimeCharacteristic->notify();
    printTime(); // Print the new time
    
    Serial.print(ts.c_str());
    Serial.print("\n");
    lastSecond = rtc.getSecond(); // Update lastSecond value
    pTimeCharacteristic->setValue(std::to_string(rtc.getSecond()) + "," + std::to_string(rtc.getMinute()) + "," + std::to_string(rtc.getHour()) + "," + getDayOfWeek() + "," + std::to_string(rtc.getDay()) + "," + std::to_string(rtc.getMonth()) + "," + std::to_string(rtc.getYear()));
    pTimeCharacteristic->notify();
    // 11/14/2023, 2:34:07 PM
    // 11/14/2023, 12:34:07 PM
    std::string ampm = (rtc.getAmPm() == "pm") ? std::string("pm") : std::string("am");

    pTimeStampCharacteristic->setValue(std::to_string(rtc.getMonth()) + "/" + std::to_string(rtc.getDay()) + "/" + std::to_string(rtc.getYear()) + ", " + std::to_string(rtc.getHour()) + ":" + std::to_string(rtc.getMinute()) + ":" + std::to_string(rtc.getSecond()) + " " + ampm);
    pTimeStampCharacteristic->notify();
    
  } 

  // Check for alarm interrupts
/*
#ifdef INTERRUPT_PIN
  // Interrupt pin is active-low, if it's low, an alarm is triggered
  if (!digitalRead(INTERRUPT_PIN))
  {
#endif
    // Check rtc.alarm1() to see if alarm 1 triggered the interrupt
    if (rtc.alarm1())
    {
      Serial.println("ALARM 1!");
      // Re-set the alarm for when s=30:
      // rtc.setAlarm1(30);
      
      pTimeCharacteristic->setValue(std::to_string(rtc.second()) + "," + std::to_string(rtc.minute()) + "," + std::to_string(rtc.hour()) + "," + std::to_string(rtc.day()) + "," + std::to_string(rtc.date()) + "," + std::to_string(rtc.month()) + "," + std::to_string(rtc.year()));
      pTimeCharacteristic->notify();
      rtc.setAlarm1(rtc.minute() + 1, rtc.hour());
    }
    // Check rtc.alarm2() to see if alarm 2 triggered the interrupt
    if (rtc.alarm2())
    {
      Serial.println("ALARM 2!");
      // Re-set the alarm for when m increments by 1
      rtc.setAlarm2(rtc.minute() + 1, rtc.hour());
    }
#ifdef INTERRUPT_PIN
  }
#endif
*/

  /** Do your thing here, this just spams notifications to all connected clients */
    /*
    if(pServer->getConnectedCount()) {
        NimBLEService* pSvc = pServer->getServiceByUUID("BAAD");
        if(pSvc) {
            NimBLECharacteristic* pChr = pSvc->getCharacteristic("F00D");
            if(pChr) {
                pChr->notify(true);
            }
        }
    }
    */
  // delay(2000);
}