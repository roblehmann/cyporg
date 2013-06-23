// CAPTOR
// Cyanobacterial Arduino-based PhotobioreacTOR
//
// bioreactor firmware for the Arduino-based photobioreactor v0.1
// Author: r.lehmann@biologie.hu-berlin.de

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Timer.h>

//----------CONSTANTS-GLOBAL--------- //
#define BIOREACTOR_STANDBY_MODE   1   // nothing on, nothing measured
#define BIOREACTOR_LIGHT_MODE     2   // light const. at full brightness, od/temp. measured
#define BIOREACTOR_DARK_MODE      3   // light off, od/temp. measured
#define BIOREACTOR_CIRCADIAN_MODE 4   // light varies circadian, od/temp. measured
#define BIOREACTOR_ERROR_MODE     5  // somewthing went wrong,switch off everything

//const char* MODE_NAMES[] = {
//  "BIOREACTOR_STANDBY_MODE","BIOREACTOR_LIGHT_MODE","BIOREACTOR_DARK_MODE",
//  "BIOREACTOR_CIRCADIAN_MODE","BIOREACTOR_ERROR_MODE"};
//----------CONSTANTS-LIGHT--------- //
#define  ledPin     3  // LED panel connected to digital pin 3
//----------CONSTANTS-GAS--------- //
#define  pumpPin    5  // Pump connected to pwm pin 5
//----------CONSTANTS-OD--------- //
#define  irPin      8  //IR-Emitter diode
#define  odPin      A0  // OD detector diode
#define  odPin2      A1  // OD detector diode
#define  odPin3      A2  // OD detector diode
//----------CONSTANTS-TEMPERATURE--------- //
#define PIN_TEMPERATURE_SENSOR_IN_LIQUID  2   // temperature on culure bottle

//----------CONSTANTS-LIGHT REGULATION ------- //
// status variable marking circadian phase
const byte PHASE_MORNING = 0; // defined as DL transition
const byte PHASE_DAY     = 1; // defined as max. light intensity
const byte PHASE_EVENING = 2; // defined as LD transition
const byte PHASE_NIGHT   = 3; // defined as min. light intensity
const byte PHASE_NONE    = 4; // if no circadian program is used

//const String PHASE_NAMES[] = {
//  "PHASE_MORNING","PHASE_EVENING","PHASE_NIGHT","PHASE_NONE"};

//----------TIMER DECLARATION--------- //
Timer t;

//----------GLOBAL VARIABLES--------- //
//global variables used by functions and are set by WebUI
boolean DEBUG = true;
byte BIOREACTOR_MODE = BIOREACTOR_STANDBY_MODE;
byte bioreactorAncientMode = BIOREACTOR_STANDBY_MODE;

// LIGHT REGULATION //
byte minLightBrightness = 0;
byte maxLightBrightness = 255;
// 13,5cm Bottle-Panel and full brightness with 22V = 60 micromol
//0: 1.32
//10: 6.08
//20: 8.38
//30: 10.69
//40: 12.99
//50: 15.3
//60: 17.59
//70: 19.88
//80: 22.14
//90: 24.44
//100: 26.73
//110: 29.01
//120: 31.3
//130: 33.6
//140: 35.9
//150: 38.18
//160: 40.45
//170: 42.73
//180: 44.98
//190: 47.22
//200: 49.46
//210: 51.71
//220:53.92
//230:56.04
//240:57.94
//250:59.4

byte lightBrightness = 0;       // holds brightness of LED panel in range 0-255 (0=off)
byte lightChangeStep = 25;       // how strong the light intensity changes between two consecutive steps
unsigned long lightChangeStepLength = 1L; // how long should a step to the next light level be  /seconds
byte dayPhase = PHASE_NONE;     // holds whether it is MORNING, EVENING, or NIGHT

// Duration: MORNING, DAY, EVENING, NIGHT in SEC!
unsigned long PHASE_DURATIONS[4] = {
  10L, 1L, 10L, 11L};

//unsigned long PHASE_MORNING_DURATION  = 10L; // defined as DL transition /seconds
//unsigned long PHASE_DAY_DURATION      = 1L; // defined as max. light intensity /seconds
//unsigned long PHASE_EVENING_DURATION  = 10L; // defined as LD transition /seconds
//unsigned long PHASE_NIGHT_DURATION    = 11L; // defined as min. light intensity /seconds


// AIR PUMP REGULATION //
boolean airPumpState;
const byte pumpSpeed = 140; // 0-255 PWM signal to adjust pump voltage to 1.5V average

// how often to measure od/temp
unsigned long sensorSamplingTime = 2L; //seconds

// store timer IDs, to be able to remove them when the BIOREACTOR_MODE or the light-change-params change
int dayPhaseChangeTimerID = -1;
int lightChangeTimerID = -1;
int sensorReadTimerID = -1;

String inputString = "";         // a string to hold incoming data
boolean stringComplete = false;  // whether the string is complete
// for logging
const String sep = ",";

//----------------------------- //
//----------setting up--------- //
//----------------------------- //
void setup()  {
  // for serial input message
  inputString.reserve(50);

  // init serial for OD
  Serial.begin(9600);
  lightSetup();
  temperatureSetup();
  odSetup();
  pumpSetup();
  // update all the sensor data for once before doing the first log
  updateSensorLogValues();

  bioreactorAncientMode = BIOREACTOR_STANDBY_MODE;
  BIOREACTOR_MODE = BIOREACTOR_STANDBY_MODE;
} 

//----------------------------- //
//----------loop--------- //
//----------------------------- //
void loop()  { 
  // update the timer
  t.update();
  // read new configuration from Serial
  checkSerialMessage();
  // check if reactor mode changed and make changes effective
  checkChangedReactorMode();
}


void checkChangedReactorMode () 
{
  // check if reactor mode changed, and put changes into effect
  if(bioreactorAncientMode != BIOREACTOR_MODE)  
  {
    bioreactorAncientMode = BIOREACTOR_MODE;
    switch(BIOREACTOR_MODE)
    {
    case BIOREACTOR_STANDBY_MODE: 
      { 
        // to go into standby, need to switch off light, remove timers for day phase/light change
        if(DEBUG) Serial.println("standby");
        // deactivate air pump 
        stopAirPump();
        // switch off light, remove timers
        stopSensorReadTimer();
        lightOff();
        break;
      }
    case BIOREACTOR_CIRCADIAN_MODE:
      {
        if(DEBUG) Serial.println("circadian");
        // activate air pump 
        startAirPump() ;
        // ------------ Adding timed actions ----------
        startSensorReadTimer();
        startCircadianLightTimers();
        break;
      }
    case BIOREACTOR_LIGHT_MODE:
      {
        if(DEBUG) Serial.println("light");
        startAirPump() ;
        lightOn();
        startSensorReadTimer();
        break;
      }
    case BIOREACTOR_DARK_MODE:  
      {
        if(DEBUG) Serial.println("dark");
        startAirPump() ;
        lightOff();
        startSensorReadTimer();
        break;
      }
    case BIOREACTOR_ERROR_MODE:
      {
        if(DEBUG) Serial.println("ERROR");
        stopAirPump() ;
        lightOff();
        stopSensorReadTimer();
        break;
      }
    default:
      Serial.println("unknown->ERROR");
      BIOREACTOR_MODE = BIOREACTOR_ERROR_MODE;
      stopAirPump() ;
      lightOff();
      stopSensorReadTimer();
      break;
    }
    loggingEvent();
  }
}

/*
  SerialEvent occurs whenever a new data comes in the
 hardware serial RX.  This routine is run between each
 time loop() runs, so using delay inside loop can delay
 response.  Multiple bytes of data may be available.
 */
void serialEvent() {
  while (Serial.available()) {
    // get the new byte:
    char inChar = (char)Serial.read(); 
    // add it to the inputString:
    inputString += inChar;
    // if the incoming character is a newline, set a flag
    // so the main loop can do something about it:
    if (inChar == '#') {
      stringComplete = true;
    }
  }
}


void checkSerialMessage() {
  if (stringComplete) {
    if (inputString != "") 
    {
      if(DEBUG) Serial.println(inputString);
      // change setup according to message
      digestMessage();
    }
    // clear the string:
    inputString = "";
    stringComplete = false;
  }
}

char charBuf[30];
String paramName;
unsigned long paramValue;
// method takes message from cotrolling computer and parses it, assigning the command 
// value to the corresponding variable.
// messages alsways contain the parameter name first, value second
void digestMessage() 
{
  inputString.toCharArray(charBuf, 30); 
  paramName = strtok(charBuf,":");
  paramValue = atol(strtok(NULL,":"));
  Serial.println(paramName);
  Serial.println(paramValue);

  if(paramName == "MODE") // which reactor program to use
  {
    switch(paramValue) 
    {
    case 1:
      BIOREACTOR_MODE = BIOREACTOR_STANDBY_MODE;
      break;
    case 2:
      BIOREACTOR_MODE = BIOREACTOR_LIGHT_MODE;
      break;
    case 3:
      BIOREACTOR_MODE = BIOREACTOR_DARK_MODE;
      break;
    case 4:
      BIOREACTOR_MODE = BIOREACTOR_CIRCADIAN_MODE;
      break;
    case 5:
      BIOREACTOR_MODE = BIOREACTOR_ERROR_MODE;
      break;
    }
  }
  else if(paramName == "lightChangeStep") 
    // brightness increments for morning/evening in circadian mode
    // should be in range 1-255
  {
    if(paramValue < 1) {
      lightChangeStep = 1;
    } 
    else if(paramValue > 255) {
      lightChangeStep = 255;
    } 
    else {
      lightChangeStep = paramValue;
    }
  }
  else if(paramName == "lightChangeStepLength") // how long to keep each brightness increment
  {
    lightChangeStepLength = paramValue;
  }
  else if(paramName == "morningLength") // duration of morning, evening, and night
  {
    PHASE_DURATIONS[PHASE_MORNING] = paramValue;
  }
  else if(paramName == "dayLength") // duration of morning, evening, and night
  {
    PHASE_DURATIONS[PHASE_DAY] = paramValue;
  }
  else if(paramName == "eveningLength") // duration of morning, evening, and night
  {
    PHASE_DURATIONS[PHASE_EVENING] = paramValue;
  }
  else if(paramName == "nightLength") // duration of morning, evening, and night
  {
    PHASE_DURATIONS[PHASE_NIGHT] = paramValue;
  }
  else if(paramName == "sensorSamplingTime") // how often to sample
  {
    sensorSamplingTime = paramValue;
    stopSensorReadTimer(); // restart sampling timer, to avoid having to go to 
    startSensorReadTimer(); // standby mode and reinit reactor mode
  }
  else if(paramName == "maxLightBrightness") // maximal brightness of the LED panel
  {
    maxLightBrightness = paramValue;
  }
  else if(paramName == "minLightBrightness") // maximal brightness of the LED panel
  {
    minLightBrightness = paramValue;
  }
  else
  {
    Serial.print("unknown:");
    Serial.println(paramName);
  }
}







