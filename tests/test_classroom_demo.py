#!/usr/bin/env python3
"""Exercise the actual deployable classroom page, without query flags or APIs."""
from functools import partial
from http.server import SimpleHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from playwright.sync_api import sync_playwright,expect
ROOT=Path(__file__).resolve().parents[1]
class Quiet(SimpleHTTPRequestHandler):
 def log_message(self,*args):pass
server=ThreadingHTTPServer(('127.0.0.1',8767),partial(Quiet,directory=str(ROOT)))
Thread(target=server.serve_forever,daemon=True).start()
checks=[]
def passed(name):checks.append(name);print('PASS:',name,flush=True)
try:
 with sync_playwright() as p:
  browser=p.chromium.launch(executable_path='/usr/bin/chromium',args=['--no-sandbox','--disable-dev-shm-usage'])
  page=browser.new_page(viewport={'width':1440,'height':1000})
  errors=[];requests=[]
  page.on('pageerror',lambda e:errors.append(str(e)))
  page.on('request',lambda r:requests.append(r.url))
  page.goto('http://127.0.0.1:8767/docs/',wait_until='networkidle')
  expect(page.locator('#simulationBadge')).to_be_visible()
  page.wait_for_function('window.Luma?.getState().telemetry.leds.length === 15')
  passed('Static subfolder opens directly into simulation without query parameters')
  before=page.evaluate('JSON.stringify(window.Luma.getState().telemetry.leds)')
  page.wait_for_function('old => JSON.stringify(window.Luma.getState().telemetry.leds)!==old',arg=before)
  for i in range(12):
   page.locator(f'[data-effect="{i}"]').click()
   page.wait_for_function('i=>window.Luma.getState().config.effect===i',arg=i)
  passed('Smooth animation and all twelve effect controls work')
  page.locator('[data-lesson="temperature"]').click()
  page.wait_for_function('window.Luma.getState().config.mode==="temperature"')
  page.evaluate('window.Luma.setSimulationSensor({temperature:40})')
  page.wait_for_function('window.Luma.getState().telemetry.leds.every(c=>c.some(v=>v>0))')
  passed('Temperature scenario and virtual sensor change LED output')
  page.locator('[data-lesson="presence"]').click()
  page.wait_for_function('window.Luma.getState().config.mode==="auto"')
  page.wait_for_function('window.Luma.getState().telemetry.leds.some(c=>c.some(v=>v>0))')
  page.evaluate('window.Luma.setSimulationSensor({ldr:0,motion:false})')
  page.wait_for_function('window.Luma.getState().telemetry.leds.every(c=>c.every(v=>v===0))',timeout=15000)
  passed('Presence scenario lights in darkness and fades off in bright room')
  page.locator('[data-lesson="music"]').click()
  page.wait_for_function('window.Luma.getState().config.mode==="music"')
  level=page.evaluate('window.Luma.getState().telemetry.micLevel')
  page.wait_for_function('old=>window.Luma.getState().telemetry.micLevel!==old',arg=level)
  page.locator('#syntheticBeat').uncheck()
  page.evaluate('window.Luma.setSimulationSensor({micLevel:67})')
  page.wait_for_timeout(250)
  assert page.evaluate('window.Luma.getState().telemetry.micLevel')==67
  passed('Synthetic music varies; manual amplitude works when automatic rhythm is off')
  page.locator('[data-lesson="timer"]').click()
  expect(page.locator('#view-automations')).to_be_visible()
  page.wait_for_function('window.Luma.getState().config.focusMinutes===1')
  page.evaluate('window.Luma.action("pomodoroStart")')
  page.wait_for_function('window.Luma.getState().telemetry.pomodoro.remainingSec<60')
  passed('Pomodoro scenario opens routines and timer actually counts down')
  page.locator('[data-lesson="showcase"]').click()
  page.wait_for_function('window.Luma.getState().config.mode==="demo"')
  effect=page.evaluate('window.Luma.getState().telemetry.activeEffect')
  page.wait_for_function('old=>window.Luma.getState().telemetry.activeEffect!==old',arg=effect,timeout=10000)
  passed('Showcase automatically advances scenes')
  page.locator('#lessonReset').click()
  page.wait_for_function('window.Luma.getState().config.mode==="effect" && window.Luma.getState().config.effect===2')
  assert page.evaluate('window.Luma.getState().telemetry.pomodoro.state')=='idle'
  expect(page.locator('#syntheticBeat')).to_be_checked()
  passed('Reset restores usable scene and cancels timers')
  for width in [390,360]:
   page.set_viewport_size({'width':width,'height':844})
   assert page.evaluate('document.documentElement.scrollWidth <= innerWidth'),width
   page.locator('[data-lesson="temperature"]').click()
   page.wait_for_function('window.Luma.getState().config.mode==="temperature"')
  passed('Mobile 360/390px layouts and scenario buttons work')
  assert not any('/api/' in url for url in requests),requests
  assert all(url.startswith('http://127.0.0.1:8767/') for url in requests),requests
  assert not errors,errors
  passed('No MCU API requests, external assets, or JavaScript exceptions')
  browser.close()
 print(f'{len(checks)} checks passed; 0 failed')
finally:
 server.shutdown();server.server_close()
