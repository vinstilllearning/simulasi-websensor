# Simulasi presentasi LUMA / 15

`index.html` adalah satu file mandiri: CSS, ikon SVG, mesin simulasi, dan JavaScript sudah tertanam. Buka langsung atau deploy folder ini. Tidak memerlukan ESP32, backend, API key, npm, atau parameter `?demo=1`.

## Deploy

- **GitHub Pages:** Settings → Pages → Deploy from a branch → branch `luma15-ecosystem` → folder `/docs` → Save. GitHub akan menampilkan URL setelah deployment berhasil.
- **Netlify:** pilih repositori/branch yang sama; kosongkan build command dan isi publish directory dengan `docs`. Bisa juga drag-and-drop folder ini.
- **Vercel:** pilih framework Other, kosongkan build command, dan atur output directory `docs`.
- **Hosting biasa:** unggah `index.html` sebagai halaman utama. Semua URL relatif aman untuk subfolder GitHub Pages.

Deployment belum diaktifkan otomatis. Tidak ada koneksi ke endpoint `/api/*`; hosting statis cukup.

## Urutan presentasi

1. Efek cahaya: pilih dari 12 efek dan ubah kecepatan.
2. Sensor suhu: geser suhu di workbench; warna dan jumlah LED berubah.
3. Gelap & gerak: ruangan gelap + gerak menyalakan lampu; setelah gerak dimatikan ada hold 5 detik sebelum fade.
4. Musik: irama suara sintetis menggerakkan tiga pilihan visual. Matikan irama otomatis untuk mengatur input suara sendiri.
5. Pomodoro: durasi contoh satu menit; mulai/pause/resume/reset di Rutinitas.
6. Showcase: pergantian efek otomatis setiap lima detik.

Tombol Mulai ulang mengembalikan adegan awal. Jadwal, sleep fade, dan diagnostik tetap bisa dieksplorasi. Semua sensor, heap, RSSI, IP, dan keluaran adalah contoh simulasi yang diberi label; ini bukan bukti pengujian perangkat fisik. Audio tidak direkam. Pengaturan simulasi disimpan lokal di browser jika penyimpanan tersedia.

## Memperbarui halaman

Dari root repositori: `python3 tools/build_demo.py`. Sumbernya dashboard perangkat `index.html` dan tambahan presentasi `tools/classroom_demo.js`; halaman firmware tetap terpisah dari versi presentasi. Periksa dengan `python3 tools/build_demo.py --check`.

Validasi: `python3 tests/test_classroom_demo.py` lulus 10 pemeriksaan Chromium: pembukaan tanpa query, 12 efek, suhu, otomatis, irama/manual suara, countdown Pomodoro, pergantian showcase, reset, ponsel 360/390 px, dan tidak ada exception JavaScript atau permintaan API/aset eksternal.
