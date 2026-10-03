/**
 * smoke.js — Node SDK end-to-end smoke test.
 *
 * Starts a real daemon, installs a Legado rule file that targets the Python
 * fixture site, and reads a book through the JS API. Run with:
 *
 *     node sdk/node/test/smoke.js
 *
 * Requires `make` to have produced build/xbridged.
 */
'use strict';

const path = require('path');
const fs = require('fs');
const os = require('os');
const { spawn } = require('child_process');

const { Bridge, BridgeError, FT } = require('../xbridge');

const ROOT = path.resolve(__dirname, '..', '..', '..');
const DAEMON = path.join(ROOT, 'build', 'xbridged');

let passed = 0;
let failed = 0;

function check(cond, msg) {
  if (cond) { passed++; process.stdout.write(`    ok   ${msg}\n`); }
  else { failed++; process.stdout.write(`    FAIL ${msg}\n`); }
}

function eq(actual, expected, msg) {
  check(actual === expected, `${msg} (got ${JSON.stringify(actual)}, want ${JSON.stringify(expected)})`);
}

/** Start tests/fixtures/content_site.py and return {base, stop}. */
function startFixtureSite() {
  return new Promise((resolve, reject) => {
    const proc = spawn('python3', [path.join(ROOT, 'tests', 'fixtures', 'content_site.py'), '0'], {
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let out = '';
    const timer = setTimeout(() => { proc.kill(); reject(new Error('fixture site did not start')); }, 10000);
    proc.stdout.on('data', (buf) => {
      out += buf.toString();
      const m = out.match(/http:\/\/127\.0\.0\.1:\d+/);
      if (m) {
        clearTimeout(timer);
        resolve({ base: m[0], stop: () => proc.kill() });
      }
    });
    proc.once('error', reject);
  });
}

/* -- frame routing (client-side, no daemon) -------------------------------- */

/** A Bridge whose socket is a stub, so frames can be injected by hand. */
function fakeBridge() {
  const b = new Bridge('fake://', 1000);
  b._sock = { write: () => true, end() {}, destroy() {} };
  b._closed = false;
  return b;
}

/** Ids of the requests currently awaiting a frame, oldest first. */
function pendingIds(b) {
  return [...b._requests.keys()];
}

async function frameRouting() {
  console.log('\nframe routing (client-side, no daemon)');

  /* The daemon answers pipelined requests in completion order. */
  {
    const b = fakeBridge();
    const first = b.call('x.one');
    const second = b.call('x.two');
    const [id1, id2] = pendingIds(b);
    b._deliver({ type: FT.RESPONSE, payload: { id: id2, ok: true, result: 'two' } });
    b._deliver({ type: FT.RESPONSE, payload: { id: id1, ok: true, result: 'one' } });
    eq(await first, 'one', 'out-of-order response routed to the first call');
    eq(await second, 'two', 'out-of-order response routed to the second call');
  }

  /* A stream in flight must not swallow a concurrent call's response. */
  {
    const b = fakeBridge();
    const it = b.stream('x.stream')[Symbol.asyncIterator]();
    const pending = it.next();
    const streamId = pendingIds(b)[0];
    const ping = b.call('x.ping');
    const pingId = pendingIds(b).find((id) => id !== streamId);
    b._deliver({ type: FT.STREAM_CHUNK, payload: { id: streamId, seq: 0, chunk: { n: 1 } } });
    b._deliver({ type: FT.RESPONSE, payload: { id: pingId, ok: true, result: 'pong' } });
    b._deliver({ type: FT.STREAM_CHUNK, payload: { id: streamId, seq: 1, chunk: { n: 2 } } });
    b._deliver({ type: FT.STREAM_END, payload: { id: streamId, done: true, count: 2 } });
    eq((await pending).value.n, 1, 'first stream chunk delivered');
    eq(await ping, 'pong', 'a call behind a stream still resolves');
    eq((await it.next()).value.n, 2, 'second stream chunk delivered in order');
    eq((await it.next()).done, true, 'stream ends');
  }

  /* After a cancel the daemon suppresses the rest of the stream: any late
   * frame must be dropped, not handed to the next call. */
  {
    const b = fakeBridge();
    const it = b.stream('x.stream')[Symbol.asyncIterator]();
    const pending = it.next();
    const streamId = pendingIds(b)[0];
    b._deliver({ type: FT.STREAM_CHUNK, payload: { id: streamId, seq: 0, chunk: { n: 1 } } });
    eq((await pending).value.n, 1, 'chunk delivered before the cancel');
    b.cancel(streamId);
    b._deliver({ type: FT.STREAM_ERR, payload: { id: streamId, error: { code: -32002, message: 'cancelled' } } });
    let code = 0;
    try { await it.next(); } catch (err) { code = err.code; }
    eq(code, -32002, 'cancelled stream reports -32002');
    b._deliver({ type: FT.STREAM_CHUNK, payload: { id: streamId, seq: 1, chunk: { n: 2 } } });
    const after = b.call('x.after');
    const afterId = pendingIds(b)[0];
    b._deliver({ type: FT.RESPONSE, payload: { id: afterId, ok: true, result: 'ok' } });
    eq(await after, 'ok', 'late frames for a cancelled stream are dropped');
  }
}

async function main() {
  if (!fs.existsSync(DAEMON)) {
    console.error(`daemon not found: ${DAEMON} (run \`make\` first)`);
    process.exit(2);
  }

  await frameRouting();

  const site = await startFixtureSite();
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'xbridge-node-'));

  const rules = {
    sourceName: 'Fixture Site',
    sourceUrl: site.base,
    bookSourceUrl: site.base,
    searchUrl: `${site.base}/?q={{key}}&page={{page}}`,
    ruleSearch: {
      bookList: 'div.book',
      name: 'h3.title a@text',
      author: 'span.author@text',
      coverUrl: 'img.cover@src',
      bookUrl: 'h3.title a@href',
      nextPage: 'a.next@href',
    },
    ruleBookInfo: {
      name: 'h1.bookname@text',
      author: 'span.writer@text',
      coverUrl: 'img.cover@src',
      intro: 'div.intro@text',
    },
    ruleToc: { chapterList: 'ul.chapters li', chapterName: 'a@text', chapterUrl: 'a@href' },
    ruleContent: { content: 'div#content@html', nextContentUrl: 'a.next@href' },
  };
  const rulesPath = path.join(tmp, 'rules.json');
  fs.writeFileSync(rulesPath, JSON.stringify(rules));

    // No --quiet: the SDK reads the ready line from stdout.
  const bridge = await Bridge.spawn(DAEMON, { dataDir: path.join(tmp, 'data') });
  const src = 'legado/rules.json';

  try {
    console.log(`fixture site: ${site.base}`);
    console.log(`daemon      : ${bridge.endpoint}\n`);

    console.log('handshake');
    eq(bridge.hello.protocol, 'XBP/1', 'protocol version');
    check(bridge.hello.pid > 0, 'ack carries the daemon pid');
    eq(bridge.hello.capabilities.formats, 10, 'ten ecosystems');

    console.log('\nping and metrics');
    const ms = await bridge.ping();
    check(ms < 100, `ping in ${ms.toFixed(2)} ms`);
    const metrics = await bridge.metrics();
    check(metrics.server.requests_total >= 1, 'requests counted');

    console.log('\nformat registry');
    const formats = await bridge.formats();
    eq(formats.length, 10, 'format.list length');
    check(formats.every((f) => f.platforms.length === 5), 'every format covers 5 platform families');

    console.log('\ndetect and install');
    const detected = await bridge.detect(rulesPath);
    eq(detected.format, 'legado', 'detected format');
    const installed = await bridge.install({ path: rulesPath });
    eq(installed.format_id, 'legado', 'installed format');
    const exts = await bridge.extensions();
    check(exts.some((e) => e.id === installed.id), 'extension listed');

    console.log('\nsearch, detail, content');
    const found = await bridge.search(src, 'solo');
    eq(found.list.length, 1, 'one match');
    eq(found.list[0].name, 'Solo Leveling', 'book name');
    eq(found.list[0].author, 'Chugong', 'author');

    const detail = await bridge.detail(src, found.list[0].url);
    eq(detail.name, 'Solo Leveling', 'detail name');
    eq(detail.episodes.length, 3, 'chapter count');
    check(detail.description.includes('portals'), 'description extracted');

    const chapter = await bridge.novelContent(src, detail.episodes[0].url);
    check(chapter.length > 40, `chapter length ${chapter.length}`);
    check(chapter.content.includes('Paragraph one'), 'chapter body parsed');
    check(!chapter.content.includes('&amp;'), 'entities decoded');
    check(Boolean(chapter.next_url), 'next chapter link');

    console.log('\nstreaming');
    let chunks = 0;
    for await (const chunk of bridge.stream('source.search', { source_id: src, query: 'solo' })) {
      chunks++;
      check(chunk !== undefined, 'chunk is a value');
      if (chunks >= 3) break;
    }
    check(chunks >= 2, `stream yielded ${chunks} chunks`);

    console.log('\nerrors');
    try {
      await bridge.search('legado/nope.json', 'x');
      check(false, 'unknown source rejected');
    } catch (err) {
      check(err instanceof BridgeError, 'BridgeError thrown');
      eq(err.code, -32003, 'not-found code');
      check(err.notFound, 'notFound helper');
    }

    console.log('\nconcurrency');
    const results = await Promise.all(
      Array.from({ length: 8 }, () => bridge.ping()),
    );
    check(results.every((r) => r < 1000), '8 concurrent pings answered');
  } finally {
    await bridge.close();
    site.stop();
    fs.rmSync(tmp, { recursive: true, force: true });
  }

  console.log(`\nnode sdk: ${passed} passed, ${failed} failed`);
  process.exit(failed === 0 ? 0 : 1);
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
