/*  File: SpotWelder.ino
  Origin: Apr-2022
  Author: Tip Partridge
  Description:
    Controller for spot welder. SCR phase control with duty and number of cycles.
    LCD display.
    Uses zero crossing detector to drive interrupts at end and start of each half cycle.
*/
#include <TM1637Display.h>    // 4 digit x 7 Segment LED display
//
////////////////////////////
// Global constant and variable declarations
////////////////////////////
//
#define verStr "SpotWelder v0.8"

#define dfltDuty 50       // default values
#define dfltCycles 10
int duty = dfltDuty;      // 0..100 = 0..100%
int nCycle = dfltCycles;  // number of cycles per pulse. 0 = continuous.
#define pulseWidth 6      // trigger pulse width in us+1 ??? Want 5 or so, so 6.
                          // Need direct write this cuz digitalWrite is really slow.
#define pulseInterval 100 // time in us between trigger pulses.  Multiple triggers each cycle to ensure SCR triggers.
int cycle;                // current cycle;
int xCycle = 1;           // used to handle ncycle = 0 continuous mode
bool first = true;        // used to track first half cycle so we always start on same one to prevent xfmr inrush.

long us100Percent;        // Half cycle active time in us
const long us100Percent_default = 8096;   // nominal at 60 Hz
unsigned long dutyus;     // calculated delay for desired duty

#define zcms 25;          // zero crossing timeout trip point
unsigned long zcTime;     // zero crossing timeout start time

char c;                   // serial command character
bool but;                 // button has been pressed flag
bool pressed;             // flag used to confirm button has been released before next press recognised
bool go;                  // free run flag, mostly for debug
volatile bool endOfCycle;     // flag from ISR indicates end of AC half cycle
volatile bool startOfCycle;   // flag from ISR indicates beginning of AC half cycle
bool zeroOK = false;          // true if receiving zero crossing pulses
bool zeroWasntOK = true;      // true if zeroOK wasn't OK last time we checked. Triggers cycle time measurement.
int zeroOKTimer = 0;          // count number of times through loop (22ms) to flash ZeroNOKLite
#define zOKTime 10            // passes per blink toggle

// Pin assignments
#define zeroPinLead 2     // Zero crossing pulse leading edge interrupt pin
#define zeroPinTrail 3    // Zero crossing pulse trailing edge interrupt pin
#define buttonPin 4       // Weld button
#define gatePin 5         // SCR trigger pulse
#define litePin 6         // Output on LED
#define DispDutyClk 7     // Cycle display
#define DispDutyDI 8
#define DispCyclesClk 9   // Duty display
#define DispCyclesDI 10
#define PhaseUp 11        // Duty up button
#define PhaseDown 12      // Duty down button
#define ZeroLite 13       // On board LED toggles with zero crossing interrupt
#define CycleUp A0        // Cycle up button
#define CycleDown A1      // Cycle down button
#define ShiftButton A2    // Shift button
#define ZeroOKLite A3     // Sync active LED
#define ZeroNOKLite A4    // Sync not active LED
// Spare pins
//spare A5
//Analog A6
//Analog A7

bool CUp = false;         // Flags to show that button has been released
bool CDn = false;
bool PUp = false;
bool PDn = false;

// Instantiate displays
TM1637Display DispDuty(DispDutyClk, DispDutyDI);        // 4 digit LED display handler
TM1637Display DispCycles(DispCyclesClk, DispCyclesDI);  // 4 digit LED display handler

// **************** SSSS **** EEEEEE *** TTTTTTTT *** UU   UU *** PPPPP ***********
// *************** SS ******* EE *********  TT ****** UU   UU *** PP   PP *********
// Setup *********** SS ***** EEEEE ******* TT ****** UU   UU *** PPPPP ***********
// ******************* SS *** EE ********** TT ****** UU   UU *** PP **************
// *************** SSSS ***** EEEEEE ****** TT ******** UUU ***** PP **************

void setup() 
  {
  Serial.begin(115200);
  Serial.println( verStr);
// ISRs for zero crossing detector
  attachInterrupt( digitalPinToInterrupt(zeroPinLead), zeroCrossIntLead, FALLING);
  attachInterrupt( digitalPinToInterrupt(zeroPinTrail), zeroCrossIntTrail, RISING);
// set up pins
  pinMode( zeroPinLead, INPUT);
  pinMode( zeroPinTrail, INPUT);
  pinMode( buttonPin, INPUT_PULLUP);
  pinMode( gatePin, OUTPUT);
  pinMode( litePin, OUTPUT);
  pinMode( DispDutyClk, OUTPUT);
  pinMode( DispDutyDI, OUTPUT);
  pinMode( DispCyclesClk, OUTPUT);
  pinMode( DispCyclesDI, OUTPUT);
  pinMode( PhaseUp, INPUT_PULLUP);
  pinMode( PhaseDown, INPUT_PULLUP);
  pinMode( ZeroLite, OUTPUT);
  pinMode( CycleUp, INPUT_PULLUP);
  pinMode( CycleDown, INPUT_PULLUP);
  pinMode( ShiftButton, INPUT_PULLUP);
  pinMode( ZeroOKLite, OUTPUT);
  pinMode( ZeroNOKLite, OUTPUT);
// check zero crossing detecter
  zeroWasntOK = !measureCycleTime();
// set up displays
  DispDuty.setBrightness(0x0f);   // LED display max brightness
  DispCycles.setBrightness(0x0f); // LED display max brightness
  }

//******************* MM      MM ****** AAA ******* IIII *** NN    NN **************************
//******************* MMMM  MMMM ***** AA AA ******* II **** NNN   NN **************************
// Main loop ******** MM  MM  MM **** AAAAAAA ****** II **** NN NN NN **************************
//******************* MM      MM *** AA     AA ***** II **** NN   NNN **************************
//******************* MM      MM ** AA       AA *** IIII *** NN    NN **************************

void loop() 
  {
// Ckeck zero crossing activity
  if (zeroOK)
    {
    digitalWrite(ZeroOKLite, HIGH);   // Sync active LED On
    digitalWrite(ZeroNOKLite, LOW);   // Sync not active LED Off
    if (zeroWasntOK)
      {
      zeroWasntOK = !measureCycleTime();
      }
    }
  else
    {
    digitalWrite(ZeroOKLite, LOW);    // Sync active LED Off
    if (zeroOKTimer > zOKTime)
      {
      digitalWrite(ZeroNOKLite, !digitalRead(ZeroNOKLite));  // Sync not active LED toggle
      zeroOKTimer = 0;
      }
    zeroWasntOK = true;
    }
  zeroOK = false;
  zeroOKTimer += 1;
  
// Update displays
  if (!go)
    {
    DispDuty.showNumberDec(duty, false, 4, 0);      // takes about 12.5ms
    DispCycles.showNumberDec(nCycle, false, 4, 0);  // takes about 12.5ms
// Check up/down buttons
    if (!digitalRead(PhaseUp) && !digitalRead(PhaseDown)) // Set to default
      {
      duty = dfltDuty;
      setxDuty();  // housekeeping
      PUp = true;
      PDn = true;
      }
    if (!digitalRead(CycleUp) && !digitalRead(CycleDown)) // Set to default
      {
      nCycle = dfltCycles;
      setxDuty();  // housekeeping
      CUp = true;
      CDn = true;
      }
    if (!digitalRead(PhaseUp))
      {
      if (!PUp || !digitalRead(ShiftButton))
        {
        duty += 1;
        setxDuty();  // housekeeping
        PUp = true;
        }
      }
    else PUp = false;
    if (!digitalRead(PhaseDown))
      {
      if (!PDn || !digitalRead(ShiftButton))
        {
        duty -= 1;
        setxDuty();  // housekeeping
        PDn = true;
        }
      }
    else PDn = false;
    if (!digitalRead(CycleUp))
      {
      if (!CUp || !digitalRead(ShiftButton))
        {
        nCycle += 1;
        setxCycle();  // housekeeping
        CUp = true;
        }
      }
    else CUp = false;
    if (!digitalRead(CycleDown))
      {
      if (!CDn || !digitalRead(ShiftButton))
        {
        nCycle -= 1;
        setxCycle();  // housekeeping
        CDn = true;
        }
      }
    else CDn = false;
    }
// Check for user command via USB serial
  if (Serial.available()) dealWithSerial();
// Check for button press
  if (!digitalRead(buttonPin) && !pressed)   // button has just been pressed
    {
    but = true;
    pressed = true;   // this remains true until button released
    }
// Button pressed (or go invoked), turn on output!
  if (but || go)  // button pressed
    {
    if (!go) Serial.print("Go!...");
    zcTime = millis() + zcms;   // set up for timeout check
    while (!first)         // wait for half cycle we start on
      if (zcTimeout()) goto Bail;
    digitalWrite( litePin, HIGH);
    setxCycle();  // housekeeping
    for (cycle=0; cycle<xCycle; cycle++)
      {
// first half cycle
      zcTime = millis() + zcms;   // set up for timeout check
      while (!endOfCycle)         // wait for end of current cycle
        if (zcTimeout()) goto Bail;
      while (!startOfCycle)       // wait for start of next cycle
        if (zcTimeout()) goto Bail;
      delayMicroseconds(dutyus);  // delay for trigger pulse
      PORTD |= 0x20;
      delayMicroseconds(pulseWidth);  // trigger pulse
      PORTD &= 0xdf;

// here more triggere. maybe until endOfCycle.
      while (!endOfCycle)         // wait for end of current cycle
        {
        delayMicroseconds(pulseInterval);
        PORTD |= 0x20;
        delayMicroseconds(pulseWidth);  // trigger pulse
        PORTD &= 0xdf;
        if (zcTimeout()) goto Bail;
        }

// second half cycle
      zcTime = millis() + zcms;   // set up for timeout check
      while (!endOfCycle)         // wait for end of current cycle
        if (zcTimeout()) goto Bail;
      while (!startOfCycle)       // wait for start of next cycle
        if (zcTimeout()) goto Bail;
      delayMicroseconds(dutyus);  // delay for trigger pulse
      PORTD |= 0x20;
      delayMicroseconds(pulseWidth);  // trigger pulse
      PORTD &= 0xdf;

// same here
      while (!endOfCycle)         // wait for end of current cycle
        {
        delayMicroseconds(pulseInterval);
        PORTD |= 0x20;
        delayMicroseconds(pulseWidth);  // trigger pulse
        PORTD &= 0xdf;
        if (zcTimeout()) goto Bail;
        }

// deal with user io
      if (!go && digitalRead(buttonPin))   // stop if button released
        {
        but = false;
        cycle = xCycle;
        }
      else if (nCycle == 0) cycle = -1; // nCycle = 0 so continue to run
      if (Serial.available()) dealWithSerial();
      }
    digitalWrite( litePin, LOW);
    if (!go) Serial.println("Done.");
    }
    goto Done;
Bail:
  go=false;
  digitalWrite( litePin, LOW);
  Serial.println("Bail");
Done:
  but = false;
  if (!go) delay(30);
  if (digitalRead(buttonPin)) pressed = false;  // button has been released
  }

//********************************************************************************
//********************************************************************************
// End Main loop *****************************************************************
//********************************************************************************
//********************************************************************************

////////////////////////////
// Command parser
////////////////////////////

void dealWithSerial()
  {
  int ii, butt0, butt1;
  unsigned long time0, time1;
  c = toupper(Serial.read());  // get character
  Serial.println(c);
  delay(10);  // wait for any more characters
  while (Serial.available()) Serial.read();   // eat newline
  switch (c)
    {
    case '?':
    case '/':
    case '>':
    case '.':
      newln();
      Serial.println("? - Menu (this) > for complicated menu");
      Serial.print(  "d - Set duty (5..100) ["); Serial.print(duty); Serial.println("]");
      Serial.print(  "n - Set number of cycles (0=button) ["); Serial.print(nCycle); Serial.println("]");
      Serial.println("+ - Increase duty");
      Serial.println("- - Decrease duty");
      Serial.println("g - Toggle Go");
      if (c=='?' || c=='/') break;
      Serial.println("t - Zero crossing Time test");
      Serial.println("w - Display write time test");
      Serial.println("b - Button bounce test");
//      Serial.println("");
      break;
    case 'N':
      Serial.print("enter number of cycles ["); Serial.print(nCycle); Serial.print("] ");
      getInt(&nCycle, 10);
      newln();
      Serial.print("nCycle = "); Serial.println(nCycle);
      setxCycle();  // housekeeping
      break;
    case 'D':
      Serial.print("enter duty cycle (5..100) ["); Serial.print(duty); Serial.print("] ");
      getInt(&duty, 10);
      newln();
      setxDuty();  // housekeeping
      Serial.print("Duty = "); Serial.println(duty);
      break;
    case 'T':
      timeZero();   // time zero crossing pulse
      break;
    case '+':
    case '=':
      duty += 5;
      setxDuty();  // housekeeping
      Serial.print("Duty = "); Serial.println(duty);
      break;
    case '-':
    case '_':
      duty -= 5;
      setxDuty();  // housekeeping
      Serial.print("Duty = "); Serial.println(duty);
      break;
    case 'G':
      go = !go;
      if (go) Serial.println("Free running!");
      else
        {
        but = false;
        cycle = xCycle;
        Serial.println("Stopped.");
        }
      break;
    case 'W':   // time display write
      time0 = millis();
      for (ii = 1; ii <= 100; ii++)
        DispCycles.showNumberDec(ii, false, 4, 0);
      Serial.print(float(millis()-time0)/100.0);
      Serial.println(" ms per write.");
      break;
    case 'B':   // Button bounce test
      Serial.print("Press Pulse button now...");
      while (digitalRead(buttonPin));  // wait for button press
      time0 = micros();
      time1 = time0;
      butt0 = 0;
      butt1 = 0;
//      Serial.println(); Serial.print(butt1); Serial.print(" "); Serial.print(time1-time0); Serial.println("us");
      while (micros() - time1 < 1000000)
        {
        butt1 = digitalRead(buttonPin);
        if (butt0 != butt1)
          {
          time1 = micros();
          Serial.print(butt1); Serial.print(" "); Serial.print(time1-time0); Serial.println("us");
          time0 = time1;
          butt0 = butt1;
          }
        }
      Serial.print("\nNow release button...");
      while (!digitalRead(buttonPin));  // wait for button release
      time0 = micros();
      time1 = time0;
      butt0 = 1;
      butt1 = 1;
//      Serial.println(); Serial.print(butt1); Serial.print(" "); Serial.print(time1-time0); Serial.println("us");
      while (micros() - time1 < 1000000)
        {
        butt1 = digitalRead(buttonPin);
        if (butt0 != butt1)
          {
          time1 = micros();
          Serial.print(butt1); Serial.print(" "); Serial.print(time1-time0); Serial.println("ms");
          time0 = time1;
          butt0 = butt1;
          }
        }
      Serial.println("Done.");
      break;
    default:
      go = false;
      but = false;
      cycle = xCycle;
      break;
    }
  }

void setxCycle()  // housekeeping
  {
  if (nCycle <= 0)
    {
    xCycle = 1;
    nCycle = 0;
    }
  else
    xCycle = nCycle;
  }

void setxDuty()  // set duty housekeeping
  {
  if (duty < 5)
    duty = 5;
  if (duty > 100)
    duty = 100;
  dutyus = (100-duty) * us100Percent / 100;
  }

////////////////////////////
// Check for zero crossing timeout
////////////////////////////

bool zcTimeout()
  {
  if (millis() > zcTime) return true;
  else return false;
  }


////////////////////////////
// Measure cycle time
////////////////////////////

bool measureCycleTime()
  {
  unsigned long t0, t1;   // used to measure cycle zero crossing time
  bool zeroMeasured = false;

  zcTime = millis() + zcms;   // set up for timeout check
  while (!endOfCycle)
    if (zcTimeout()) goto Bail;
  zcTime = millis() + zcms;   // set up for timeout check
  while (!startOfCycle)
    if (zcTimeout()) goto Bail;
  t0=micros();
  zcTime = millis() + zcms;   // set up for timeout check
  while (!endOfCycle)
    if (zcTimeout()) goto Bail;
  t1=micros();
  us100Percent = t1-t0;
  zcTime = millis() + zcms;   // set up for timeout check
  while (!startOfCycle)
    if (zcTimeout()) goto Bail;
  t0=micros();
  zcTime = millis() + zcms;   // set up for timeout check
  while (!endOfCycle)
    if (zcTimeout()) goto Bail;
  t1=micros();
  us100Percent = (us100Percent + t1-t0) / 2;
  dutyus = (100-duty) * us100Percent / 100;
  zeroMeasured = true;        // zero crossings detected
Bail:
  if (zeroMeasured)
    {
    Serial.print("Active sync width (us): ");
    Serial.println(us100Percent);
    digitalWrite( ZeroLite, HIGH);
    }
  else
    {
    us100Percent = us100Percent_default;
    Serial.println("Zero crossing sync pulses not detected!");
    digitalWrite( ZeroLite, LOW);
    }
  dutyus = (100-duty) * us100Percent / 100;
  return( zeroMeasured);
  }

////////////////////////////
// Time zero crossing pulse, for debug
////////////////////////////

void timeZero()
  {
  unsigned long t0, t1, t2, t3, t4;
  long us100Percent;
// current setup
  while (!endOfCycle) {}
  while (!startOfCycle) {}
  t0=micros();
  while (!endOfCycle) {}
  t1=micros();
  us100Percent = t1-t0;
  while (!startOfCycle) {}
  t0=micros();
  while (!endOfCycle) {}
  t1=micros();
  us100Percent = (us100Percent + t1-t0) / 2;
  dutyus = (100-duty) * us100Percent / 100;
  Serial.print("Without timeout - Active sync width (us): ");
  Serial.println(us100Percent);

// with timeout sensing
  zcTime = millis() + zcms;   // set up for timeout check
  while (!endOfCycle)
    if (zcTimeout()) goto Bail;
  zcTime = millis() + zcms;   // set up for timeout check
  while (!startOfCycle)
    if (zcTimeout()) goto Bail;
  t0=micros();
  zcTime = millis() + zcms;   // set up for timeout check
  while (!endOfCycle)
    if (zcTimeout()) goto Bail;
  t1=micros();
  us100Percent = t1-t0;
  zcTime = millis() + zcms;   // set up for timeout check
  while (!startOfCycle)
    if (zcTimeout()) goto Bail;
  t0=micros();
  zcTime = millis() + zcms;   // set up for timeout check
  while (!endOfCycle)
    if (zcTimeout()) goto Bail;
  t1=micros();
  us100Percent = (us100Percent + t1-t0) / 2;
  dutyus = (100-duty) * us100Percent / 100;
  Serial.print("With timeout -    Active sync width (us): ");
  Serial.println(us100Percent);
Bail:
  while (!startOfCycle) {}
  while (!endOfCycle) {}
  t0=micros();
  while (!startOfCycle) {}
  t1=micros();
  while (!endOfCycle) {}
  t2=micros();
  while (!startOfCycle) {}
  t3=micros();
  while (!endOfCycle) {}
  t4=micros();
  Serial.print("Zero-1  (us): "); Serial.println(t1-t0);
  Serial.print("Cycle-1 (us): "); Serial.println(t2-t1);
  Serial.print("Zero-2  (us): "); Serial.println(t3-t2);
  Serial.print("Cycle-2 (us): "); Serial.println(t4-t3);
  Serial.print("Total   (us): "); Serial.println(t4-t0);
  }

////////////////////////////
// ISRs for zero crossing detector
////////////////////////////

void zeroCrossIntLead()
  {
  digitalWrite( ZeroLite, HIGH);
  endOfCycle = true;
  startOfCycle = false;
  zeroOK = true;
  first = !first;
  }

void zeroCrossIntTrail()
  {
  digitalWrite( ZeroLite, LOW);
  startOfCycle = true;
  endOfCycle = false;
  zeroOK = true;
  }

//********************************************************************************
// End Main Program *****************************************************************
//********************************************************************************
