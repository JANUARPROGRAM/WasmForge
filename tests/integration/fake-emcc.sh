#!/usr/bin/env bash
# TEST DOUBLE - this is NOT Emscripten. It mimics the emcc/em++ command-line contract just enough
# to exercise WasmForge's orchestration: argument construction, caching, scheduling, packaging.
# Objects are "compiled" by copying the source; the "link" step emits a minimal valid .wasm header.
args=("$@")
out=""; compile=0; src=""
for ((i=0;i<${#args[@]};i++)); do
  a="${args[$i]}"
  case "$a" in
    --version) echo "emcc (fake test double) 0.0.0"; exit 0 ;;
    -c) compile=1 ;;
    -o) out="${args[$((i+1))]}" ;;
    -*) ;;
  esac
done
if [ "$compile" = 1 ]; then
  for a in "${args[@]}"; do case "$a" in *.c|*.cpp|*.cc) src="$a" ;; esac; done
  if grep -q FAKE_COMPILE_ERROR "$src" 2>/dev/null; then echo "$src:1:1: error: forced failure" >&2; exit 1; fi
  echo "$FAKE_LOG_PREFIX compile $src" >> "${FAKE_LOG:-/dev/null}"
  cp "$src" "$out"
  exit 0
fi
echo "link -> $out" >> "${FAKE_LOG:-/dev/null}"
mkdir -p "$(dirname "$out")"
base="${out%.*}"
printf '\x00\x61\x73\x6d\x01\x00\x00\x00' > "$base.wasm"
echo "console.log(\"fake program ran\"); process.exit(Number(process.env.FAKE_EXIT||0));" > "$base.js"
exit 0
