#include <Arduino.h>

constexpr unsigned long SERIAL_BAUD = 115200;
constexpr unsigned long BLINK_INTERVAL_MS = 500;

unsigned long lastBlinkMs = 0;
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
    const unsigned long now = millis();

    if (now - lastBlinkMs >= BLINK_INTERVAL_MS) {
        lastBlinkMs = now;
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
    }
}
