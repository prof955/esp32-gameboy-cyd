#include <Arduino.h>

// The emulator implementation lives in cyd_gameboy_app.cpp so the Arduino
// IDE's .ino auto-prototype generator cannot reorder the Peanut-GB declarations.
void cydGameBoySetup();
void cydGameBoyLoop();

void setup() {
  cydGameBoySetup();
}

void loop() {
  cydGameBoyLoop();
}
