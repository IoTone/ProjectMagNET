#ifndef ARDUINOGLUE_H
#define ARDUINOGLUE_H


//============ Includes ====================
#include <ArduinoBLE.h>

//============ Defines & Macros====================
#define VERSION "0.0.4"
#define LED_BUILTIN 13  // If your ESP32 DOES NOT DEFINE LED_BUILTIN

//============ Structs, Unions & Enums ============
//-- from M5Atom_DigDug_Camera_Button.ino
enum ButtonState {
  B_On,             // Camera Shooting
  B_Off,            // Camera Not Shooting
  B_Pwr             // Camera Off
};

//============ Extern Variables ============
extern const int       BUTTON_PIN;                        		//-- from M5Atom_DigDug_Camera_Button
extern BLEByteCharacteristic buttonCharacteristic;              		//-- from M5Atom_DigDug_Camera_Button
extern int             currentState;                      		//-- from M5Atom_DigDug_Camera_Button
// extern ButtonState     g_btn_state;                       		//-- from M5Atom_DigDug_Camera_Button
extern int             lastState;                         		//-- from M5Atom_DigDug_Camera_Button
extern const int       ledPin;                            		//-- from M5Atom_DigDug_Camera_Button
extern BLEService      ledService;                        		//-- from M5Atom_DigDug_Camera_Button

#endif // ARDUINOGLUE_H
