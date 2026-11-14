# Status jujur terhadap acceptance criteria

Lingkungan pengembangan: Linux x86_64, g++ 13, **tanpa** Emscripten, CMake, atau compiler Windows.
Backend "fake" di `tests/integration/fake-emcc.sh` adalah *test double* untuk menguji orkestrasi — itu bukan Emscripten.
Modul `.wasm` untuk tes dirakit tangan (`tests/integration/wasmgen.py`) dan divalidasi silang dengan `WebAssembly.validate` milik Node.

| # | Kriteria | Status |
|---|---|---|
| 1 | `build` sederhana berfungsi | ✅ terverifikasi dengan test double; ⚠️ belum dengan Emscripten asli |
| 2 | Deteksi hardware tanpa root | ✅ Linux x86_64 (cgroup, affinity); jalur Termux/proot/Windows ditulis, belum dijalankan di sana |
| 3 | AUTO / MANUAL / HYBRID | ✅ unit + integrasi |
| 4 | Simpan/muat konfigurasi | ✅ |
| 5 | Backend adapter | ✅ Emscripten (dengan test double), ⚠️ WASI SDK belum dijalankan |
| 6–7 | Proyek C dan C++ dikompilasi | ⚠️ alur terverifikasi, **kompilasi WASM sungguhan belum** |
| 8 | Incremental build | ✅ (ubah header → hanya dependen yang dikompilasi ulang) |
| 9 | Asset detection | ✅ |
| 10 | Output package valid | ✅ biner `.wasm` di-parse penuh (section/import/memori), Web-vs-WASI dicek, manifest ditulis; ⚠️ belum dijalankan di browser asli |
| 11 | Error diagnosis jelas | ✅ |
| 12–14 | Termux ARM64 / proot ARM64 / Linux desktop | ⚠️ hanya Linux x86_64 yang dibangun dan diuji |
| 15 | Chocolate Doom sampai runtime | ❌ belum dikerjakan |
| 16 | Tidak ada benchmark rekayasa | ✅ `benchmark` hanya mencetak hasil ukur lokal; tidak ada angka perbandingan |
| 17 | Dokumentasi dan pengujian | ✅ |

## Phase
- Phase 1 (Foundation) dan 2 (Build Engine): diimplementasikan; menunggu verifikasi dengan Emscripten asli.
- Phase 3: cache inkremental, resource management, asset intelligence ✅. Optimizer sendiri ❌ (sengaja:
  optimasi didelegasikan ke LLVM/Binaryen; lihat `wasmfrg optimize`).
- Phase 4: loader VFS (diuji dengan stub di mode Node dan browser), `manifest.json`, `wasm-info`, `run`/`--verify` dengan timeout
  (modul WASI nyata dijalankan lewat `node:wasi`), dan `serve` (diuji dengan curl) ✅. **Belum** diuji: loader dengan runtime Emscripten asli,
  dan halaman runner di browser sungguhan (kanvas/audio/input). `dev` + live reload diuji dengan curl dan backend palsu.
- Phase 5 (Chocolate Doom): persiapan ✅ — pembuatan `config.h` dari template tanpa CMake, diagnosis header hilang,
  deteksi dependensi SDL2/SDL2_mixer/zlib/libpng. ❌ Belum: kompilasi Chocolate Doom sungguhan, banyak `main()` (butuh `--entry`),
  uji audio/input/WAD di browser. Nilai `HAVE_*` yang berstatus *guess* belum diverifikasi terhadap sysroot Emscripten asli.
- Phase 6: ❌ belum dimulai (profiling, optimasi memori, uji keandalan lanjutan).

Tidak ada phase yang dinyatakan selesai sebelum acceptance test-nya lulus.

## Langkah berikutnya yang disarankan
1. Pasang Emscripten, jalankan `wasmfrg build examples/hello-c` dan `examples/hello-cpp`, perbaiki temuan.
2. Tes runtime: jalankan hasil `.js/.wasm` dengan Node (`node dist/hello-c.js`).
3. Uji `wasmfrg serve` di browser HP (Termux) dan halaman runner dengan hasil build Emscripten.
4. "Bridge mode" untuk proyek CMake/autotools (membuat `config.h`) dengan persetujuan pengguna.
5. Kompilasi dan uji di Termux ARM64, proot ARM64, dan Windows.
