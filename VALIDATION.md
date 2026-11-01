# Validation report — 2026-11-01

## Final firmware build

**PASS**, exit status 0, on the complete final source and embedded dashboard.

- Target: `esp32:esp32:esp32` (original ESP32 Dev Module / WROOM).
- Arduino CLI 1.3.1; Espressif Arduino core 3.3.6.
- FastLED 3.9.20; ArduinoJson 7.4.2.
- Program storage: **1,105,919 / 1,310,720 bytes (84%)**.
- Static global memory: **58,516 / 327,680 bytes (17%)**.
- Remaining compile-time RAM budget: 269,164 bytes; this is not measured live free heap.
- Self-contained HTML: **65,202 bytes**; gzip flash payload: **20,502 bytes**.
- `index.html` and `dashboard.html` are identical; generated `dashboard.h` matches the canonical HTML.

Executed with the checked-in build helper:

```sh
python3 tools/build_firmware.py \
  --cli /workspace/toolchains/arduino/arduino-cli \
  --config /workspace/toolchains/arduino/arduino-cli.yaml \
  --build-path /workspace/build/luma15 \
  --output /workspace/build/luma15-binaries
```

These absolute toolchain paths describe this cloud instance. On a normal workstation with the pinned packages installed, use `python3 tools/build_firmware.py`.

The cloud network denied the Arduino download-index destination. The compiler, Espressif core, classic-ESP32 libraries and required binary tools were instead obtained from their official allowed GitHub releases and checked against the publishers' package-index SHA256 / release checksum files. Library sources were checked out at the pinned official Git tags/commits. TLS and artifact verification remained enabled. Only tools for this target were installed; no C3/S3 or USB-DFU target capability is claimed.

## Native firmware logic

**PASS: 76 assertions**, exit status 0. The tests include the same `schedule_logic.h` used by the ESP32 sketch:

- All seven weekday masks and rejected empty/invalid masks.
- Inclusive start and exclusive end boundaries.
- Overnight windows owned by the previous start day after midnight.
- UTC offsets including UTC+7, +14 and −12.
- Wake fade progression, including midnight crossings.
- Invalid clock and malformed settings fail closed.

```sh
c++ -std=c++17 -Wall -Wextra -pedantic tests/test_firmware_logic.cpp -o /tmp/luma-schedule-test
/tmp/luma-schedule-test
```

## Browser integration

**PASS: 17 integration categories, zero failures**, exit status 0, in real system Chromium through Python Playwright. Command: `python3 tests/test_dashboard.py`.

Coverage includes all 24 configuration controls, all 12 effect tiles, the three music styles, sensor failure rendering, power/automatic gates, real timer countdown and pause/resume/reset, full focus-to-break transition, idle expiry, sleep completion/cancellation, clock-invalid gating/recovery, sub-minute wake progression, overnight weekday and UTC-offset behavior, showcase progression, circadian selection and mobile overflow checks.

An explicit REST fixture verifies JSON POST commands, waiting beyond a 202 queue acknowledgement for applied state, rejecting HTTP errors, honest offline display, initial/reboot clock synchronization, revision reset on reboot, rejection of delayed/out-of-order and retired-boot responses, and unknown/prototype-key validation. No unhandled JavaScript exceptions occurred.

The device transport tests use **mock HTTP responses**, not a running ESP32. Preview images are explicitly labeled simulation. The native tests exercise actual firmware scheduler code; browser simulation is a separate implementation and does not establish pixel-for-pixel equivalence with FastLED's HSV conversion or electrical behavior.

## Other checks and remaining commissioning

`python3 tools/embed_dashboard.py --check` and `git diff --check` passed. Firmware static review covered DHT interrupt sequencing, validated atomic command queuing, snapshot locking, millis rollover handling, deferred NVS writes, origin checks and power-budget accounting. No application `delay()` or `delayMicroseconds()` call exists; finite library/driver calls do not make this a hard real-time system.

No board, LED strip or sensor was attached. Physical flashing, DHT pulse timing, ADC calibration and signal levels, microphone response, PIR behavior, Wi-Fi/SNTP recovery on a real network, reboot persistence and actual current consumption remain **unrun hardware commissioning checks**. The configured 800 mA software estimate is not a hardware fuse or a measured current guarantee.
