# Arsitektur

```
cli ──► build (Session: prepareSession → runBuild)
          ├─ hardware   deteksi perangkat (via platform/)
          ├─ config     lapisan konfigurasi + resolusi adaptif (+ alasan tiap keputusan)
          ├─ project    discovery, scan #include, graph dependensi, deteksi main()/build system,
          │             templates: config.h dari config.h.cin/.in (tanpa CMake, tanpa mengubah source)
          ├─ assets     klasifikasi (magic bytes) + keputusan EMBED/EXTERNAL/LINK/SKIP
          ├─ backend    Backend (Emscripten, WASI SDK): argv, flag optimasi, dukungan dependensi
          └─ platform   runProcess/queryMemory/... : posix & win32
          ├─ runtime    wasm (parser biner aman), runner (node/WASI + timeout), server (HTTP statis)
diagnostics/doctor      cek lingkungan
```

Aturan: `core`, `config`, `project`, `assets`, `build` **tidak pernah** menyebut `emcc`/`clang`. Hanya `backend/`.

## Alur `build`
1. `discoverProject` – scan file (tanpa mengikuti symlink, melewati `.git`, `dist`, `.wasmforge`), scan `#include`,
   resolusi header proyek, klasifikasi header eksternal (std / posix / dependensi dikenal / tidak dikenal).
2. `detectHardware` + gabung konfigurasi (AUTO < global < lokal < CLI) → `resolveConfig`.
3. Pilih backend (probe), cek kompatibilitas dependensi/aset, kumpulkan diagnosis.
4. Hitung kunci cache per unit: `hash(backend+versi, flag, define, -I, source, semua header transitif)`.
5. Compile paralel (worker adaptif, throttle memori) ke `.wasmforge/cache/<key>.o` (tulis ke file sementara lalu rename).
6. Salin aset EXTERNAL apa adanya, buat loader VFS, link, validasi (file ada, header `.wasm` benar), tulis `manifest.json`.

## Batasan yang disengaja
- Scanner `#include` mengabaikan `#if/#ifdef` → dependensi adalah *over-approximation* (aman untuk inkremental,
  bisa memberi peringatan palsu untuk header yang dijaga `#ifdef _WIN32`).
- Build dilakukan langsung dari source (WasmForge membuat build graph sendiri). Proyek yang membutuhkan header hasil
  generate CMake/autotools (mis. `config.h`) didiagnosis (`W-MISSING-INCLUDE`), belum dijembatani otomatis.
- Hash cache = FNV-1a 64-bit: deteksi perubahan, **bukan** hash kriptografis.

## Menambah backend
Implementasikan `Backend` di `src/backend/backend.cpp`, daftarkan di `backendIds()`/`makeBackend()`. Tidak ada
perubahan lain yang diperlukan di core.
