/*
Data logger library
Written by Andy Wickert
September-October 2011

# LICENSE: GNU GPL v3

Logger.cpp is part of Logger, an Arduino library written by Andrew D. Wickert.
Copyright (C) 2011-2013, Andrew D. Wickert

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

////////////////////////////////////////////////////
// INCLUDE HEADER FILE AND ITS INCLUDED LIBRARIES //
////////////////////////////////////////////////////

#include <Logger.h>

// Breaking things up at first, but will try to just put all of the initialize / 
// setup stuff in the constructor eventually (or maybe just lump "initialize" 
// and "setup")

// Avoid spurious warnings -- copied from RTClib code, but hopefully will avoid warnings about text in progmem
#undef PROGMEM
#define PROGMEM __attribute__(( section(".progmem.data") ))
#undef PSTR
#define PSTR(s) (__extension__({static prog_char __c[] PROGMEM = (s); &__c[0];}))


/////////////////////////////////////////////////////
// STATIC MEMBERS OF CLASS, SO ACCESSIBLE ANYWHERE //
/////////////////////////////////////////////////////

/////////////////
// ASSIGN PINS //
/////////////////

  // SD card: CSpinSD and protected pins
  const int CLKpin = 52;
  const int MISOpin = 50;
  const int MOSIpin = 51;
  const int CSpinSD = 53;
  // Protected clock pins -- I2C not protected here
  // const int SDApin = 20;
  // const int SCLpin = 21;
  const int CSpinRTC = 47;
  // Digital pins
  const int SensorPin3V3 = 21; // Activates 3.3V voltage regulator to give power to sensors
  const int SensorPinVCC = 39; // MOSFET to turn on and off VCC power to sensors
  const int SDpin = 48; // Turns on voltage source to SD card
  const int LEDpin = 49; // LED to tell user if logger is working properly
  const int wakePin = 2; // interrupt pin used for waking up via the alarm
  const int interruptNum = 0; // =0 for pin 2, 1 for pin 3
  const int manualWakePin = 43; // Wakes the logger with a manual button - overrides the "wait for right minute" commands

/////////////////////////////////////////////////
// GLOBAL VARIABLES DEFINED IN INITIALIZE STEP //
/////////////////////////////////////////////////

// Logging interval - wake when minutes == this
int log_minutes;
bool camera_is_on = false; // for a video camera

// testing
uint8_t tmp;

// Filename and logger name
// Filename is set up as 8.3 filename:
//char filename[12];
char* filename;
char* logger_name;

// For interrupt
bool extInt;
bool tip = false;

/////////////////////////
// INSTANTIATE CLASSES //
/////////////////////////

// Both for same clock, but need to create instances of both
// classes in library (due to my glomming of two libs together)
// Mega just uses RTClib: put the clock-setting functions inside it

RTC_DS3234 RTC(CSpinRTC);

// SD CLASSES
SdFat sd;
SdFile datafile;
SdFile otherfile; // for rain gage, camera timing, and anything else that 
                  // doesn't follow the standard logging cycle / regular timing

// Datetime
DateTime now;

//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!//
//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!//
//!! LOGGER LIBRARY COMPONENTS !!//
//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!//
//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!//

// Constructor
Logger::Logger(){}

void Logger::initialize(char* _logger_name, char* _filename, int _log_minutes, bool _ext_int){
  /*
  Model automatically determined from the MCU type and is used to modify 
  pinout-dependent functions. There may be a need to add a board version in the 
  future, and I don't see a way around passing that via a human-coded variable. 

  log_minutes is the number of minutes between logging intervals
  The logger will wake up, decrement this value, check and check if it is <=0.
  If so, it will log; otherwise, it will go back to sleep.
  */
  
  ///////////////////
  // SLEEP COUNTER //
  ///////////////////
  
  // Assign the class-wide static members to the input values
  logger_name = _logger_name;
  filename = _filename;
  log_minutes = _log_minutes;

  //////////////////////////////////////////
  // EXTERNAL INTERRUPT (E.G., RAIN GAGE) //
  //////////////////////////////////////////
  
  // Specific for the bottle logger!
  extInt = _ext_int;
  if (extInt){
    pinMode(extInt, INPUT);
    digitalWrite(3, HIGH); // enable internal 20K pull-up
    //pinMode(6, INPUT);
    //digitalWrite(6, LOW);
  }
  
  ////////////
  // SERIAL //
  ////////////

  Serial.begin(57600);

  //////////////////
  // Logger setup //
  //////////////////

  // From weather station code
  // For power savings
  // http://jeelabs.net/projects/11/wiki/Weather_station_code
  #ifndef cbi
  #define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
  #endif
  #ifndef sbi
  #define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
  #endif
  
  //////////////////////
  // LOGGER FILE NAME //
  //////////////////////
  
  delay(20);
  
  Serial.print("Filename: ");
  Serial.println(filename);
  
  Serial.println(F("Logger done initializing."));
  
}

void Logger::setupLogger(){

  Serial.println(F("Beginning logger setup."));

  // We use a 3.3V regulator that we can switch on and off (to conserve power) 
  // to power the instruments. Therefore, we set the analog reference to 
  // "EXTERNAL". Do NOT set it to the internal 1.1V reference voltage or to 
  // "DEFAULT" (VCC), UNLESS you are  absolutely sure that you need to and the
  // 3.3V regulator connected to the AREF pin is off. Otherwise, you will short
  // 1.1V (or VCC) against 3.3V and likely damage the MCU / fry the ADC(?)
  analogReference(EXTERNAL); // Commented out in other code - check on how to make this all work

  // Start up SPI and RTC
  SPI.begin();
  RTC.begin();

  ///////////////////////////////////
  // CHECK IF LOG_MINUTES IS VALID //
  ///////////////////////////////////

  // Warn if log_minutes is bad.
  // This only works for intervals of < 1 hour
  if (log_minutes > 59){
    Serial.println(F("CANNOT LOG AT INTERVALS OF >= 1 HOUR"));
    Serial.println(F("PLEASE CHANGE <log_minutes> PASSED TO FUNCTION <sleep> TO AN INTEGER <= 59"));
    LEDwarn(300); // 300 quick flashes of the LED - serious badness!
  }
  else if (log_minutes == -1){
    Serial.println(F("Disabling time-lapse data logging (log_minutes = -1 is the call for this)"));
    Serial.println(F("This also disables the ""Log Now"" button."));
  }
  // 0 will always log - so need to figure out what to do about that. maybe just decide hourly ok default?

  //////////////
  // SET PINS //
  //////////////

  pinMode(wakePin,INPUT); // Interrupt to wake up
  digitalWrite(wakePin,HIGH); // enable internal 20K pull-up
  // Set the rest of the pins: this is my pinModeRunning() function in other code,
  // but really is just as good to plop in here
  pinMode(CSpinSD,OUTPUT);
  pinMode(SensorPin3V3,OUTPUT);
  pinMode(LEDpin,OUTPUT);
  pinMode(SDpin,OUTPUT);
  // Manual wake pin - only on the new bottle loggers
  Serial.println(F("Setting manualWakePin"));
  pinMode(manualWakePin,INPUT);
  digitalWrite(manualWakePin,HIGH); // enable internal 20K pull-up
  // FIX FIX FIX - PUT ALL PINS INTO "LOW" MODE, BASED ON LOGGER MODEL! COMPLETE COMPLETE!
  //pinMode(6, OUTPUT);
  //digitalWrite(6, LOW);
  //Start out with SD, Sensor pins set LOW
  digitalWrite(SDpin,LOW);
  digitalWrite(SensorPin3V3,LOW);
  // Start out with CS pins high -- no data transmission
  digitalWrite(CSpinSD, HIGH);
  digitalWrite(CSpinRTC, HIGH);

////////////
// SERIAL //
////////////

Serial.begin(57600);

// Announce start
announce_start();

/////////////////
// CHECK CLOCK //
/////////////////

// Includes check whether you are talking to Python terminal
startup_sequence();

/////////////////////////////////////////////////////////////////////
// Set alarm to go off every time seconds==00 (i.e. once a minute) //
/////////////////////////////////////////////////////////////////////

DS3234_alarm1_1min(); // Change this to specific minutes later -- but will require a deprecation or bringing-up-to-date decision (DS3231 code)
                      // So for now just use clunky way of waking every minute.


///////////////////
// SD CARD SETUP //
///////////////////

// Initialize SdFat or print a detailed error message and halt
// Use half speed like the native library.
// change to SPI_FULL_SPEED for more performance.

delay(1000);

name();
Serial.print(F("Initializing SD card..."));
digitalWrite(SDpin,HIGH); // SD CARD ON
SDstart();
delay(10);
if (!sd.begin(CSpinSD, SPI_HALF_SPEED)){
  Serial.println(F("Card failed, or not present"));
  LEDwarn(20); // 20 quick flashes of the LED
  sd.initErrorHalt();
}
SDend();

Serial.println(F("card initialized."));
Serial.println();
LEDgood(); // LED flashes peppy happy pattern, indicating that all is well

delay(50);

name();
Serial.println(F("Logger initialization complete! Ciao bellos."));

delay(50);

digitalWrite(SDpin,LOW); // SD CARD OFF

}

/////////////////////////////////////////////////////
// PRIVATE FUNCTIONS: UTILITIES FOR LOGGER LIBRARY //
/////////////////////////////////////////////////////

  /*
  Not using at the moment -- just hope users don't override the utility pin commands!
  
  void Logger::pinUnavailable(int pin){
    int _errorFlag = 0;
    #define protected_pins_list_length 7
    if (_model == bottle_logger)
    {
      char* _pinNameList[protected_pins_list_length] = {"CSpinSD", "SensorPin3V3", "SDpin", "LEDpin", "wakePin", "SDApin", "SCLpin"};
      int _pinList[protected_pins_list_length] = {CSpinSD, SensorPin3V3, SDpin, LEDpin, wakePin, SDApin, SCLpin};
    }
    else if (_model == log_mega)
    {
      char* _pinNameList[protected_pins_list_length] = {"CSpinSD", "SensorPin3V3", "SDpin", "LEDpin", "wakePin", "CSpinRTC", "SensorPinVCC"};
      int _pinList[protected_pins_list_length] = {MISOpin, MOSIpin, CSpinSD, SensorPin3V3, SDpin, LEDpin, wakePin, CSpinRTC, SensorPinVCC};
    }

    for (int i=0; i<protected_pins_list_length; i++){
      if (pin == _pinList[i]){
        _errorFlag++;
        Serial.print("Error: trying to alter the state of Pin ");
        Serial.println(_pinList[i]);
        Serial.print("This pin is assigned in a system-critical role as: ");
        Serial.println(_pinNameList[i]);
        // Note: numbers >13 in standard Arduino are analog pins
      }
    }
    
    if (_errorFlag){
      Serial.println(F("Error encountered."));
      Serial.println(F("Stalling program: cannot reassign critical pins to sensors, etc."));
      LEDwarn(50); // 50 quick flashes of the LED
      // Do nothing until reset
      while(1){}
    }
  }
  */

  void Logger::sleepNow()         // here we put the arduino to sleep
  {

      /* Now is the time to set the sleep mode. In the Atmega8 datasheet
       * http://www.atmel.com/dyn/resources/prod_documents/doc2486.pdf on page 35
       * there is a list of sleep modes which explains which clocks and 
       * wake up sources are available in which sleep mode.
       *
       * In the avr/sleep.h file, the call names of these sleep modes are to be found:
       *
       * The 5 different modes are:
       *     SLEEP_MODE_IDLE         -the least power savings 
       *     SLEEP_MODE_ADC
       *     SLEEP_MODE_PWR_SAVE
       *     SLEEP_MODE_STANDBY
       *     SLEEP_MODE_PWR_DOWN     -the most power savings
       *
       * For now, we want as much power savings as possible, so we 
       * choose the according 
       * sleep mode: SLEEP_MODE_PWR_DOWN
       * 
       */  
      set_sleep_mode(SLEEP_MODE_STANDBY);   // sleep mode is set here

  //    setPrescaler(6); // Clock prescaler of 64, slows down to conserve power
      cbi(ADCSRA,ADEN);                    // switch Analog to Digitalconverter OFF

      sleep_enable();          // enables the sleep bit in the mcucr register
                               // so sleep is possible. just a safety pin 

      /* Now it is time to enable an interrupt. We do it here so an 
       * accidentally pushed interrupt button doesn't interrupt 
       * our running program. if you want to be able to run 
       * interrupt code besides the sleep function, place it in 
       * setup() for example.
       * 
       * In the function call attachInterrupt(A, B, C)
       * A   can be either 0 or 1 for interrupts on pin 2 or 3.   
       * 
       * B   Name of a function you want to execute at interrupt for A.
       *
       * C   Trigger mode of the interrupt pin. can be:
       *             LOW        a low level triggers
       *             CHANGE     a change in level triggers
       *             RISING     a rising edge of a level triggers
       *             FALLING    a falling edge of a level triggers
       *
       * In all but the IDLE sleep modes only LOW can be used.
       */

      if (log_minutes == -1 && extInt == false){
        Serial.println(F("All inputs to wake from sleep disabled! Reprogram, please!"));
      }
      if (log_minutes != -1){
        attachInterrupt(interruptNum, wakeUpNow, LOW); // wakeUpNow when wakePin gets LOW 
      }
      if (extInt){
        attachInterrupt(1, wakeUpNow_tip, LOW);
      }

      // Copied from http://www.arduino.cc/cgi-bin/yabb2/YaBB.pl?num=1243973204
      //power_adc_disable();
      //power_spi_disable();
      //power_timer0_disable();
      //power_timer1_disable();
      //power_timer2_disable(); // uncommented because unlike forum poster, I don't rely
                              // on an internal timer
      //power_twi_disable();

      // Clearing the Serial buffer: http://forum.arduino.cc/index.php?topic=134764.0
      /*
      delayMicroseconds(10000);
      Serial.flush();
      while (!(UCSR0A & (1 << UDRE0)))  // Wait for empty transmit buffer
       UCSR0A |= 1 << TXC0;  // mark transmission not complete
      while (!(UCSR0A & (1 << TXC0)));   // Wait for the transmission to complete
      */
      
      // End Serial
      //Serial.end();

      sleep_mode();            // here the device is actually put to sleep!!
                               // THE PROGRAM CONTINUES FROM HERE AFTER WAKING UP

      // After waking, run sleep mode function, and then remainder of this function (below)
      sleep_disable();         // first thing after waking from sleep:
                               // disable sleep...
      detachInterrupt(1); // crude, but keeps interrupts from clashing. Need to improve this to allow both measurements types!
                          // Maybe move this to specific post-wakeup code?
      detachInterrupt(interruptNum);      // disables interrupt so the 
                               // wakeUpNow code will not be executed 
                               // during normal running time.
                               
      // Reset the alarm flag (end pull-down of interrupt pin)
      RTC.clear_alarm_flag(1);

      //delay(3); // Slight delay before I feel OK taking readings

      // Copied from http://www.arduino.cc/cgi-bin/yabb2/YaBB.pl?num=1243973204
      //power_all_enable();
      /*
      Serial.begin(57600);
      Serial.flush();
      while (!(UCSR0A & (1 << UDRE0)))  // Wait for empty transmit buffer
       UCSR0A |= 1 << TXC0;  // mark transmission not complete
      while (!(UCSR0A & (1 << TXC0)));   // Wait for the transmission to complete
      */

  }

  // Must be defined outside of Logger class
  void wakeUpNow()        // here the interrupt is handled after wakeup
  {
    // execute code here after wake-up before returning to the loop() function
    // timers and code using timers (serial.print and more...) will not work here.
    // we don't really need to execute any special functions here, since we
    // just want the thing to wake up
    sbi(ADCSRA,ADEN);                    // switch Analog to Digitalconverter ON
  }

  void wakeUpNow_tip()        // here the interrupt is handled after wakeup
  {
    // execute code here after wake-up before returning to the loop() function
    // timers and code using timers (serial.print and more...) will not work here.
    // we don't really need to execute any special functions here, since we
    // just want the thing to wake up
    //sleep_disable();         // first thing after waking from sleep:
                               // disable sleep...
    //delayMicroseconds(10000);
    tip = true;
  }

  void Logger::DS3234_alarm1_1min()
  {
    // Sets an alarm that will go off once a minute
    // for intermittent data logging
    // (This will use the AVR interrupt)
    uint8_t flags[5] = { 0, 1, 1, 1, 1 }; // only care about seconds count
    int wake_SECOND = 0; // wake on the 00 seconds
    RTC.set_alarm_1(wake_SECOND, 0, 0, 0, flags);
    RTC.enable_alarm(1);
    RTC.clear_alarm_flag(1);
  }

  void Logger::LEDwarn(int nflash)
  {
    // Flash LED quickly to say that the SD card (and therefore the logger)
    // has not properly initialized upon restart
    for(int i=0;i<=nflash;i++){
      digitalWrite(LEDpin,HIGH);
      delay(50);
      digitalWrite(LEDpin,LOW);
      delay(50);
    }
  }

  void Logger::LEDgood()
  {
    // Peppy blinky pattern to show that the logger has successfully initialized
    digitalWrite(LEDpin,HIGH);
    delay(1000);
    digitalWrite(LEDpin,LOW);
    delay(300);
    digitalWrite(LEDpin,HIGH);
    delay(100);
    digitalWrite(LEDpin,LOW);
    delay(100);
    digitalWrite(LEDpin,HIGH);
    delay(100);
    digitalWrite(LEDpin,LOW);
  }

  void Logger::LEDtimeWrong(int ncycles)
  {
    // Syncopated pattern to show that the clock has probably reset to January
    // 1st, 2000
    for(int i=0;i<=ncycles;i++)
    {
      digitalWrite(LEDpin,HIGH);
      delay(250);
      digitalWrite(LEDpin,LOW);
      delay(100);
      digitalWrite(LEDpin,HIGH);
      delay(100);
      digitalWrite(LEDpin,LOW);
      delay(100);
    }
  }

  void Logger::unixDatestamp(){

    now = RTC.now();

    // SD
    SDstart();
    datafile.print(now.unixtime()); // Should work without clock's CSpinRTC -- digging into object that is already made
    datafile.print(",");
    SDend();
    // Echo to serial
    Serial.print(now.unixtime());
    Serial.print(F(","));
  }

  void Logger::endLine(){
    // Ends the line in the file; do this at end of recording instance
    // before going back to sleep
    SDstart();
    datafile.println();
    Serial.println();
    SDend();
    }

float Logger::_vdivR(int pin,float Rref){
  // Same as public vidvR code, but returns value instead of 
  // saving it to a file
  int _ADC;
  _ADC = analogRead(pin);
  float _ADCnorm = _ADC/1023.0; // Normalize to 0-1
  float _R = Rref/_ADCnorm - Rref; // R1 = (R2-Vin)/Vout - R2
  return _R;
}

////////////////////////////////////////////////////////////
// PUBLIC UTILITY FUNCTIONS TO IMPLEMENT LOGGER IN SKETCH //
////////////////////////////////////////////////////////////

void Logger::sleep(int log_minutes){
  // Go to sleep
  backtosleep:
  sleepNow();
  delay(100);
  //Serial.println("Hi");
  Serial.println(tip);
  // First, check if there was a bucket tip from the rain gage, if present
  if (tip){
    Serial.println("Tip!");
    pinMode(LEDpin, OUTPUT);
    digitalWrite(LEDpin, HIGH);
    TippingBucketRainGage();
    delay(50);
    digitalWrite(LEDpin, LOW);
    pinMode(LEDpin, INPUT);
    tip = false;
    goto backtosleep; // Could remove this to log everything else on each bucket tip
  }
  // Check if the logger has been awakend by someone pushing the button
  // If so, bypass everything else
  if (digitalRead(manualWakePin) == LOW){
  }
  else{
    DateTime rightnow = RTC.now();
    int minute = rightnow.minute();
    // Only wake if you really have to
    if (minute % log_minutes == 0){
      Serial.println(F("Logging!"));
    }
    else {
      Serial.print(F("Going back to sleep for "));
      Serial.print(minute % log_minutes);
      if (minute % log_minutes == 1){
        Serial.println(F(" more minute"));
      }
      else{
        Serial.println(F(" more minutes"));
      }
      goto backtosleep;
    }
  }
}
  
void Logger::startLogging(){
  // TROUBLESHOOTING: HAD TO USE cs() IN RTC_DS3234 SUB-FUNCTIONS
  //tmp = RTC.clear_alarm_flag(1);
  //Serial.print(F("TMP: "));
  //Serial.println(tmp);
  /*
  SPI.setDataMode(SPI_MODE3);
  digitalWrite(CSpinRTC, LOW);
  SPI.transfer(0x8F);
  SPI.transfer(0);
  digitalWrite(CSpinRTC, HIGH);
  */
  pinMode(SDpin,OUTPUT); // Seemed to have forgotten between loops... ?
  // Initialize logger
  digitalWrite(SDpin,HIGH); // Turn on SD card before writing to it
                            // Delay required after this??
  SDstart();
  delay(10);
  if (!sd.begin(CSpinSD, SPI_HALF_SPEED)) {
    // Just use Serial.println: don't kill batteries by aborting code 
    // on error
    Serial.println(F("Error initializing SD card for writing"));
  }
  delay(10);
  // open the file for write at end like the Native SD library
  if (!datafile.open(filename, O_WRITE | O_CREAT | O_AT_END)) {
    // Just use Serial.println: don't kill batteries by aborting code 
    // on error
    Serial.print(F("Opening "));
    Serial.print(filename);
    Serial.println(F(" for write failed"));
  delay(10);
  }
  SDend();
  // Datestamp the start of the line
  unixDatestamp();
}

void Logger::endLogging(){
  // Ends line, turns of SD card, and resets alarm: ready to sleep
  endLine();

  // close the file: (This does the actual sync() step too - writes buffer)
  SDstart();
  datafile.close();
  delay(2);
  digitalWrite(SDpin,LOW); // Turns off SD card
  SDend();
}

void Logger::startAnalog(){
  // Turn on power to analog sensors
  digitalWrite(SensorPin3V3,HIGH);
  delay(2);
}

void Logger::endAnalog(){
  // Turn off power to analog sensors
  digitalWrite(SensorPin3V3,LOW);
  delay(2);
}


////////////////////////////////
// SENSOR INTERFACE FUNCTIONS //
////////////////////////////////

// Thermistor - with b-value
//////////////////////////////

void Logger::thermistorB(float R0,float B,float Rref,float T0degC,int thermPin){
  // R0 and T0 are thermistor calibrations
  //
  // EXAMPLES:
  // thermistorB(10000,3950,30000,25,tempPin); // Cantherm from DigiKey
  // thermistorB(10000,3988,13320,25,tempPin); // EPCOS, DigiKey # 495-2153-ND

  // Voltage divider
  float Rtherm = _vdivR(thermPin,Rref);
  //float Rtherm = 10000;
  
  // B-value thermistor equations
  float T0 = T0degC + 273.15;
  float Rinf = R0*exp(-B/T0);
  float T = B / log(Rtherm/Rinf);
  
  // Convert to celsius
  T = T - 273.15;
  
  ///////////////
  // SAVE DATA //
  ///////////////

  // SD
  SDstart();
  datafile.print(T);
  datafile.print(",");
  SDend();
  // Echo to serial
  Serial.print(T);
  Serial.print(F(","));

}

// MaxBotix ruggedized standard size ultrasonic rangefinder: 
// 1 cm = 1 10-bit ADC interval
//////////////////////////////////////////////////////////////

void Logger::ultrasonicMB_analog_1cm(int nping, int EX, int sonicPin, bool writeAll){
  // Returns distance in cm
  // set EX=99 if you don't need it
  
  float range; // The most recent returned range
  float ranges[nping]; // Array of returned ranges
  float sumRange = 0; // The sum of the ranges measured
  float meanRange; // The average range over all the pings

//  Serial.println();
  // Get range measurements
  // Get rid of any trash; Serial.flush() unnecessary; main thing that is important is
  // getting the 2 pings of junk out of the way
  Serial.flush();
  for (int i=1; i<=2; i++){
    if(EX != 99){
      digitalWrite(EX,HIGH);
        delay(1);
      digitalWrite(EX,LOW);
      }
    delay(100);
    }
  for(int i=1;i<=nping;i++){
    if(EX != 99){
      digitalWrite(EX,HIGH);
        delay(1);
      digitalWrite(EX,LOW);
      }
    delay(100);
    range = analogRead(sonicPin);
    ranges[i-1] = range; // 10-bit ADC value = range in cm
                         // C is 0-indexed, hence the "-1"
    if (writeAll){
      Serial.print(range);
      Serial.print(F(","));
      SDstart();
      datafile.print(range);
      datafile.print(",");
      SDend();
    }
  sumRange += range;
  }
 
  // Find mean of range measurements from sumRange and nping
  meanRange = sumRange/nping;
  
  // Find standard deviation
  float sumsquares = 0;
  float sigma;
  for(int i=0;i<nping;i++){
    // Sum the squares of the differences from the mean
    sumsquares += square(ranges[i]-meanRange);
  }
  // Calculate stdev
  sigma = sqrt(sumsquares/nping);
    
  ///////////////
  // SAVE DATA //
  ///////////////

  SDstart();
  datafile.print(meanRange);
  datafile.print(",");
  datafile.print(sigma);
  datafile.print(",");
  SDend();
  // Echo to serial
  Serial.print(meanRange);
  Serial.print(F(","));
  Serial.print(sigma);
  Serial.print(F(","));

}

void Logger::maxbotixHRXL_WR_analog(int nping, int sonicPin, int EX, bool writeAll){
  // Returns distance in mm, +/- 5 mm
  // Each 10-bit ADC increment corresponds to 5 mm.
  // set EX=99 if you don't need it (or leave it clear -- this is default)
  // Default nping=10 and sonicPin=A0
  // Basically only differs from older MB sensor function in its range scaling
  // and its added defaults.
  
  float range; // The most recent returned range
  float ranges[nping]; // Array of returned ranges
  float sumRange = 0; // The sum of the ranges measured
  float meanRange; // The average range over all the pings
  int sp; // analog reading of sonic pin; probably unnecessary, but Arduino warns against having too many fcns w/ artihmetic, I think

  // Get range measurements
  // Get rid of any trash; Serial.flush() unnecessary; main thing that is important is
  // getting the 2 pings of junk out of the way
  Serial.flush();
  for (int i=1; i<=2; i++){
    if(EX != 99){
      digitalWrite(EX,HIGH);
        delay(1);
      digitalWrite(EX,LOW);
      }
    delay(100);
    }
  for(int i=1;i<=nping;i++){
    if(EX != 99){
      digitalWrite(EX,HIGH);
        delay(1);
      digitalWrite(EX,LOW);
      }
    delay(100);
    sp = analogRead(sonicPin);
    range = (sp + 1) * 5;
    ranges[i-1] = range; // 10-bit ADC value (1--1024) * 5 = range in mm
                         // C is 0-indexed, hence the "-1"
    if (writeAll){
      Serial.print(range);
      Serial.print(F(","));
      SDstart();
      datafile.print(range);
      datafile.print(",");
      SDend();
    }
  sumRange += range;
  }
 
  // Find mean of range measurements from sumRange and nping
  meanRange = sumRange/nping;
  
  // Find standard deviation
  float sumsquares = 0;
  float sigma;
  for(int i=0;i<nping;i++){
    // Sum the squares of the differences from the mean
    sumsquares += square(ranges[i]-meanRange);
  }
  // Calculate stdev
  sigma = sqrt(sumsquares/nping);
    
  ///////////////
  // SAVE DATA //
  ///////////////

  SDstart();
  datafile.print(meanRange);
  datafile.print(",");
  datafile.print(sigma);
  datafile.print(",");
  SDend();
  // Echo to serial
  Serial.print(meanRange);
  Serial.print(F(","));
  Serial.print(sigma);
  Serial.print(F(","));

}

float Logger::maxbotixHRXL_WR_Serial(int Ex, int Rx, int npings, bool writeAll, int maxRange, bool RS232){
  // Stores the ranging output from the MaxBotix sensor
  int myranges[npings]; // Declare an array to store the ranges [mm] // Should be int, but float for passing to fcns
  // Get nodata value - 5000 or 9999 based on logger max range (in meters)
  // I have also made 0 a nodata value, because it appears sometimes and shouldn't
  // (minimum range = 300 mm)
  int nodata_value;
  if (maxRange == 5){ 
    nodata_value = 5000;
  }
  else if (maxRange == 10){
    nodata_value = 9999;
  }
  // Put all of the range values in the array 
  for (int i=0; i<npings; i++){
    myranges[i] = maxbotix_Serial_parse(Ex, Rx, RS232);
  }
  // Then get the mean and standard deviation of all of the data
  int npings_with_nodata_returns = 0;
  unsigned long sum_of_good_ranges = 0;
  int good_ranges[npings];
  int j=0;
  for (int i=0; i<npings; i++){
    if (myranges[i] != nodata_value && myranges[i] != 0){
      sum_of_good_ranges += myranges[i];
      good_ranges[j] = myranges[i];
      j++;
    }
    else{
      npings_with_nodata_returns ++;
    }
  }
  float npings_with_real_returns = npings - npings_with_nodata_returns;
  float mean_range;
  float standard_deviation;
  // Avoid div0 errors
  if (npings_with_real_returns > 0){
    mean_range = sum_of_good_ranges / npings_with_real_returns;
    standard_deviation = standard_deviation_from_array(good_ranges, npings_with_real_returns, mean_range);
  }
  else {
    mean_range = -9999;
    standard_deviation = -9999;
  }
  SDstart();
  // Write all values if so desired
  if (writeAll){
    for (int i=0; i<npings; i++){
      datafile.print(myranges[i]);
      datafile.print(",");
      // Echo to serial
      Serial.print(myranges[i]);
      Serial.print(F(","));
    }
  }
  // Always write the mean, standard deviation, and number of good returns
  datafile.print(mean_range);
  datafile.print(",");
  datafile.print(standard_deviation);
  datafile.print(",");
  datafile.print(npings_with_real_returns);
  datafile.print(",");
  SDend();
  // Echo to serial
  Serial.print(mean_range);
  Serial.print(F(","));
  Serial.print(standard_deviation);
  Serial.print(F(","));
  Serial.print(npings_with_real_returns);
  Serial.print(F(","));
  
  // return mean range for functions that need it, e.g., to trigger camera
  return mean_range;
}

float Logger::standard_deviation_from_array(float values[], int nvalues, float mean){
  float sumsquares = 0;
  for (int i=0; i<nvalues; i++){
    sumsquares += square(values[i] - mean);
  }
  return sqrt(sumsquares/nvalues);
}

float Logger::standard_deviation_from_array(int values[], int nvalues, float mean){
  float sumsquares = 0;
  for (int i=0; i<nvalues; i++){
    sumsquares += square(values[i] - mean);
  }
  return sqrt(sumsquares/nvalues);
}


int Logger::maxbotix_Serial_parse(int Ex, int Rx, bool RS232){
  // Excites the MaxBotix sensor and receives its ranging output
  char range[7]; // R####<\r>, so R + 4 chars + carriage return + null
  SoftwareSerial mySerial(Rx, -1, RS232); // RX, TX, inverse logic - RS232 true, TTL false; defaults to TTL (false)
  mySerial.begin(9600);
  //Excite the sensor to produce a pulse
  pinMode(Ex, OUTPUT);
  digitalWrite(Ex, HIGH);
  delay(1);
  digitalWrite(Ex, LOW);
  // Record the result of the ranging
  int i=0; // counter
  // Not sure if this will work - maybe loop around to the other end of the array?
  while (range[i-1] != 13){
    if (mySerial.available()){
      range[i] = mySerial.read();
      i++;
    }
  }
  //Serial.print(range);
  // Convert to integer
  char r2[4]; // range stripped of all of its extra characters
  for (int i=1; i<5; i++){
    r2[i-1] = range[i];
  }
  int r3 = atol(r2);
  //Serial.print(", ");
  //Serial.print(r3);
  //Serial.print("; ");
  return r3;
  //return atol(r2); // Return integer values in mm; no parsing of error values
}

void Logger::HackHD(int control_pin, bool want_camera_on){
//void Logger::HackHD(int control_pin, int indicator_pin, bool want_camera_on){
  // 0.2 mA quiescent current draw
  // 600 mA while recording
  // Use audio outut pin as indicative of whether the camera is on or not:
  // HIGH on, LOW off. This is "indicator_pin".
  // Drop control_pin to GND to turn camera on or off
  //startAnalog();
  //delay(5000);
  // Analog functionality is off unless I explicity turn on the 3V3 regulator
  // Indicator seems to be 909 for off, and 1023 for on.
  //int indicator = 0
  //int indicator_pin = A1;
  //int indicator = analogRead(indicator_pin);
  //Serial.print(indicator);
  //endAnalog();
  // So I check if it is > 1000
  //bool camera_is_on = (indicator > 1000);
  //bool camera_is_on = (indicator > 0); // unnecessary holdover, and just if(indicator)
  Serial.print("C");
  Serial.print(camera_is_on);
  Serial.print(want_camera_on);
  // Turn camera on or off if needed
  //if ( (camera_is_on == 0 && want_camera_on == 1) || (camera_is_on == 1 && want_camera_on == 0)){
  if (camera_is_on != want_camera_on){
    pinMode(control_pin, OUTPUT);
    digitalWrite(control_pin, LOW);
    delay(200);
    pinMode(control_pin, INPUT);
    digitalWrite(control_pin, HIGH);
    camera_is_on = 1 - camera_is_on; // flips it from true to false and vice versa
    // Use this to get times of camera on/off
    start_logging_to_otherfile("camera.txt");
    if (want_camera_on == 1){
      otherfile.print("ON");
    }
    else if (want_camera_on == 0){
      otherfile.print("OFF");
    }
    end_logging_to_otherfile();
  }
  // Otherwise, these conditions match and we are in good shape.  
}

void Logger::TippingBucketRainGage(){
  // Uses the interrupt to read a tipping bucket rain gage.
  // Then prints date stamp
  pinMode(SDpin,OUTPUT); // Seemed to have forgotten between loops... ?
  digitalWrite(SDpin,HIGH); // might want to use a digitalread for better incorporation into normal logging cycle
  SDstart();
  delay(10);
  if (!sd.begin(CSpinSD, SPI_HALF_SPEED)) {
    // Just use Serial.println: don't kill batteries by aborting code 
    // on error
    Serial.println(F("Error initializing SD card for writing"));
  }
  SDend();
  delay(10);
  start_logging_to_otherfile("b_tips.txt");
  end_logging_to_otherfile();
  digitalWrite(SDpin,LOW);
  Serial.println(F("Tip!"));
  delay(200); // to make sure tips aren't double-counted
}

void Logger::start_logging_to_otherfile(char* filename){
  // open the file for write at end like the Native SD library
  SDstart();
  delay(10);
  if (!otherfile.open(filename, O_WRITE | O_CREAT | O_AT_END)) {
    // Just use Serial.println: don't kill batteries by aborting code 
    // on error
    Serial.print(F("Opening "));
    Serial.print(filename);
    Serial.println(F(" for write failed"));
  delay(10);
  }
  SDend();
  // Datestamp the start of the line - modified from unixDateStamp function
  now = RTC.now();
  // SD
  otherfile.print(now.unixtime());
  otherfile.print(",");
  // Echo to serial
  SDstart();
  Serial.print(now.unixtime());
  Serial.print(F(","));
  SDend();
}

void Logger::end_logging_to_otherfile(){
  // Ends line and closes otherfile
  // Copied from endLine function
  otherfile.println();
  Serial.println();
  // close the file: (This does the actual sync() step too - writes buffer)
  otherfile.close();
}

//reads a 5tm soil moisture probe and prints results to Serial
// Modified from Steve Hicks' code for an LCD reader by Andy Wickert

/*
void Logger::decagon5TE(int excitPin, int dataPin){

  NewSoftSerial mySerial(excitPin, dataPin);  //5tm's red wire (serial data out) connected to pin 5, pin 6 goes nowhere
  int Epsilon_Raw, Sigma_Raw, T_Raw;   //temporary integer variables to store the 3 parts of the incoming serial stream from the 5TM
  char dataStream[14];   // Max 14 characters: 4x3 + 2 spaces
  int startflag=1;
  int endflag=0;
  int i=0;
  unsigned int startMillis; // same comment as right below
  unsigned int elapsed = 0; // shouldn't overflow on the time scales I'm using

  if(startflag){
    digitalWrite(excitPin,HIGH);
    startMillis = millis();
    startflag=0;
    Serial.println(startMillis);
  }

  // OK if it takes longer, so long as data stream is continuous
  // so we don't break out of inner while loop, and we start 
  // receiving before 200 ms is up
  while (elapsed < 200){
    elapsed = millis() - startMillis;
    Serial.print(F("ms elapsed = "));
    Serial.println(elapsed);
    //  code keeps looping until incoming serial data appears on the mySerial pin
    while (mySerial.available()) {
      //Serial.println("Getting data:");
        delay(1);  
      if (mySerial.available() >0) {
    		endflag=1;
        char c = mySerial.read();  //gets one byte from serial buffer
          Serial.println(c);
        if((c>='0' and c<='9') || c==' ') {
         dataStream[i] = c; //makes the string readString 
         i++;
        }
      }
    }
  }
    
  if(endflag==1){

    digitalWrite(excitPin,LOW);
    Serial.println(dataStream);
    endflag=0;

    // parse the array into 3 integers  (for the 5TM, y is always 0)
    sscanf (dataStream, "%d %d %d", &Epsilon_Raw, &Sigma_Raw, &T_Raw);     

    // Change measured values into real values, via equations in Decagon 5TE
    // manual

    // Dielectric permittivity [-unitless-]
    if (Epsilon_Raw == 4095){
      // Error alert!
      char Epsilon_a[6] = "ERROR";
    }
    else {
      float Epsilon_a = Epsilon_Raw/50.;
    }
    // Electrical Conductivity [dS/m]
    if (Sigma_Raw == 1023){
      // Error alert!
      char EC[6] = "ERROR";
    }
    else if (Sigma_Raw <= 700){
      float EC = Sigma_Raw/100.;
    }
    else {
      // (i.e. Sigma_Raw > 700, but no elif needed so long as input string
      // parses correctly... hmm, should maybe protect against that)
      float EC = (700. + 5.*(Sigma_Raw- 700.))/100.;
    }
    // Temperature [degrees C]
    // Combined both steps of the operation as given in the manual
    if (T_Raw == 1023){
      // Error alert!
      char T[6] = "ERROR";
    }
    else if (T_Raw <= 900){
      float T = (T_Raw - 400.) / 10.;
    }
    else {
      // (i.e. T_Raw > 900, but no elif needed so long as input string
      // parses correctly... hmm, should maybe protect against that)
      float T = ((900. + 5.*(T_Raw-900.) - 400.)) / 10.;
    }
    
*/    ///////////////
    // SAVE DATA //
    ///////////////
/*
    datafile.print(Epsilon_a);
    datafile.print(",");
    datafile.print(EC);
    datafile.print(",");
    datafile.print(T);
    datafile.print(",");
    // Echo to serial
    Serial.print(Epsilon_a);
    Serial.print(F(","));
    Serial.print(EC);
    Serial.print(F(","));
    Serial.print(T);
    Serial.print(F(","));
*/
//  }
//}


void Logger::vdivR(int pin, float Rref){
  float _R = _vdivR(pin, Rref);
  
  ///////////////
  // SAVE DATA //
  ///////////////

  SDstart();
  datafile.print(_R);
  datafile.print(",");
  SDend();
  // Echo to serial
  Serial.print(_R);
  Serial.print(F(","));

}

void Logger::flex(int flexPin, float Rref, float calib1, float calib2){
  float _Rflex = _vdivR(flexPin, Rref);
  // FINISH WRITING CODE
}

void Logger::linearPotentiometer(int linpotPin, float Rref, float slope, float intercept){
  float _Rpot = _vdivR(linpotPin, Rref);
  float _dist = slope*_Rpot + intercept;
}


// NEW STUFF:

void Logger::name(){
  // Self-identify before talking
  Serial.print(F("<"));
  Serial.print(logger_name);
  Serial.print(F(">: "));
}


void Logger::print_time(){
  boolean exit_flag = 1;
  // Wait for computer to tell logger to start sending its time
  char go;
  while( exit_flag ){
    go = Serial.read();
    if( (go == 'g') && exit_flag ){
      exit_flag = 0; // Exit loop once this is done
      // Print times before setting clock
      for (int i=0; i<5; i++){
        now = RTC.now();
        Serial.println(now.unixtime());
        if ( i<4 ){
          // No need to delay on the last time through
          delay(1000);
        }
      }
    }
  } // This is the end of the while loop for clock setting and passing the old
    // time over to the computer.
}

void Logger::set_time_main(){
  // Now set clock and returns 5 more times as part of that function
  // First thing coming in should be the time from the computer
  boolean exit_flag = 1;
  while( exit_flag ){
    if ( Serial.available() ){
      clockSet();
      exit_flag = 0; // Totally out of loop now
    }
  }
}
void Logger::announce_start(){
  Serial.println();
  name();
  Serial.println(F(" = this logger's name."));
  Serial.println();
  delay(500);
  Serial.println(F("********************** Logger initializing. **********************"));
}

void Logger::startup_sequence(){
  boolean comp = 0;
  unsigned long unixtime_at_start;
  int millisthen = millis();
  while ( (millis() - millisthen) < 2000 && (comp == 0)){
    if ( Serial.available() ){
      comp = 1;
    }
  }
  name();
  Serial.println(F("HELLO, COMPUTER."));
  delay(500);
  if ( comp ){
    delay(4000); // Give Python time to print
    name();
    Serial.print(F("LOGGING TO FILE ["));
    Serial.print(filename);
    Serial.println(F("]"));
    delay(1500);
    name();
    Serial.print(F("UNIX TIME STAMP ON MY WATCH IS: "));
    now = RTC.now();
    unixtime_at_start = now.unixtime();
    Serial.println(unixtime_at_start);
    delay(1500);
    if(unixtime_at_start < 1000000000){
      name();
      Serial.println(F("Uh-oh: that doesn't sound right! Clock reset to 1/1/2000?"));
      LEDtimeWrong(3);
      print_time();
      set_time_main();
      delay(2000);
      name();
      Serial.println(F("Thanks, computer! I think I'm all set now."));
    }
    else{
      name();
      //Serial.println(F("Clock is probably fine"));
      ///*
      Serial.println(F("How does that compare to you, computer?"));
      delay(1500);
      print_time();
      delay(1500);
      name();
      Serial.println(F("Would you like to set the logger's clock to the computer's time? (y/n)"));
      boolean waiting = 1;
      char yn;
      while ( waiting ){
        if ( Serial.available() ){
          yn = Serial.read();
          if ( yn == 'y' || yn == 'Y'){
            waiting = 0;
            set_time_main();
          }
          else if ( yn == 'n' || yn == 'N'){
            waiting = 0;
            name();
            Serial.println(F("Not selecting time; continuing."));
            delay(1500);
          }
          else{
            name();
            Serial.println(F("Please select <y> or <n>."));
          }
        }
      }//*/
    }
  }
  else{
    // No serial; just blink
    DateTime now = RTC.now();
    unixtime_at_start = now.unixtime();
    // Keep Serial just in case computer is connected w/out Python terminal
    Serial.print(F("Current UNIX time stamp according to logger is: "));
    Serial.println(unixtime_at_start);
    if(unixtime_at_start < 1000000000){
      LEDtimeWrong(3);
    }
  }
  if ( comp ){
    delay(1500);
    name();
    Serial.println(F("Now beginning to log."));
    delay(1000);
  }
}

// CLOCK SETTING -- NEED TO REWRITE LIBRARY FOR DS3234
void Logger::clockSet(){

  byte Year;
  byte Month;
  byte Date;
  byte DoW;
  byte Hour;
  byte Minute;
  byte Second;

  bool Century=false;
  bool h12;
  bool PM;

  const int len = 32;
  static char buf[len];

  DateTime nowPreSet = RTC.now();  

	GetDateStuff(Year, Month, Date, DoW, Hour, Minute, Second);

	RTC.setClockMode(false);	// set to 24h
	//setClockMode(true);	// set to 12h

  Serial.println("Setting  time");
	RTC.setYear(Year);
	RTC.setMonth(Month);
	RTC.setDate(Date);
	RTC.setDoW(DoW);
	RTC.setHour(Hour);
	RTC.setMinute(Minute);
	RTC.setSecond(Second);

	// Give time at next five seconds
	for (int i=0; i<5; i++){
	    delay(1000);
      DateTime now = RTC.now();
      Serial.println(now.toString(buf,len));
	}
  delay(1000);
  unsigned long unixtime_at_receive_string = nowPreSet.unixtime();
  Serial.print(F("Logger's UNIX time at which it received the new time string: "));
  Serial.println(unixtime_at_receive_string);
  Serial.println(F("Clock set!"));
}

void Logger::GetDateStuff(byte& Year, byte& Month, byte& Day, byte& DoW, 
		byte& Hour, byte& Minute, byte& Second) {
	// Call this if you notice something coming in on 
	// the serial port. The stuff coming in should be in 
	// the order YYMMDDwHHMMSS, with an 'x' at the end.
	boolean GotString = false;
	char InChar;
	byte Temp1, Temp2;
	char InString[20];

	byte j=0;
	while (!GotString) {
		if (Serial.available()) {
			InChar = Serial.read();
			InString[j] = InChar;
			j += 1;
			if (InChar == 'x') {
				GotString = true;
			}
		}
	}
	Serial.println(InString);
	// Read Year first
	Temp1 = (byte)InString[0] -48;
	Temp2 = (byte)InString[1] -48;
	Year = Temp1*10 + Temp2;
	// now month
	Temp1 = (byte)InString[2] -48;
	Temp2 = (byte)InString[3] -48;
	Month = Temp1*10 + Temp2;
	// now date
	Temp1 = (byte)InString[4] -48;
	Temp2 = (byte)InString[5] -48;
	Day = Temp1*10 + Temp2;
	// now Day of Week
	DoW = (byte)InString[6] - 48;		
	// now Hour
	Temp1 = (byte)InString[7] -48;
	Temp2 = (byte)InString[8] -48;
	Hour = Temp1*10 + Temp2;
	// now Minute
	Temp1 = (byte)InString[9] -48;
	Temp2 = (byte)InString[10] -48;
	Minute = Temp1*10 + Temp2;
	// now Second
	Temp1 = (byte)InString[11] -48;
	Temp2 = (byte)InString[12] -48;
	Second = Temp1*10 + Temp2;
}

// SPI mode library for SD card -- don't want to edit SdFatLib, too much active
// work by Bill Greiman and a huge pile of code. So will do SPI switches here
// with these functions
void Logger::SDstart()
{
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  digitalWrite(CSpinSD, LOW); // CSpinSD is already set to output at the beginning, and this shoudl not change
}

void Logger::SDend()
{
  digitalWrite(CSpinSD, HIGH); // CSpinSD is already set to output at the beginning, and this shoudl not change
}

