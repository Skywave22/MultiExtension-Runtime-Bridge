# Xbridge — one runtime bridge for every extension ecosystem

Xbridge runs extension formats from **ten different platforms** behind **one
API**, on **every desktop and mobile OS**, from a single dependency-free C11
daemon.

| Ecosystem | Manager id | What an extension is | Served by |
|---|---|---|---|
| Mangayomi | `mangayomi` | Dart/JS source + manifest | `js` worker |
| Sora | `sora` | Swift/JS module bundle | `js` worker |
| Aniyomi | `aniyomi` | Kotlin/Java `.jar` (ext-lib) | `jvm` worker |
| CloudStream | `cloudstream` | Kotlin/Java `.jar` | `jvm` worker |
| Kotatsu | `kotatsu` | Kotlin/Java or `.js` parser | `jvm` / `js` worker |
| Legado | `legado` | JSON book-source rules | built-in rule engine |
| LnReader | `lnreader` | JS plugin (`.js`) | `js` worker |
| Tsundoku | `tsundoku` | JS/Mihon-style source | `js` / `jvm` worker |
| iReader | `ireader` | JS source bundle | `js` worker |
| TorrServer Addon | `torrserver` | HTTP JSON-RPC addon | built-in native engine |

Everything is **clean-room**: the reference project was read for its public
contract only, never copied. See `docs/CLEANROOM.md` for the method and
`docs/ANALYSIS.md` for what was learned from it.

---

## Why it is faster

The reference implementation launches a JVM sidecar
(`java -Xms128m -Xmx512m -noverify -jar bridge.jar`) and waits up to **10
seconds**, grepping stderr for a startup string, before it will answer a call.

Xbridge is a ~180 KB native binary that binds a socket and answers in
**1.6 ms**:

| | Reference architecture | Xbridge (measured, this tree) |
|---|---|---|
| Cold start | JVM launch, 10 s readiness timeout, silent on failure | **1.6 ms** to an accepting socket |
| Handshake | grep stderr for a string | real `HELLO`/`HELLO_ACK` with capabilities |
| Request round-trip | Java heap, JSON over stdin/stdout | **0.079 ms** p50, 0.13 ms p99 |
| Streaming | `status:'partial'` chunks dropped by the host | `STREAM_CHUNK`/`STREAM_END`, client-controlled |
| Cancellation | client-side `TimeoutException`, then a best-effort cancel | daemon-side deadlines, `CANCEL` per request id, `-32002` propagation |
| Repeat reads | one fetch per call | L1 cache + single-flight coalescing (**6×** on a warm read) |
| Resident memory | 128 MiB minimum heap | **1.0 MiB** |

Full methodology and raw numbers: `docs/PERFORMANCE.md`. Micro-benchmarks:
`make bench`.

---

## Quick start

```sh
make                    # build/build/xbridged
make check              # unit + protocol + end-to-end + SDK tests
make bench              # micro-benchmarks
```

Start a daemon and drive it:

```sh
./build/xbridged --endpoint unix:///tmp/xbridge.sock --data-dir ~/.local/share/xbridge
```

```python
from xbridge import Bridge

with Bridge.spawn("./build/xbridged") as bridge:
    bridge.install("legado/book-source.json")
    results = bridge.search("legado/book-source.json", "solo leveling")
    detail  = bridge.detail("legado/book-source.json", results["list"][0]["url"])
    chapter = bridge.novel_content("legado/book-source.json",
                                   detail["episodes"][0]["url"])
    print(chapter["content"][:200])
```

```js
const { Bridge } = require('./sdk/node/xbridge');

const bridge = await Bridge.spawn('./build/xbridged');
await bridge.install('legado/book-source.json');
const found = await bridge.search('legado/book-source.json', 'solo leveling');
console.log(found.list[0].name);
await bridge.close();
```

Or embed the whole thing in-process — same dispatcher, no socket, for iOS and
for hosts that cannot spawn a process (`core/include/bridge/abi.h`):

```c
xb_abi_handle *h = xb_abi_create("{\"cache_entries\":4096}");
xb_abi_str out = NULL;
xb_abi_request req = { .method = "source.search",
                       .params_json = "{\"source_id\":\"legado/x.json\",\"query\":\"solo\"}" };
if (xb_abi_call(h, &req, &out) == 0) puts(out);
xb_abi_destroy(h);
```

---

## Repository layout

```
core/include/bridge/     public headers — one per subsystem, plus abi.h
core/src/                the daemon: framing, dispatch, cache, engines,
                         HTTP client, HTML parser, rule interpreter, registry
core/formats/*.json      the format registry as data (generated, verified by tests)
sdk/python/xbridge.py    Python client (+ Bridge.spawn lifecycle helper)
sdk/node/xbridge.js      Node client, zero dependencies (+ TypeScript types)
core/workers/js_worker.js  the reference JavaScript engine worker (Node)
tests/unit/              per-module suites (JSON, HTML, cache, util, registry, ABI)
tests/protocol/          XBP/1 conformance over a real socket
tests/e2e/run_e2e.py     the acceptance test: real daemon, real site, real rules
tests/fixtures/          a miniature content site (paginated search, chapters)
tests/bench/             micro-benchmarks
tools/                   format-mirror generator, daemon benchmark
docs/                    PROTOCOL, ARCHITECTURE, METHODS, ANALYSIS, CLEANROOM,
                         PERFORMANCE
```

## Testing

Every claim in this README is enforced by a test that runs on `make check`:

| Suite | What it proves | Result on this tree |
|---|---|---|
| `tests/unit` | JSON, HTML, cache, single-flight, hashing, sockets, artifacts, ABI | 85 tests, 2 878 checks |
| `tests/protocol` | framing, pipelining, concurrency, cancellation, hostile clients | included above |
| `tests/e2e` | install → search → detail → chapter over real HTTP | 84/84 checks |
| `tests/e2e/js_engine.py` | a **second engine**: Node worker hosting a source, deadlines, cancel | 43/43 checks |
| `sdk/python/test_smoke.py` | the Python client, from spawn to retry | 17/17 checks |
| `sdk/node/test/smoke.js` | the Node client, same journey + frame routing | 36/36 checks |

```sh
$ make test
85 tests, 2878 checks, 0 failures
PASS (no leaks)

$ make asan          # AddressSanitizer + UndefinedBehaviorSanitizer
85 tests, 2878 checks, 0 failures
PASS (no leaks)

$ make e2e
e2e: 84/84 checks passed, 0 failed

$ make js-test       # a real Node worker hosting a real source
js engine: 43 passed, 0 failed

$ make node-test && make python-test
node sdk: 36 passed, 0 failed
python sdk: 17 passed, 0 failed
```

`make check` runs the unit, protocol, end-to-end, js-engine and Node suites.

The suites found real bugs during development and they are recorded in the
commit log: a vanished client could kill the daemon with `SIGPIPE`; every
thread was spawned detached so shutdown raced with in-flight work; the HTML
selector parser dropped `.class`/`[attr]` suffixes after a tag name; URL joins
produced double slashes; the Python client unlinked the daemon's socket when a
plain client closed; a restarting daemon would delete its successor's socket;
`deadline_ms` was treated as absolute on one path and relative on another, so
client deadlines were never really enforced; `CANCEL` frames could not match
the engine job they named, so cancellation only looked implemented; the
shutdown wake-up connection could be lost and leave the daemon hanging in
`accept()`; a coalesced waiter was told `-32000` whatever the real reason was;
the Node client could deadlock when responses arrived out of order; and the
POSIX wake-up pair handed back the read end first, so it never signalled; and
the archive sniffer demanded nine bytes for an eight-byte PNG signature, so a
PNG was not classified as a native binary. Each has a regression test now.

## Platforms

The core is plain C11 + libc. Transports, engines and builds adapt per OS:

| | Linux | macOS | Windows | Android | iOS |
|---|---|---|---|---|---|
| Daemon over UDS / pipe | ✅ | ✅ | ✅ named pipe | ✅ | — |
| Daemon over TCP | ✅ | ✅ | ✅ | ✅ | ✅ |
| In-process ABI (`abi.h`) | ✅ | ✅ | ✅ | ✅ NDK | ✅ static lib |
| Built-in rule engine | ✅ | ✅ | ✅ | ✅ | ✅ |
| JVM extensions | external JRE | external JRE | external JRE | `XBRIDGE_JVM_WORKER` | — |
| JS extensions | `core/workers/js_worker.js` (Node) | same | same | same | same |

Anything that needs a runtime Xbridge does not embed is attached through
worker processes, so the bridge never becomes a hard dependency on Node or a
JRE: it reports the engine as `unavailable` and keeps working for everything
else.

## Documentation

- `docs/ANALYSIS.md` — what the reference project does and where it hurts
- `docs/PROTOCOL.md` — the XBP/1 wire protocol, in full
- `docs/ARCHITECTURE.md` — how the daemon is put together, and why
- `docs/METHODS.md` — the unified method surface for all ten ecosystems
- `docs/PERFORMANCE.md` — measured numbers and how to reproduce them
- `docs/CLEANROOM.md` — how this was written without copying anything

## Licence

GPL-3.0 (see `LICENSE`).
