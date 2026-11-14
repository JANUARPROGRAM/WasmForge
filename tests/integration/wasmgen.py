#!/usr/bin/env python3
"""Hand-assembles tiny WebAssembly modules for tests (no toolchain needed).
Usage: wasmgen.py <kind> <output.wasm>   kinds: wasi-hello | emscripten-like | wasi-import-only"""
import sys

def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)

def name(s):
    b = s.encode()
    return leb(len(b)) + b

def vec(items):
    return leb(len(items)) + b"".join(items)

def section(sid, payload):
    return bytes([sid]) + leb(len(payload)) + payload

HEADER = b"\x00asm\x01\x00\x00\x00"
I32 = 0x7F

def functype(params, results):
    return b"\x60" + vec([bytes([p]) for p in params]) + vec([bytes([r]) for r in results])

def wasi_hello():
    msg = b"hello from wasi\n"
    types = section(1, vec([functype([I32] * 4, [I32]), functype([], [])]))
    imports = section(2, vec([name("wasi_snapshot_preview1") + name("fd_write") + b"\x00" + leb(0)]))
    funcs = section(3, vec([leb(1)]))
    mem = section(5, vec([b"\x00" + leb(1)]))
    exports = section(7, vec([name("memory") + b"\x02" + leb(0), name("_start") + b"\x00" + leb(1)]))
    body = b"\x00" + b"\x41\x01" + b"\x41\x00" + b"\x41\x01" + b"\x41\x14" + b"\x10\x00" + b"\x1a" + b"\x0b"
    code = section(10, vec([leb(len(body)) + body]))
    iov = (8).to_bytes(4, "little") + len(msg).to_bytes(4, "little")
    data = section(11, vec([b"\x00" + b"\x41\x00\x0b" + leb(len(iov) + len(msg)) + iov + msg]))
    return HEADER + types + imports + funcs + mem + exports + code + data

def emscripten_like():
    types = section(1, vec([functype([I32], [I32]), functype([], [])]))
    imports = section(2, vec([name("env") + name("emscripten_resize_heap") + b"\x00" + leb(0)]))
    funcs = section(3, vec([leb(1)]))
    mem = section(5, vec([b"\x01" + leb(2) + leb(4)]))  # min 2 pages, max 4
    exports = section(7, vec([name("memory") + b"\x02" + leb(0), name("main") + b"\x00" + leb(1)]))
    body = b"\x00\x0b"
    code = section(10, vec([leb(len(body)) + body]))
    data = section(11, vec([b"\x00\x41\x00\x0b" + leb(4) + b"abcd"]))
    return HEADER + types + imports + funcs + mem + exports + code + data

def wasi_import_only():
    types = section(1, vec([functype([I32] * 4, [I32])]))
    imports = section(2, vec([name("wasi_snapshot_preview1") + name("fd_write") + b"\x00" + leb(0)]))
    return HEADER + types + imports

if __name__ == "__main__":
    kinds = {"wasi-hello": wasi_hello, "emscripten-like": emscripten_like, "wasi-import-only": wasi_import_only}
    open(sys.argv[2], "wb").write(kinds[sys.argv[1]]())
