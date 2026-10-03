# Architecture

## The shape of the thing

```
                    ┌──────────────────────────── host application ────────────────────────────┐
                    │  Python SDK      Node SDK      (Dart/Swift: bind to the C ABI)           │
                    └───────┬─────────────┬────────────────────────────────┬─────────────────┘
                            │             │              │                   │
                XBP/1 over UDS / abstract UDS / named pipe / TCP      in-process C ABI
                            │             │              │                   │
                    ┌───────▼─────────────▼──────────────▼───────────────────▼────────────────┐
                    │                            xbridged (this repo)                        │
                    │  server.c    framing, accept loop, task pool, streaming, deadlines      │
                    │  bridge.c    unified method surface, source routing, cache/coalescing   │
                    │  registry.c  the ten ecosystems as a data table                          │
                    │  rule_engine.c  Legado rule interpreter (in-process, no JS runtime)      │
                    └──────┬─────────────────────┬───────────────────────┬─────────────────────┘
                           │ XBP/1 over stdio    │ HTTP/1.1              │ (same process)
                    ┌──────▼──────┐       ┌──────▼───────┐       ┌──────▼──────────┐
                    │ js worker   │       │ jvm worker   │       │ other engines   │
                    │ (Node/Bun/  │       │ (JRE + our   │       │ (contributions  │
                    │  Deno/qjs)  │       │  worker jar) │       │  speak XBP/1)   │
                    └─────────────┘       └──────────────┘       └─────────────────┘
                       shipped:              external:              anything:
                       core/workers/         XBRIDGE_JVM_WORKER     XBRIDGE_*_WORKER
```

One protocol, three relationships: host↔daemon, daemon↔engine, and (in-process)
host↔library. Nothing else is a special case.

## Why a daemon and not a library

Three of the ten ecosystems (Aniyomi, CloudStream, Kotatsu) are JVM artifacts,
and the reference architecture pays for a JVM on every platform — including
desktop sessions that only ever load a Legado JSON rule file. Splitting
"the bridge" from "the runtimes it drives" makes the cost proportional to what
is actually loaded:

- The daemon itself always works. It compiles to ~180 KiB with no dependencies
  and starts in ~2 ms.
- A JS or JVM engine is attached **when a worker binary is configured**
  (`XBRIDGE_JS_WORKER`, `XBRIDGE_JVM_WORKER`) and is reported as `unavailable`
  when it is not. A host that only uses rules never pays for either.
- On iOS, where a second process is not an option, the same dispatcher is
  reachable through the C ABI (`core/include/bridge/abi.h`) instead of a socket.
  A Dart FFI binding and a Swift wrapper are thin translations of that header,
  which is why the ABI — not a language SDK — is the interface that matters for
  embedded hosts.

## Subsystems

| File | Responsibility | Notes |
|---|---|---|
| `core/src/server.c` | accept loop, frame codec, 16 MiB ceiling, task pool, one writer lock per connection, per-connection task registry, deadline propagation, `CANCEL` | A streaming request is terminated only by `STREAM_END`/`STREAM_ERR`, so a truncated stream can never be mistaken for a complete one. |
| `core/src/bridge.c` | the unified method surface, source-id routing, cache key construction, single-flight coalescing, metrics, log ring, format registry access | Handlers are registered with metadata (engine, streamable, cacheable, summary) so routing is data, not a switch. `bridge.methods` introspects this. |
| `core/src/engine.c` | worker supervision: spawn, `HELLO` handshake over stdio, job queue, per-job deadlines, chunk forwarding, restart with backoff, health | An engine that cannot start is reported, not fatal. |
| `core/src/rule_engine.c` | Legado book-source interpreter: URL templating (`{{key}}`, `{{page}}`), field rules (`selector@attr`, `@text`, `@html`, `##regex`), search/detail/toc/content builders | This is why Legado needs no JavaScript runtime: 90 % of real book sources are declarative. |
| `core/src/html.c` | tolerant parser + CSS-lite selection + text/attribute extraction | Tolerant on purpose: extension targets are real-world pages. Script bodies are skipped, nesting and node counts are bounded. |
| `core/src/http.c` | HTTP/1.1 client: chunked, redirects, cookies, gzip-free raw bodies, TLS delegation, URL resolution/encoding | Reports TLS as unavailable rather than failing opaquely. |
| `core/src/cache.c` | TTL + LRU cache and single-flight groups | Bounded by entries and bytes; `bridge.metrics` exposes hits, misses, evictions, coalesced. |
| `core/src/json.c` | parser, DOM on arenas, writer, canonical serialisation, canonical hash | Canonical form is what makes `cache`: keys order-independent. |
| `core/src/registry.c`, `core/formats/*.json` | the ten ecosystems: ids, engines, artifact kinds, identify markers, capabilities, platforms | The JSON mirror is generated from the C table and a unit test fails if they diverge. |
| `core/src/archive.c` | artifact detection by content, not extension: zip listing, markers, confidence + evidence | `format.detect` returns the evidence string, so a misdetection is debuggable. |
| `core/src/proc.c`, `core/src/net.c`, `core/src/util.c`, `core/src/log.c`, `core/src/buf.c` | process spawning, four transports behind one socket-shaped API, portable threads/time/hashing/strings, structured logging, buffers | These are the only files that need `#ifdef _WIN32`, which is deliberate: platform variance is contained. |
| `core/src/abi.c` | the embedding ABI | Same handlers, same error table, no socket. |
| `core/workers/js_worker.js` | the reference JavaScript engine worker | A Node process that speaks XBP/1 on stdio and hosts CommonJS source modules. Written in JS because that is the language the ecosystems it serves are written in, and it is the model for any other worker. |

## How an engine gets attached

```
XBRIDGE_JS_WORKER="node /usr/share/xbridge/js_worker.js {}"  ./build/xbridged
                                  │
                                  ├─ split on whitespace, honouring quotes → argv
                                  ├─ spawn one process per configured worker
                                  └─ HELLO / HELLO_ACK over its stdio
```

The daemon then treats that worker exactly like a client: it writes `REQUEST`
frames and reads `RESPONSE`/`STREAM_*` frames. Anything that speaks XBP/1 can be
an engine — a Bun process, a JVM, a Rust helper — which is why "support every
ecosystem" does not mean "embed every runtime".

`engine.load`/`engine.unload` forward to a worker and register the source in the
daemon's extension table, so the unified `source.*` surface addresses
worker-hosted sources with the same call as a built-in rule file. If a worker
refuses the extension, the install fails at that point rather than at first use.

## Data flow of one request

```
host ──REQUEST{id,method,params,deadline_ms,cache}──▶ server.c
                                                       │ parse, route by method name
                                                       │ cache key = sha256(canonical params)
                                                       │ single-flight join or start
                                                       ▼
                                                   bridge.c handler
                                                       │ resolve source_id → extension entry
                                                       │ merge params + source metadata
                                                       ▼
                                              engine (rule / js / jvm / native)
                                                       │ returns a JSON value, or chunks
                                                       ▼
                              cache store ◀── server.c ──▶ RESPONSE / STREAM_*
```

Cancellation is a path of its own, not a best effort. Every in-flight request is
registered in a per-connection table under the id the client chose, and the
daemon mints a unique engine job id for it. `CANCEL {"id":"a1"}` marks that task
and cancels the engine job by its generated id, so two connections can both use
`"a1"` without interfering, and a late `CANCEL` for a finished request is a
no-op. A worker-hosted job ends the moment the cancel reaches the daemon; a
handler the daemon runs in-process (the rule engine) cannot be interrupted
mid-call, so it answers `-32002` when it returns instead of pretending the work
never happened.

Deadlines are absolute monotonic instants inside the daemon (`ctx.deadline_ms`)
and are turned into "time left" only where a job is handed to an engine, so a
request can never inherit another request's clock — or have its own ignored.

## Testing strategy

The suites are layered so a failure points at one layer:

1. `tests/unit` — one suite per module, no daemon. A failure here is a module
   bug.
2. `tests/protocol` — a real daemon on a real TCP socket, with the framing
   *re-implemented in the test* so a codec bug cannot hide behind a shared
   encoder. Covers pipelining, concurrent connections, a peer that vanishes
   mid-request, oversize frames, and forward compatibility with unknown frame
   types.
3. `tests/e2e` — the product: a real daemon, a real HTTP fixture site, a real
   Legado rule file, from install to chapter text, plus cache, coalescing and
   load behaviour.
4. `sdk/*/test` — each client library, from its own ergonomics (spawn,
   context manager, retry) through a full read.

`make check` runs the C suites, the js-engine suite and the Node SDK suite.
`make asan` runs the C suites under
AddressSanitizer + UndefinedBehaviorSanitizer, and the harness fails the run on
any leaked byte.

## Choices worth defending

- **Length-prefixed binary framing instead of newline-delimited JSON.** A
  request containing a newline is a data-loss bug waiting to happen, and a
  length prefix makes the 16 MiB ceiling enforceable *before* allocating.
- **Rule files interpreted natively.** 90 % of the Legado corpus is declarative;
  running a JavaScript engine for it would add a runtime dependency and a
  process hop to the most common case.
- **Bounded everything.** Frame size, HTML node count, nesting depth, cache
  bytes, cache entries, queue depth, workers. An extension runtime that can be
  fed arbitrary third-party artifacts must not have an unbounded path.
- **The same error numbers on every transport.** A host can move a call between
  the socket and the in-process ABI without rewriting its error handling, and
  `tests/unit/test_abi.c` asserts the table.
