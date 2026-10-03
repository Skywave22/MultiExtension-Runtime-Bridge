# XBP/1 — eXtension Bridge Protocol, version 1

XBP is the wire protocol spoken between a **host** (your app / SDK) and the **bridge daemon**
(`xbridged`), and between the daemon and its **engines**. The same protocol is used on every
link; only the transport changes.

Design goals, in order: *(1)* zero ambiguity — a reader can always find the next frame;
*(2)* concurrency without head-of-line blocking; *(3)* deadlines and cancellation that actually
stop work; *(4)* streaming that cannot be silently dropped; *(5)* cheap to parse in any language.

## 1. Transport bindings

| Binding | Where | Address form |
|---|---|---|
| Unix domain socket (pathname) | Linux, macOS, BSD | `unix:///run/user/1000/xbridge.sock` |
| Unix domain socket (**abstract**, Linux only) | Linux | `unix-abstract://xbridge-<uid>` — no filesystem entry, auto-released on crash |
| Named pipe | Windows | `npipe://\\.\pipe\xbridge-<user>` |
| TCP loopback | everywhere, fallback / remote debugging | `tcp://127.0.0.1:0` (port printed on stdout) |
| stdio | engine workers, embedding | `stdio://` |
| Shared library ABI | iOS / Android in-process | `core/include/bridge/abi.h` — same dispatcher, same error table, no socket |

The daemon prints exactly one JSON line to stdout when it is ready:

```json
{"event":"ready","protocol":"XBP/1","version":"1.0.0","endpoint":"unix:///tmp/xbridge.sock","pid":4242,"engines":["js","rule"]}
```

`endpoint` is the canonical address a host should connect to. If the host passed `--endpoint`,
it already knows it.

## 2. Framing

Every message on a byte stream is:

```
+--------+--------+-------------------------------+
| u32 BE |  u8    |  payload (UTF-8 JSON)          |
| length | type   |  length bytes                  |
+--------+--------+-------------------------------+
  ^ length counts type + payload
```

* `length` ≥ 1 and ≤ `max_frame` (default **16 MiB**, negotiated in `HELLO`).
* Frames are independent; a writer may interleave frames from different requests.
* Unknown frame types **must** be skipped, not treated as an error (forward compatibility).

### Frame types

| Code | Name | Direction | Payload |
|---|---|---|---|
| `0x01` | `HELLO` | both | `{"protocol":"XBP/1","client":"node-sdk/1.0.0","max_frame":16777216,"features":["stream","cancel"]}` |
| `0x02` | `HELLO_ACK` | daemon | `{"protocol":"XBP/1","software":"xbridged/1.0.0","protocol_version":1,"pid":N,"started_ms":T,"max_frame":16777216,"task_threads":16,"cache_entries":4096,"cache_ttl_ms":60000,"endpoint":"unix://…","data_dir":"…","features":[…],"capabilities":{"tls":true,"tls_backend":"curl","formats":10,"methods":30,"installed_extensions":0,"repositories":0,"allow_shutdown":false},"engines":[{"name":"rule","status":"ready","workers":1,"workers_configured":1,"formats":["legado"]}]}` — `pid` and `started_ms` let a supervisor detect a replacement process, and `capabilities.formats`/`methods` let a host build a UI without probing |
| `0x03` | `REQUEST` | both | see §3 |
| `0x04` | `RESPONSE` | daemon | see §4 |
| `0x05` | `STREAM_CHUNK` | daemon | see §5 |
| `0x06` | `STREAM_END` | daemon | see §5 |
| `0x07` | `STREAM_ERR` | daemon | see §5 |
| `0x08` | `CANCEL` | both | `{"id":"a1"}` |
| `0x09` | `PING` | both | `{}` (or opaque echo token) |
| `0x0A` | `PONG` | both | echo of the PING payload |
| `0x0B` | `ERROR` | daemon | protocol-level error, not tied to a request: `{"code":-32700,"message":"bad frame"}` |
| `0x0C` | `BYE` | both | `{"reason":"shutdown"}` — sender will close |

## 3. `REQUEST`

```json
{
  "id": "a1",
  "method": "source.search",
  "params": { "source_id": "mangayomi/en.animex", "query": "one piece", "page": 1 },
  "deadline_ms": 15000,
  "cache": { "ttl_ms": 60000, "key": null },
  "coalesce": true,
  "stream": false,
  "meta": { "trace": "…optional host-side correlation…" }
}
```

* `id` — host-chosen, **unique per connection**, any non-empty string ≤ 64 bytes.
* `deadline_ms` — milliseconds from receipt. The daemon owns the timer. `0` = no deadline.
  When it fires, the daemon: (a) replies `RESPONSE{ok:false, error.code:-32001,"deadline exceeded"}`,
  (b) sends `CANCEL` to the engine, (c) releases all resources for the request.
* `cache.ttl_ms` — `null`/absent = use method default; `0` = bypass cache; `>0` = override TTL.
* `cache.key` — optional explicit key; when omitted the key is
  `sha256(method ‖ canonical(params))`, where canonical JSON sorts object keys.
* `coalesce` — when `true` (default) and a *cacheable, byte-identical* request is already
  in flight, the daemon attaches this request to it and both receive the same response.
* `stream` — when `true` the reply is a stream (§5); `RESPONSE` will never be sent.

Unknown fields are ignored. Unknown `method` → `RESPONSE{ok:false, error.code:-32601}`.

## 4. `RESPONSE`

```json
{
  "id": "a1",
  "ok": true,
  "result": { "list": [ ... ] },
  "meta": { "ms": 41, "cached": false, "coalesced": false, "engine": "js", "worker": 2 }
}
```

```json
{
  "id": "a1",
  "ok": false,
  "error": { "code": -32001, "message": "deadline exceeded", "retryable": true },
  "meta": { "ms": 15000, "engine": "js" }
}
```

Exactly one of `result` / `error` is present. `meta` is always present.

### Error codes

| Code | Meaning | Retryable |
|---|---|---|
| `-32700` | malformed JSON frame | no |
| `-32600` | invalid request (missing id/method, bad types) | no |
| `-32601` | method not found | no |
| `-32602` | invalid params | no |
| `-32000` | engine/runtime failure | yes |
| `-32001` | deadline exceeded | yes |
| `-32002` | cancelled by host | no |
| `-32003` | source not installed / not found | no |
| `-32004` | engine unavailable (not installed, failed to start) | yes |
| `-32005` | network failure inside an extension | yes |
| `-32006` | extension rejected by format validation | no |
| `-32007` | payload exceeds a hard limit | no |

## 5. Streaming

For `stream:true` the daemon emits zero or more `STREAM_CHUNK` frames, then exactly one
terminal frame (`STREAM_END` or `STREAM_ERR`).

```json
{ "id": "s7", "seq": 0, "chunk": { "name": "1080p", "url": "https://…" } }
{ "id": "s7", "seq": 1, "chunk": { "name": "720p",  "url": "https://…" } }
{ "id": "s7", "done": true, "count": 2, "meta": { "ms": 310 } }
```

* `seq` is monotonically increasing per stream, starting at 0. Gaps are not allowed.
* The daemon applies backpressure: it stops reading from the engine when the connection's
  output buffer exceeds `high_water` (default 4 MiB) and resumes below `low_water` (1 MiB).
* `STREAM_ERR`: `{"id":"s7","error":{"code":-32005,"message":"…"}}`.
* A host `CANCEL` for a stream id stops production and suppresses everything after the first
  frame already in flight.

## 6. Handshake

1. Client connects and sends `HELLO` (framed).
2. Daemon replies `HELLO_ACK` or closes the connection (wrong protocol / version).
3. Client may then send requests. The daemon rejects requests that arrive before `HELLO`
   with `ERROR{code:-32600}` and keeps the connection open for one more attempt.

## 7. Method surface

Version 1 exposes these namespaces. Full parameter schemas: `docs/METHODS.md`.

**Admin / introspection**

| Method | Purpose |
|---|---|
| `bridge.handshake` | capabilities without a new connection |
| `bridge.ping` | liveness + round-trip |
| `bridge.metrics` | counters, latency percentiles, cache hit rate, per-engine stats |
| `bridge.log` | stream recent log lines (stream) |
| `bridge.shutdown` | graceful stop (requires `--allow-shutdown`) |

**Formats, repositories, extensions**

| Method | Purpose |
|---|---|
| `format.list` | all supported formats + engine + capabilities + artifact rules |
| `format.detect` | detect the format of a file/dir/bytes, with confidence and evidence |
| `repo.list` / `repo.add` / `repo.remove` / `repo.refresh` | unified repository model across ecosystems |
| `extension.list` | installed + available, filterable by manager |
| `extension.install` / `extension.uninstall` | manage artifacts |
| `extension.info` | manifest of one extension |
| `extension.update` | update all/one |

**Sources** — the unified surface, identical names to the ecosystem contract:

| Method | Notes |
|---|---|
| `source.getPopular` | `{source_id, page}` |
| `source.getLatestUpdates` | `{source_id, page}` |
| `source.search` | `{source_id, query, page, filters}` |
| `source.getDetail` | `{source_id, media}` |
| `source.getVideoList` | `{source_id, episode}` |
| `source.getVideoListStream` | `stream:true` |
| `source.getPageList` | `{source_id, episode}` |
| `source.getNovelContent` | `{source_id, title, id}` |
| `source.getPreference` / `source.setPreference` | per-extension KV |
| `source.methods` | capability map for one source |

**Torrent addon**

| Method | Purpose |
|---|---|
| `torrserver.status` / `install` / `start` / `stop` | engine lifecycle |
| `torrserver.stream` | start a stream, returns `{stream_url}` |
| `torrserver.stopStream` | by hash |

## 8. Canonical JSON (for cache keys)

Object keys are sorted by UTF-8 byte order, no insignificant whitespace, numbers are
serialised with `%.17g` trimmed, and the hash is SHA-256 in lowercase hex. Both the C core
(`json.c: jw_canonical`) and the SDKs implement this identically; the conformance suite
(`tests/protocol/`) cross-checks them.

## 9. Versioning

`XBP/1` will not change compatibly-breaking behaviour. Additive fields, new frame types and
new methods are allowed within `/1`. Hosts **must** ignore unknown fields and frame types.
A future `/2` will be negotiated in `HELLO`.
