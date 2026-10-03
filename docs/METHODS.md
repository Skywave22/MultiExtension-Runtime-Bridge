# The unified method surface

Every ecosystem is reached through **one** set of method names. A host app does
not need to know whether a source is a Kotlin `.jar`, a Legado JSON rule file or
a Mangayomi Dart bundle — it calls `source.search` and the bridge routes it.

Ask the daemon what it has at any time:

```
bridge.methods                        -> {"count":30,"methods":[{"name","engine","streamable","cacheable","summary"}]}
bridge.methods {"prefix":"source."}   -> just the source surface
```

## Namespaces

| Namespace | Purpose |
|---|---|
| `bridge.*` | the daemon itself: handshake, liveness, metrics, introspection, logs, shutdown |
| `format.*` | the ten ecosystems: listing and artifact classification |
| `repo.*` | extension repositories: list, add, remove, refresh |
| `extension.*` | installed extensions: list, install, uninstall, info, update |
| `source.*` | the unified content surface (below) |
| `torrserver.*` | the TorrServer addon surface |

## `source.*`

All of them take `source_id` — the installed extension id, e.g.
`legado/book-source.json` or `aniyomi/en.animex` — and they all return JSON.

| Method | Extra params | Returns |
|---|---|---|
| `source.methods` | — | which of these methods this source implements, and which are streamable |
| `source.info` | — | name, version, manager, language, capabilities |
| `source.getPopular` | `page` | `{list:[…], has_next, page, total}` |
| `source.getLatestUpdates` | `page` | same shape |
| `source.search` | `query`, `page`, `filters[]` | same shape; items are `{name, url, cover, author, …}` |
| `source.getDetail` | `media:{url}` | `{name, url, cover, author, description, episodes:[{name,url,index}], episode_count}` |
| `source.getVideoList` | `episode:{url}` | `{videos:[{url, quality, …}]}` |
| `source.getVideoListStream` | `episode:{url}` | **streaming**: chunks as they are resolved |
| `source.getPageList` | `episode:{url}` | page/image list for readers |
| `source.getNovelContent` | `id` (chapter url) | `{content, url, length, next_url?}` |
| `source.getPreference` | — | source preferences as JSON |
| `source.setPreference` | `key`, `value` | persisted settings |

`media` and `episode` accept either an object or a plain URL string. The SDK
shortcuts do that for you: `bridge.detail(src, url)`,
`bridge.novel_content(src, url)`.

### Streaming

A request with `"stream": true` is answered with `STREAM_CHUNK` frames carrying
`{"id","seq","chunk"}` and terminated by `STREAM_END` (`{"id","done":true,"count","meta"}`)
or `STREAM_ERR`. List-shaped results are sent one item per chunk with a final
summary chunk, so a UI can render while the rest is still being fetched:

```python
for chunk in bridge.stream("source.search", {"source_id": src, "query": "solo"}):
    if chunk.get("summary"):
        print("total:", chunk["count"])
    else:
        render(chunk["item"])
```

### Errors

One error table on every transport (`bridge/server.h`, mirrored in every SDK):

| Code | Meaning | Retryable |
|---|---|---|
| `-32700` | parse error | no |
| `-32600` | invalid request | no |
| `-32601` | no such method | no |
| `-32602` | bad params | no |
| `-32000` | engine failure | yes |
| `-32001` | deadline exceeded | yes |
| `-32002` | cancelled | no |
| `-32003` | not found (unknown source, missing chapter) | no |
| `-32004` | engine/daemon unavailable | yes |
| `-32005` | network failure reaching the site | yes |
| `-32006` | artifact rejected (malformed extension) | no |
| `-32007` | limit exceeded (frame/target size) | no |

## `format.*`

| Method | Params | Returns |
|---|---|---|
| `format.list` | — | `{count, formats:[{id, label, manager_id, engine, artifact_kinds[], identify_markers[], capabilities[], platforms[], notes}]}` |
| `format.detect` | `path` | `{format, manager_id, kind, confidence, evidence, entry_point}` |

`format.detect` reads content, not the file extension, and always returns the
`evidence` string that drove the decision — so a wrong guess is debuggable
rather than mysterious.

## `repo.*` and `extension.*`

| Method | Params | Notes |
|---|---|---|
| `repo.list` | — | `{repos:[{id, name, url, manager_id, added_ms}], count}` |
| `repo.add` | `url`, `manager_id`, `name?` | |
| `repo.remove` | `id` | |
| `repo.refresh` | `id` or `url` | fetches the index and returns its entries |
| `extension.list` | `manager_id?` | `{extensions:[…], count, managers:[…]}` |
| `extension.install` | `path` or `url`, `manager_id?` | validates **at install time**; a malformed rule file is rejected here, not at first use |
| `extension.uninstall` | `id` | |
| `extension.info` | `id` | |
| `extension.update` | `id?` | re-fetch from the repository |

## Coverage per ecosystem

`source.methods` reports what a given extension actually implements; this is the
ceiling each manager can reach:

This table is generated from `core/formats/*.json`; the registry is the single
source of truth, and `make formats` regenerates the mirrors.

| Ecosystem | popular | latest | search | detail | video | pages | novel | prefs |
|---|---|---|---|---|---|---|---|---|
| Aniyomi | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ |
| CloudStream | ✅ | ✅ | ✅ | ✅ | ✅ | — | — | ✅ |
| Kotatsu | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| Legado | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ |
| LnReader | — | ✅ | ✅ | ✅ | — | — | ✅ | ✅ |
| Mangayomi | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Sora | — | ✅ | ✅ | ✅ | ✅ | — | — | — |
| Tsundoku | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ |
| iReader | ✅ | ✅ | ✅ | ✅ | — | — | ✅ | ✅ |
| TorrServer Addon | via `torrserver.*` | | | | | | | |

Read the raw matrix:

```sh
./build/xbridged --list-formats | python3 -m json.tool    # every field of every format
```

Where a manager cannot do something (Mangayomi can serve manga *and* novels,
Sora is anime-only), the capability matrix in `core/formats/<id>.json` says so
once, and every host reads the same data instead of assuming.
