/*
 * LUMA / 15 — ESP32 ambient instrument firmware
 * Target: ESP32 Dev Module / WROOM, Arduino-ESP32 3.3.x.
 * Libraries: FastLED 3.9.x+, ArduinoJson 7.x. dashboard.h is generated from
 * index.html by tools/embed_dashboard.py. No DHT library is required.
 *
 * GPIO16: 15 WS2812B GRB; GPIO4: DHT11 (external 4.7k pull-up to 3.3 V);
 * GPIO34: LDR divider (dark = larger ADC); GPIO35: MAX4466; GPIO27: PIR.
 * LED supply: external regulated 5 V, common GND, 800 mA software ceiling.
 * ADC inputs MUST remain below 3.3 V. See the pinout documentation.
 *
 * Loop-owned state, interrupt-only DHT pulse capture, bounded HTTP command
 * queue and immutable telemetry snapshot. HTTP runs on the IDF server task.
 * Every animation, reconnect, sensor and timer has its own time state machine.
 * NVS commits are deferred and wear-limited; timer progress deliberately does
 * not resume after reboot. No delay(), delayMicroseconds(), or polling waits.
 *
 * Optional station credentials: set LUMA_WIFI_SSID and LUMA_WIFI_PASS as build
 * macros in a local, untracked build configuration. Otherwise reuse previously
 * provisioned ESP32 station credentials. Never commit home Wi-Fi credentials.
 * A unique WPA2 access point is always available; read its password on serial.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <FastLED.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <time.h>
#include <sys/time.h>
#include "dashboard.h"
#include "schedule_logic.h"

#ifndef LUMA_WIFI_SSID
#define LUMA_WIFI_SSID ""
#endif
#ifndef LUMA_WIFI_PASS
#define LUMA_WIFI_PASS ""
#endif

class LumaFirmware {
public:
  static constexpr uint8_t LED_PIN = 16, PIXELS = 15, DHT_PIN = 4;
  static constexpr uint8_t LDR_PIN = 34, MIC_PIN = 35, PIR_PIN = 27;
  static constexpr uint16_t MAX_MILLIAMPS = 800;
  static constexpr size_t BODY_LIMIT = 1024, STATUS_CAPACITY = 4096;
  static LumaFirmware *instance;

  struct Config {
    bool power = true;
    char mode[12] = "manual";
    uint8_t effect = 0;
    char color[8] = "#c4b5fd";
    uint8_t brightness = 128, speed = 35, saturation = 85;
    bool reverse = false;
    char musicStyle[8] = "bar";
    uint8_t micGain = 8;
    uint16_t motionHoldSec = 45, darkThreshold = 2000;
    bool autoDim = true;
    uint8_t idleMinutes = 0, demoIntervalSec = 15;
    uint16_t sleepMinutes = 20;
    uint8_t wakeMinutes = 10;
    bool scheduleEnabled = false;
    uint16_t scheduleStart = 1080, scheduleEnd = 1380;
    uint8_t scheduleDays = 127;
    int16_t utcOffsetMinutes = 420;
    uint8_t focusMinutes = 25, breakMinutes = 5;
  } config;

  struct Command {
    uint8_t kind; // 0 config, 1 action, 2 time
    char body[BODY_LIMIT + 1];
  };

  CRGB leds[PIXELS], work[PIXELS];
  uint8_t heat[PIXELS] = {};
  Preferences prefs;
  QueueHandle_t commands = nullptr;
  SemaphoreHandle_t snapshotMutex = nullptr;
  httpd_handle_t http = nullptr;
  char snapshot[STATUS_CAPACITY] = "{}", buildBuffer[STATUS_CAPACITY];
  Config publishedConfig, acceptedConfig; // acceptedConfig includes queued patches, protected by snapshotMutex
  uint32_t revision = 0;
  bool dirty = false, storageOk = false;
  uint32_t changedAt = 0, lastSaveAt = 0, lastSnapshotAt = 0;
  bool forceSave = false;

  // Time is monotonic from esp_timer, independent of the millis() rollover.
  bool clockValid = false;
  uint64_t baseEpoch = 0, baseEpochMicros = 0;
  uint32_t lastFrameAt = 0, lastSensorAt = 0, lastMicAt = 0, lastClockCheckAt = 0;
  uint64_t lastMotionUs = 0, lastDhtGoodUs = 0;
  uint32_t lastWifiTry = 0, lastMdnsTry = 0, wifiRetryMs = 5000;
  bool hasMotion = false, motion = false, wifiWasConnected = false;
  bool mdnsStarted = false;
  float ldr = 0, micDc = 2048, micSquareSum = 0, micLevel = 0;
  uint16_t micSamples = 0;
  float temperature = NAN, humidity = NAN;
  uint32_t lastDhtAt = 0, dhtLowAt = 0, dhtCaptureAt = 0;
  uint8_t dhtState = 0; // 0 idle, 1 start LOW, 2 interrupt capture
  portMUX_TYPE dhtMux = portMUX_INITIALIZER_UNLOCKED;
  volatile uint8_t dhtStage = 0, dhtBit = 0, dhtBytes[5] = {};
  volatile uint32_t dhtRiseAt = 0;
  volatile bool dhtDone = false, dhtBadPulse = false;

  uint8_t pomoState = 0; // 0 idle, 1 focus, 2 break
  bool pomoRunning = false;
  uint32_t pomoRemainingMs = 0, pomoPhaseDurationMs = 0, pomoTickAt = 0, pomoSessions = 0;
  bool sleepRunning = false, sleepExpired = false;
  uint32_t sleepStartedAt = 0, sleepDurationMs = 0;
  uint32_t demoStartedAt = 0, lastFireAt = 0, lastConfettiAt = 0;
  char activeMode[12] = "manual";
  uint8_t activeEffect = 0, outputBrightness = 0;
  const char *gateReason = "active";
  float autoFade = 0;
  bool darkLatch = false;
  char apName[25] = {}, apPassword[21] = {}, bootId[17] = {};

  static float clamp01(float v) { return fmaxf(0, fminf(1, v)); }
  static float wave(float v) { return .5f + .5f * sinf(v); }
  static uint32_t elapsed(uint32_t now, uint32_t then) { return now - then; }
  static bool oneOf(const char *s, const char *const *values, size_t count) {
    for (size_t i = 0; i < count; ++i) if (!strcmp(s, values[i])) return true;
    return false;
  }
  static uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15;
    x *= 0x846ca68bU; return x ^ (x >> 16);
  }
  static CRGB hsv(float hue, float sat, float level = 1) {
    hue = fmodf(hue, 256); if (hue < 0) hue += 256;
    CRGB c; hsv2rgb_rainbow(CHSV(uint8_t(hue), uint8_t(255 * clamp01(sat)),
                              uint8_t(255 * clamp01(level))), c); return c;
  }
  static CRGB mix(CRGB a, CRGB b, float fraction) {
    return blend(a, b, uint8_t(255 * clamp01(fraction)));
  }

  void configToJson(JsonObject o, const Config &c) {
#define PUT(KEY) o[#KEY] = c.KEY
    PUT(power); PUT(mode); PUT(effect); PUT(color); PUT(brightness); PUT(speed);
    PUT(saturation); PUT(reverse); PUT(musicStyle); PUT(micGain);
    PUT(motionHoldSec); PUT(darkThreshold); PUT(autoDim); PUT(idleMinutes);
    PUT(demoIntervalSec); PUT(sleepMinutes); PUT(wakeMinutes);
    PUT(scheduleEnabled); PUT(scheduleStart); PUT(scheduleEnd); PUT(scheduleDays);
    PUT(utcOffsetMinutes); PUT(focusMinutes); PUT(breakMinutes);
#undef PUT
  }

  // Strict type/range validation. Work on a copy so no partial update can leak.
  bool patchConfig(JsonObjectConst patch, Config &candidate, String &error) {
    static const char *const modes[] = {"manual", "effect", "temperature", "music", "auto", "demo", "circadian"};
    static const char *const musicStyles[] = {"bar", "center", "pulse"};
    for (JsonPairConst pair : patch) {
      const char *key = pair.key().c_str();
      JsonVariantConst value = pair.value();
#define BOOL_FIELD(KEY) if (!strcmp(key, #KEY)) { if (!value.is<bool>()) {error = "Expected boolean: " #KEY; return false;} candidate.KEY = value.as<bool>(); continue; }
#define INT_FIELD(KEY, LOW, HIGH) if (!strcmp(key, #KEY)) { if (!value.is<int>() || value.is<bool>()) {error = "Expected integer: " #KEY; return false;} int n = value.as<int>(); if (n < LOW || n > HIGH) {error = "Out of range: " #KEY; return false;} candidate.KEY = n; continue; }
      BOOL_FIELD(power) BOOL_FIELD(reverse) BOOL_FIELD(autoDim) BOOL_FIELD(scheduleEnabled)
      INT_FIELD(effect, 0, 11) INT_FIELD(brightness, 0, 255)
      INT_FIELD(speed, 1, 100) INT_FIELD(saturation, 0, 100)
      INT_FIELD(micGain, 1, 30) INT_FIELD(motionHoldSec, 5, 600)
      INT_FIELD(darkThreshold, 0, 4095) INT_FIELD(idleMinutes, 0, 120)
      INT_FIELD(demoIntervalSec, 5, 120) INT_FIELD(sleepMinutes, 1, 180)
      INT_FIELD(wakeMinutes, 0, 60) INT_FIELD(scheduleStart, 0, 1439)
      INT_FIELD(scheduleEnd, 0, 1439) INT_FIELD(scheduleDays, 1, 127)
      INT_FIELD(utcOffsetMinutes, -720, 840) INT_FIELD(focusMinutes, 1, 120)
      INT_FIELD(breakMinutes, 1, 60)
#undef BOOL_FIELD
#undef INT_FIELD
      if (!strcmp(key, "mode") || !strcmp(key, "musicStyle") || !strcmp(key, "color")) {
        if (!value.is<const char *>()) { error = "Expected string"; return false; }
        const char *v = value.as<const char *>();
        if (!strcmp(key, "mode")) {
          if (!oneOf(v, modes, 7)) { error = "Unknown mode"; return false; }
          strlcpy(candidate.mode, v, sizeof(candidate.mode));
        } else if (!strcmp(key, "musicStyle")) {
          if (!oneOf(v, musicStyles, 3)) { error = "Unknown musicStyle"; return false; }
          strlcpy(candidate.musicStyle, v, sizeof(candidate.musicStyle));
        } else {
          if (strlen(v) != 7 || v[0] != '#') { error = "Use #RRGGBB color"; return false; }
          for (uint8_t i = 1; i < 7; ++i) if (!isxdigit((unsigned char)v[i])) { error = "Invalid color"; return false; }
          strlcpy(candidate.color, v, sizeof(candidate.color));
          for (uint8_t i = 1; i < 7; ++i) candidate.color[i] = tolower(candidate.color[i]);
        }
        continue;
      }
      error = String("Unknown configuration key: ") + key;
      return false;
    }
    if (candidate.scheduleStart == candidate.scheduleEnd) {
      error = "Schedule start and end must differ"; return false;
    }
    return true;
  }

  void loadConfig() {
    storageOk = prefs.begin("luma15", false);
    if (!storageOk) return;
    if (prefs.getUChar("schema", 0) != 1) return;
    String text = prefs.getString("config", "");
    if (!text.length() || text.length() > BODY_LIMIT) return;
    JsonDocument doc;
    if (deserializeJson(doc, text) || !doc.is<JsonObject>()) return;
    Config candidate;
    String error;
    if (patchConfig(doc.as<JsonObjectConst>(), candidate, error)) config = candidate;
    else Serial.printf("[NVS] Invalid saved configuration; safe defaults: %s\n", error.c_str());
  }

  void persist(uint32_t now) {
    if (!dirty && !forceSave) return;
    // Explicit save bypasses the quiet period, never the flash-wear ceiling.
    if (elapsed(now, lastSaveAt) < 20000) return;
    if (!forceSave && elapsed(now, changedAt) < 2000) return;
    if (!dirty) { forceSave = false; return; }
    forceSave = false;
    lastSaveAt = now;
    if (!storageOk) { prefs.end(); storageOk = prefs.begin("luma15", false); }
    if (!storageOk) return;
    JsonDocument doc; configToJson(doc.to<JsonObject>(), config);
    String text; serializeJson(doc, text);
    bool ok = prefs.putString("config", text) == text.length();
    if (ok) ok = prefs.putUChar("schema", 1) == 1;
    storageOk = ok;
    if (ok) dirty = false;
  }

  static void IRAM_ATTR dhtInterrupt();

  void updateDht(uint32_t now) {
    if (dhtState == 0 && elapsed(now, lastDhtAt) >= 2500) {
      lastDhtAt = now; dhtLowAt = now;
      pinMode(DHT_PIN, OUTPUT); digitalWrite(DHT_PIN, LOW); dhtState = 1;
    } else if (dhtState == 1 && elapsed(now, dhtLowAt) >= 20) {
      portENTER_CRITICAL(&dhtMux);
      dhtStage = 0; dhtBit = 0; dhtDone = false; dhtBadPulse = false;
      for (uint8_t i = 0; i < 5; ++i) dhtBytes[i] = 0;
      portEXIT_CRITICAL(&dhtMux);
      // Attach while host is still LOW, then release as open-drain pull-up.
      attachInterrupt(digitalPinToInterrupt(DHT_PIN), dhtInterrupt, CHANGE);
      pinMode(DHT_PIN, INPUT_PULLUP);
      dhtCaptureAt = micros(); dhtState = 2;
    } else if (dhtState == 2 && (dhtDone || elapsed(micros(), dhtCaptureAt) > 8000)) {
      detachInterrupt(digitalPinToInterrupt(DHT_PIN));
      uint8_t b[5]; bool good;
      portENTER_CRITICAL(&dhtMux);
      for (uint8_t i = 0; i < 5; ++i) b[i] = dhtBytes[i];
      good = dhtDone && !dhtBadPulse;
      portEXIT_CRITICAL(&dhtMux);
      good = good && uint8_t(b[0] + b[1] + b[2] + b[3]) == b[4];
      float h = b[0] + b[1] * .1f;
      float t = (b[2] & 0x7f) + b[3] * .1f;
      if (b[2] & 0x80) t = -t;
      if (good && h >= 0 && h <= 100 && t >= -20 && t <= 80) {
        temperature = t; humidity = h; lastDhtGoodUs = esp_timer_get_time();
      }
      dhtState = 0;
    }
  }

  bool dhtOk(uint32_t /*now*/) const { return isfinite(temperature) && uint64_t(esp_timer_get_time()) - lastDhtGoodUs < 10000000ULL; }

  void updateSensors(uint32_t now) {
    // One ADC sample per invocation; no batch-read waits or boot calibration.
    uint32_t us = micros();
    if (elapsed(us, lastMicAt) >= 400) {
      lastMicAt = us;
      float sample = analogRead(MIC_PIN);
      micDc += .002f * (sample - micDc);
      float ac = sample - micDc;
      micSquareSum += ac * ac;
      if (++micSamples >= 64) {
        float rms = sqrtf(micSquareSum / micSamples);
        float level = clamp01(fmaxf(0, rms - 10) * config.micGain / 2048.0f) * 100;
        micLevel += .3f * (level - micLevel);
        micSamples = 0; micSquareSum = 0;
      }
    }
    if (elapsed(now, lastSensorAt) < 25) return;
    lastSensorAt = now;
    ldr += .15f * (analogRead(LDR_PIN) - ldr);
    motion = digitalRead(PIR_PIN) == HIGH;
    if (motion) { lastMotionUs = esp_timer_get_time(); hasMotion = true; }
    // Hysteresis is clipped at ADC endpoints so thresholds 0/4095 still work.
    int threshold = config.darkThreshold;
    if (!darkLatch && ldr > threshold) darkLatch = true;
    else if (darkLatch && ldr <= max(0, threshold - 120)) darkLatch = false;
  }

  uint64_t epoch() const {
    return clockValid ? baseEpoch + (uint64_t(esp_timer_get_time()) - baseEpochMicros) / 1000000ULL : 0;
  }

  void updateClock(uint32_t now) {
    if (elapsed(now, lastClockCheckAt) < 1000) return;
    lastClockCheckAt = now;
    struct timeval wall{};
    gettimeofday(&wall, nullptr);
    if (wall.tv_sec < 1704067200LL || wall.tv_sec > 4102444799LL) return;
    int64_t difference = int64_t(wall.tv_sec) - int64_t(epoch());
    if (!clockValid || difference > 1 || difference < -1) {
      baseEpoch = wall.tv_sec;
      // Preserve native fractional seconds when anchoring the monotonic clock.
      baseEpochMicros = uint64_t(esp_timer_get_time()) - wall.tv_usec;
      clockValid = true;
      ++revision;
    }
  }

  // The exact scheduler helper is also compiled into native boundary tests.
  bool scheduleWindow(float &wakeFactor) const {
    const luma::ScheduleSettings settings{config.scheduleEnabled,
      config.scheduleStart, config.scheduleEnd, config.scheduleDays,
      config.utcOffsetMinutes, config.wakeMinutes};
    const luma::ScheduleResult result = luma::evaluateSchedule(epoch(), clockValid, settings);
    wakeFactor = result.wakeFactor;
    return result.open;
  }

  uint32_t motionRemaining(uint32_t /*now*/) const {
    if (!hasMotion) return 0;
    uint64_t age = (uint64_t(esp_timer_get_time()) - lastMotionUs) / 1000ULL;
    uint32_t hold = config.motionHoldSec * 1000UL;
    return age < hold ? (hold - age + 999) / 1000 : 0;
  }

  void updateTimers(uint32_t now) {
    uint32_t step = elapsed(now, pomoTickAt); pomoTickAt = now;
    if (pomoRunning && pomoState) {
      if (step >= pomoRemainingMs) {
        uint32_t overshoot = step - pomoRemainingMs;
        if (pomoState == 1) { ++pomoSessions; pomoState = 2; pomoRemainingMs = config.breakMinutes * 60000UL; }
        else { pomoState = 1; pomoRemainingMs = config.focusMinutes * 60000UL; }
        pomoPhaseDurationMs = pomoRemainingMs;
        // The loop never waits for a timer. Discard only multi-phase stalls.
        pomoRemainingMs -= min(overshoot, pomoRemainingMs - 1);
      } else pomoRemainingMs -= step;
    }
    if (sleepRunning && elapsed(now, sleepStartedAt) >= sleepDurationMs) {
      sleepRunning = false; sleepExpired = true;
    }
  }

  void applyAction(const char *action, uint32_t now) {
    if (!strcmp(action, "pomodoroStart")) {
      pomoState = 1; pomoRunning = true; pomoRemainingMs = config.focusMinutes * 60000UL; pomoPhaseDurationMs = pomoRemainingMs; pomoTickAt = now;
    } else if (!strcmp(action, "pomodoroPause")) pomoRunning = false;
    else if (!strcmp(action, "pomodoroResume")) { if (pomoState) { pomoRunning = true; pomoTickAt = now; } }
    else if (!strcmp(action, "pomodoroReset")) { pomoState = 0; pomoRunning = false; pomoRemainingMs = 0; pomoSessions = 0; }
    else if (!strcmp(action, "sleepStart")) { sleepRunning = true; sleepExpired = false; sleepStartedAt = now; sleepDurationMs = config.sleepMinutes * 60000UL; }
    else if (!strcmp(action, "sleepCancel")) { sleepRunning = false; sleepExpired = false; }
    else if (!strcmp(action, "save")) forceSave = true;
    ++revision;
  }

  void drainCommands(uint32_t now) {
    Command cmd;
    // Limit per pass to prevent clients starving animation and sensor deadlines.
    for (uint8_t count = 0; count < 3 && xQueueReceive(commands, &cmd, 0) == pdTRUE; ++count) {
      JsonDocument doc;
      if (deserializeJson(doc, cmd.body) || !doc.is<JsonObject>()) continue;
      if (cmd.kind == 0) {
        Config candidate = config; String error;
        if (patchConfig(doc.as<JsonObjectConst>(), candidate, error)) {
          bool resetDemo = strcmp(candidate.mode, config.mode) || candidate.demoIntervalSec != config.demoIntervalSec;
          config = candidate;
          if (resetDemo) demoStartedAt = now;
          dirty = true; changedAt = now; ++revision;
        }
      } else if (cmd.kind == 1) applyAction(doc["action"].as<const char *>(), now);
      else {
        baseEpoch = doc["epoch"].as<uint64_t>();
        struct timeval wall{}; wall.tv_sec = baseEpoch; wall.tv_usec = 0;
        settimeofday(&wall, nullptr); // keep SNTP/native clock in sync with explicit browser input
        baseEpochMicros = esp_timer_get_time();
        clockValid = true; ++revision;
      }
      publishSnapshot(now);
    }
  }

  void updateWifi(uint32_t now) {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wifiWasConnected) {
      wifiRetryMs = 5000;
      // ESP-IDF SNTP resolves/synchronizes on its own task; never wait for time.
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      Serial.printf("[STA] Connected: %s\n", WiFi.localIP().toString().c_str());
    }
    wifiWasConnected = connected;
    if (!connected && elapsed(now, lastWifiTry) >= wifiRetryMs) {
      lastWifiTry = now;
      if (strlen(LUMA_WIFI_SSID)) WiFi.begin(LUMA_WIFI_SSID, LUMA_WIFI_PASS);
      else if (WiFi.SSID().length()) WiFi.reconnect();
      wifiRetryMs = min(uint32_t(60000), wifiRetryMs * 2);
    }
    if (!mdnsStarted && elapsed(now, lastMdnsTry) >= 5000) {
      lastMdnsTry = now;
      mdnsStarted = MDNS.begin("luma-15");
      if (mdnsStarted) MDNS.addService("http", "tcp", 80);
    }
  }

  void renderEffect(uint8_t effect, float t, uint32_t now) {
    float sat = config.saturation / 100.0f;
    for (uint8_t i = 0; i < PIXELS; ++i) {
      float x = i / float(PIXELS - 1);
      switch (effect) {
      case 0: work[i] = hsv(t * 18 + x * 220, sat); break;
      case 1: work[i] = hsv(t * 7 + 180, sat, .035f + .965f * powf(wave(t * 1.8f - PI / 2), 2)); break;
      case 2: {
        float w = wave(x * 8 + t) * wave(x * 3 - t * .7f);
        work[i] = hsv(100 + w * 75 + 15 * sinf(t * .3f), sat, .12f + .88f * w); break;
      }
      case 3: {
        float head = fmodf(t * 5, PIXELS + 8.0f) - 3;
        float distance = head - i;
        float v = distance >= 0 && distance < 7 ? expf(-distance * .7f) : 0;
        work[i] = hsv(t * 8 + 150, sat, v); break;
      }
      case 4: work[i] = hsv(heat[i] / 7.0f, sat, heat[i] / 255.0f); break;
      case 5: work[i] = hsv(135 + 25 * sinf(x * 6 + t), sat, .1f + .9f * powf(wave(x * 8 - t * 1.4f), 1.5f)); break;
      case 6: {
        uint32_t h = hash(i * 199 + 17);
        float v = powf(wave(t * (.6f + (h % 100) / 70.0f) + h % 700), 12);
        work[i] = hsv((h >> 8) % 256 + t, sat, v); break;
      }
      case 7: work[i] = hsv(x * 190 + 20 * sinf(t), sat, .18f + .82f * wave(x * 15 - t * 2)); break;
      case 8: {
        float position = wave(t * 1.5f - PI / 2) * (PIXELS - 1);
        float distance = (i - position) / 1.25f;
        work[i] = hsv(170 + t * 5, sat, expf(-distance * distance)); break;
      }
      case 9: break; // persistent particle buffer updated below
      case 10: {
        float phase = fmodf(t, 2.2f);
        float a = (phase - .28f) / .11f, b = (phase - .62f) / .15f;
        float v = .025f + .8f * expf(-a*a) + .55f * expf(-b*b);
        work[i] = hsv(245 + x * 10, sat, v); break;
      }
      default: {
        float noise = (hash(uint32_t(t * 9) + i * 17) % 1000) / 1000.0f;
        float v = .55f + .15f * sinf(t * 4 + i) + .2f * noise;
        work[i] = hsv(17 + 5 * sinf(t + x), sat, v); break;
      }
      }
    }
    if (effect == 4 && elapsed(now, lastFireAt) >= uint32_t(110 - config.speed)) {
      lastFireAt = now;
      for (uint8_t i = 0; i < PIXELS; ++i) heat[i] = qsub8(heat[i], random8(0, 32));
      for (int i = PIXELS - 1; i >= 2; --i) heat[i] = (heat[i-1] + heat[i-2] + heat[i-2]) / 3;
      if (random8() < 150) { uint8_t spark = random8(3); heat[spark] = qadd8(heat[spark], random8(100, 220)); }
    }
    if (effect == 9) {
      fadeToBlackBy(work, PIXELS, 24);
      uint32_t period = 170 - config.speed;
      if (elapsed(now, lastConfettiAt) >= period) {
        lastConfettiAt = now; work[random8(PIXELS)] += hsv(random8(), sat);
      }
    }
  }

  void renderFrame(uint32_t now) {
    uint32_t frameMs = elapsed(now, lastFrameAt);
    if (frameMs < 16 || dhtState == 2) return; // avoid LED RMT activity during DHT capture
    lastFrameAt = now;
    float dt = min(frameMs, uint32_t(100)) / 1000.0f;
    strlcpy(activeMode, config.mode, sizeof(activeMode)); activeEffect = config.effect;
    if (!strcmp(config.mode, "demo")) {
      uint8_t step = (elapsed(now, demoStartedAt) / (config.demoIntervalSec * 1000UL)) % 15;
      if (step < 12) { strlcpy(activeMode, "effect", sizeof(activeMode)); activeEffect = step; }
      else strlcpy(activeMode, step == 12 ? "temperature" : step == 13 ? "music" : "auto", sizeof(activeMode));
    }
    float t = double(esp_timer_get_time() % 86400000000LL) / 1000000.0 * (.15f + config.speed * .045f);
    float sat = config.saturation / 100.0f;
    if (!strcmp(activeMode, "manual")) {
      uint32_t rgb = strtoul(config.color + 1, nullptr, 16);
      fill_solid(work, PIXELS, CRGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255));
    } else if (!strcmp(activeMode, "effect")) renderEffect(activeEffect, t, now);
    else if (!strcmp(activeMode, "temperature")) {
      fill_solid(work, PIXELS, CRGB::Black);
      if (dhtOk(now)) {
        float warmth = clamp01((temperature - 15) / 25);
        int lit = 1 + lroundf(warmth * (PIXELS - 1));
        for (int i = 0; i < lit; ++i) work[i] = hsv(160 * (1 - warmth), sat, .85f + .15f * sinf(t + i));
      }
    } else if (!strcmp(activeMode, "music")) {
      float level = clamp01(micLevel / 100);
      for (uint8_t i = 0; i < PIXELS; ++i) {
        float p = i / float(PIXELS - 1);
        float v = 0;
        if (!strcmp(config.musicStyle, "pulse")) v = powf(level, .7f);
        else if (!strcmp(config.musicStyle, "center")) v = clamp01((level - fabsf(2*p - 1)) * PIXELS);
        else v = clamp01((level - p) * PIXELS);
        work[i] = hsv(100 * (1 - p), sat, v);
      }
    } else if (!strcmp(activeMode, "auto")) {
      bool occupied = motion || motionRemaining(now) > 0;
      float target = darkLatch && occupied ? 1 : 0;
      autoFade += (target - autoFade) * fminf(1, dt * (target > autoFade ? 2 : 1));
      if (autoFade < .001f) autoFade = 0;
      fill_solid(work, PIXELS, hsv(22, .6f * sat, autoFade));
    } else { // circadian: warm sunrise/daylight/dim amber evening
      float hour = clockValid ? ((epoch() + int64_t(config.utcOffsetMinutes) * 60) % 86400) / 3600.0f : 21;
      float daylight = clamp01(sinf((hour - 6) / 12 * PI));
      CRGB c = mix(CRGB(255, 90, 18), CRGB(220, 238, 255), daylight);
      c.nscale8(uint8_t(80 + daylight * 175)); fill_solid(work, PIXELS, c);
    }
    // Pomodoro is a spatial progress overlay; all safety/energy gates still win.
    if (pomoState) {
      uint32_t duration = pomoPhaseDurationMs;
      float progress = clamp01(1 - pomoRemainingMs / float(duration));
      for (uint8_t i = 0; i < PIXELS; ++i) {
        float v = .08f + .92f * clamp01(progress * PIXELS - i);
        work[i] = hsv(pomoState == 1 ? 28 : 98, .85f, v);
      }
    }
    float wakeFactor = 1, gate = 1;
    bool scheduled = scheduleWindow(wakeFactor);
    gateReason = "active";
    if (!config.power) { gate = 0; gateReason = "power"; }
    else if (config.scheduleEnabled && !clockValid) { gate = 0; gateReason = "clock"; }
    else if (!scheduled) { gate = 0; gateReason = "schedule"; }
    else if (sleepExpired) { gate = 0; gateReason = "sleep"; }
    else if (config.idleMinutes && strcmp(activeMode, "auto") && !motion && (uint64_t(esp_timer_get_time()) - lastMotionUs) / 1000ULL >= config.idleMinutes * 60000UL) { gate = 0; gateReason = "idle"; }
    gate *= wakeFactor;
    if (sleepRunning) gate *= clamp01(1 - elapsed(now, sleepStartedAt) / float(sleepDurationMs));
    if (config.autoDim) gate *= .18f + .82f * (1 - ldr / 4095.0f);
    uint8_t brightness = uint8_t(config.brightness * clamp01(gate));
    for (uint8_t i = 0; i < PIXELS; ++i) {
      leds[i] = work[config.reverse ? PIXELS - 1 - i : i];
      leds[i].nscale8(brightness);
    }
    // Global overload includes the same MCU power overhead as FastLED.show().
    // Pre-scale samples, so the final controller limiter has no hidden reduction.
    uint8_t powerScale = calculate_max_brightness_for_power_mW(255, 5UL * MAX_MILLIAMPS);
    if (powerScale < 255) nscale8(leds, PIXELS, powerScale);
    outputBrightness = uint16_t(brightness) * powerScale / 255;
    FastLED.show();
  }

  void publishSnapshot(uint32_t now) {
    JsonDocument doc;
    doc["apiVersion"] = 1; doc["revision"] = revision;
    configToJson(doc["config"].to<JsonObject>(), config);
    JsonObject t = doc["telemetry"].to<JsonObject>();
    t["bootId"] = bootId;
    t["uptimeSec"] = uint64_t(esp_timer_get_time()) / 1000000ULL;
    t["freeHeap"] = ESP.getFreeHeap(); t["minHeap"] = ESP.getMinFreeHeap(); t["heapSize"] = ESP.getHeapSize();
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) t["rssi"] = WiFi.RSSI(); else t["rssi"] = nullptr;
    t["wifiConnected"] = connected;
    t["ip"] = connected ? WiFi.localIP().toString() : String("");
    t["apIp"] = WiFi.softAPIP().toString(); t["apClients"] = WiFi.softAPgetStationNum();
    t["clockValid"] = clockValid;
    if (clockValid) t["epoch"] = epoch(); else t["epoch"] = nullptr;
    bool good = dhtOk(now);
    t["dhtOk"] = good;
    if (good) { t["temperature"] = temperature; t["humidity"] = humidity; }
    else { t["temperature"] = nullptr; t["humidity"] = nullptr; }
    t["ldr"] = uint16_t(constrain(ldr, 0.0f, 4095.0f)); t["micLevel"] = roundf(micLevel * 10) / 10;
    t["motion"] = motion; t["motionRemainingSec"] = motionRemaining(now);
    t["activeMode"] = activeMode; t["activeEffect"] = activeEffect;
    t["outputBrightness"] = outputBrightness; t["gateReason"] = gateReason;
    JsonObject p = t["pomodoro"].to<JsonObject>();
    p["state"] = pomoState == 1 ? "focus" : pomoState == 2 ? "break" : "idle";
    p["durationSec"] = pomoState ? pomoPhaseDurationMs / 1000 : 0;
    p["running"] = pomoRunning; p["remainingSec"] = (pomoRemainingMs + 999) / 1000; p["sessions"] = pomoSessions;
    t["sleepRemainingSec"] = sleepRunning ? (sleepDurationMs - min(sleepDurationMs, elapsed(now, sleepStartedAt)) + 999) / 1000 : 0;
    t["dirty"] = dirty; t["storageOk"] = storageOk;
    JsonArray pixels = t["leds"].to<JsonArray>();
    for (uint8_t i = 0; i < PIXELS; ++i) {
      JsonArray pixel = pixels.add<JsonArray>(); pixel.add(leds[i].r); pixel.add(leds[i].g); pixel.add(leds[i].b);
    }
    if (doc.overflowed() || measureJson(doc) >= STATUS_CAPACITY) return;
    serializeJson(doc, buildBuffer, STATUS_CAPACITY);
    // A try-lock never stalls deadlines behind a client or server task.
    if (xSemaphoreTake(snapshotMutex, 0) == pdTRUE) {
      strlcpy(snapshot, buildBuffer, sizeof(snapshot));
      publishedConfig = config;
      xSemaphoreGive(snapshotMutex);
    }
    lastSnapshotAt = now;
  }

  static esp_err_t jsonResponse(httpd_req_t *req, const char *status, const char *body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
  }
  static esp_err_t errorResponse(httpd_req_t *req, const char *status, const String &error) {
    JsonDocument doc; doc["ok"] = false; doc["error"] = error;
    String body; serializeJson(doc, body); return jsonResponse(req, status, body.c_str());
  }

  // Reject foreign origins and DNS-rebinding hosts. Requests without an Origin
  // header remain usable by local command-line clients; no CORS headers added.
  static bool localRequest(httpd_req_t *req) {
    char host[96] = {}, origin[128] = {};
    size_t hlen = httpd_req_get_hdr_value_len(req, "Host");
    if (!hlen || hlen >= sizeof(host) || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    String hostname(host);
    if (hostname.endsWith(":80")) hostname.remove(hostname.length() - 3);
    if (hostname != "luma-15.local" && hostname != "luma-15" && hostname != WiFi.softAPIP().toString() && hostname != WiFi.localIP().toString()) return false;
    size_t olen = httpd_req_get_hdr_value_len(req, "Origin");
    if (!olen) return true;
    if (olen >= sizeof(origin) || httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    return String(origin) == String("http://") + host;
  }

  static esp_err_t rootHandler(httpd_req_t *req) {
    if (!localRequest(req)) return errorResponse(req, "403 Forbidden", "Local same-origin access only");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
    return httpd_resp_send(req, reinterpret_cast<const char *>(DASHBOARD_GZ), DASHBOARD_GZ_LEN);
  }
  static esp_err_t statusHandler(httpd_req_t *req) {
    if (!localRequest(req)) return errorResponse(req, "403 Forbidden", "Local same-origin access only");
    LumaFirmware *a = instance;
    char *copy = static_cast<char *>(malloc(STATUS_CAPACITY));
    if (!copy) return errorResponse(req, "503 Service Unavailable", "Low memory");
    if (xSemaphoreTake(a->snapshotMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
      free(copy); return errorResponse(req, "503 Service Unavailable", "Status busy");
    }
    strlcpy(copy, a->snapshot, STATUS_CAPACITY);
    xSemaphoreGive(a->snapshotMutex);
    esp_err_t result = jsonResponse(req, "200 OK", copy); free(copy); return result;
  }
  static esp_err_t commandHandler(httpd_req_t *req) {
    if (!localRequest(req)) return errorResponse(req, "403 Forbidden", "Local same-origin access only");
    if (req->content_len == 0) return errorResponse(req, "400 Bad Request", "JSON body required");
    if (req->content_len > BODY_LIMIT) return errorResponse(req, "413 Payload Too Large", "Body exceeds 1024 bytes");
    char contentType[80] = {};
    size_t length = httpd_req_get_hdr_value_len(req, "Content-Type");
    if (!length || length >= sizeof(contentType) || httpd_req_get_hdr_value_str(req, "Content-Type", contentType, sizeof(contentType)) != ESP_OK) return errorResponse(req, "400 Bad Request", "Content-Type application/json required");
    String mime(contentType); mime.toLowerCase();
    if (mime != "application/json" && !mime.startsWith("application/json;")) return errorResponse(req, "400 Bad Request", "Content-Type application/json required");
    Command cmd{};
    size_t received = 0;
    // Bounded socket reads occur only on the server task (1 s recv timeout).
    while (received < req->content_len) {
      int count = httpd_req_recv(req, cmd.body + received, req->content_len - received);
      if (count <= 0) return errorResponse(req, "408 Request Timeout", "Incomplete JSON body");
      received += count;
    }
    cmd.body[received] = 0;
    JsonDocument doc;
    DeserializationError parse = deserializeJson(doc, cmd.body, received);
    if (parse || !doc.is<JsonObject>()) return errorResponse(req, "400 Bad Request", "Expected JSON object");
    LumaFirmware *a = instance;
    if (!strcmp(req->uri, "/api/config")) {
      if (!doc.size()) return errorResponse(req, "400 Bad Request", "Empty configuration patch");
      Config candidate;
      if (xSemaphoreTake(a->snapshotMutex, pdMS_TO_TICKS(10)) != pdTRUE) return errorResponse(req, "503 Service Unavailable", "Status busy");
      candidate = a->acceptedConfig;
      String error;
      if (!a->patchConfig(doc.as<JsonObjectConst>(), candidate, error)) {
        xSemaphoreGive(a->snapshotMutex);
        return errorResponse(req, "400 Bad Request", error);
      }
      cmd.kind = 0;
      // Validate against all previously accepted patches, including pending
      // ones. This makes cross-field validation atomic across rapid requests.
      bool queued = xQueueSend(a->commands, &cmd, 0) == pdTRUE;
      if (queued) a->acceptedConfig = candidate;
      xSemaphoreGive(a->snapshotMutex);
      if (!queued) return errorResponse(req, "503 Service Unavailable", "Command queue full; retry later");
      return jsonResponse(req, "202 Accepted", "{\"ok\":true,\"queued\":true}");
    } else if (!strcmp(req->uri, "/api/action")) {
      static const char *const actions[] = {"pomodoroStart", "pomodoroPause", "pomodoroResume", "pomodoroReset", "sleepStart", "sleepCancel", "save"};
      if (doc.size() != 1 || !doc["action"].is<const char *>() || !oneOf(doc["action"].as<const char *>(), actions, 7)) return errorResponse(req, "400 Bad Request", "Unknown action or extra fields");
      cmd.kind = 1;
    } else {
      if (doc.size() != 1 || !doc["epoch"].is<uint64_t>() || doc["epoch"].is<bool>()) return errorResponse(req, "400 Bad Request", "Integer UTC epoch required");
      uint64_t seconds = doc["epoch"].as<uint64_t>();
      if (seconds < 1704067200ULL || seconds > 4102444799ULL) return errorResponse(req, "400 Bad Request", "Epoch must be in 2024..2099");
      cmd.kind = 2;
    }
    if (xQueueSend(a->commands, &cmd, 0) != pdTRUE) return errorResponse(req, "503 Service Unavailable", "Command queue full; retry later");
    return jsonResponse(req, "202 Accepted", "{\"ok\":true,\"queued\":true}");
  }

  void begin() {
    instance = this;
    Serial.begin(115200);
    commands = xQueueCreate(6, sizeof(Command));
    snapshotMutex = xSemaphoreCreateMutex();
    if (!commands || !snapshotMutex) { Serial.println("[FATAL] Cannot allocate command transport"); return; }
    pinMode(PIR_PIN, INPUT); pinMode(LDR_PIN, INPUT); pinMode(MIC_PIN, INPUT);
    pinMode(DHT_PIN, INPUT_PULLUP);
    analogReadResolution(12);
    analogSetPinAttenuation(LDR_PIN, ADC_11db); analogSetPinAttenuation(MIC_PIN, ADC_11db);
    ldr = analogRead(LDR_PIN); darkLatch = ldr > config.darkThreshold;
    micDc = analogRead(MIC_PIN);
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, PIXELS);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
    FastLED.setBrightness(255); fill_solid(leds, PIXELS, CRGB::Black); FastLED.show();
    loadConfig();
    acceptedConfig = config;
    darkLatch = ldr > config.darkThreshold;
    lastSaveAt = millis() - 20000UL; // first intentional write may commit immediately
    WiFi.persistent(false);
    WiFi.mode(WIFI_AP_STA); WiFi.setHostname("luma-15"); WiFi.setAutoReconnect(false);
    snprintf(bootId, sizeof(bootId), "%08lX%08lX", (unsigned long)esp_random(), (unsigned long)esp_random());
    random16_set_seed(uint16_t(esp_random()));
    uint64_t mac = ESP.getEfuseMac();
    snprintf(apName, sizeof(apName), "LUMA-15-%06lX", (unsigned long)((mac >> 24) & 0xffffff));
    // Random password retained in NVS; no universal password and no weak PRNG.
    String savedPassword = storageOk ? prefs.getString("apPassword", "") : String("");
    if (savedPassword.length() >= 12 && savedPassword.length() <= 20) strlcpy(apPassword, savedPassword.c_str(), sizeof(apPassword));
    else {
      static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
      for (uint8_t i = 0; i < 16; ++i) apPassword[i] = alphabet[esp_random() % (sizeof(alphabet)-1)];
      apPassword[16] = 0;
      if (storageOk && prefs.putString("apPassword", apPassword) != strlen(apPassword)) storageOk = false;
    }
    WiFi.softAP(apName, apPassword, 1, false, 3);
    if (strlen(LUMA_WIFI_SSID)) WiFi.begin(LUMA_WIFI_SSID, LUMA_WIFI_PASS);
    else WiFi.begin(); // core-stored station credentials, if any
    Serial.printf("\n[LUMA / 15] AP: %s | WPA2 password: %s\n", apName, apPassword);
    Serial.printf("[HTTP] AP address: http://%s | mDNS: http://luma-15.local\n", WiFi.softAPIP().toString().c_str());
    uint32_t now = millis(); lastMotionUs = esp_timer_get_time(); demoStartedAt = now; pomoTickAt = now;
    renderFrame(now); publishSnapshot(now);
    httpd_config_t options = HTTPD_DEFAULT_CONFIG();
    options.stack_size = 8192; options.max_uri_handlers = 5; options.max_open_sockets = 4;
    options.recv_wait_timeout = 1; options.send_wait_timeout = 1; options.lru_purge_enable = true;
    if (httpd_start(&http, &options) == ESP_OK) {
      httpd_uri_t root{}; root.uri = "/"; root.method = HTTP_GET; root.handler = rootHandler; httpd_register_uri_handler(http, &root);
      httpd_uri_t status{}; status.uri = "/api/status"; status.method = HTTP_GET; status.handler = statusHandler; httpd_register_uri_handler(http, &status);
      const char *paths[] = {"/api/config", "/api/action", "/api/time"};
      for (const char *path : paths) { httpd_uri_t route{}; route.uri = path; route.method = HTTP_POST; route.handler = commandHandler; httpd_register_uri_handler(http, &route); }
    } else Serial.println("[HTTP] Server failed to start; check available heap");
  }

  void tick() {
    if (!commands || !snapshotMutex) { yield(); return; }
    uint32_t now = millis();
    updateDht(now);
    // Capturing DHT pulses must not contend with ADC, NVS or animation work.
    if (dhtState != 2) {
      updateSensors(now);
      drainCommands(now);
      updateTimers(now);
      updateWifi(now);
      updateClock(now);
      renderFrame(now);
      persist(now);
      if (elapsed(now, lastSnapshotAt) >= 250) publishSnapshot(now);
    }
    yield(); // relinquishes the CPU; never waits for a sensor or timer deadline
  }
};

// Out-of-class IRAM ISR: avoid Xtensa inline COMDAT literal relocation.
void IRAM_ATTR LumaFirmware::dhtInterrupt() {
    LumaFirmware *a = instance;
    if (!a) return;
    int level = gpio_get_level((gpio_num_t)DHT_PIN);
    uint32_t now = micros();
    portENTER_CRITICAL_ISR(&a->dhtMux);
    if (a->dhtDone) { portEXIT_CRITICAL_ISR(&a->dhtMux); return; }
    // Ignore host release rising edge. Consume response LOW/HIGH preamble.
    if (a->dhtStage == 0 && !level) a->dhtStage = 1;
    else if (a->dhtStage == 1 && level) a->dhtStage = 2;
    else if (a->dhtStage == 2 && !level) a->dhtStage = 3;
    else if (a->dhtStage == 3 && level) { a->dhtRiseAt = now; a->dhtStage = 4; }
    else if (a->dhtStage == 4 && !level) {
      uint32_t width = now - a->dhtRiseAt;
      if (width < 12 || width > 105) a->dhtBadPulse = true;
      uint8_t pos = a->dhtBit;
      a->dhtBytes[pos / 8] = uint8_t((a->dhtBytes[pos / 8] << 1) | (width > 48 ? 1 : 0));
      a->dhtBit = pos + 1;
      if (a->dhtBit >= 40) a->dhtDone = true;
      else a->dhtStage = 3;
    }
    portEXIT_CRITICAL_ISR(&a->dhtMux);
  }


LumaFirmware *LumaFirmware::instance = nullptr;
LumaFirmware lampController;
void setup() { lampController.begin(); }
void loop() { lampController.tick(); }
