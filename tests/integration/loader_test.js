// Verifies the generated virtual-filesystem loader in both environments (Node and browser) using stubs.
// usage: node loader_test.js <vfs-preload.js> <dist dir> <asset rel path>
const fs = require('fs'), path = require('path'), vm = require('vm');
const [loaderPath, distDir, rel] = process.argv.slice(2);
const code = fs.readFileSync(loaderPath, 'utf8');
const expected = fs.readFileSync(path.join(distDir, 'assets', rel));

function makeContext(extra) {
  const log = { written: {}, dirs: [], deps: 0, removed: 0, errors: [] };
  const ctx = Object.assign({
    Module: {},
    FS: { mkdirTree: (d) => log.dirs.push(d), writeFile: (p, b) => { log.written[p] = Buffer.from(b); } },
    addRunDependency: () => { log.deps++; },
    removeRunDependency: () => { log.removed++; },
    err: (m) => log.errors.push(m),
  }, extra);
  return { ctx: vm.createContext(ctx), log };
}
async function until(cond, ms = 2000) {
  const t0 = Date.now();
  while (!cond()) { if (Date.now() - t0 > ms) throw new Error('timeout'); await new Promise((r) => setTimeout(r, 10)); }
}
(async () => {
  let failed = 0;
  const check = (name, ok) => { console.log((ok ? '  ok   ' : '  FAIL ') + name); if (!ok) failed++; };

  // Node: reads from disk next to the script.
  let { ctx, log } = makeContext({ process, require, __dirname: distDir });
  vm.runInContext(code, ctx);
  check('registers a preRun hook', ctx.Module.preRun.length === 1);
  ctx.Module.preRun[0]();
  await until(() => log.removed === 1);
  check('node: bytes written unchanged', Buffer.compare(log.written['/' + rel], expected) === 0);
  check('node: parent directory created', log.dirs.includes('/' + path.posix.dirname(rel)) || path.posix.dirname(rel) === '.');
  check('node: run dependency balanced', log.deps === 1 && log.removed === 1 && log.errors.length === 0);

  // Browser: no `process`/`require`, uses fetch relative to the page.
  const urls = [];
  ({ ctx, log } = makeContext({ fetch: (u) => { urls.push(u); return Promise.resolve({ ok: true, arrayBuffer: () => Promise.resolve(expected.buffer.slice(expected.byteOffset, expected.byteOffset + expected.length)) }); } }));
  vm.runInContext(code, ctx);
  ctx.Module.preRun[0]();
  await until(() => log.removed === 1);
  check('browser: fetches assets/<path>', urls[0] === 'assets/' + rel);
  check('browser: bytes written unchanged', Buffer.compare(log.written['/' + rel], expected) === 0);

  // Failure is reported, not swallowed.
  ({ ctx, log } = makeContext({ fetch: () => Promise.resolve({ ok: false, status: 404 }) }));
  vm.runInContext(code, ctx);
  ctx.Module.preRun[0]();
  await until(() => log.errors.length === 1);
  check('browser: HTTP error is reported via err()', /404/.test(log.errors[0]) && log.removed === 0);
  process.exit(failed ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
