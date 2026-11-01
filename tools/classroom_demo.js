// Included inside the standalone classroom page by build_demo.py.
const lessonScenes={
 effects:{mode:'effect',effect:2,title:'01 · Eksplorasi 12 efek',note:'Pilih ubin efek, lalu ubah kecepatan, saturasi, atau arah. Lima belas titik pada lampu memperlihatkan keluaran simulasi.'},
 temperature:{mode:'temperature',title:'02 · Termometer cahaya',note:'Geser Temperature di workbench sensor. Suhu rendah memberi cahaya biru; suhu tinggi menambah titik menyala dan menggeser warna ke merah.'},
 presence:{mode:'auto',title:'03 · Lampu peka ruangan',note:'Room darkness di atas 2000 berarti gelap. Matikan Motion detected: lampu mempertahankan cahaya selama 5 detik, kemudian memudar. Ruangan terang membuat lampu padam.'},
 music:{mode:'music',title:'04 · Cahaya mengikuti suara',note:'Irama sintetis menggerakkan indikator suara. Coba Rising bar, Center bloom, dan Full pulse. Matikan irama otomatis untuk mengatur amplitudo secara manual.'},
 timer:{mode:'manual',title:'05 · Fokus dengan Pomodoro',note:'Durasi contoh fokus dan istirahat masing-masing 1 menit. Buka Rutinitas, tekan Mulai, kemudian coba Pause, Resume, dan Reset.'},
 showcase:{mode:'demo',title:'06 · Showcase otomatis',note:'Setiap 5 detik sistem berganti efek. Setelah 12 efek, giliran mode suhu, musik, dan deteksi ruangan. Kembali ke Studio untuk melihat lampu.'}
};
let lessonBusy=false;
async function lessonScene(name){
 if(lessonBusy)return;
 lessonBusy=true;
 document.querySelectorAll('[data-lesson]').forEach(b=>b.disabled=true);
 try{
 const scene=lessonScenes[name];
 await action('sleepCancel');await action('pomodoroReset');
 await patchConfig({power:true,scheduleEnabled:false,idleMinutes:0,autoDim:false,brightness:200,mode:scene.mode,effect:scene.effect??0,motionHoldSec:5,demoIntervalSec:5,focusMinutes:1,breakMinutes:1});
 window.Luma.setSimulationSensor({temperature:27.5,humidity:62,ldr:3200,micLevel:35,motion:true,dhtOk:true,clockValid:true});
 document.querySelector('[data-view="'+(name==='timer'?'automations':'studio')+'"]').click();
 $('lessonTitle').textContent=scene.title;$('lessonNote').textContent=scene.note;
 document.querySelectorAll('[data-lesson]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.lesson===name)));
 }catch(error){showError(error)}finally{lessonBusy=false;document.querySelectorAll('[data-lesson]').forEach(b=>b.disabled=false)}
}
document.querySelectorAll('[data-lesson]').forEach(b=>b.addEventListener('click',()=>lessonScene(b.dataset.lesson)));
$('lessonReset').addEventListener('click',async()=>{
 if(lessonBusy)return;
 await action('sleepCancel');await action('pomodoroReset');await patchConfig({...defaults});
 $('syntheticBeat').checked=true;await lessonScene('effects');
});
let lessonLastFrame=0;
function classroomFrame(now){
 if(!document.hidden&&now-lessonLastFrame>=50){
 lessonLastFrame=now;
 if($('syntheticBeat').checked&&(state.config.mode==='music'||state.config.mode==='demo'&&state.telemetry.activeMode==='music')){
 const phase=now/1000;
 const amplitude=Math.round(12+76*Math.pow(Math.max(0,Math.sin(phase*5.8)),2));
 window.Luma.setSimulationSensor({micLevel:amplitude});
 }else readStatus().catch(showError);
 }
 requestAnimationFrame(classroomFrame);
}
requestAnimationFrame(classroomFrame);
