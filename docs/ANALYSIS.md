# Analysis of the reference implementation

> **Scope note.** This document analyses the *public behaviour and architecture* of
> [`RyanYuuki/AnymeXExtensionRuntimeBridge`](https://github.com/RyanYuuki/AnymeXExtensionRuntimeBridge)
> at the commit it was read (read-only examination). No source file, symbol, comment or
> algorithm from that repository is copied into this project. Everything in this repository
> is a clean-room implementation written against the *observable contract* (extension
> formats, repository layouts and method names, which are de-facto ecosystem interfaces
> defined by Aniyomi / CloudStream / Kotatsu / Mangayomi / LnReader / Legado, not by that
> repository). See `docs/CLEANROOM.md`.

## 1. What the reference is

A Flutter plugin (`anymex_extension_runtime_bridge`, v1.6.1) that gives a Dart app a unified
API over ten third-party extension ecosystems:

| Ecosystem | Artifact | Reference runtime |
|---|---|---|
| Mangayomi | `.js` / `.dart` script bundles | Dart JS engine (`flutter_qjs`) + `d4rt` Dart eval |
| Sora | JS modules | Dart JS engine |
| LnReader | `plugin.js` + `manifest.json` | Dart JS engine |
| Legado (阅读) | JSON rule files | pure-Dart rule interpreter |
| Aniyomi | `.apk` / `.jar` (Tachiyomi contract) | JVM |
| CloudStream | `.jar` / `.cs` plugins | JVM |
| Kotatsu | `.jar` (`kotatsu-parsers`) | JVM |
| Tsundoku | `.apk` / `.jar` | JVM |
| iReader | `.apk` / `.jar` | JVM |
| TorrServer | native binary / iOS framework | subprocess or in-process framework |

Runtime hosting is per-platform: **Android** = a bridge APK whose DEX is injected into the host
app process; **desktop** = a JVM sidecar process (`java -jar bridge.jar`); **iOS** = an
in-process OpenJDK Zero VM driven over JNI.

## 2. Measured structure

| Metric | Value |
|---|---|
| Dart files | 158 |
| Kotlin files (Android runtime host) | 13 |
| Java files (desktop runtime host) | 58 |
| C++/Swift/ObjC files (iOS) | 14 |
| `RuntimeBridge.kt` | 1 275 lines, single class |
| In-repo test files | **0** (only a stray `coverage/lcov.info`) |
| CI | none in repo |
| Transport | newline-delimited JSON over the sidecar's stdio |

## 3. Where it is slow (and what we do about it)

| # | Observation | Consequence | Our design |
|---|---|---|---|
| 1 | Framing is **newline-delimited JSON on a single stdio pipe**, decoded with Dart's `LineSplitter` + `jsonDecode` per line, then string-compared for `status`. | A request containing a newline breaks the stream; every response is parsed twice (line split + JSON); no length/size limits; no backpressure; the pipe is a single serialisation point for *all* concurrent calls. | **Length-prefixed binary framing** (`[len][type][json]`) with a hard `max_frame` limit. Ordering is *not* required: responses carry ids and may interleave. |
| 2 | Sidecar JVM is started with `-Xms128m -Xmx512m -noverify` and readiness is detected by grepping stderr for a magic string, with a 10 s timeout that **silently continues anyway**. | ~1–3 s cold start per app launch; a failed JVM is indistinguishable from a slow one. | **Worker pool with health checks**, explicit `HELLO` handshake with protocol + capability negotiation, bounded start-up deadline that *fails loudly*, exponential-backoff restart, and optional pre-warm at daemon start. |
| 3 | `status: 'partial'` responses are acknowledged but dropped in code. | Streaming (`getVideoListStream`) silently loses chunks. | **First-class streaming frames** (`STREAM_CHUNK`/`STREAM_END`/`STREAM_ERR`) with sequence numbers, per-stream window (backpressure) and cancellation. |
| 4 | Timeouts are **client-side only**; on timeout the client sends `{method:'cancel'}` and throws, but the engine keeps working and the pipe stays occupied. | Wasted CPU/network, cascading slow-downs, leaked sockets. | **Deadlines travel with the request** (`deadline_ms`); the daemon owns the timer, propagates cancellation to engines, and reclaims resources deterministically. |
| 5 | No caching, no request coalescing. | Ten identical `getDetail` calls = ten engine round-trips. | **L1 LRU+TTL response cache** and **single-flight coalescing** of identical in-flight requests, both observable via `bridge.metrics`. |
| 6 | Per-format code is duplicated per platform (`XExtensions.dart`, `XSourceMethods.dart`, `DesktopXExtensions.dart`, `DesktopXSourceMethods.dart`). | 158 Dart files; two implementations of every format to keep in sync. | **One declarative format registry** (`formats/*.json` + `registry.c`) with a single capability matrix; engines are language-agnostic workers, so there is exactly one implementation per format. |
| 7 | Extension-format logic lives *inside the Flutter app*. | Impossible to use from a CLI, a Python tool, a Node service or a test runner. | The daemon is a **standalone process**; SDKs exist for Node/Bun/Deno, Python, C and Dart. Any language can host or *be* an engine. |
| 8 | On Android the runtime host is **DEX-injected into the host app process**. | A misbehaving extension can crash or hang the host app; no isolation; no way to restart. | Out-of-process worker supervision everywhere, plus an opt-in *in-process* fast path later via a shared library ABI (`bridge_abi.h`). |
| 9 | No versioning or capability negotiation. | Host and bridge must ship in lockstep; feature detection is impossible. | `HELLO`/`bridge.handshake` returns protocol version, semver, enabled engines, formats, limits and features. |
| 10 | No tests, no CI. | Regressions land silently. | `tests/` with unit, protocol-conformance, end-to-end, fuzz and benchmark suites; GitHub Actions matrix on Linux/macOS/Windows × x64/arm64. |

## 4. Where it is functionally narrow

* Only Android gets in-process hosting; desktop is JVM-only even for JS-based ecosystems, so
  a host that only needs Mangayomi/LnReader/Legado still pays for a JVM.
* Repository handling is per-service; there is no unified repository model across ecosystems.
* No metrics/observability surface, so the host cannot tell why something is slow.

## 5. What we keep (the contract, not the code)

The **manager ids** (`aniyomi`, `cloudstream`, `kotatsu`, `legado`, `lnreader`, `tsundoku`,
`ireader`, `mangayomi`, `sora`, `torrserver`) and the **unified method names**
(`getPopular`, `getLatestUpdates`, `search`, `getDetail`, `getVideoList`, `getPageList`,
`getNovelContent`, `getPreference`, `setPreference`) are preserved verbatim so that a host app
can switch bridges without touching its call sites. They are ecosystem vocabulary; the
implementation behind them is entirely new.
