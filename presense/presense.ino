/*
  Presense — occupancy-state room automation
  Arduino Uno | 3x HC-SR501 | 2x relay | 16x2 I2C LCD | 2 buttons

  States: EMPTY -> ACTIVE -> WARNING -> SLEEPING -> EXITING -> EMPTY

  Direction comes from WHICH DOORWAY SENSOR ROSE FIRST, compared by
  timestamp — tolerates overlapping cones and long HC-SR501 output holds.

  Talks to the PC bridge (presense.py) over USB serial:
    out:  $occ,room,light,fan,snooze,mode,secs,in,out,amb,part
    in:   M = cycle mode   S = toggle snooze   R = reset count
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- Pins ----------------
const uint8_t P1_PIN      = 2;   // entrance exterior
const uint8_t P2_PIN      = 3;   // entrance interior
const uint8_t P3_PIN      = 4;   // room interior
const uint8_t RELAY_FAN   = 5;   // CH2 sleep-essential
const uint8_t LED_PIN     = 6;   // status indicator
const uint8_t RELAY_LIGHT = 7;   // CH1 wake-only
const uint8_t BTN_MODE    = 8;
const uint8_t BTN_SNOOZE  = 9;
// LCD SDA = A4, SCL = A5

// ---------------- Config ----------------
#define DEMO_MODE 1

const bool RELAY_ACTIVE_LOW = false;

const unsigned long PIR_DEBOUNCE_MS = 30;
const unsigned long BTN_DEBOUNCE_MS = 50;
const unsigned long PAIR_WINDOW_MS  = 4000;
const unsigned long AMBIGUOUS_MS    = 15;

#if DEMO_MODE
  const unsigned long SLEEP_MS  = 30000UL;
  const unsigned long EXIT_MS   = 20000UL;
  const unsigned long SNOOZE_MS = 60000UL;
  const unsigned long STALE_MS  = 90000UL;
#else
  const unsigned long SLEEP_MS  = 20UL * 60UL * 1000UL;
  const unsigned long EXIT_MS   = 2UL * 60UL * 1000UL;
  const unsigned long SNOOZE_MS = 60UL * 60UL * 1000UL;
  const unsigned long STALE_MS  = 40UL * 60UL * 1000UL;
#endif

const unsigned long WARN_MS  = 10000UL;
const unsigned long BLINK_MS = 400UL;

// ---------------- PIR inputs (0=P1, 1=P2, 2=P3) ----------------
const uint8_t PIR_PIN[3]       = {P1_PIN, P2_PIN, P3_PIN};
bool          pirStable[3]     = {false, false, false};
bool          pirLastRaw[3]    = {false, false, false};
unsigned long pirLastChange[3] = {0, 0, 0};
unsigned long pirRiseTime[3]   = {0, 0, 0};
bool          pirRose[3]       = {false, false, false};

// Capture the raw rise instantly for fine timing, but only trust it
// once the signal has held steady for the debounce period.
void updatePIR(uint8_t i, unsigned long now) {
  bool raw = digitalRead(PIR_PIN[i]);

  if (raw != pirLastRaw[i]) {
    pirLastRaw[i] = raw;
    pirLastChange[i] = now;
    if (raw) pirRiseTime[i] = now;
  } else if ((now - pirLastChange[i]) >= PIR_DEBOUNCE_MS) {
    if (raw && !pirStable[i]) pirRose[i] = true;   // confirmed rising edge
    pirStable[i] = raw;
  }
}

// ---------------- Buttons (0=mode, 1=snooze) ----------------
const uint8_t BTN_PIN[2]       = {BTN_MODE, BTN_SNOOZE};
bool          btnStable[2]     = {true, true};
bool          btnLastRaw[2]    = {true, true};
unsigned long btnLastChange[2] = {0, 0};
bool          btnPressed[2]    = {false, false};

void updateBtn(uint8_t i, unsigned long now) {
  bool raw = digitalRead(BTN_PIN[i]);

  if (raw != btnLastRaw[i]) {
    btnLastRaw[i] = raw;
    btnLastChange[i] = now;
  } else if ((now - btnLastChange[i]) >= BTN_DEBOUNCE_MS) {
    if (!raw && btnStable[i]) btnPressed[i] = true;   // falling edge = press
    btnStable[i] = raw;
  }
}

// ---------------- Doorway ----------------
unsigned long pend1 = 0, pend2 = 0;
bool waitingClear = false;

int occupants = 0;
unsigned long lastDoorMotion = 0;

// Diagnostics — use these while aiming the sensors
unsigned int entries = 0, exits = 0, ambiguous = 0, partial = 0;

// ---------------- Room state ----------------
enum RoomState { EMPTY, ACTIVE, WARNING, SLEEPING, EXITING };
RoomState room = EMPTY;
const char *ROOM_NAME[] = {"EMPTY", "ACTIVE", "WARNING", "SLEEPING", "EXITING"};

unsigned long lastActivity = 0, warnStart = 0, exitStart = 0;
bool lightsOn = false, fanOn = false;

bool snoozeActive = false;
unsigned long snoozeStart = 0;

enum Mode { AUTO, FORCE_ON, FORCE_OFF };
Mode mode = AUTO;

// ---------------- Relay helper ----------------
void writeRelay(uint8_t pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? !on : on);
}

void setup() {
  // Drive the relays LOW first, before anything slow like the LCD.
  pinMode(RELAY_LIGHT, OUTPUT);
  pinMode(RELAY_FAN, OUTPUT);
  writeRelay(RELAY_LIGHT, false);
  writeRelay(RELAY_FAN, false);

  for (uint8_t i = 0; i < 3; i++) pinMode(PIR_PIN[i], INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_MODE, INPUT_PULLUP);
  pinMode(BTN_SNOOZE, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("    PRESENSE    ");
  lcd.setCursor(0, 1);
  lcd.print("Warming up...   ");

  Serial.begin(9600);
  delay(3000);
  lcd.clear();

  Serial.println(F("== PRESENSE =="));
  Serial.print(F("Startup  P1:")); Serial.print(digitalRead(P1_PIN));
  Serial.print(F(" P2:"));         Serial.print(digitalRead(P2_PIN));
  Serial.print(F(" P3:"));         Serial.println(digitalRead(P3_PIN));
  Serial.println(F("A sensor reading 1 with nobody moving is misaimed."));
  Serial.println(F("Give the PIRs 60s to settle before trusting counts."));

  lastDoorMotion = millis();
  lastActivity   = millis();
}

void loop() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < 3; i++) updatePIR(i, now);
  for (uint8_t i = 0; i < 2; i++) updateBtn(i, now);

  handleButtons(now);
  handleDoorway(now);
  handleActivity(now);
  handleState(now);
  applyOutputs(now);
  updateDisplay(now);
  sendData();
  readCommands();

  delay(10);            // 100 Hz — good resolution for edge ordering
}

// ---------------- Doorway: direction by rise order ----------------
void handleDoorway(unsigned long now) {

  if (pirRose[0]) { pirRose[0] = false; if (!waitingClear && !pend1) pend1 = pirRiseTime[0]; }
  if (pirRose[1]) { pirRose[1] = false; if (!waitingClear && !pend2) pend2 = pirRiseTime[1]; }

  // Both fired — direction is decided by which rose first.
  if (pend1 && pend2) {
    long diff = (long)(pend2 - pend1);          // positive = P1 first = entry

    if (diff > (long)AMBIGUOUS_MS) {
      occupants++;
      entries++;
      lastActivity = now;
    } else if (diff < -(long)AMBIGUOUS_MS) {
      if (occupants > 0) occupants--;
      exits++;
    } else {
      ambiguous++;        // rose together — cones overlap too much to tell
    }

    pend1 = pend2 = 0;
    waitingClear = true;
  }

  // Only one fired and the window ran out — approached and turned back.
  if (!waitingClear && (pend1 || pend2)) {
    unsigned long t = pend1 ? pend1 : pend2;
    if (now - t > PAIR_WINDOW_MS) {
      pend1 = pend2 = 0;
      partial++;
    }
  }

  // Both must clear before the next crossing, so one person cannot be
  // counted twice while the sensor outputs are still held high.
  if (waitingClear && !pirStable[0] && !pirStable[1]) waitingClear = false;

  if (pirStable[0] || pirStable[1]) lastDoorMotion = now;
}

// ---------------- Interior activity ----------------
void handleActivity(unsigned long now) {
  if (pirStable[2])  lastActivity = now;
  if (snoozeActive)  lastActivity = now;   // snooze freezes sleep AND stale timers

  if (occupants > 0 &&
      (now - lastActivity   > STALE_MS) &&
      (now - lastDoorMotion > STALE_MS)) {
    occupants = 0;                          // safety net for a missed exit
  }
}

// ---------------- State machine ----------------
void handleState(unsigned long now) {

  if (snoozeActive && (now - snoozeStart >= SNOOZE_MS)) snoozeActive = false;

  if (occupants > 0) {

    if (room == EMPTY || room == EXITING) {
      room = ACTIVE;
      lastActivity = now;
    }

    if (snoozeActive) {
      if (room == WARNING || room == SLEEPING) {
        room = ACTIVE;
        lastActivity = now;
      }
    } else {
      unsigned long preWarn = (SLEEP_MS > WARN_MS) ? (SLEEP_MS - WARN_MS) : 0;
      if (room == ACTIVE && (now - lastActivity > preWarn)) {
        room = WARNING;
        warnStart = now;
      }
      if (room == WARNING) {
        if (pirStable[2]) { room = ACTIVE; lastActivity = now; }
        else if (now - warnStart >= WARN_MS) room = SLEEPING;
      }
      if (room == SLEEPING && pirStable[2]) {
        room = ACTIVE;
        lastActivity = now;
      }
    }

  } else {
    if (room != EMPTY && room != EXITING) {
      room = EXITING;
      exitStart = now;
    }
    if (room == EXITING && (now - exitStart >= EXIT_MS)) {
      room = EMPTY;
      snoozeActive = false;
    }
  }

  switch (room) {
    case ACTIVE:
    case WARNING:   lightsOn = true;  fanOn = true;  break;
    case SLEEPING:  lightsOn = false; fanOn = true;  break;   // the whole idea
    case EXITING:   lightsOn = false; fanOn = true;  break;
    case EMPTY:     lightsOn = false; fanOn = false; break;
  }
}

// ---------------- Outputs ----------------
void applyOutputs(unsigned long now) {
  bool l = lightsOn, f = fanOn;

  if (room == WARNING && l) l = ((now / BLINK_MS) % 2 == 0);   // blink to warn

  if (mode == FORCE_ON)  { l = true;  f = true;  }
  if (mode == FORCE_OFF) { l = false; f = false; }

  writeRelay(RELAY_LIGHT, l);
  writeRelay(RELAY_FAN, f);
  digitalWrite(LED_PIN, (l || f) ? HIGH : LOW);
}

// ---------------- Buttons ----------------
void handleButtons(unsigned long now) {
  if (btnPressed[0]) {
    btnPressed[0] = false;
    if (mode == AUTO)          mode = FORCE_ON;
    else if (mode == FORCE_ON) mode = FORCE_OFF;
    else                       mode = AUTO;
  }

  if (btnPressed[1]) {
    btnPressed[1] = false;
    snoozeActive = !snoozeActive;
    snoozeStart  = now;
    lastActivity = now;
    if (snoozeActive && (room == WARNING || room == SLEEPING)) room = ACTIVE;
  }
}

// ---------------- LCD ----------------
void updateDisplay(unsigned long now) {
  char line1[17];
  char modeTag   = (mode == AUTO) ? ' ' : (mode == FORCE_ON ? '1' : '0');
  char snoozeTag = snoozeActive ? 'S' : ' ';
  snprintf(line1, sizeof(line1), "Occupants: %-2d %c%c", occupants, snoozeTag, modeTag);
  lcd.setCursor(0, 0);
  lcd.print(line1);

  char line2[17];
  if (room == WARNING) {
    unsigned long left = (WARN_MS - (now - warnStart)) / 1000UL;
    snprintf(line2, sizeof(line2), "Sleeping in %2lus ", left);
  } else if (room == EXITING) {
    unsigned long left = (EXIT_MS - (now - exitStart)) / 1000UL;
    snprintf(line2, sizeof(line2), "Timer: %02lu:%02lu    ", left / 60UL, left % 60UL);
  } else {
    snprintf(line2, sizeof(line2), "Status: %-8s", ROOM_NAME[room]);
  }
  lcd.setCursor(0, 1);
  lcd.print(line2);

  // A motor switching nearby can corrupt the display. Quietly re-init
  // every 10 s so a glitch never persists into the demo.
  static unsigned long lastReinit = 0;
  if (now - lastReinit > 10000) {
    lastReinit = now;
    lcd.init();
    lcd.backlight();
  }
}

// ---------------- PC bridge: telemetry out ----------------
void sendData() {
  static unsigned long last = 0;
  if (millis() - last < 500) return;
  last = millis();

  unsigned long secs = 0;
  if (room == WARNING)      secs = (WARN_MS - (millis() - warnStart)) / 1000UL;
  else if (room == EXITING) secs = (EXIT_MS - (millis() - exitStart)) / 1000UL;

  Serial.print('$');
  Serial.print(occupants);      Serial.print(',');
  Serial.print((int)room);      Serial.print(',');
  Serial.print(lightsOn);       Serial.print(',');
  Serial.print(fanOn);          Serial.print(',');
  Serial.print(snoozeActive);   Serial.print(',');
  Serial.print((int)mode);      Serial.print(',');
  Serial.print(secs);           Serial.print(',');
  Serial.print(entries);        Serial.print(',');
  Serial.print(exits);          Serial.print(',');
  Serial.print(ambiguous);      Serial.print(',');
  Serial.println(partial);
}

// ---------------- PC bridge: commands in ----------------
void readCommands() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == 'M') {
      if (mode == AUTO)          mode = FORCE_ON;
      else if (mode == FORCE_ON) mode = FORCE_OFF;
      else                       mode = AUTO;
    }
    else if (c == 'S') {
      snoozeActive = !snoozeActive;
      snoozeStart  = millis();
      lastActivity = millis();
      if (snoozeActive && (room == WARNING || room == SLEEPING)) room = ACTIVE;
    }
    else if (c == 'R') {
      occupants = 0;
    }
  }
}