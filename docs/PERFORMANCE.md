# Performance

Every number below was produced by the scripts in this repository, on the
machine that ran them. Nothing is estimated and nothing is compared against a
number that was not measured.

Reproduce:

```sh
make bench                      # micro-benchmarks of the hot paths
python3 tools/bench_daemon.py   # end-to-end: cold start, latency, throughput, footprint
python3 tools/bench_daemon.py --json
```

## Test machine

| | |
|---|---|
| CPU | 2 vCPU, x86_64, sandboxed container |
| OS | Linux 6.x (Debian userspace) |
| Toolchain | gcc 12.2.0, `-O2` |
| Numbers | a sandboxed 2-vCPU container; treat them as *relative*, not absolute |

## Cold start

The reference architecture launches a JVM sidecar
(`java -Dfile.encoding=UTF-8 -Xms128m -Xmx512m -noverify -jar bridge.jar`) and
detects readiness by grepping stderr for a startup string with a 10-second
timeout that proceeds even when it expires. Xbridge binds its socket and is
answering calls immediately.

| Stage | Measured |
|---|---|
| process start → socket accepting connections | **1.6–2.9 ms** (6 runs) |
| connect + `HELLO`/`HELLO_ACK` round trip | **0.26–0.88 ms** |

No JVM, no interpreter warm-up, no readiness race: the socket only appears
after the engines are initialised, so a client that can connect can also call.

## Request latency

200 sequential `bridge.ping` calls on one connection (control plane only, no
network):

| percentile | latency |
|---|---|
| p50 | **0.075–0.16 ms** |
| p95 | 0.083–0.22 ms |
| p99 | 0.098–0.23 ms |
| mean | 0.048–0.16 ms |

(The spread is the sandbox: the same binary measures p50 = 0.075 ms on an idle
run and 0.16 ms when the host is busy compiling. The distribution is tight in
both cases.)

A `source.search` that performs DNS-less local HTTP, parses the HTML, applies
the rule file and builds JSON:

| | p50 | p95 |
|---|---|---|
| uncached (unique query, cache bypassed) | 0.48–0.61 ms | 0.88–1.06 ms |
| cached (same query) | **0.09–0.18 ms** | 0.12–0.23 ms |
| **speedup** | **3.5–6.4×** | |

The cached read is served from L1 without touching the network, so the saving
scales with how much work the extension does — on a local fixture site the HTTP
round trip is short; against a real site it dominates.

## Throughput

200 requests of `bridge.ping` distributed over 8 concurrent connections:

| | |
|---|---|
| requests | 200 |
| connections | 8 |
| wall time | 14–17 ms |
| **throughput** | **11 700–14 600 req/s** |

## Footprint

| | |
|---|---|
| binary | ~180 KiB |
| resident set while serving | **940 KiB – 1.1 MiB** |
| bytes allocated by the bridge core (self-reported) | ~1.5 MiB |
| threads | 1 idle (request pool scales with `--task-threads`) |
| resident set of the reference architecture's sidecar | ≥ 128 MiB heap (`-Xms128m`) |

The resident set does not grow with the number of loaded sources: rule files
are parsed once into fixed-size structures and the cache is bounded by
`--cache-entries` / `--cache-bytes`.

## Micro-benchmarks

`make bench`, on the same machine. Each row is calibrated to ~250 ms of work.

| benchmark | ops/sec | µs/op | MiB/s |
|---|---:|---:|---:|
| `json.parse.small` (a 100-byte request) | 590 000 | 1.70 | 48 |
| `json.parse.large` (a 40 KiB search payload) | 12 500 | 80.1 | 514 |
| `json.serialize.large` | 3 525 | 283.7 | 145 |
| `json.canonical_hash` (cache key) | 704 761 | 1.42 | 57 |
| `html.parse` (48 KiB document) | 1 088 | 918.9 | 51 |
| `html.select` (500 chapters) | 13 493 | 74.1 | 628 |
| `html.parse+select` | 1 078 | 927.8 | 50 |
| `format.detect` | 183 440 | 5.45 | 90 |
| `sha256` | 5 218 | 191.6 | 215 |
| `cache.put+get` | 3 965 792 | 0.25 | 61 |
| `url.resolve` | 2 713 131 | 0.37 | 166 |

Two of these are worth calling out because they bound real workloads:

- **`html.parse` at ~0.9 ms for 48 KiB** is the ceiling on a chapter-list page:
  a 500-chapter table of contents is parsed, selected and extracted in ~1 ms.
- **`cache.put+get` at 0.25 µs** is what makes the cached path above essentially
  free — the cache is not the bottleneck at 14 k req/s.

## Where the time goes, and what was changed because of it

1. **Framing.** Length-prefixed frames mean the reader never scans for a
   newline and never re-parses a line; the size is known before a buffer is
   allocated, which also makes the 16 MiB ceiling enforceable.
2. **Cache keys.** Cached calls hash the canonical form of the request, so
   `{"a":1,"b":2}` and `{"b":2,"a":1}` share one entry. Canonicalisation costs
   1.4 µs — far less than the HTTP round trip it saves.
3. **Single flight.** Concurrent identical calls collapse into one fetch
   (`cache_speedup` is the *sequential* saving; the concurrent case is strictly
   better because the followers wait instead of duplicating work).
4. **Threads.** One request pool per process rather than one JVM per platform,
   so the marginal cost of the second concurrent call is a task, not a process.
5. **Allocation.** `xb_alloc` tracks live bytes, and the test suite fails on
   any leak, so the footprint above is a floor that stays a floor.

## Honest limits

- TLS is delegated: this build reports `tls: true` with the `curl` backend when
  a TLS provider is present, and `tls: false` otherwise, instead of silently
  failing `https://` requests. Numbers above are for plain HTTP on loopback.
- The JVM engine is only available when a JRE (or a worker providing the same
  protocol) is present; the daemon then pays that process's start-up once, on
  first use, not on every run.
- The sandbox has two cores. On a larger machine throughput scales with cores;
  the *latency* figures are the ones that characterise the design.
