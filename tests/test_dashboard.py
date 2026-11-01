#!/usr/bin/env python3
"""Real Chromium integration checks for the bundled UI and both transports.

Run: python3 tests/test_dashboard.py
No ESP32 is connected: hardware transport uses an explicit REST mock; simulation
checks run only when ?demo=1 is deliberately selected.
"""
from __future__ import annotations

import copy
from datetime import datetime
import functools
import http.server
import json
from pathlib import Path
import threading
import time
from playwright.sync_api import sync_playwright, expect

ROOT = Path(__file__).resolve().parents[1]
PORT = 8765
BASE = f"http://127.0.0.1:{PORT}"
CONFIG = {
    "power": True, "mode": "manual", "effect": 0, "color": "#c4b5fd",
    "brightness": 128, "speed": 35, "saturation": 85, "reverse": False,
    "musicStyle": "bar", "micGain": 8, "motionHoldSec": 45,
    "darkThreshold": 2000, "autoDim": True, "idleMinutes": 0,
    "demoIntervalSec": 15, "sleepMinutes": 20, "wakeMinutes": 10,
    "scheduleEnabled": False, "scheduleStart": 1080, "scheduleEnd": 1380,
    "scheduleDays": 127, "utcOffsetMinutes": 420, "focusMinutes": 25,
    "breakMinutes": 5,
}
TELEMETRY = {
    "bootId": "1111111122222222",
    "uptimeSec": 1234, "freeHeap": 120000, "minHeap": 115000,
    "heapSize": 300000, "rssi": -56, "wifiConnected": True,
    "ip": "192.168.1.73", "apIp": "192.168.4.1", "apClients": 0,
    "clockValid": True, "epoch": 1793577600, "temperature": 27.5,
    "humidity": 62, "dhtOk": True, "ldr": 2400, "micLevel": 30,
    "motion": True, "motionRemainingSec": 40, "activeMode": "manual",
    "activeEffect": 0, "outputBrightness": 128, "gateReason": "active",
    "pomodoro": {"state": "idle", "running": False, "remainingSec": 1500,
                  "sessions": 0},
    "sleepRemainingSec": 0, "dirty": False, "storageOk": True,
    "leds": [[23, 45, 67] for _ in range(15)],
}


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args):
        pass


def check(condition, message):
    assert condition, message


def run():
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", PORT), functools.partial(QuietHandler, directory=str(ROOT)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    results = []
    errors = []

    def passed(name):
        results.append(name)
        print(f"PASS: {name}", flush=True)

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(
                executable_path="/usr/bin/chromium", headless=True,
                args=["--no-sandbox", "--disable-dev-shm-usage"])
            page = browser.new_page(viewport={"width": 1440, "height": 1000})
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(BASE + "/?demo=1", wait_until="networkidle")
            page.wait_for_function("window.Luma && window.Luma.getState()?.config")

            def state():
                return page.evaluate("window.Luma.getState()")

            def patch(value):
                page.evaluate("patch => window.Luma.patchConfig(patch)", value)
                page.wait_for_function(
                    "patch => Object.entries(patch).every(([k,v]) => window.Luma.getState().config[k] === v)",
                    arg=value)

            def action(name):
                page.evaluate("name => window.Luma.action(name)", name)

            check(set(state()["config"]) == set(CONFIG), "Config schema differs from shared contract")
            controls = set(page.locator("[data-config]").evaluate_all(
                "els => els.map(el => el.dataset.config)"))
            check((set(CONFIG) - {"mode", "effect"}) <= controls, f"Missing controls: {set(CONFIG) - controls - {'mode', 'effect'}}")
            check(page.locator("[data-mode]").count() == 7, "All seven mode tiles must exist")
            check(page.locator("[data-effect]").count() == 12, "All twelve effect tiles must exist")
            check("simulation" in page.locator("body").inner_text().lower(), "Simulation badge missing")
            passed("Simulation is explicit and every config parameter has a control")

            patch({"mode": "manual", "color": "#ff0000", "brightness": 160,
                   "power": True, "scheduleEnabled": False, "idleMinutes": 0})
            page.wait_for_function("window.Luma.getState().telemetry.leds.some(c => c[0] > 0 && c[1] === 0 && c[2] === 0)")
            patch({"power": False})
            page.wait_for_function("window.Luma.getState().telemetry.gateReason === 'power'")
            check(all(not any(c) for c in state()["telemetry"]["leds"]), "Power off must black LEDs")
            patch({"power": True, "mode": "effect"})
            for effect in range(12):
                page.locator(f'[data-effect="{effect}"]').click()
                page.wait_for_function("effect => window.Luma.getState().config.effect === effect", arg=effect)
                page.wait_for_timeout(120)
                t = state()["telemetry"]
                check(t["activeEffect"] == effect, f"Effect {effect} not active")
                check(len(t["leds"]) == 15, "Incorrect LED count")
                check(all(len(c) == 3 and all(isinstance(v, (int, float)) and 0 <= v <= 255 for v in c)
                          for c in t["leds"]), f"Effect {effect} invalid RGB")
            passed("Manual color, power gating, and all twelve effects render valid frames")

            patch({"mode": "temperature"})
            page.evaluate("window.Luma.setSimulationSensor({temperature:40, humidity:80})")
            page.wait_for_function("window.Luma.getState().telemetry.temperature === 40")
            hot = state()["telemetry"]["leds"]
            page.evaluate("window.Luma.setSimulationSensor({temperature:15})")
            page.wait_for_timeout(150)
            cold = state()["telemetry"]["leds"]
            check(hot != cold, "Temperature mode ignores sensor")
            patch({"mode": "music"})
            for style in ["bar", "center", "pulse"]:
                patch({"musicStyle": style})
                page.evaluate("window.Luma.setSimulationSensor({micLevel:70})")
                page.wait_for_timeout(150)
                check(any(any(c) for c in state()["telemetry"]["leds"]), f"Music style {style} is dark")
            passed("Temperature sensor and all three music styles produce output")

            page.evaluate("window.Luma.setSimulationSensor({dhtOk:false})")
            page.wait_for_function("window.Luma.getState().telemetry.temperature === null && window.Luma.getState().telemetry.humidity === null")
            page.locator('[data-view="diagnostics"]').click()
            expect(page.locator("#temperature")).to_have_text("—")
            expect(page.locator("#humidity")).to_have_text("—")
            page.evaluate("window.Luma.setSimulationSensor({dhtOk:true})")
            page.locator('[data-view="studio"]').click()
            passed("Sensor failure is shown as unavailable instead of invented readings")

            patch({"mode": "auto", "darkThreshold": 2000, "motionHoldSec": 5})
            page.evaluate("window.Luma.setSimulationSensor({ldr:4095, motion:true})")
            page.wait_for_function("window.Luma.getState().telemetry.outputBrightness > 0")
            page.evaluate("window.Luma.setSimulationSensor({ldr:0})")
            page.wait_for_function("window.Luma.getState().telemetry.outputBrightness === 0")
            passed("Automatic mode responds to darkness and motion")

            patch({"mode": "manual", "focusMinutes": 1, "breakMinutes": 1})
            action("pomodoroStart")
            page.wait_for_function("window.Luma.getState().telemetry.pomodoro.running")
            page.wait_for_function("window.Luma.getState().telemetry.pomodoro.remainingSec < 60")
            action("pomodoroPause")
            page.wait_for_function("!window.Luma.getState().telemetry.pomodoro.running")
            remaining = state()["telemetry"]["pomodoro"]["remainingSec"]
            page.wait_for_timeout(1150)
            check(state()["telemetry"]["pomodoro"]["remainingSec"] == remaining, "Paused timer still ticks")
            action("pomodoroResume")
            page.wait_for_function("window.Luma.getState().telemetry.pomodoro.remainingSec < " + str(remaining))
            action("pomodoroReset")
            page.wait_for_function("window.Luma.getState().telemetry.pomodoro.state === 'idle'")
            patch({"sleepMinutes": 1})
            action("sleepStart")
            page.wait_for_function("window.Luma.getState().telemetry.sleepRemainingSec > 0")
            action("sleepCancel")
            page.wait_for_function("window.Luma.getState().telemetry.sleepRemainingSec === 0")
            action("save")
            page.wait_for_function("!window.Luma.getState().telemetry.dirty")
            passed("Pomodoro pause/resume/reset, sleep start/cancel, and persistence action")

            before = copy.deepcopy(state()["config"])
            result = page.evaluate("async () => {try {await window.Luma.patchConfig({brightness:999});return 'accepted';} catch(e){return 'rejected';}}")
            check(result == "rejected", "Invalid brightness should reject")
            check(state()["config"] == before, "Invalid update changed config")
            for payload in ['{"__proto__":{}}', '{"toString":"invalid"}', '{"constructor":{}}']:
                result = page.evaluate(
                    "async json => {try {await window.Luma.patchConfig(JSON.parse(json));return 'accepted';} catch(e){return 'rejected';}}",
                    payload)
                check(result == "rejected", f"Prototype-inherited unknown config accepted: {payload}")
                check(state()["config"] == before, "Prototype key changed configuration")
            passed("Invalid config is rejected atomically")

            # Check clock/day ownership independently of wall-clock test timing.
            scheduled = browser.new_page(viewport={"width": 1280, "height": 900})
            scheduled.on("pageerror", lambda error: errors.append(str(error)))
            scheduled.clock.install(time=datetime.fromisoformat("2026-11-01T23:30:00+00:00"))
            scheduled.goto(BASE + "/?demo=1", wait_until="networkidle")
            scheduled.wait_for_function("window.Luma?.getState()?.config")
            scheduled.evaluate("window.Luma.patchConfig({mode:'manual',power:true,scheduleEnabled:true,scheduleStart:1380,scheduleEnd:60,scheduleDays:1,utcOffsetMinutes:0,wakeMinutes:0})")

            def schedule_case(iso, expected):
                scheduled.clock.set_system_time(datetime.fromisoformat(iso))
                scheduled.evaluate("window.Luma.syncTime()")
                scheduled.wait_for_function(
                    "expected => window.Luma.getState().telemetry.gateReason === expected",
                    arg=expected)

            schedule_case("2026-11-01T23:30:00+00:00", "active")  # selected Sunday
            schedule_case("2026-11-02T00:30:00+00:00", "active")  # Sunday-owned tail
            schedule_case("2026-11-02T01:00:00+00:00", "schedule")  # exclusive end
            schedule_case("2026-11-02T23:30:00+00:00", "schedule")  # Monday unselected
            scheduled.evaluate("window.Luma.patchConfig({utcOffsetMinutes:420})")
            schedule_case("2026-11-01T17:30:00+00:00", "active")  # Monday 00:30 local
            passed("Overnight schedule honors selected start day, exclusive end, and UTC offset")

            scheduled.evaluate("window.Luma.setSimulationSensor({clockValid:false})")
            scheduled.wait_for_function("window.Luma.getState().telemetry.gateReason === 'clock' && window.Luma.getState().telemetry.outputBrightness === 0")
            scheduled.evaluate("window.Luma.syncTime()")
            scheduled.wait_for_function("window.Luma.getState().telemetry.clockValid && window.Luma.getState().telemetry.gateReason === 'active'")
            scheduled.evaluate("window.Luma.patchConfig({utcOffsetMinutes:0,wakeMinutes:10,brightness:255,autoDim:false})")
            schedule_case("2026-11-01T23:00:00+00:00", "active")
            check(scheduled.evaluate("window.Luma.getState().telemetry.outputBrightness") == 0,
                  "Scheduled wake should start at zero")
            schedule_case("2026-11-01T23:00:30+00:00", "active")
            check(0 < scheduled.evaluate("window.Luma.getState().telemetry.outputBrightness") < 255,
                  "Wake fade should advance within a minute")
            passed("Invalid clock gates safely off and scheduled wake fades with sub-minute precision")

            scheduled.evaluate("window.Luma.patchConfig({scheduleEnabled:false,mode:'demo',demoIntervalSec:5})")
            scheduled.wait_for_function("window.Luma.getState().telemetry.activeEffect === 0")
            scheduled.clock.fast_forward(5200)
            scheduled.wait_for_function("window.Luma.getState().telemetry.activeEffect === 1")
            scheduled.evaluate("window.Luma.patchConfig({mode:'circadian'})")
            scheduled.wait_for_function("window.Luma.getState().telemetry.activeMode === 'circadian'")
            passed("Self-cycling demo advances scenes and circadian mode uses the clock")

            scheduled.evaluate("window.Luma.patchConfig({mode:'manual',idleMinutes:1,sleepMinutes:1,focusMinutes:1,breakMinutes:1})")
            scheduled.evaluate("window.Luma.setSimulationSensor({motion:false})")
            scheduled.clock.fast_forward(61000)
            scheduled.wait_for_function("window.Luma.getState().telemetry.gateReason === 'idle'")
            scheduled.evaluate("window.Luma.patchConfig({idleMinutes:0})")
            scheduled.evaluate("window.Luma.action('sleepStart')")
            scheduled.clock.fast_forward(61000)
            scheduled.wait_for_function("window.Luma.getState().telemetry.gateReason === 'sleep'")
            scheduled.evaluate("window.Luma.action('sleepCancel')")
            scheduled.wait_for_function("window.Luma.getState().telemetry.gateReason === 'active'")
            scheduled.evaluate("window.Luma.action('pomodoroStart')")
            scheduled.clock.fast_forward(61000)
            scheduled.wait_for_function("window.Luma.getState().telemetry.pomodoro.state === 'break' && window.Luma.getState().telemetry.pomodoro.sessions === 1")
            scheduled.evaluate("window.Luma.action('pomodoroReset')")
            passed("Idle saving, completed sleep latch/cancel, and complete focus-to-break transition")
            scheduled.close()

            page.set_viewport_size({"width": 390, "height": 844})
            page.wait_for_timeout(200)
            check(page.evaluate("document.documentElement.scrollWidth <= innerWidth + 1"), "Mobile horizontal overflow")
            page.screenshot(path=str(ROOT / "tests/dashboard-mobile.png"), full_page=True)
            page.set_viewport_size({"width": 1440, "height": 1000})
            page.screenshot(path=str(ROOT / "tests/dashboard-desktop.png"), full_page=True)
            passed("Responsive mobile layout has no horizontal overflow")

            # A real-device connection uses HTTP, never silent synthetic fallbacks.
            device = browser.new_page(viewport={"width": 1280, "height": 900})
            device.on("pageerror", lambda error: errors.append(str(error)))
            fixture = {"apiVersion": 1, "revision": 1,
                       "config": copy.deepcopy(CONFIG), "telemetry": copy.deepcopy(TELEMETRY)}
            fixture["telemetry"].update(clockValid=False, epoch=None)
            queued = []
            calls = []
            fail_updates = False
            offline = False
            hold_next_status = False
            held_status = []
            status_override = None

            def mock(route):
                nonlocal fail_updates, offline, hold_next_status, status_override
                path = route.request.url.split("?", 1)[0]
                if offline:
                    route.abort("failed")
                    return
                if path.endswith("/api/status"):
                    if status_override is not None:
                        old = status_override
                        status_override = None
                        route.fulfill(status=200, content_type="application/json", body=json.dumps(old))
                        return
                    if hold_next_status:
                        hold_next_status = False
                        held_status.append((route, copy.deepcopy(fixture)))
                        return
                    route.fulfill(status=200, content_type="application/json", body=json.dumps(fixture))
                    return
                calls.append((path, route.request.method, route.request.post_data_json))
                if path.endswith("/api/config"):
                    if fail_updates:
                        route.fulfill(status=400, content_type="application/json",
                                      body='{"ok":false,"error":"invalid brightness"}')
                    else:
                        queued.append(route.request.post_data_json)
                        route.fulfill(status=202, content_type="application/json",
                                      body='{"ok":true,"queued":true}')
                    return
                if path.endswith("/api/time"):
                    fixture["telemetry"]["clockValid"] = True
                    fixture["telemetry"]["epoch"] = route.request.post_data_json["epoch"]
                    fixture["revision"] += 1
                route.fulfill(status=202, content_type="application/json", body='{"ok":true,"queued":true}')

            device.route("**/api/**", mock)
            device.goto(BASE + "/", wait_until="networkidle")
            device.wait_for_function("window.Luma?.getState()?.telemetry?.uptimeSec === 1234")
            device.wait_for_function("window.Luma.getState().telemetry.clockValid")
            check(sum(path.endswith("/api/time") for path, _, _ in calls) == 1,
                  "Initial invalid device clock should sync once")
            check(device.evaluate("window.Luma.getState().telemetry.leds[0]") == [23, 45, 67],
                  "Device output must preserve actual reported LED sample")
            device.evaluate("window.__patchDone=false;window.__patchError='';window.Luma.patchConfig({brightness:91}).then(()=>window.__patchDone=true).catch(e=>window.__patchError=e.message);void 0")
            device.wait_for_function("document.readyState === 'complete'")
            deadline = time.monotonic() + 5
            while not queued and time.monotonic() < deadline:
                device.wait_for_timeout(50)
            check(bool(queued), "Device config did not reach endpoint")
            device.wait_for_timeout(200)
            check(not device.evaluate("window.__patchDone"), "202 queue acknowledgement was mistaken for applied state")
            fixture["config"].update(queued.pop(0))
            fixture["revision"] += 1
            device.wait_for_function("window.__patchDone", timeout=10000)
            check(device.evaluate("window.Luma.getState().config.brightness") == 91, "Confirmed config missing")
            check(all(method == "POST" for _, method, _ in calls), "Mutations must POST")
            passed("Device transport sends POST JSON and waits for status confirmation after queue acknowledgement")

            # A delayed older poll must not undo a newer command confirmation.
            hold_next_status = True
            device.evaluate("document.getElementById('retry').click()")
            deadline = time.monotonic() + 5
            while not held_status and time.monotonic() < deadline:
                device.wait_for_timeout(50)
            check(bool(held_status), "Could not capture a pending old status response")
            device.evaluate("window.__nextDone=false;window.Luma.patchConfig({brightness:90}).then(()=>window.__nextDone=true).catch(e=>window.__patchError=e.message);void 0")
            deadline = time.monotonic() + 5
            while not queued and time.monotonic() < deadline:
                device.wait_for_timeout(50)
            check(bool(queued), "Concurrent command was not queued")
            fixture["config"].update(queued.pop(0))
            fixture["revision"] += 1
            device.wait_for_function("window.__nextDone", timeout=10000)
            old_route, old_snapshot = held_status.pop(0)
            old_route.fulfill(status=200, content_type="application/json", body=json.dumps(old_snapshot))
            device.wait_for_timeout(200)
            check(device.evaluate("window.Luma.getState().config.brightness") == 90,
                  "Late status response overwrote newer confirmed config")
            passed("Out-of-order status responses cannot roll back confirmed state")

            # Reboot resets both monotonic uptime and the mutation revision.
            retired_fixture = copy.deepcopy(fixture)
            fixture["revision"] = 0
            fixture["config"] = copy.deepcopy(CONFIG)
            fixture["telemetry"].update(bootId="3333333344444444", uptimeSec=2, clockValid=False, epoch=None)
            device.wait_for_function("window.Luma.getState().telemetry.uptimeSec === 2", timeout=10000)
            device.wait_for_function("window.Luma.getState().telemetry.clockValid", timeout=10000)
            check(device.evaluate("window.Luma.getState().config.brightness") == 128,
                  "Dashboard retained stale config after MCU reboot")
            check(sum(path.endswith("/api/time") for path, _, _ in calls) == 2,
                  "Reboot should reset auto-clock attempt and resynchronize once")
            status_override = retired_fixture
            device.evaluate("document.getElementById('retry').click()")
            device.wait_for_timeout(200)
            check(status_override is None, "Retired boot fixture was not served")
            check(device.evaluate("window.Luma.getState().telemetry.bootId") == "3333333344444444",
                  "Retired boot response resurrected pre-reboot state")
            passed("MCU reboot revision reset is accepted and invalid clock resynchronizes")

            fail_updates = True
            result = device.evaluate("async () => {try {await window.Luma.patchConfig({brightness:90});return 'accepted';}catch(e){return 'rejected';}}")
            check(result == "rejected", "HTTP 400 must reject command")
            check(device.evaluate("window.Luma.getState().config.brightness") == 128, "Rejected command changed shown state")
            offline = True
            device.wait_for_timeout(4000)
            expect(device.locator("#simulationBadge")).not_to_be_visible()
            expect(device.locator("#connectionError")).to_be_visible()
            check(device.evaluate("window.Luma.getState().telemetry.uptimeSec") == 2,
                  "Offline device telemetry was fabricated")
            passed("HTTP failures reject updates and offline devices never silently become simulation")
            check(not errors, f"JavaScript exceptions: {errors}")
            passed("No unhandled JavaScript exceptions in simulation or device transport")
            browser.close()
        print(json.dumps({"passed": len(results), "failed": 0, "checks": results}, indent=2))
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    run()
