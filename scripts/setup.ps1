# Configure, compile, and test WasmForge on Windows (PowerShell).
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $Root

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake tidak ditemukan. Install CMake dan compiler C++ (Visual Studio Build Tools) dulu.'
}

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DWASMFORGE_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw 'Konfigurasi CMake gagal.' }
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'Kompilasi WasmForge gagal.' }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Pengujian WasmForge gagal.' }

New-Item -ItemType Directory -Force -Path bin | Out-Null
Copy-Item build/Release/wasmfrg.exe bin/wasmfrg.exe -Force
Write-Host "`n[WasmForge] Selesai. Binary: $Root/bin/wasmfrg.exe"
Write-Host "[WasmForge] Jalankan: .\bin\wasmfrg.exe --help"
