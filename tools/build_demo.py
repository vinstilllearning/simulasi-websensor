#!/usr/bin/env python3
"""Build an always-simulated, standalone classroom site from the device UI."""
from pathlib import Path
import argparse
ROOT=Path(__file__).resolve().parents[1]

def build(check=False):
 s=(ROOT/'index.html').read_text()
 def replace(old,new):
  nonlocal s
  assert old in s, f'Dashboard changed: missing anchor {old[:70]}'
  s=s.replace(old,new)
 replace('<html lang="en">','<html lang="id">')
 replace('<title>LUMA / 15 — Light studio</title>','<title>Lampu Hias Pintar — Simulasi LUMA / 15</title>\n<meta name="description" content="Simulasi interaktif lampu hias pintar ESP32: 15 LED, sensor suhu, cahaya, suara, gerak, dan otomasi. Demonstrasi tanpa perangkat fisik.">')
 replace("const isSimulation=location.protocol==='file:'||new URLSearchParams(location.search).get('demo')==='1';","const isSimulation=true; // This deployed classroom site never connects to an MCU.")
 replace("mode:'manual',effect:0,color:'#c4b5fd',brightness:128","mode:'effect',effect:2,color:'#c4b5fd',brightness:200")
 replace('autoDim:true,idleMinutes:0','autoDim:false,idleMinutes:0')
 replace('luma15-simulation-v1','luma15-classroom-v1')
 replace('LIGHT STUDIO','SIMULASI KELAS')
 replace('>SIMULATION<','>SIMULASI<')
 replace('Simulation studio. Sensor readings and output are generated locally; no hardware is connected.','Demonstrasi virtual · Tidak memerlukan ESP32. Nilai sensor, memori, RSSI, dan jaringan adalah data simulasi, bukan pengukuran perangkat fisik.')
 for old,new in [('Light, with intention.','Satu lampu. Banyak kemungkinan.'),('Shape a moment. Let the room follow.','Eksperimen cahaya, sensor, dan otomasi dalam satu simulasi interaktif.'),('SYSTEM UPTIME','DURASI SIMULASI'),('YOUR SPACE. YOUR ATMOSPHERE.','PROYEK IOT · ESP32 + 15 LED WS2812B'),('>Routines<','>Rutinitas<'),('>System<','>Diagnostik<'),('Simulation workbench','Workbench sensor virtual'),('Controlled inputs for exploring the complete lighting system.','Geser nilai sensor untuk menunjukkan respons lampu secara langsung.'),('LOCAL ONLY','SENSOR VIRTUAL'),('Simulation live','Simulasi aktif'),('Save scene','Simpan adegan')]:
  replace(old,new)
 replace("const modeLabels=['Manual','Effects','Thermal','Music','Presence','Showcase','Circadian'];","const modeLabels=['Manual','Efek','Suhu','Musik','Otomatis','Showcase','Sirkadian'];")
 css='''\n.lesson{margin:0 0 22px;padding:20px 24px;border:1px solid #a6f6e02b;border-radius:18px;background:linear-gradient(115deg,#a6f6e008,#c4b5fd08)}.lesson-head{display:flex;align-items:center;justify-content:space-between;gap:12px}.lesson p{color:var(--muted);font-size:12px;line-height:1.8;margin-top:8px}.lesson-scenes{display:flex;flex-wrap:wrap;gap:8px;margin:16px 0}.lesson-scenes button[aria-pressed=true]{border-color:var(--cyan);color:var(--cyan);background:#a6f6e00c}.lesson-meta{display:flex;flex-wrap:wrap;gap:12px;align-items:center;font-size:11px;color:var(--muted);margin-top:14px}.lesson-meta label{display:flex;align-items:center;gap:8px}.lesson-note{padding:12px 14px;border-left:2px solid var(--lilac);background:#c4b5fd07;border-radius:0 8px 8px 0}.lamp-svg{width:210px;height:390px}.lamp-stage{min-height:410px}@media(max-width:760px){.lesson{padding:17px}.lesson-head{align-items:flex-start}.lesson-scenes button{flex:1 1 42%}.lamp-stage{min-height:300px}.lamp-svg{width:170px;height:290px}}\n'''
 replace('</style>',css+'</style>')
 lesson='''<section class="lesson" aria-label="Panduan presentasi">
<div class="lesson-head"><div><div class="eyebrow">PANDUAN PRESENTASI</div><h2>Kenali cara kerja lampu pintar</h2></div><button class="btn" id="lessonReset">Mulai ulang</button></div>
<p>ESP32 mengolah masukan DHT11, LDR, mikrofon MAX4466, dan PIR untuk mengendalikan 15 LED. Pilih skenario, lalu ubah sensornya di workbench di bagian bawah halaman.</p>
<div class="lesson-scenes"><button class="btn" data-lesson="effects" aria-pressed="true">01 · Efek cahaya</button><button class="btn" data-lesson="temperature" aria-pressed="false">02 · Sensor suhu</button><button class="btn" data-lesson="presence" aria-pressed="false">03 · Gelap &amp; gerak</button><button class="btn" data-lesson="music" aria-pressed="false">04 · Musik</button><button class="btn" data-lesson="timer" aria-pressed="false">05 · Pomodoro</button><button class="btn" data-lesson="showcase" aria-pressed="false">06 · Showcase</button></div>
<div class="lesson-note" aria-live="polite"><strong id="lessonTitle">01 · Eksplorasi 12 efek</strong><p id="lessonNote">Pilih ubin efek, lalu ubah kecepatan, saturasi, atau arah. Lima belas titik pada lampu memperlihatkan keluaran simulasi.</p></div>
<div class="lesson-meta"><label><input type="checkbox" class="toggle" id="syntheticBeat" checked> Irama suara sintetis otomatis</label><span>Tanpa backend · Tanpa izin mikrofon · Siap hosting statis</span></div>
</section>'''
 replace('<section class="view" id="view-studio"',lesson+'\n<section class="view" id="view-studio"')
 replace('setConnection(false);render();poll();',(ROOT/'tools/classroom_demo.js').read_text()+'\nsetConnection(false);render();poll();')
 target=ROOT/'docs/index.html'
 if check: assert target.read_text()==s, 'Demo is stale; run python3 tools/build_demo.py'
 else:
  target.parent.mkdir(exist_ok=True);target.write_text(s)
 print(f'Classroom demo: {len(s.encode()):,} bytes; standalone, simulation always on')

if __name__=='__main__':
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--check',action='store_true');build(parser.parse_args().check)
