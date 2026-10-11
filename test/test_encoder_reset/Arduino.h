#pragma once

// Host only: this suite links the actual EncoderInput.cpp and VehicleRuntime.cpp.
// One pending A interrupt coalesces while masked; this is NOT an edge-capture model.
#include <cstddef>
#include <cstdint>
#include <string>

constexpr int LOW = 0, HIGH = 1, OUTPUT = 1, INPUT_PULLUP = 2, CHANGE = 3;
constexpr unsigned A1 = 15, A2 = 16;

namespace fakeArduino {
extern int pins[256];
extern int duty[256];
extern uint32_t primask;
extern void (*encoderCallback)();
extern void (*boundaryHook)(int boundary);
extern bool pending;
extern bool inIsr;
extern unsigned admitted;
extern unsigned blocked;
extern unsigned saves;
extern unsigned disables;
extern unsigned restores;
void admitPending();
void requestEncoderInterrupt();
void boundary(int point);
}

inline void pinMode(unsigned, int) {}
inline void digitalWrite(unsigned pin, int value) { fakeArduino::pins[pin] = value; }
inline int digitalRead(unsigned pin) {
    fakeArduino::boundary(2);
    return fakeArduino::pins[pin];
}
inline int analogRead(unsigned) { return 0; }
inline void analogWrite(unsigned pin, int value) {
    fakeArduino::duty[pin] = value;
    fakeArduino::pins[pin] = value ? HIGH : LOW;
}
inline void analogWriteResolution(unsigned) {}
inline unsigned digitalPinToInterrupt(unsigned pin) { return pin; }
inline void attachInterrupt(unsigned, void (*callback)(), int) {
    fakeArduino::encoderCallback = callback;
}
inline uint32_t __get_PRIMASK() {
    ++fakeArduino::saves;
    fakeArduino::boundary(0);
    return fakeArduino::primask;
}
inline void __disable_irq() {
    fakeArduino::primask = 1U;
    ++fakeArduino::disables;
    fakeArduino::boundary(1);
}
inline void __set_PRIMASK(uint32_t mask) {
    fakeArduino::boundary(3);
    fakeArduino::primask = mask;
    ++fakeArduino::restores;
    fakeArduino::admitPending();
}

struct FakeSerial {
    std::string bytes;
    void begin(unsigned long) {}
    int availableForWrite() const { return 4096; }
    std::size_t write(const uint8_t* data, std::size_t size) {
        bytes.append(reinterpret_cast<const char*>(data), size);
        return size;
    }
    int read() { return -1; }
    explicit operator bool() const { return true; }
    bool rts() const { return false; }
};
extern FakeSerial Serial;
