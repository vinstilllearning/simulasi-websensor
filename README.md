# LUMA / 15

A complete local lighting instrument for the **original ESP32 / ESP32-WROOM Dev Module** and the hardware already present in the uploaded Lampu Hias Pintar sketch. The system pairs a nonblocking firmware state machine with a self-contained, responsive dashboard. It adds no imaginary motors or actuators: the only actuator is a 15-pixel WS2812B strip; the inputs are DHT11, LDR, MAX4466 and PIR.

## System blueprint

- **Twelve light engines:** Prism, Breath, Aurora, Meteor, Ember, Tide, Twinkle, Spectrum, Scanner, Confetti, Heartbeat and Candle. Shared speed, saturation, direction and brightness controls shape their output.
- **Seven operating modes:** manual color; effect; temperature; reactive music; motion/light automation; a self-cycling showcase; and time-of-day circadian light. The showcase advances through all effects and sensor modes.
- **Useful routines:** weekly schedules, including overnight windows; a configurable wake fade; motion hold and idle shutdown; ambient-light dimming; a sleep fade/timer; and focus/break Pomodoro sessions.
- **Resilient operation:** settings survive reboot in ESP32 NVS; temporary countdowns restart safely rather than resuming stale sessions. Deferred writes reduce flash wear. Wi-Fi reconnects without pausing the animation loop, and a device access point remains available.
- **Honest feedback:** live heap and minimum-heap health, uptime, Wi-Fi and AP status, sensor availability, clock validity, automation gate reason, timer state and the actual rendered RGB values for all 15 pixels.
- **Vanilla delivery:** HTML, CSS, JavaScript and SVG are bundled in one page, with no CDN or external framework. The generated gzip page is served from flash. REST requests run on the ESP-IDF HTTP server task; validated commands enter a bounded queue and only the main loop changes application state.

The runtime contains no application `delay()` calls. DHT11 acquisition uses timed start/response states and interrupt edge capture, rather than a blocking DHT library. ADC sampling, animation, sensor reads, timers, clockkeeping, reconnects and persistence have independent timing responsibilities. Library calls, flash writes and the physical WS2812 transfer still take finite execution time; this is cooperative firmware, not a hard real-time guarantee.

## Files

| File | Purpose |
| --- | --- |
| `firmware/LampuHiasPintar/LampuHiasPintar.ino` | Complete ESP32 sketch |
| `index.html`, `dashboard.html` | Identical, standalone dashboard |
| `firmware/LampuHiasPintar/dashboard.h` | Generated gzip/PROGMEM dashboard payload |
| `tools/embed_dashboard.py` | Rebuild the embedded page after dashboard changes |
| `API_CONTRACT.md` | Shared firmware/dashboard protocol definition |

## Pinout and electrical reference

| Component | ESP32 pin | Interface / interpretation |
| --- | --- | --- |
| 15 × WS2812B | GPIO16 → first LED DIN | 800 kHz, GRB, LED 0 at the lamp base |
| DHT11 | GPIO4 | Digital data; checksum-validated temperature and humidity |
| LDR divider/module | GPIO34 | ADC1 channel 6, input only; larger raw value means darker |
| GY-MAX4466 analog microphone | GPIO35 | ADC1 channel 7, input only; software envelope/gain processing |
| HC-SR501 PIR | GPIO27 | Digital motion input; a high signal refreshes motion hold |

Use an ESP32-WROOM board on which GPIO16 is free. ESP32-WROVER boards can reserve GPIO16 for PSRAM; ESP32-C3/S3 boards have different pin mappings and are not drop-in targets.

- Power the LED strip from a regulated **5 V supply**, with a common ground shared with ESP32 and sensors. Do not power the strip from the ESP32 3.3 V pin. Size wiring and the supply for the strip's physical maximum, even though firmware requests a 5 V / 800 mA software power budget. Software estimation is not a fuse or hardware current limiter.
- Use a 3.3 V → 5 V logic buffer such as 74AHCT125 on the LED data line for reliable WS2812 operation, a 330–470 Ω series data resistor near the first LED and a suitable bulk capacitor, typically 470–1000 µF, across the strip supply. Observe capacitor polarity and voltage rating.
- ESP32 GPIO and ADC inputs are **not 5 V tolerant**. Power the MAX4466 and LDR analog circuitry at 3.3 V or divide/condition their output so it never exceeds the ESP32 input rating. ADC calibration depends on the actual divider and room acoustics.
- GPIO34 and GPIO35 are input-only and have no internal pull-up or pull-down. Provide the external LDR divider/bias circuitry. Their ADC1 mapping avoids the classic ESP32 ADC2/Wi-Fi conflict.
- Pull DHT11 data up to **3.3 V**, typically with 4.7–10 kΩ unless the module already provides a suitable resistor. A module powered at 5 V must not pull its data line up to 5 V.
- The HC-SR501 normally uses a 5 V supply and a roughly 3.3 V digital output; verify your module. Allow its hardware warm-up time, and remember that its own hold/retrigger jumpers affect the signal seen by firmware.

## Build and flash

The validated versions are **Arduino CLI 1.3.1**, **ESP32 by Espressif 3.3.6**, **FastLED 3.9.20** and **ArduinoJson 7.4.2**, pinned in `firmware/dependencies.lock.json`. Wi-Fi, Preferences, mDNS and `esp_http_server` are provided by the ESP32 core; a DHT library is not required.

Install these versions through Arduino IDE's board/library managers, or with Arduino CLI:

```sh
arduino-cli core update-index --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.6 --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli lib install "FastLED@3.9.20" "ArduinoJson@7.4.2"
```

From the repository root, rebuild the page before every firmware build:

```sh
python3 tools/embed_dashboard.py
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/LampuHiasPintar
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/ttyUSB0 firmware/LampuHiasPintar
arduino-cli monitor --port /dev/ttyUSB0 --config baudrate=115200
```

Alternatively, `python3 tools/build_firmware.py` embeds and builds in one step, preserving the compiler exit status. It accepts `--cli`, `--config`, `--jobs`, `--build-path` and `--output`; binaries default to `build/binaries/`. Do not flash a binary for a different ESP32 family or pin mapping.

Select your actual serial port rather than assuming `/dev/ttyUSB0`. Board-core and library installation require their ordinary upstream package downloads; keep TLS and package checksums enabled. Check the sketch's Wi-Fi configuration section before flashing if you want it to join a home LAN. Never commit real LAN credentials. AP-only use does not require home Wi-Fi credentials.

Connect to the device access point using the device-specific password printed on the serial console, then open the AP address printed there. On a joined LAN, use the reported station IP or `http://luma-15.local` where mDNS is supported. Keep this device on a trusted local network; do not port-forward its HTTP API to the Internet. The generated AP credential protects Wi-Fi association, but the local HTTP API has no per-user login or TLS. It rejects foreign browser origins and does not enable permissive CORS; this is not a substitute for network access control.

## Dashboard and simulation

Normal device mode uses same-origin `/api/*` endpoints and displays hardware telemetry. Losing the device produces a visible connection error; it never silently invents sensor values or falls back to simulation. A failed or missing DHT reading is shown as unavailable.

For an explicit demonstration, open the standalone HTML as a file or use its `?demo=1` query. Simulation is visibly labeled and has sensor controls. It implements the same config keys, automation gates, timer actions and effect selection, but is not proof of physical sensor or power behavior. The dashboard preview in device mode uses the RGB frames reported by firmware.

## Automation and timer rules

- **Clock:** on station Wi-Fi connection the ESP32 starts nonblocking SNTP against `pool.ntp.org` and `time.nist.gov`; valid network time is adopted without waiting in the main loop. The browser can also synchronize Unix UTC through `/api/time` (and initializes an invalid clock on connection), which updates both the native and monotonic software clocks. The configured UTC offset determines local minutes/day. Polling does not repeatedly replace time. There is no battery-backed RTC: after reboot, schedules stay safely off until network time or browser synchronization succeeds. AP-only operation supports browser synchronization without Internet access.
- **Schedule:** `scheduleStart` is inclusive and `scheduleEnd` exclusive. The day mask uses bit 0 for Sunday and bit 6 for Saturday. An overnight window belongs to its **start day**: Monday 22:00–02:00 includes Tuesday 01:00 when Monday is selected. Equal start/end is rejected. An enabled schedule with an invalid clock gates output off rather than guessing the time.
- **Wake fade:** `wakeMinutes` softens the beginning of the scheduled on-window; zero requests immediate output. It is not a second independent alarm.
- **Motion:** a high PIR refreshes the configured `motionHoldSec`. Auto mode also requires LDR darkness above `darkThreshold`. `idleMinutes = 0` disables additional motion-idle gating outside Auto mode.
- **Sleep:** `sleepStart` starts the configured `sleepMinutes` countdown/fade. `sleepCancel` cancels that sleep routine; changing the duration alone does not start it.
- **Pomodoro:** explicit Start, Pause, Resume and Reset actions control a focus/break cycle. The dashboard reports whether a session is running, its phase, remaining seconds and completed focus sessions.
- **Persistence:** settings are retained; clocks and active countdown/pause state are not promises of continuity across reboot. The UI exposes pending-save and storage health, plus an explicit Save action. A queued HTTP response is not confirmation that flash storage succeeded.
- **Output gates:** power, schedule/clock, idle and sleep gates can suppress any visual mode. `gateReason` explains the current suppression. Brightness and automatic fades operate within the LED power budget.

## REST API

All writes use same-origin JSON POST requests. State changes are validated before queuing; the main loop applies accepted commands. Responses are `202 {"ok":true,"queued":true}`; confirm the applied values/revision using status. Invalid keys, values or types receive 400, oversized bodies 413, and an unavailable/full command queue 503. Configuration objects are limited to 1024 bytes. Telemetry is read-only.

| Method / route | Request | Result |
| --- | --- | --- |
| `GET /` | — | Self-contained gzip dashboard |
| `GET /api/status` | — | `{apiVersion:1, revision, config, telemetry}` |
| `POST /api/config` | Partial config, e.g. `{"mode":"effect","effect":2,"brightness":120}` | Atomically validated settings update |
| `POST /api/action` | `{"action":"…"}` | Timer/persistence command |
| `POST /api/time` | `{"epoch":1793534400}` | Synchronize UTC software clock |

Supported action names: `pomodoroStart`, `pomodoroPause`, `pomodoroResume`, `pomodoroReset`, `sleepStart`, `sleepCancel`, `save`. Extra action keys are rejected. Valid browser time is an integer Unix timestamp from 2024-01-01 through 2099-12-31 UTC (1704067200–4102444799).

## Configurable parameter reference

| Key | Type / accepted values | Default | Function |
| --- | --- | --- | --- |
| `power` | Boolean | `true` | Master output gate |
| `mode` | `manual`, `effect`, `temperature`, `music`, `auto`, `demo`, `circadian` | `manual` | Operating mode |
| `effect` | Integer 0–11 | 0 | Light engine index |
| `color` | `#RRGGBB` string | `#c4b5fd` | Manual/base color |
| `brightness` | Integer 0–255 | 128 | Requested brightness ceiling |
| `speed` | Integer 1–100 | 35 | Animation speed |
| `saturation` | Integer 0–100 | 85 | Effect saturation |
| `reverse` | Boolean | `false` | Reverse spatial direction |
| `musicStyle` | `bar`, `center`, `pulse` | `bar` | Sound-reactive rendering |
| `micGain` | Integer 1–30 | 8 | Microphone envelope gain |
| `motionHoldSec` | Integer 5–600 | 45 | PIR retrigger hold |
| `darkThreshold` | Integer 0–4095 | 2000 | LDR darkness threshold |
| `autoDim` | Boolean | `true` | Ambient-light dimming |
| `idleMinutes` | Integer 0–120 | 0 | Motion-idle shutdown; 0 disables |
| `demoIntervalSec` | Integer 5–120 | 15 | Showcase step duration |
| `sleepMinutes` | Integer 1–180 | 20 | Sleep routine duration |
| `wakeMinutes` | Integer 0–60 | 10 | Scheduled fade-in duration |
| `scheduleEnabled` | Boolean | `false` | Enable weekly output schedule |
| `scheduleStart` | Integer 0–1439 | 1080 / 18:00 | Local start minute |
| `scheduleEnd` | Integer 0–1439 | 1380 / 23:00 | Local end minute |
| `scheduleDays` | Integer mask 1–127 | 127 / every day | Start-day selection |
| `utcOffsetMinutes` | Integer −720–840 | 420 / UTC+07:00 | Local time offset; no automatic DST |
| `focusMinutes` | Integer 1–120 | 25 | Pomodoro focus duration |
| `breakMinutes` | Integer 1–60 | 5 | Pomodoro break duration |

Effect index mapping: **0** Prism · **1** Breath · **2** Aurora · **3** Meteor · **4** Ember · **5** Tide · **6** Twinkle · **7** Spectrum · **8** Scanner · **9** Confetti · **10** Heartbeat · **11** Candle.

## Telemetry reference

| Group | Fields |
| --- | --- |
| Device / memory | `bootId`, `uptimeSec`, `freeHeap`, `minHeap`, `heapSize` |
| Network | `wifiConnected`, `rssi` (null when unavailable), `ip`, `apIp`, `apClients` |
| Clock | `clockValid`, `epoch` (null when invalid) |
| Sensors | `temperature`, `humidity`, `dhtOk`, `ldr`, `micLevel` (0–100), `motion`, `motionRemainingSec` |
| Output | `activeMode`, `activeEffect`, `outputBrightness`, `gateReason`, `leds` (15 RGB triplets) |
| Routines | `pomodoro.state` (`idle`, `focus`, `break`), `.running`, `.remainingSec`, `.durationSec`, `.sessions`; `sleepRemainingSec` |
| Persistence | `dirty`, `storageOk` |

`revision` changes when a config, action or clock update is applied. Heap fields are bytes, RSSI is dBm, ADC is 12-bit raw, durations are seconds, temperature is °C and humidity is % RH. Sensor nulls must remain distinguishable from zero. Status polling does not write flash.

## Validation boundaries

Run the browser checks with Python Playwright and Chromium installed:

```sh
python3 tests/test_dashboard.py
c++ -std=c++17 -Wall -Wextra -pedantic tests/test_firmware_logic.cpp -o /tmp/luma-schedule-test
/tmp/luma-schedule-test
python3 tools/embed_dashboard.py --check
```

The browser test starts its own local server on port 8765. Its device-side HTTP responses are explicitly mocked; the native tests exercise the same scheduler header compiled into the firmware. `VALIDATION.md` records the final build and test results.

Compilation, API/browser tests and physical commissioning are separate checks. Use the current build/test report to determine what ran; a simulator is not hardware validation. Before unattended use, verify the real board's DHT checksum acquisition, ADC scale/polarity, PIR warm-up/retrigger timing, LED direction, actual supply current, Wi-Fi reconnect and reboot persistence. No ESP32 or sensors are attached to the cloud execution environment.
