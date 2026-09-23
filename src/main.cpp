#include <Arduino.h>

#include <IntervalTimer.h>

constexpr unsigned long SERIAL_BAUD = 115200;
constexpr uint32_t BLINK_INTERVAL_MS = 500;

IntervalTimer blinkTimer(BLINK_INTERVAL_MS);
bool ledState = false;

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);

    Serial.begin(SERIAL_BAUD);

    const unsigned long startMs = millis();
    while (!Serial && millis() - startMs < 2000) {
    }

    Serial.println("Science Olympiad EV");
    Serial.println("Nano R4 firmware baseline");
    Serial.println("READY");
}

void loop() {
    if (blinkTimer.elapsed(millis())) {
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
    }
}
