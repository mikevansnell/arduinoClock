/** arduino alarm clock 
 *  Mike Van Snellenberg 2019
 *  Uses a DS3231 real time clock (battery backed!) for timekeeping. Readily available from amazon, aliexpress etc, and claims <2ppm deviation.
 *  Uses a TM1637 7-segment clock display. Also readily available at your favorite online store / auction site. Assumes 12 hour time for now.
 *  Uses a little piezo buzzer and an LED to indicate PM times
 *  
 *  Requires 5 input buttons:
 *   [Set]       : Long press to set the time, and then short-press to cycle through hours, minutes
 *   [Set Alarm] : Long press to set the alarm time. Short press to turn alarm on or off.
 *   [Minus]     : removes hours or minutes when setting the time or an alarm
 *   [Plus]      : adds hours or minutes when setting the time or an alarm
 *   [Snooz]     : snoozes an alarm in progress (by default for 9 minutes). Long press to cancel the alarm for the day.
 *   
 *   
 */


#include <Arduino.h>
#include <EEPROM.h>
#include <TM1637Display.h>
#include <Wire.h>
#include "RTClib.h"
#include <AceButton.h>
using namespace ace_button;

// PIN ASSIGNMENTS
#define BTN_SET       9
#define BTN_SET_ALARM 8
#define BTN_MINUS     5
#define BTN_PLUS      6
#define BTN_SNOOZ     7

#define DISP_CLK      3
#define DISP_DIO      4
#define PM_LED        10
#define PIEZO         2 

// UI TIMING CHARACTERISTICS
#define TEST_DELAY 200
#define REFRESH_DISPLAY_INTERVAL 1000
#define BLINK_INTERVAL_ON 400
#define BLINK_INTERVAL_OFF 400
#define SHOW_LABEL_INTERVAL 1000
#define ALARM_REPEAT_INTERVAL 5000
#define FAST_TIME_SET_INTERVAL 100
const unsigned long ALARM_SNOOZ_INTERVAL = 540000; // 540000 = 9 minutes

// EEPROM
#define EEPROM_ADDR_ALARM_HOUR    2
#define EEPROM_ADDR_ALARM_MINUTE  3
#define EEPROM_ADDR_IS_ALARM_SET  4

// user interface states. These are used by the main loop and button handler.
enum UiState {
  run,
  setStart,
  setHours,
  setMinutes,
  setAlarmStart,
  setAlarmHours,
  setAlarmMinutes,
  setAlarmOn,
  setAlarmOff,
  alarmSounding,
  alarmSnoozed
};

// UI State management
volatile UiState uiState = UiState::run;
volatile unsigned long lastStateSet = 0;
unsigned long lastDisplayRefresh = 0;
bool blinkState = 0;
volatile unsigned long lastAlarmAt = 0;
volatile unsigned long lastSnoozAt = 0;
volatile bool requestEepromRead = false;
volatile bool requestEepromWrite = false;
DateTime alarmTime;
volatile bool isAlarmSet = true;
volatile int fastTimeDirection = 0;
unsigned long lastFastTimeTick = 0;
volatile byte brightness = 0x7;     // brightness values 0x1..0x7


/**  FROM https://www.mcielectronics.cl/website_MCI/static/documents/Datasheet_TM1637.pdf
 *   
 *      a
 *     ---
 *  f | g | b
 *     ---
 *  e |   | c
 *     ---
 *      d
 */
const uint8_t SEG_SET[] = {
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D, // S
    SEG_A | SEG_D | SEG_E | SEG_F | SEG_G, // E
    SEG_F | SEG_E | SEG_D | SEG_G,         // t
    0
  };

const uint8_t SEG_ALAR[] = {
    SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G, // A
    SEG_D | SEG_E | SEG_F,                         // L
    SEG_A | SEG_B | SEG_C | SEG_E | SEG_F | SEG_G, // A
    SEG_E | SEG_G,                                 // r
  };

const uint8_t SEG_ON[] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, // O
    SEG_C | SEG_E | SEG_G,                         // n
    0,                
    0,                                 
  };

const uint8_t SEG_OFF[] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F, // O
    SEG_A | SEG_E | SEG_F | SEG_G,                 // F
    SEG_A | SEG_E | SEG_F | SEG_G,                 // F
    0
  };
  

TM1637Display display(DISP_CLK,DISP_DIO);
RTC_DS3231 rtc;

AceButton btnSet(BTN_SET);
AceButton btnSetAlarm(BTN_SET_ALARM);
AceButton btnMinus(BTN_MINUS);
AceButton btnPlus(BTN_PLUS);
AceButton btnSnooz(BTN_SNOOZ);

void handleButton(AceButton* , uint8_t , uint8_t /*buttonState*/);


void setup() {
  
  Serial.begin(9600);
  Serial.println("initializing clock...");

  pinMode(BTN_SET, INPUT_PULLUP);
  pinMode(BTN_SET_ALARM, INPUT_PULLUP);
  pinMode(BTN_MINUS, INPUT_PULLUP);
  pinMode(BTN_PLUS, INPUT_PULLUP);
  pinMode(BTN_SNOOZ, INPUT_PULLUP);
  pinMode(PM_LED, OUTPUT);

  display.setBrightness(brightness); 

  if (! rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  if (rtc.lostPower()) {
    Serial.println("RTC lost power, lets set the time!");
    // following line sets the RTC to the date & time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    // This line sets the RTC with an explicit date & time, for example to set
    // January 21, 2014 at 3am you would call:
    // rtc.adjust(DateTime(2014, 1, 21, 3, 0, 0));
  }

  readAlarmFromEeprom();

  btnSet.setEventHandler(handleButton);
  btnSet.getButtonConfig()->setFeature(ButtonConfig::kFeatureLongPress);
  btnSetAlarm.setEventHandler(handleButton);
  btnSetAlarm.getButtonConfig()->setFeature(ButtonConfig::kFeatureLongPress | ButtonConfig::kFeatureClick);
  btnMinus.setEventHandler(handleButton);
  btnMinus.getButtonConfig()->setFeature(ButtonConfig::kFeatureLongPress);
  btnPlus.setEventHandler(handleButton);
  btnPlus.getButtonConfig()->setFeature(ButtonConfig::kFeatureLongPress);
  btnSnooz.setEventHandler(handleButton);

 
}

void loop() {

  // button handlers can ask the main loop to set/read alarm from eeprom
  handleEepromState(); 

  // stateful clock mode display
  switch (uiState) {
    case UiState::run:
      showCurrentTimeIfNeeded();

      // check if it's time to sound the alarm
      if (isAlarmTime()) {
        txToState(UiState::alarmSounding);
      }
      
      break;
    case UiState::setStart:
      if (millis() <= lastStateSet + SHOW_LABEL_INTERVAL ) {
        display.setSegments(SEG_SET);
      } else {
        txToState(UiState::setHours);
      }
      break;
    case UiState::setHours:
      if (fastTimeDirection != 0) {
        if (millis() > lastFastTimeTick + FAST_TIME_SET_INTERVAL) {
          changeTime(fastTimeDirection,0);
          lastFastTimeTick = millis();
        }
        showSettableTime(rtc.now(),false,false); // don't blink during fast set
      } else {
        showSettableTime(rtc.now(),true,false);
      } 
      
      break;
    case UiState::setMinutes:
      if (fastTimeDirection != 0) {
        if (millis() > lastFastTimeTick + FAST_TIME_SET_INTERVAL) {
          changeTime(0,fastTimeDirection);
          lastFastTimeTick = millis();
        }
        showSettableTime(rtc.now(),false,false); // don't blink during fast set
      } else {
        showSettableTime(rtc.now(),false,true);
      }
      break;
    case UiState::setAlarmStart:
      if (millis() <= lastStateSet + SHOW_LABEL_INTERVAL ) {
        display.setSegments(SEG_ALAR);
      } else {
        txToState(UiState::setAlarmHours);
      }
      break;
    case UiState::setAlarmHours:
      if (fastTimeDirection != 0) {
        if (millis() > lastFastTimeTick + FAST_TIME_SET_INTERVAL) {
          changeAlarmTime(fastTimeDirection,0);
          lastFastTimeTick = millis();
        }
        showSettableTime(alarmTime,false,false); // don't blink during fast set
      } else {
        showSettableTime(alarmTime,true,false);  
      }
      break;
    case UiState::setAlarmMinutes:
      if (fastTimeDirection != 0) {
        if (millis() > lastFastTimeTick + FAST_TIME_SET_INTERVAL) {
          changeAlarmTime(0,fastTimeDirection);
          lastFastTimeTick = millis();
        }
        showSettableTime(alarmTime,false,false); // don't blink during fast set
      } else {
        showSettableTime(alarmTime,false,true);  
      }
      break;
    case UiState::setAlarmOn:
      if (millis() <= lastStateSet + SHOW_LABEL_INTERVAL ) {
        display.setSegments(SEG_ON);
      } else {
        writeAlarmToEeprom();
        txToState(UiState::run);
      }
      break;
    case UiState::setAlarmOff:
      if (millis() <= lastStateSet + SHOW_LABEL_INTERVAL ) {
        display.setSegments(SEG_OFF);
      } else {
        writeAlarmToEeprom();
        txToState(UiState::run);
      }
      break;
    case UiState::alarmSounding:
      showCurrentTimeIfNeeded();
      if (millis() > lastAlarmAt + ALARM_REPEAT_INTERVAL) {
        soundAlarm();
        lastAlarmAt = millis();
      }
      break;
    case UiState::alarmSnoozed:
      showCurrentTimeIfNeeded();
      if (millis() > lastSnoozAt + ALARM_SNOOZ_INTERVAL) {
        Serial.println("Snooze interval expired -- wakey wakey");
        txToState(UiState::alarmSounding);
      }
      break;
      
  }
  
  btnSet.check();
  btnSetAlarm.check();
  btnMinus.check();
  btnPlus.check();
  btnSnooz.check();
}

/** tests out the clock display by runnuing through all hours:minutes from 12:00 to 11:59 */
void loopClock() {
  for (int hours = 0; hours<12; hours++) {
    for (int minutes = 0; minutes < 60; minutes++) {
      int hourMinDec = (hours == 0 ? 12 : hours) * 100 + minutes;
      //display.showNumberDec(, false);
      display.showNumberDecEx(hourMinDec, (0x80 >> 1), false);
      delay(TEST_DELAY);
    }
  }

}

/** checks if it's time to update the display, and if so update the display */
void showCurrentTimeIfNeeded() {
  if (millis() > lastDisplayRefresh + REFRESH_DISPLAY_INTERVAL || 
      lastStateSet > lastDisplayRefresh) {
    showCurrentTime();
    lastDisplayRefresh = millis();
  }
}

/**  gets the current time from the RTC, and shows on the LCD display */
void showCurrentTime() {
  DateTime now = rtc.now();
 
  int hour = now.hour() % 12;
  if (hour == 0) { hour = 12; }
  int minute = now.minute();
  int hourMinDec = hour * 100 + minute;

  bool isPM = now.hour() >= 12;
  analogWrite(PM_LED, isPM ? ((brightness + 1) * 0x1F) : 0);
  //digitalWrite(PM_LED,isPM);

  display.showNumberDecEx(hourMinDec, 0x80 >> 1, false);

  Serial.print("showing time ");
  Serial.print(hour);
  Serial.print(":");
  Serial.println(minute);
}

/** shows a given time on the display in a settable format with optional blinking hours or minutes */
void showSettableTime(DateTime t, bool blinkHours, bool blinkMinutes) {
  
  int hour = t.hour() % 12;
  if (hour == 0) { hour = 12; }
  int minute = t.minute();

  bool isPM = t.hour() >= 12;
  digitalWrite(PM_LED,isPM);

  bool shouldBlink = blinkHours || blinkMinutes;
  
  if ((shouldBlink && blinkState  && (millis() > lastDisplayRefresh + BLINK_INTERVAL_OFF)) ||
      (shouldBlink && !blinkState && (millis() > lastDisplayRefresh + BLINK_INTERVAL_ON)) ) {
      blinkState = !blinkState;
      lastDisplayRefresh = millis();
      display.clear();
  }
  
  int hourMinDec = hour * 100 + minute;

  if (blinkHours && blinkState) {
    // hide hours during the dark part of the blink
    display.showNumberDecEx(minute,0x80 >> 1,true,2,2); 
  }
  else if (blinkMinutes && blinkState) {
      // hide minutes during the dark part of the blink
    display.showNumberDecEx(hour,0x80 >> 1,false,2,0); 
  } else {
    display.showNumberDecEx(hourMinDec, 0x80 >> 1, false);
  }

}

void changeBrightness (byte delta) {
  
  brightness = brightness + delta;
  if (brightness == 0xff) { brightness = 0x0; }
  if (brightness > 0x7) { brightness = 0x7; }

  Serial.print("Brightness ");
  Serial.print(brightness);
  Serial.print(", PM LED gets (0-255) ");
  Serial.println((brightness + 1) * 0x1F);
  
  display.setBrightness(brightness); 
  showCurrentTime();
  

}


/** changes the current time by a number of minutes and/or hours delta. Can be positive or negative.
 *  Of the minutes are being set, the seconds will be set to 0. This gives some ability to get "to the second"
 *  accuracy if you increment a minute right as it ticks over.
 */
void changeTime(int hoursDelta, int minutesDelta) {
    DateTime now = rtc.now();
    // reset seconds to 0 if adjusting the minutes
    now = now + TimeSpan(0,hoursDelta,minutesDelta,(minutesDelta == 0 ? 0 : -now.second()));
    rtc.adjust(now);
    
}

/** changes the hours and/or minutes on the in-memory alarm DateTime. This is expected to be called
 *  several times as the user is setting the alarm, so in order to conserve EEPROM write cycles, this 
 *  is *not* stored to the EEPROM yet. That can be done by calling writeAlarmToEeprom().
 */
void changeAlarmTime(int hoursDelta, int minutesDelta) {
    alarmTime = alarmTime + TimeSpan(0,hoursDelta,minutesDelta,0);
    
}

/* checks the volatile UI state flags to see if it's time to read or write the alarm
 * memory from EEPROM. The button handler can request eeprom read or write by setting the 
 * requestEepromRead or requestEepromWrite flags.
 * This may not be strictly necessary, but EEPROM writes take some time to complete per 
 * the documentation, so it is probaby better do to this in the main loop and not in the 
 * button handler. 
 */
void handleEepromState() {
  if (requestEepromRead) {
    readAlarmFromEeprom();
    requestEepromRead = false;
  } else if (requestEepromWrite) {
    writeAlarmToEeprom();
    requestEepromWrite = false;
  }
}

/** reads the alarm time and set state from EEPROM (this is persistent across power outages)
 *  Note that in this case we're using the ATMEGA's EEPROM, but according to the DS3231 datasheet,
 *  it actually has native support for alarms which might be interesting to explore in the future
*/
void readAlarmFromEeprom() {
  int alarmHour = EEPROM.read(EEPROM_ADDR_ALARM_HOUR);
  if (alarmHour < 0 || alarmHour > 23) { alarmHour = 0; } 
  int alarmMinute = EEPROM.read(EEPROM_ADDR_ALARM_MINUTE);
  if (alarmMinute < 0 || alarmMinute > 59) { alarmMinute = 0; }
  alarmTime = DateTime(2019,01,01,alarmHour,alarmMinute,0);
  isAlarmSet = EEPROM.read(EEPROM_ADDR_IS_ALARM_SET);

  Serial.print("read alarm from EEPROM: ");
  char buff[] = "YYYY-MM-DD hh:mm:ss";
  Serial.print(alarmHour);
  Serial.print(":");
  Serial.print(alarmMinute);
  Serial.print(" --> ");
  Serial.print(alarmTime.toString(buff));
  Serial.print(". Alarm is ");
  Serial.println(isAlarmSet ? "on" : "off");
  
  
}

/** Writes the alarm hour, minute, and on/off state to EEPROM
 *  
 */
void writeAlarmToEeprom() {
  
  byte alarmHour = alarmTime.hour();
  byte alarmMinute = alarmTime.minute();
  Serial.print("writing alarm time to EEPROM: ");
  Serial.print(alarmHour);
  Serial.print(":");
  Serial.print(alarmMinute);
  Serial.print(". Alarm is ");
  Serial.println(isAlarmSet ? "on" : "off");
  

  EEPROM.write(EEPROM_ADDR_ALARM_HOUR, alarmHour);  
  EEPROM.write(EEPROM_ADDR_ALARM_MINUTE, alarmMinute);
  EEPROM.write(EEPROM_ADDR_IS_ALARM_SET, isAlarmSet);
  
}

/** checks if we should transition from regular clock run state to alarming state based on the
 *  alarm set time, and whether the alarm is set
 *  
 */
bool isAlarmTime() {
  if (!isAlarmSet) { return false; }
  DateTime now = rtc.now();
  return (now.hour() == alarmTime.hour() && now.minute() == alarmTime.minute() && now.second() == 0);
}

/** cheesy piezo alarm tones.
 *  todo: this is one of the only pieces of code that is blocking with delays(), which
 *  isn't great because it's likely that the user will want to snooze during the alarm tone, 
 *  and the button handler will miss this event.
 *  
 */
void soundAlarm() {

  switch(2) {
    case 1:
      // whoop
      for (int i=100; i<1500; i= i+10) {
        tone(PIEZO,i);
        delay(1);
      }
      noTone(PIEZO);
      break;
    case 2:
      // arpegio
      int startNote = 523.25; // middle c
      float arpegio[] = {
        startNote, 
        startNote * pow(2.0, 4.0/12.0),
        startNote * pow(2.0, 7.0/12.0),
        startNote * pow(2.0, 12.0/12.0)
      };
      
      tone(PIEZO, arpegio[0], 180);
      delay(200);
      tone(PIEZO, arpegio[1], 180);
      delay(200);
      tone(PIEZO, arpegio[2], 180);
      delay(200);
      tone(PIEZO, arpegio[3], 180);
      delay(200);
      tone(PIEZO, arpegio[2], 180);
      delay(200);
      tone(PIEZO, arpegio[1], 180);
      delay(200);
      tone(PIEZO, arpegio[0], 800);
      
  }
  
  
}


/** transitions to a new state. This is called either by the button handler based on button press events
 *  or from the main event loop based on timer-driven events (e.g. hiding a label after a second or two)
 *  TODO: would be nice to abstract this out as a FSM and then declare states and transitions instead of 
 *  weaving them across button handlers and the main event loop.
 *  
 */
void txToState(int newState) {
  display.clear();
  uiState = newState;
  lastStateSet = millis();
  switch (newState) {
    case UiState::run :
      Serial.println("->run");
      break;
    case UiState::setStart :
      Serial.println("->setStart");
      break;
    case UiState::setHours :
      Serial.println("->setHours");
      break;
    case UiState::setMinutes :
      Serial.println("->setMinutes");
      break;
    case UiState::setAlarmStart :
      Serial.println("->setAlarmStart");
      break;
    case UiState::setAlarmHours :
      Serial.println("->setAlarmHours");
      break;
    case UiState::setAlarmMinutes :
      Serial.println("->setAlarmMinutes");
      break;
    case UiState::setAlarmOn:
      Serial.println("->setAlarmOn");
      break;
    case UiState::setAlarmOff:
      Serial.println("->setAlarmOff");
      break;
    case UiState::alarmSounding :
      Serial.println("->alarmSounding");
      break;
    case UiState::alarmSnoozed :
      Serial.println("->alarmSnoozed");
      break;
    default:
      Serial.print("-> state ");
      Serial.println(newState);
  }
}

/** handles all button presses. 
 *  Since this seems (?) to run as an interrupt handler, I've tried to avoid doing any long-running code here,
 *  and instead just transition between various clock states that can be picked up and run in the main event loop
 *  
 */
void handleButton(AceButton* btn, uint8_t eventType, uint8_t /*buttonState*/) {
  
  int pin = btn->getPin();
  switch (pin) {
    case BTN_SET:
      if (eventType == AceButton::kEventLongPressed) {
        if (uiState == UiState::run) {
          // enter set mode with long press
          txToState(UiState::setStart);
        } else if (uiState >= UiState::setStart && uiState <= UiState::setMinutes) {
          // exit set mode with long press
          txToState(UiState::run);
        }
      } else if (eventType == AceButton::kEventPressed) {
        if (uiState == UiState::setStart) {
          // quick advance to hour set
          txToState(UiState::setHours);
        } else if (uiState == UiState::setHours) {
          // advance hours -> minutes set
          txToState(UiState::setMinutes);
        } else if (uiState == UiState::setMinutes) {
          // end set after setting minutes 
          txToState(UiState::run);
        }
      }
      break;
    case BTN_SET_ALARM:
      if (eventType == AceButton::kEventLongPressed) {
        if (uiState == UiState::run) {
          // enter set mode with long press
          requestEepromRead = true;
          txToState(UiState::setAlarmStart);
        } else if (uiState >= UiState::setAlarmStart && uiState <= UiState::setAlarmMinutes) {
          // exit set mode with long press. always turn alarm on after setting.
          requestEepromWrite = true;
          txToState(UiState::setAlarmOn);
        }
      } else if (eventType == AceButton::kEventClicked) { // clicked = press and release (not long-pressed)
        if (uiState == UiState::run || (uiState >= UiState::setAlarmOn && uiState <= UiState::alarmSnoozed)) {
          // if we're not setting the alarm, then a short press toggles alarm state
          // this also means if the alarm is currently alarming or snoozed, pressing set will cancel the current alarm
          noTone(PIEZO);
          if (isAlarmSet) {
            isAlarmSet = false;
            txToState(UiState::setAlarmOff);
          } else {
            isAlarmSet = true;
            txToState(UiState::setAlarmOn);
          }

        } else if (uiState == UiState::setAlarmStart) {
          // quick advance to hour set
          txToState(UiState::setAlarmHours);
        } else if (uiState == UiState::setAlarmHours) {
          // advance hours -> minutes set
          txToState(UiState::setAlarmMinutes);
        } else if (uiState == UiState::setAlarmMinutes) {
          // end set after setting minutes. always turn alarm on after setting.
          requestEepromWrite = true;
          txToState(UiState::setAlarmOn);
        }
      }
      break;
    case BTN_MINUS:
      if (eventType == AceButton::kEventPressed) {
        if(uiState == UiState::setHours) {
          changeTime(-1,0);
        } else if(uiState == UiState::setMinutes) {
          changeTime(0,-1);
        } else if (uiState == UiState::setAlarmHours) {
          changeAlarmTime(-1,0);
        } else if (uiState == UiState::setAlarmMinutes) {
          changeAlarmTime(0,-1);
        } else if (uiState == UiState::run) {
          changeBrightness(-1);
        }
      } else if (eventType == AceButton::kEventLongPressed) {
        Serial.println("fast -");
        fastTimeDirection = -1;
      } else if (eventType == AceButton::kEventReleased) {
        fastTimeDirection = 0;
      }
      
      break;
    case BTN_PLUS:
      if (eventType == AceButton::kEventPressed) {
        if(uiState == UiState::setHours) {
          changeTime(1,0);
        } else if(uiState == UiState::setMinutes) {
          changeTime(0,1);
        } else if (uiState == UiState::setAlarmHours) {
          changeAlarmTime(1,0);
        } else if (uiState == UiState::setAlarmMinutes) {
          changeAlarmTime(0,1);
        } else if (uiState == UiState::run) {
          changeBrightness(+1);
        }
      } else if (eventType == AceButton::kEventLongPressed) {
        Serial.println("fast +");
        fastTimeDirection = 1;
      } else if (eventType == AceButton::kEventReleased) {
        fastTimeDirection = 0;
      }
      break;
    case BTN_SNOOZ:
      if (eventType == AceButton::kEventPressed) {
        if (uiState == UiState::alarmSounding) {
          Serial.print("Snoozing for ");
          Serial.print(ALARM_SNOOZ_INTERVAL);
          Serial.println("ms");
          lastSnoozAt = millis();
          
          noTone(PIEZO);
          txToState(UiState::alarmSnoozed);

        }
      } else if (eventType == AceButton::kEventLongPressed) {
        // cancel the current alarm, but leave the alarm on so it'll go off again tomorrow.
        noTone(PIEZO);
        txToState(UiState::run);
      }
      
      break;
  
  }

  
  Serial.print("Button pressed! Pin: ");
  Serial.print(pin);
  Serial.print(" event: ");
  Serial.println(eventType);
  

}
