# Science Olympiad Electric Vehicle

Firmware and supporting tooling for the 2026-2027 Science Olympiad Division C Electric Vehicle.

## Hardware baseline

- Controller: Arduino Nano R4
- Build system: PlatformIO
- Framework: Arduino

## Development

Build:

    pio run

Upload when the Nano R4 is connected:

    pio run -t upload

Serial monitor:

    pio device monitor

## Current state

Initial development-environment baseline only. No competition motion-control firmware has been implemented yet.
