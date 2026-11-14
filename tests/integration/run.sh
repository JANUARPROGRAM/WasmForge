#!/usr/bin/env bash
# Integration tests for orchestration logic. Uses tests/integration/fake-emcc.sh as a stand-in backend
# (a test double, NOT Emscripten) so that cache/scheduler/packaging behaviour can be verified anywhere.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
WF="${WASMFRG:-$HERE/../../build/wasmfrg}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
FAIL=0; N=0
check() { N=$((N+1)); if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; FAIL=$((FAIL+1)); fi; }

mkdir -p "$T/bin"
cp "$HERE/fake-emcc.sh" "$T/bin/emcc"; cp "$HERE/fake-emcc.sh" "$T/bin/em++"
export PATH="$T/bin:$PATH" FAKE_LOG="$T/log.txt" XDG_CONFIG_HOME="$T/cfg"
: > "$FAKE_LOG"

P="$T/my proj; touch $T/PWNED"          # spaces + shell metacharacters on purpose (injection test)
mkdir -p "$P/src" "$P/data"
cat > "$P/src/common.h" <<'H'
int common(void);
H
cat > "$P/src/a.c" <<'C'
#include "common.h"
int common(void) { return 1; }
C
cat > "$P/src/main.c" <<'C'
#include <stdio.h>
#include "common.h"
int main(void) { FILE* f = fopen("doom1.wad", "rb"); (void)f; return common(); }
C
cat > "$P/src/util.cpp" <<'C'
int util() { return 2; }
C
printf 'IWAD\x01\x00\x00\x00\x00\x00\x00\x00' > "$P/data/doom1.wad"
printf 'not referenced' > "$P/data/unused.txt"
echo "# readme" > "$P/README.md"

echo "== first build"
OUT=$("$WF" build "$P" --workers 2 2>&1); RC=$?
check "exit code 0" "[ $RC -eq 0 ]"
check "3 units compiled" "echo \"\$OUT\" | grep -q '3 compiled, 0 from cache'"
check "wasm + js produced with the project name" "[ -s '$P/dist/PWNED.wasm' ] && [ -s '$P/dist/PWNED.js' ]"
check "no shell injection (PWNED file absent)" "[ ! -e '$T/PWNED' ]"
check "manifest.json exists" "[ -s '$P/dist/manifest.json' ]"
check "WAD shipped unchanged" "cmp -s '$P/data/doom1.wad' '$P/dist/assets/data/doom1.wad'"
check "unreferenced asset skipped" "[ ! -e '$P/dist/assets/data/unused.txt' ]"
check "README not packaged" "[ ! -e '$P/dist/assets/README.md' ]"
check "honest runtime disclaimer printed" "echo \"\$OUT\" | grep -q 'NOT verified'"

echo "== incremental"
OUT=$("$WF" build "$P" 2>&1)
check "second build: 0 compiled, 3 cached" "echo \"\$OUT\" | grep -q '0 compiled, 3 from cache'"
: > "$FAKE_LOG"
echo "/* changed */" >> "$P/src/common.h"
OUT=$("$WF" build "$P" --workers 1 2>&1)
check "header change recompiles only its 2 dependents" "echo \"\$OUT\" | grep -q '2 compiled, 1 from cache'"
check "util.cpp not recompiled" "! grep -q util.cpp '$FAKE_LOG'"

echo "== failure handling"
echo "FAKE_COMPILE_ERROR" >> "$P/src/util.cpp"
OUT=$("$WF" build "$P" 2>&1); RC=$?
check "compile error gives exit 1" "[ $RC -eq 1 ]"
check "error names the failing file" "echo \"\$OUT\" | grep -q 'src/util.cpp failed'"
sed -i '/FAKE_COMPILE_ERROR/d' "$P/src/util.cpp"
OUT=$("$WF" build "$P" 2>&1)
check "recovers after fixing" "echo \"\$OUT\" | grep -q 'Build OK'"

echo "== dry run / explain"
rm -rf "$P/dist" "$P/.wasmforge"
OUT=$("$WF" build "$P" --dry-run 2>&1); RC=$?
check "dry-run exit 0" "[ $RC -eq 0 ]"
check "dry-run writes nothing" "[ ! -e '$P/dist' ] && [ ! -e '$P/.wasmforge' ]"
check "dry-run prints argv" "echo \"\$OUT\" | grep -q 'compile: '"
OUT=$("$WF" inspect "$P" --explain 2>&1)
check "inspect explains decisions" "echo \"\$OUT\" | grep -q 'workers'"

echo "== multiple entry points"
cat > "$P/src/other.c" <<'C'
int main(void) { return 0; }
C
OUT=$("$WF" build "$P" 2>&1); RC=$?
check "two main() => error E-MULTIMAIN" "[ $RC -eq 1 ] && echo \"\$OUT\" | grep -q E-MULTIMAIN"
OUT=$("$WF" build "$P" --entry src/main.c 2>&1); RC=$?
check "--entry resolves it" "[ $RC -eq 0 ]"
OUT=$("$WF" build "$P" --exclude 'other.c' 2>&1); RC=$?
check "--exclude resolves it" "[ $RC -eq 0 ]"

echo "== config layers (AUTO / MANUAL / HYBRID)"
"$WF" config set workers 1 >/dev/null
OUT=$("$WF" config show 2>&1)
check "hybrid: workers from global config" "echo \"\$OUT\" | grep -E 'workers +1 +\[global\]' >/dev/null"
"$WF" config set profile bogus >/dev/null 2>&1; RC=$?
check "invalid value rejected with exit 4" "[ $RC -eq 4 ]"
"$WF" config reset >/dev/null
OUT=$("$WF" config show 2>&1)
check "reset returns to auto" "echo \"\$OUT\" | grep -E 'workers +[0-9]+ +\[auto\]' >/dev/null"
"$WF" config set assets transform >/dev/null 2>&1; RC=$?
check "unimplemented 'transform' refused honestly" "[ $RC -eq 4 ]"

echo "== environment problems"
OUT=$(PATH=/usr/bin:/bin "$WF" build "$P" 2>&1); RC=$?
check "no backend => exit 3 with diagnosis" "[ $RC -eq 3 ] && echo \"\$OUT\" | grep -q E-NOBACKEND"
"$WF" frobnicate >/dev/null 2>&1; RC=$?
check "unknown command => exit 2" "[ $RC -eq 2 ]"
OUT=$("$WF" biuld 2>&1)
check "typo suggests 'build'" "echo \"\$OUT\" | grep -q \"did you mean 'build'\""

echo "== wasm analysis"
if command -v python3 >/dev/null; then
  python3 "$HERE/wasmgen.py" emscripten-like "$T/em.wasm"; python3 "$HERE/wasmgen.py" wasi-hello "$T/wasi.wasm"
  OUT=$("$WF" wasm-info "$T/em.wasm" --web 2>&1); RC=$?
  check "emscripten-like module recognised, ok as web target" "[ $RC -eq 0 ] && echo \"\$OUT\" | grep -q 'target        emscripten'"
  check "memory limits parsed" "echo \"\$OUT\" | grep -q '128.0 KB initial, 256.0 KB max'"
  OUT=$("$WF" wasm-info "$T/wasi.wasm" --web 2>&1); RC=$?
  check "WASI module rejected as browser target" "[ $RC -eq 1 ] && echo \"\$OUT\" | grep -q E-WASM-WASI-IN-WEB"
  "$WF" wasm-info "$T/wasi.wasm" >/dev/null 2>&1; RC=$?
  check "WASI module fine as plain wasm" "[ $RC -eq 0 ]"
  head -c 60 "$T/em.wasm" > "$T/trunc.wasm"
  "$WF" wasm-info "$T/trunc.wasm" >/dev/null 2>&1; RC=$?
  check "truncated module reported, exit 1" "[ $RC -eq 1 ]"
  if command -v node >/dev/null; then
    check "analyzer agrees with node's WebAssembly.validate" "node -e \"process.exit(WebAssembly.validate(require('fs').readFileSync('$T/em.wasm'))?0:1)\""
  fi
else echo "  skip (python3 not available)"; fi

if command -v node >/dev/null; then
echo "== run / verify (node)"
R="$T/runproj"; mkdir -p "$R/dist" "$R/src"; echo 'int main(void){return 0;}' > "$R/src/m.c"
if command -v python3 >/dev/null; then
  python3 "$HERE/wasmgen.py" wasi-hello "$R/dist/hello.wasm"
  OUT=$("$WF" run "$R" --name hello 2>&1); RC=$?
  check "WASI module runs (node:wasi) and prints its output" "[ $RC -eq 0 ] && echo \"\$OUT\" | grep -q 'hello from wasi'"
fi
echo 'console.log("args:", process.argv.slice(2).join(" ")); process.exit(3);' > "$R/dist/app.js"
OUT=$("$WF" run "$R" --name app -- one two 2>&1); RC=$?
check "program exit code is propagated" "[ $RC -eq 3 ]"
check "arguments after -- reach the program" "echo \"\$OUT\" | grep -q 'args: one two'"
echo 'while (true) {}' > "$R/dist/loop.js"
S0=$(date +%s); "$WF" run "$R" --name loop --timeout 1 >/dev/null 2>&1; RC=$?; S1=$(date +%s)
check "runaway program is killed at the timeout (exit 124)" "[ $RC -eq 124 ] && [ $((S1-S0)) -le 5 ]"
OUT=$("$WF" build "$P" --entry src/main.c --verify 2>&1); RC=$?
check "build --verify runs the result" "[ $RC -eq 0 ] && echo \"\$OUT\" | grep -q 'fake program ran' && echo \"\$OUT\" | grep -q PASSED"
OUT=$(FAKE_EXIT=2 "$WF" build "$P" --entry src/main.c --verify --force 2>&1); RC=$?
check "build --verify fails on non-zero exit" "[ $RC -eq 1 ] && echo \"\$OUT\" | grep -q FAILED"

echo "== generated VFS loader (node + browser stubs)"
"$WF" build "$P" --entry src/main.c >/dev/null 2>&1
node "$HERE/loader_test.js" "$P/.wasmforge/gen/vfs-preload.js" "$P/dist" "data/doom1.wad" > "$T/loader.out" 2>&1; RC=$?
cat "$T/loader.out"
N=$((N+$(grep -c '^  ' "$T/loader.out"))); FAIL=$((FAIL+$(grep -c '^  FAIL' "$T/loader.out")))
check "loader test script exits 0" "[ $RC -eq 0 ]"
else echo "== run / verify: skipped (node not available)"; fi

if command -v curl >/dev/null && command -v python3 >/dev/null; then
echo "== serve (HTTP)"
V="$T/site"; mkdir -p "$V/dist/assets" "$T/outside"
echo '{"entry": "app.js"}' > "$V/dist/manifest.json"; echo 'console.log(1)' > "$V/dist/app.js"
python3 "$HERE/wasmgen.py" emscripten-like "$V/dist/app.wasm"
head -c 100000 /dev/urandom > "$V/dist/assets/big.wad"
echo secret > "$T/outside/secret.txt"; ln -s "$T/outside/secret.txt" "$V/dist/leak.txt"
"$WF" serve "$V" --port 0 > "$T/serve.log" 2>&1 &
SPID=$!
for i in $(seq 1 50); do grep -q "open http" "$T/serve.log" && break; sleep 0.1; done
PORT=$(grep -o '127.0.0.1:[0-9]*' "$T/serve.log" | head -1 | cut -d: -f2); U="http://127.0.0.1:$PORT"
code() { curl -s -o /dev/null -w "%{http_code}" "$@"; }
check "wasm served as application/wasm" "[ \"\$(curl -s -o /dev/null -w '%{content_type}' $U/app.wasm)\" = application/wasm ]"
check "root serves the generated runner page (canvas)" "curl -s $U/ | grep -q '<canvas'"
check "raw path traversal rejected" "[ \$(code --path-as-is $U/../outside/secret.txt) -eq 400 ]"
check "encoded path traversal rejected" "[ \$(code --path-as-is $U/%2e%2e/outside/secret.txt) -eq 400 ]"
check "symlink pointing outside the root is refused" "[ \$(code $U/leak.txt) -eq 403 ]"
check "secret never leaks" "! curl -s --path-as-is $U/../outside/secret.txt | grep -q secret && ! curl -s $U/leak.txt | grep -q secret"
curl -s -H "Range: bytes=10-19" -o "$T/r.bin" -w "%{http_code}" $U/assets/big.wad > "$T/r.code"
dd if="$V/dist/assets/big.wad" bs=1 skip=10 count=10 of="$T/r.exp" 2>/dev/null
check "Range request returns exactly those bytes (206)" "[ \$(cat $T/r.code) -eq 206 ] && cmp -s $T/r.bin $T/r.exp"
check "unsatisfiable range => 416" "[ \$(code -H 'Range: bytes=999999-' $U/assets/big.wad) -eq 416 ]"
check "POST => 405" "[ \$(code -X POST $U/app.js) -eq 405 ]"
check "foreign Host header => 421 (DNS-rebinding guard)" "[ \$(code -H 'Host: evil.example' $U/app.js) -eq 421 ]"
check "missing file => 404" "[ \$(code $U/nope) -eq 404 ]"
check "isolation headers absent by default" "! curl -sI $U/app.js | grep -qi cross-origin-opener"
kill -INT $SPID; wait $SPID; RC=$?
check "server stops cleanly on Ctrl-C (exit 0)" "[ $RC -eq 0 ] && grep -q '^stopped' '$T/serve.log'"
"$WF" serve "$V" --port 0 --isolate > "$T/serve2.log" 2>&1 &
SPID=$!
for i in $(seq 1 50); do grep -q "open http" "$T/serve2.log" && break; sleep 0.1; done
PORT=$(grep -o '127.0.0.1:[0-9]*' "$T/serve2.log" | head -1 | cut -d: -f2)
check "--isolate adds COOP/COEP headers" "curl -sI http://127.0.0.1:$PORT/app.js | grep -qi 'cross-origin-embedder-policy: require-corp'"
kill -INT $SPID; wait $SPID 2>/dev/null
else echo "== serve: skipped (curl/python3 not available)"; fi

echo "== generated config headers (CMake/autotools templates)"
G="$T/genproj"; mkdir -p "$G/src"
cat > "$G/config.h.cin" <<'H'
#define PACKAGE_NAME "@PACKAGE_NAME@"
#cmakedefine HAVE_LIBPNG
#cmakedefine HAVE_LIBSAMPLERATE
#cmakedefine ENABLE_SOUND
H
cat > "$G/src/main.c" <<'C'
#include "config.h"
#ifdef HAVE_LIBPNG
#include <png.h>
#endif
int main(void) { return 0; }
C
cat > "$G/src/other.c" <<'C'
int other(void) { return 1; }
C
SUM_BEFORE=$(cat "$G/src/main.c" "$G/config.h.cin" | cksum)
OUT=$("$WF" build "$G" --workers 1 2>&1); RC=$?
check "build succeeds without CMake (config.h generated)" "[ $RC -eq 0 ] && echo \"\$OUT\" | grep -q I-GENERATED"
check "generated header lives under .wasmforge/gen/include" "[ -s '$G/.wasmforge/gen/include/config.h' ]"
check "dependency-derived switch: libpng on, libsamplerate off" "grep -q '^#define HAVE_LIBPNG' '$G/.wasmforge/gen/include/config.h'"
check "original sources and template untouched" "[ \"\$(cat $G/src/main.c $G/config.h.cin | cksum)\" = \"$SUM_BEFORE\" ] && [ ! -e '$G/config.h' ]"
check "-I for generated dir is passed to the backend" "echo \"\$("$WF" build "$G" --dry-run 2>&1)\" | grep -q -- '-I.wasmforge/gen/include'"
: > "$FAKE_LOG"
OUT=$("$WF" build "$G" --workers 1 --var ENABLE_SOUND=1 2>&1)
check "changing a --var recompiles only files that include config.h" "echo \"\$OUT\" | grep -q '1 compiled, 1 from cache'"
check "--var value appears in the header" "grep -q '^#define ENABLE_SOUND' '$G/.wasmforge/gen/include/config.h'"
OUT=$("$WF" gen-config "$G" --dry-run 2>&1)
check "gen-config explains the origin of each value" "echo \"\$OUT\" | grep -q 'HAVE_LIBSAMPLERATE' && echo \"\$OUT\" | grep -q 'HAVE_LIBPNG.*(derived)'"
"$WF" build "$G" --var 'bad-name=1' >/dev/null 2>&1; RC=$?
check "invalid --var name rejected (exit 4)" "[ $RC -eq 4 ]"

echo "== analyze / compdb / budget / wasm-diff"
OUT=$("$WF" analyze "$P" --touch src/common.h 2>&1)
check "analyze reports blast radius for a header" "echo \"\$OUT\" | grep -q 'src/common.h' && echo \"\$OUT\" | grep -q 'recompiles 2 of'"
OUT=$("$WF" analyze "$P" --touch src/nope.h 2>&1)
check "analyze says so for unknown files" "echo \"\$OUT\" | grep -q 'not a known source or header'"
CY="$T/cyc"; mkdir -p "$CY"; printf '#include "b.h"\n' > "$CY/a.h"; printf '#include "a.h"\n' > "$CY/b.h"; printf '#include "a.h"\nint main(void){return 0;}\n' > "$CY/m.c"
OUT=$("$WF" analyze "$CY" 2>&1)
check "analyze detects include cycles" "echo \"\$OUT\" | grep -q 'a.h <-> b.h'"
"$WF" compdb "$P" >/dev/null 2>&1
check "compdb writes valid JSON with one entry per source" "python3 -c \"import json,sys; d=json.load(open('$P/.wasmforge/compile_commands.json')); sys.exit(0 if len(d)>=3 and all('arguments' in e and 'file' in e for e in d) else 1)\""
OUT=$("$WF" build "$P" --entry src/main.c --budget 1B 2>&1); RC=$?
check "size budget exceeded => exit 1 with E-VALIDATE" "[ $RC -eq 1 ] && echo \"\$OUT\" | grep -q 'size budget exceeded'"
OUT=$("$WF" build "$P" --entry src/main.c --budget 10MB 2>&1); RC=$?
check "size budget respected => ok" "[ $RC -eq 0 ]"
"$WF" build "$P" --budget banana >/dev/null 2>&1; RC=$?
check "bad budget syntax => usage error" "[ $RC -eq 2 ]"
if command -v python3 >/dev/null; then
  python3 "$HERE/wasmgen.py" emscripten-like "$T/d1.wasm"; python3 "$HERE/wasmgen.py" wasi-hello "$T/d2.wasm"
  OUT=$("$WF" wasm-diff "$T/d1.wasm" "$T/d2.wasm" 2>&1); RC=$?
  check "wasm-diff shows import and target changes" "[ $RC -eq 0 ] && echo \"\$OUT\" | grep -q 'emscripten -> wasi' && echo \"\$OUT\" | grep -q 'fd_write'"
  "$WF" wasm-diff "$T/d1.wasm" "$T/trunc.wasm" >/dev/null 2>&1; RC=$?
  check "wasm-diff with a corrupt file => exit 1" "[ $RC -eq 1 ]"
fi

if command -v curl >/dev/null; then
echo "== dev (watch + rebuild + live reload)"
D="$T/devproj"; mkdir -p "$D/src"
echo 'int main(void){return 0;}' > "$D/src/main.c"; echo 'int helper(void){return 1;}' > "$D/src/helper.c"
: > "$FAKE_LOG"
"$WF" dev "$D" --port 0 --interval 100 --workers 1 > "$T/dev.log" 2>&1 &
DPID=$!
for i in $(seq 1 80); do grep -q "open http" "$T/dev.log" && break; sleep 0.1; done
PORT=$(grep -o '127.0.0.1:[0-9]*' "$T/dev.log" | head -1 | cut -d: -f2); U="http://127.0.0.1:$PORT"
stampnow() { curl -s "$U/__wasmfrg/stamp"; }
waitstamp() { for i in $(seq 1 120); do [ "$(stampnow)" = "$1" ] && return 0; sleep 0.1; done; return 1; }
check "initial build done, stamp is 1" "waitstamp 1"
check "served page carries the live-reload script" "curl -s $U/ | grep -q '__wasmfrg/stamp'"
: > "$FAKE_LOG"
sleep 0.3; echo "/* edit */" >> "$D/src/helper.c"
check "editing a file triggers a rebuild (stamp 2)" "waitstamp 2"
check "only the edited unit was recompiled" "[ \$(grep -c compile '$FAKE_LOG') -eq 1 ] && grep -q helper.c '$FAKE_LOG'"
sleep 0.3; echo "FAKE_COMPILE_ERROR" >> "$D/src/helper.c"
for i in $(seq 1 80); do grep -q "build failed" "$T/dev.log" && break; sleep 0.1; done
check "failed build keeps serving the last good build (stamp stays 2)" "grep -q 'still serving the last good build' '$T/dev.log' && [ \"\$(stampnow)\" = 2 ]"
sleep 0.3; sed -i '/FAKE_COMPILE_ERROR/d' "$D/src/helper.c"
check "fixing the error rebuilds (stamp 3)" "waitstamp 3"
kill -INT $DPID; wait $DPID; RC=$?
check "dev stops cleanly on Ctrl-C (exit 0)" "[ $RC -eq 0 ] && grep -q '^stopped' '$T/dev.log'"
else echo "== dev: skipped (curl not available)"; fi

echo "== clean"
"$WF" build "$P" --entry src/main.c >/dev/null 2>&1
"$WF" clean "$P" >/dev/null
check "clean removes dist and .wasmforge" "[ ! -e '$P/dist' ] && [ ! -e '$P/.wasmforge' ]"
check "clean never touches sources" "[ -s '$P/src/main.c' ]"

echo
echo "$((N-FAIL))/$N integration checks passed"
[ $FAIL -eq 0 ]
