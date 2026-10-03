#!/usr/bin/env node
/**
 * js_worker.js — the reference `js` engine worker for xbridged.
 *
 * The daemon spawns this process and talks XBP/1 to it over stdin/stdout, the
 * same framing it speaks to a JVM worker or to a host. That symmetry is the
 * point: an engine is just another XBP/1 peer, so adding a runtime Xbridge does
 * not embed is a matter of writing a worker, not of changing the daemon.
 *
 * Extensions are CommonJS modules that export the standard source shape:
 *
 *   module.exports = {
 *     id: 'lnreader/example',
 *     name: 'Example',
 *     version: '1.0.0',
 *     async search(query, page) { return { list: [...], has_next: false }; },
 *     async getDetail(media)      { return { ...media, episodes: [...] }; },
 *     async getNovelContent(chapter) { return { content: '...' }; },
 *   };
 *
 * Worker protocol (all frames, all directions, see docs/PROTOCOL.md):
 *   <- HELLO/HELLO_ACK  handshake at startup
 *   <- REQUEST {id, method, params, stream?, source_id}   one job
 *   -> STREAM_CHUNK {id, seq, chunk}                      when stream is set
 *   -> STREAM_END   {id} | STREAM_ERR {id, error}
 *   -> RESPONSE     {id, ok:true, result} | {id, ok:false, error:{code,message}}
 *   <- CANCEL {id}                                        stop producing
 *   <- PING  (answered with PONG)
 *   <- BYE                                                exit
 *
 * Usage (the daemon does this for you):
 *   XBRIDGE_JS_WORKER=node ... or
 *   xbridged --js-worker "node /path/to/js_worker.js {}"
 */
'use strict';

const fs = require('fs');
const path = require('path');

const FT = {
  HELLO: 0x01,
  HELLO_ACK: 0x02,
  REQUEST: 0x03,
  RESPONSE: 0x04,
  STREAM_CHUNK: 0x05,
  STREAM_END: 0x06,
  STREAM_ERR: 0x07,
  CANCEL: 0x08,
  PING: 0x09,
  PONG: 0x0a,
  ERROR: 0x0b,
  BYE: 0x0c,
};

const ERR = {
  INVALID: -32600,
  NO_METHOD: -32601,
  PARAMS: -32602,
  ENGINE: -32000,
  NOT_FOUND: -32003,
  UNAVAILABLE: -32004,
};

/* ------------------------------------------------------------- extensions -- */

/** id -> loaded module */
const extensions = new Map();

/** The unified surface every js extension may implement. */
const SOURCE_METHODS = [
  'getPopular', 'getLatestUpdates', 'search', 'getDetail', 'getVideoList',
  'getVideoListStream', 'getPageList', 'getNovelContent', 'getPreference',
  'setPreference',
];

function loadExtension(id, filePath) {
  const resolved = path.resolve(filePath);
  delete require.cache[resolved];
  const mod = require(resolved);
  extensions.set(id, { mod, file: resolved });
  return {
    id,
    name: mod.name || path.basename(resolved),
    version: mod.version || 'unknown',
    methods: SOURCE_METHODS.filter((m) => typeof mod[m] === 'function'),
  };
}

function unloadExtension(id) {
  const entry = extensions.get(id);
  if (!entry) return false;
  delete require.cache[entry.file];
  extensions.delete(id);
  return true;
}

/* ------------------------------------------------------------------ jobs -- */

const cancelled = new Set();

function send(type, payload) {
  const body = Buffer.from(JSON.stringify(payload), 'utf8');
  const header = Buffer.alloc(5);
  header.writeUInt32BE(body.length + 1, 0);
  header.writeUInt8(type, 4);
  process.stdout.write(Buffer.concat([header, body]));
}

function fail(id, code, message) {
  send(FT.RESPONSE, { id, ok: false, error: { code, message } });
}

function resolveSource(params, sourceId) {
  const id = sourceId || (params && (params.source_id || params.id)) || '';
  const entry = extensions.get(id);
  if (!entry) {
    // Fall back to the only loaded extension: a worker may be dedicated to one
    // source, and the daemon addresses it by source id.
    if (extensions.size === 1) return [...extensions.values()][0];
    return null;
  }
  return entry;
}

async function runJob(req) {
  const { id, method, params = {}, stream = false } = req;
  const sourceId = req.source_id || params.source_id || '';

  /* Engine-level methods, answered by the worker itself rather than by an
   * extension: this is how a host asks what a source can do. */
  if (method === 'engine.load') {
    try {
      const info = loadExtension(params.id || sourceId, params.path);
      send(FT.RESPONSE, { id, ok: true, result: info });
    } catch (err) {
      fail(id, ERR.ENGINE, `cannot load ${params.path}: ${err.message}`);
    }
    return;
  }
  if (method === 'engine.unload') {
    send(FT.RESPONSE, { id, ok: true, result: { removed: unloadExtension(params.id) } });
    return;
  }
  if (method === 'engine.list') {
    send(FT.RESPONSE, {
      id, ok: true,
      result: Array.from(extensions.entries()).map(([extId, e]) => ({
        id: extId, name: e.mod.name || extId, version: e.mod.version || 'unknown',
        methods: SOURCE_METHODS.filter((m) => typeof e.mod[m] === 'function'),
      })),
    });
    return;
  }

  const name = method.startsWith('source.') ? method.slice('source.'.length) : method;
  if (!SOURCE_METHODS.includes(name)) {
    fail(id, ERR.NO_METHOD, `js worker does not implement ${method}`);
    return;
  }

  const entry = resolveSource(params, sourceId);
  if (!entry) {
    fail(id, ERR.UNAVAILABLE, `no js extension loaded for '${sourceId}'`);
    return;
  }
  const fn = entry.mod[name];
  if (typeof fn !== 'function') {
    fail(id, ERR_NOT_FOUND_X, `extension does not implement ${name}`);
    return;
  }

  try {
    const returned = fn.call(entry.mod, ...argsFor(name, params));

    /* A source may answer with a value or with an async iterator. Both are
     * first class: an iterator is forwarded item by item with backpressure, a
     * value is chunked below. Note the check is on the *result*, not on the
     * function — an async generator function only exposes Symbol.asyncIterator
     * once it has been called. */
    if (returned && typeof returned[Symbol.asyncIterator] === 'function') {
      let seq = 0;
      for await (const item of returned) {
        if (cancelled.has(id)) break;
        if (stream) send(FT.STREAM_CHUNK, { id, seq: seq++, chunk: item });
        else if (seq === 0) seq = 1;   /* non-stream callers get the first item */
      }
      if (stream) {
        send(FT.STREAM_END, { id, count: seq });
      } else {
        fail(id, ERR.PARAMS, 'this source streams; call it with stream: true');
      }
      return;
    }

    const result = await returned;
    if (stream) {
      // A one-shot result is still streamable: send the list items
      // individually and a summary, exactly like the in-process engine does.
      const list = Array.isArray(result && result.list) ? result.list : null;
      if (list) {
        let seq = 0;
        for (const item of list) {
          if (cancelled.has(id)) break;
          send(FT.STREAM_CHUNK, { id, seq: seq++, chunk: { index: seq - 1, item } });
        }
        send(FT.STREAM_CHUNK, {
          id, seq: seq++,
          chunk: { summary: true, count: list.length, has_next: !!result.has_next, page: result.page || 1 },
        });
      } else {
        send(FT.STREAM_CHUNK, { id, seq: 0, chunk: { index: 0, item: result } });
      }
      send(FT.STREAM_END, { id });
      return;
    }
    send(FT.RESPONSE, { id, ok: true, result: result === undefined ? null : result });
  } catch (err) {
    if (stream) send(FT.STREAM_ERR, { id, error: { code: ERR.ENGINE, message: String(err && err.message || err) } });
    else fail(id, ERR.ENGINE, String(err && err.message || err));
  } finally {
    cancelled.delete(id);
  }
}

const ERR_NOT_FOUND_X = -32003;

/** Map the unified call onto the extension's positional signature. */
function argsFor(name, params) {
  switch (name) {
    case 'search':        return [params.query || '', params.page || 1, params.filters || []];
    case 'getPopular':    return [params.page || 1];
    case 'getLatestUpdates': return [params.page || 1];
    case 'getDetail':     return [params.media || { url: params.url }];
    case 'getVideoList':  return [params.episode || { url: params.url }];
    case 'getVideoListStream': return [params.episode || { url: params.url }];
    case 'getPageList':   return [params.episode || { url: params.url }];
    case 'getNovelContent': return [params.id || params.url, params.title || '', params.page || 1];
    case 'getPreference': return [];
    case 'setPreference': return [params.key, params.value];
    default:              return [params];
  }
}

/* ------------------------------------------------------------- framing ---- */

let buffer = Buffer.alloc(0);

process.stdin.on('data', (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  for (;;) {
    if (buffer.length < 5) return;
    const total = buffer.readUInt32BE(0);
    if (buffer.length < 5 + (total - 1)) return;
    const type = buffer.readUInt8(4);
    const body = buffer.slice(5, 5 + total - 1).toString('utf8');
    buffer = buffer.slice(5 + total - 1);

    let payload = {};
    if (body) {
      try { payload = JSON.parse(body); } catch { continue; }
    }

    switch (type) {
      case FT.HELLO:
        send(FT.HELLO_ACK, {
          protocol: 'XBP/1',
          software: 'xbridge-js-worker/1.0.0',
          runtime: process.version,
          methods: SOURCE_METHODS.length + 3,
          engines: [{ name: 'js', status: 'ready', formats: ['lnreader', 'mangayomi', 'sora', 'tsundoku', 'ireader', 'kotatsu'] }],
        });
        break;
      case FT.REQUEST:
        runJob(payload).catch((err) => fail(payload.id, ERR.ENGINE, String(err)));
        break;
      case FT.CANCEL:
        cancelled.add(payload.id);
        break;
      case FT.PING:
        send(FT.PONG, payload);
        break;
      case FT.BYE:
        process.exit(0);
        break;
      default:
        break;   // forward compatibility: ignore what we do not know
    }
  }
});

process.stdin.on('end', () => process.exit(0));
process.on('SIGTERM', () => process.exit(0));
