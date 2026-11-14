# Windows

Dukungan Windows ditulis sebagai implementasi platform terpisah (`src/platform/platform_win32.cpp`), jadi
seluruh logika di atasnya sama dengan Linux.

| Fungsi | Implementasi Win32 |
|---|---|
| RAM total/tersedia | `GlobalMemoryStatusEx` |
| Core | `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` |
| Model CPU | registry `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` |
| Arsitektur | `GetNativeSystemInfo` (x86_64 / arm64 / x86) |
| Menjalankan proses | `CreateProcessW` + pipa, quoting sesuai aturan CRT, **tanpa cmd.exe** |
| Mencari program | `PATH` + `PATHEXT` |
| Folder konfigurasi | `%APPDATA%\WasmForge\config.ini` |
| Argumen UTF-8 | `CommandLineToArgvW` → UTF-8, konsol diset ke CP_UTF8 |

**Launcher `.bat`**: `emcc`/`em++` dari emsdk di Windows adalah `emcc.bat`. File batch hanya bisa dijalankan lewat
`cmd.exe`, yang mem-parse ulang baris perintah. Karena itu WasmForge **menolak** argumen yang mengandung
`" % ^ & | < > !` atau baris baru saat memakai launcher batch, daripada mencoba meng-escape-nya.
Jika Anda butuh flag berisi karakter itu, gunakan emsdk yang memanggil `python emcc.py` langsung atau WSL.

| Timeout proses | thread pembaca pipa + `WaitForSingleObject(timeout)` + `TerminateProcess` |
| `wasmfrg serve` | Winsock2 (`select` untuk accept, `closesocket`), IPv4 |

Catatan: pada Windows hanya proses utama yang dihentikan saat timeout (POSIX mematikan seluruh process group).

**Status:** kode Windows ditulis dengan hati-hati tetapi **belum dikompilasi maupun dijalankan** (lingkungan
pengembangan tidak punya compiler Windows). Langkah verifikasi: build dengan MSVC/MinGW, jalankan
`wasmfrg doctor`, lalu `wasmfrg build examples\hello-c --dry-run`.
