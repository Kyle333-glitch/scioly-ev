#pragma once

// Host-side mock of the Arduino core API surface used by the firmware headers
// under test (R3 diagnostics harness).
//
// This mock exists so the REAL production sources (`src/VehicleRuntime.cpp`,
// `src/EncoderInput.cpp`, `src/bench/BenchConsole.h` and every real header
// they include) can be compiled and exercised by host tests. Nothing here
// models hardware behavior: pins hold whatever the test sets, the serial
// port is a scripted RX queue plus a captured TX log, and interrupts only
// fire when the test fires them. Any test that passes here proves logic
// about the firmware source, never anything about the vehicle.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define CHANGE 2
#define LED_BUILTIN 13
#define A0 14
#define A1 15
#define A2 16

typedef int pin_size_t;

namespace evmock {

// --- Digital/analog pins ---------------------------------------------------

struct PinWrite {
    int pin = 0;
    int value = 0;
    bool isAnalog = false;
};

inline std::vector<PinWrite>& pinWriteLog() {
    static std::vector<PinWrite> log;
    return log;
}

inline int* pinLevels() {
    static int levels[64];
    return levels;
}

inline int* analogValues() {
    static int values[64];
    return values;
}

inline int& pwmResolution() {
    static int bits = 8;
    return bits;
}

inline void resetPins() {
    for (int i = 0; i < 64; ++i) {
        pinLevels()[i] = HIGH;
        analogValues()[i] = 0;
    }
    pinWriteLog().clear();
    pwmResolution() = 8;
}

}  // namespace evmock

inline void pinMode(int /*pin*/, int /*mode*/) {}

inline void digitalWrite(int pin, int value) {
    if (pin >= 0 && pin < 64) {
        evmock::pinLevels()[pin] = value;
    }
    evmock::PinWrite write;
    write.pin = pin;
    write.value = value;
    write.isAnalog = false;
    evmock::pinWriteLog().push_back(write);
}

inline int digitalRead(int pin) {
    if (pin >= 0 && pin < 64) {
        return evmock::pinLevels()[pin];
    }
    return LOW;
}

inline int analogRead(int pin) {
    if (pin >= 0 && pin < 64) {
        return evmock::analogValues()[pin];
    }
    return 0;
}

inline void analogWrite(int pin, int value) {
    evmock::PinWrite write;
    write.pin = pin;
    write.value = value;
    write.isAnalog = true;
    evmock::pinWriteLog().push_back(write);
}

inline void analogWriteResolution(int bits) { evmock::pwmResolution() = bits; }

// --- Interrupts ------------------------------------------------------------

namespace evmock {

typedef void (*IsrHandler)(void);

inline IsrHandler& storedIsr() {
    static IsrHandler handler = nullptr;
    return handler;
}

inline void resetIsr() { storedIsr() = nullptr; }

}  // namespace evmock

inline int digitalPinToInterrupt(int pin) { return pin; }

inline void attachInterrupt(int /*interruptNum*/, void (*userFunc)(void), int /*mode*/) {
    evmock::storedIsr() = userFunc;
}

// Fires the handler captured by attachInterrupt, if any. The handler reads
// the mock pin levels, so a test advances the decoder by setting levels and
// calling this.
inline void evmock_fireStoredIsr() {
    if (evmock::storedIsr() != nullptr) {
        evmock::storedIsr()();
    }
}

// --- Critical sections (no real concurrency on the host) -------------------

inline uint32_t __get_PRIMASK() { return 0U; }
inline void __disable_irq() {}
inline void __set_PRIMASK(uint32_t /*primask*/) {}

// --- Clock -----------------------------------------------------------------

namespace evmock {
inline unsigned long& mockMillis() {
    static unsigned long now = 0UL;
    return now;
}
}  // namespace evmock

inline unsigned long millis() { return evmock::mockMillis(); }

// --- Serial ----------------------------------------------------------------

namespace evmock {

class MockSerial {
  public:
    void begin(unsigned long /*baud*/) {}

    int availableForWrite() { return txBlocked ? 0 : 4096; }

    size_t write(const uint8_t* bytes, size_t count) {
        if (txBlocked) {
            return 0U;
        }
        txBytes.append(reinterpret_cast<const char*>(bytes), count);
        return count;
    }

    int read() {
        if (rxBytes.empty()) {
            return -1;
        }
        const char front = rxBytes.front();
        rxBytes.erase(rxBytes.begin());
        return static_cast<unsigned char>(front);
    }

    operator bool() const { return connected; }

    bool rts() const { return rtsAsserted; }

    // Test control: feed received bytes, read back everything staged so far.
    void injectRx(const char* text) {
        while (*text != '\0') {
            rxBytes.push_back(*text);
            ++text;
        }
    }

    void injectRx(const std::string& text) {
        for (char c : text) {
            rxBytes.push_back(c);
        }
    }

    std::string takeTx() {
        const std::string out = txBytes;
        txBytes.clear();
        return out;
    }

    void reset() {
        rxBytes.clear();
        txBytes.clear();
        connected = true;
        rtsAsserted = false;
        txBlocked = false;
    }

    bool connected = true;
    bool rtsAsserted = false;
    // S19 host-test control: when true the port accepts nothing, which is how
    // a test stages a stalled-host backpressure condition against the REAL
    // transport/framer and dispatch. Production firmware never sets this; the
    // real USB endpoint reports its own free space.
    bool txBlocked = false;

  private:
    std::string rxBytes;
    std::string txBytes;
};

}  // namespace evmock

extern evmock::MockSerial Serial;
