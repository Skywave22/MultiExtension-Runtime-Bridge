/**
 * xbridge.js — XBP/1 client for Node.js.
 *
 * Same protocol, same error table, same method names as the Python, Dart and
 * Swift clients; only the transport binding differs per platform.
 *
 *   const { Bridge } = require('./xbridge');
 *
 *   const bridge = await Bridge.spawn('xbridged');       // or Bridge.connect(uri)
 *   const found  = await bridge.search('legado/x.json', 'solo');
 *   const book   = await bridge.detail('legado/x.json', found.list[0].url);
 *   for await (const chunk of bridge.stream('source.search', {...})) { ... }
 *   await bridge.close();
 *
 * No dependencies. Node 16+.
 */
'use strict';

const net = require('net');
const os = require('os');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');
const { randomBytes } = require('crypto');

const PROTOCOL = 'XBP/1';
const VERSION = '1.0.0';

// Frame types (keep in sync with core/include/bridge/server.h).
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
  PARSE: -32700,
  INVALID: -32600,
  NO_METHOD: -32601,
  PARAMS: -32602,
  ENGINE: -32000,
  DEADLINE: -32001,
  CANCELLED: -32002,
  NOT_FOUND: -32003,
  UNAVAILABLE: -32004,
  NETWORK: -32005,
  REJECTED: -32006,
  LIMIT: -32007,
};

const RETRYABLE = new Set([ERR.ENGINE, ERR.DEADLINE, ERR.UNAVAILABLE, ERR.NETWORK]);

class BridgeError extends Error {
  constructor(code, message, meta = undefined) {
    super(message);
    this.name = code === ERR.DEADLINE ? 'TimeoutError' : 'BridgeError';
    this.code = code;
    this.meta = meta;
  }
  get retryable() { return RETRYABLE.has(this.code); }
  get notFound() { return this.code === ERR.NOT_FOUND; }
  get unavailable() { return this.code === ERR.UNAVAILABLE; }
}

/* ------------------------------------------------------------ transport -- */

function parseEndpoint(uri) {
  if (uri.startsWith('unix://')) return { kind: 'unix', path: uri.slice(7) };
  if (uri.startsWith('unix-abstract://')) return { kind: 'abstract', name: uri.slice(16) };
  if (uri.startsWith('npipe://')) return { kind: 'npipe', path: uri.slice(8) };
  if (uri.startsWith('tcp://')) {
    const [host, port] = uri.slice(6).split(':');
    return { kind: 'tcp', host, port: Number(port) };
  }
  if (uri.startsWith('\\\\.\\pipe\\')) return { kind: 'npipe', path: uri };
  throw new BridgeError(ERR.INVALID, `unsupported endpoint: ${uri}`);
}

function connectSocket(endpoint, timeoutMs = 10000) {
  const ep = parseEndpoint(endpoint);
  return new Promise((resolve, reject) => {
    let sock;
    const onError = (err) => { sock?.destroy(); reject(err); };
    if (ep.kind === 'tcp') {
      sock = net.connect({ host: ep.host, port: ep.port });
    } else if (ep.kind === 'abstract') {
      // Linux abstract sockets are addressed by a leading NUL in sun_path.
      sock = net.connect({ path: '\0' + ep.name });
    } else {
      sock = net.connect({ path: ep.path });
    }
    sock.setNoDelay(true);
    sock.setTimeout(timeoutMs, () => onError(new BridgeError(ERR.UNAVAILABLE, 'connect timed out')));
    sock.once('connect', () => { sock.setTimeout(0); resolve(sock); });
    sock.once('error', onError);
  });
}

/* --------------------------------------------------------------- bridge -- */

class Bridge {
  constructor(endpoint = '', timeoutMs = 60000) {
    this.endpoint = endpoint;
    this.timeoutMs = timeoutMs;
    this.hello = {};
    this._sock = null;
    this._buf = Buffer.alloc(0);
    /* One ordered queue of frames, exactly like the Python SDK: a response is
     * matched by id as it is read, and a frame that belongs to another call is
     * held aside until that call is reached. One in-flight call per connection;
     * open a second connection for concurrency, which is what the load tests
     * do and what the connection-per-purpose model expects. */
    this._frames = [];
    this._frameWaiters = [];
    this._counter = 0;
    this._closed = false;
    this._proc = null;
    this._readLoop = null;
  }

  /** Connect to an already-running daemon and complete the handshake. */
  static async connect(endpoint, timeoutMs = 10000) {
    const bridge = new Bridge(endpoint, timeoutMs);
    bridge._sock = await connectSocket(endpoint, timeoutMs);
    bridge._startReading();
    await bridge._hello();
    return bridge;
  }

  /** Start a daemon and connect to it. */
  static async spawn(binary = 'xbridged', { dataDir, endpoint, extraArgs = [], timeoutMs = 20000 } = {}) {
    if (!endpoint) {
      const tmp = path.join(os.tmpdir(), `xbridge-${randomBytes(6).toString('hex')}.sock`);
      endpoint = process.platform === 'win32'
        ? `npipe://xbridge-${process.pid}-${randomBytes(3).toString('hex')}`
        : `unix://${tmp}`;
    }
    const args = ['--endpoint', endpoint];
    if (dataDir) args.push('--data-dir', dataDir);
    args.push(...extraArgs);

    const proc = spawn(binary, args, { stdio: ['ignore', 'pipe', 'inherit'] });
    const ready = await new Promise((resolve, reject) => {
      let buffered = '';
      const deadline = setTimeout(() => reject(new BridgeError(ERR.UNAVAILABLE, `${binary} did not become ready`)), timeoutMs);
      proc.stdout.on('data', (buf) => {
        buffered += buf.toString('utf8');
        let nl;
        while ((nl = buffered.indexOf('\n')) >= 0) {
          const line = buffered.slice(0, nl);
          buffered = buffered.slice(nl + 1);
          try {
            const msg = JSON.parse(line);
            if (msg && msg.event === 'ready') {
              clearTimeout(deadline);
              resolve(msg);
              return;
            }
          } catch { /* not the ready line */ }
        }
      });
      proc.once('exit', (code) => {
        clearTimeout(deadline);
        reject(new BridgeError(ERR.UNAVAILABLE, `${binary} exited with code ${code}`));
      });
    });

    const bridge = await Bridge.connect(ready.endpoint, timeoutMs);
    bridge.endpoint = ready.endpoint;
    bridge._proc = proc;
    return bridge;
  }

  /* -- framing ---------------------------------------------------------- */

  _startReading() {
    this._sock.on('data', (buf) => {
      this._buf = Buffer.concat([this._buf, buf]);
      this._drainFrames();
    });
    this._sock.on('error', (err) => this._failAll(err));
    this._sock.on('close', () => {
      this._closed = true;
      this._failAll(new BridgeError(ERR.UNAVAILABLE, 'connection closed by daemon'));
    });
  }

  _drainFrames() {
    for (;;) {
      if (this._buf.length < 5) return;
      const total = this._buf.readUInt32BE(0);
      if (this._buf.length < 5 + (total - 1)) return;
      const type = this._buf.readUInt8(4);
      const body = this._buf.slice(5, 5 + total - 1).toString('utf8');
      this._buf = this._buf.slice(5 + total - 1);
      let payload;
      try {
        payload = body ? JSON.parse(body) : {};
      } catch {
        payload = { raw: body };
      }
      this._deliver({ type, payload });
    }
  }

  _deliver(frame) {
    if (frame.type === FT.PING) {
      this._writeFrame(FT.PONG, frame.payload);
      return;
    }
    const waiter = this._frameWaiters.shift();
    if (waiter) waiter.resolve(frame);
    else this._frames.push(frame);
  }

  _nextFrame() {
    if (this._frames.length) return Promise.resolve(this._frames.shift());
    if (this._closed) return Promise.reject(new BridgeError(ERR.UNAVAILABLE, 'connection closed'));
    return new Promise((resolve, reject) => this._frameWaiters.push({ resolve, reject }));
  }

  /** Read frames until one belongs to `id`, holding the others aside. */
  async _nextFrameFor(id) {
    const skipped = [];
    try {
      for (;;) {
        const frame = await this._nextFrame();
        const fid = frame.payload ? frame.payload.id : undefined;
        if (fid === undefined || fid === id) return frame;
        skipped.push(frame);
      }
    } finally {
      if (skipped.length) this._frames.unshift(...skipped);
    }
  }

  _failAll(err) {
    for (const { reject } of this._frameWaiters.splice(0)) reject(err);
  }

  _writeFrame(type, payload) {
    if (!this._sock || this._closed) throw new BridgeError(ERR_UNAVAILABLE, 'not connected');
    const body = Buffer.from(JSON.stringify(payload), 'utf8');
    const header = Buffer.alloc(5);
    header.writeUInt32BE(body.length + 1, 0);
    header.writeUInt8(type, 4);
    this._sock.write(Buffer.concat([header, body]));
  }

  async _hello() {
    this._writeFrame(FT.HELLO, {
      protocol: PROTOCOL,
      client: `node-sdk/${VERSION}`,
      features: ['stream', 'cancel'],
    });
    const frame = await this._nextFrame();
    if (frame.type !== FT.HELLO_ACK) {
      throw new BridgeError(frame.payload.code || ERR.INVALID,
        frame.payload.message || 'handshake rejected');
    }
    this.hello = frame.payload;
    return this.hello;
  }

  nextId(prefix = 'js') {
    this._counter += 1;
    return `${prefix}-${this._counter}`;
  }

  /* -- calls ------------------------------------------------------------ */

  async call(method, params = {}, { timeoutMs = 0, cacheTtlMs = undefined, coalesce = true } = {}) {
    const id = this.nextId();
    const payload = { id, method, params: params || {} };
    if (timeoutMs) payload.deadline_ms = timeoutMs;
    if (cacheTtlMs !== undefined) payload.cache = { ttl_ms: cacheTtlMs };
    if (!coalesce) payload.coalesce = false;

    this._writeFrame(FT.REQUEST, payload);

    for (;;) {
      const frame = await this._nextFrameFor(id);
      if (frame.type === FT.RESPONSE && frame.payload.id === id) {
        if (frame.payload.ok) return frame.payload.result;
        const err = frame.payload.error || {};
        const code = err.code !== undefined ? err.code : ERR.ENGINE;
        throw new BridgeError(code, err.message || 'engine error', frame.payload.meta);
      }
      if (frame.type === FT.ERROR) {
        throw new BridgeError(frame.payload.code || ERR.INVALID, frame.payload.message || 'protocol error');
      }
    }
  }

  async *stream(method, params = {}, { timeoutMs = 0 } = {}) {
    const id = this.nextId('stream');
    const payload = { id, method, params: params || {}, stream: true };
    if (timeoutMs) payload.deadline_ms = timeoutMs;

    this._writeFrame(FT.REQUEST, payload);

    for (;;) {
      const frame = await this._nextFrameFor(id);
      if (frame.type === FT.STREAM_CHUNK && frame.payload.id === id) {
        yield frame.payload.chunk;
      } else if (frame.type === FT.STREAM_END && frame.payload.id === id) {
        return;
      } else if (frame.type === FT.STREAM_ERR && frame.payload.id === id) {
        const err = frame.payload.error || {};
        throw new BridgeError(err.code || ERR.ENGINE, err.message || 'stream error');
      } else if (frame.type === FT.ERROR) {
        throw new BridgeError(frame.payload.code || ERR.INVALID, frame.payload.message || 'protocol error');
      }
    }
  }

  cancel(requestId) {
    this._writeFrame(FT.CANCEL, { id: requestId });
  }

  ping() {
    const t0 = process.hrtime.bigint();
    return this.call('bridge.ping').then(() => Number(process.hrtime.bigint() - t0) / 1e6);
  }

  /* -- convenience ------------------------------------------------------ */

  metrics() { return this.call('bridge.metrics'); }
  async formats() { return (await this.call('format.list')).formats; }
  detect(filePath) { return this.call('format.detect', { path: filePath }); }
  async repositories() { return (await this.call('repo.list')).repos; }
  addRepo(url, managerId = 'aniyomi', name = '') {
    const params = { url, manager_id: managerId };
    if (name) params.name = name;
    return this.call('repo.add', params);
  }
  removeRepo(repoId) { return this.call('repo.remove', { id: repoId }); }
  refreshRepo(repoId = '', url = '') {
    const params = {};
    if (repoId) params.id = repoId;
    if (url) params.url = url;
    return this.call('repo.refresh', params);
  }
  async extensions(managerId = '') {
    return (await this.call('extension.list', managerId ? { manager_id: managerId } : {})).extensions;
  }
  install({ path: filePath = '', url = '', managerId = '' } = {}) {
    const params = {};
    if (filePath) params.path = filePath;
    if (url) params.url = url;
    if (managerId) params.manager_id = managerId;
    return this.call('extension.install', params);
  }
  uninstall(extId) { return this.call('extension.uninstall', { id: extId }); }
  sourceMethods(sourceId) { return this.call('source.methods', { source_id: sourceId }); }
  popular(sourceId, page = 1) { return this.call('source.getPopular', { source_id: sourceId, page }); }
  latest(sourceId, page = 1) { return this.call('source.getLatestUpdates', { source_id: sourceId, page }); }
  search(sourceId, query, page = 1, filters = []) {
    return this.call('source.search', { source_id: sourceId, query, page, filters });
  }
  detail(sourceId, media) {
    const m = typeof media === 'string' ? { url: media } : { ...media };
    return this.call('source.getDetail', { source_id: sourceId, media: m });
  }
  videoList(sourceId, episode) {
    const e = typeof episode === 'string' ? { url: episode } : { ...episode };
    return this.call('source.getVideoList', { source_id: sourceId, episode: e });
  }
  videoListStream(sourceId, episode) {
    const e = typeof episode === 'string' ? { url: episode } : { ...episode };
    return this.stream('source.getVideoListStream', { source_id: sourceId, episode: e });
  }
  pageList(sourceId, episode) {
    const e = typeof episode === 'string' ? { url: episode } : { ...episode };
    return this.call('source.getPageList', { source_id: sourceId, episode: e });
  }
  novelContent(sourceId, title, chapterId = '') {
    if (!chapterId) return this.call('source.getNovelContent', { source_id: sourceId, id: title });
    return this.call('source.getNovelContent', { source_id: sourceId, title, id: chapterId });
  }

  admin = {
    stats: () => this.call('admin.stats'),
    logs: (limit = 200) => this.call('admin.logs', { limit }),
  };

  /* -- lifecycle -------------------------------------------------------- */

  async close() {
    if (this._closed) return;
    this._closed = true;
    try { this._writeFrame(FT.BYE, { reason: 'client closing' }); } catch { /* already gone */ }
    try { this._sock.end(); this._sock.destroy(); } catch { /* ignore */ }
    if (this._proc) {
      const proc = this._proc;
      this._proc = null;
      await new Promise((resolve) => {
        const timer = setTimeout(() => { proc.kill('SIGKILL'); resolve(); }, 5000);
        proc.once('exit', () => { clearTimeout(timer); resolve(); });
        proc.kill('SIGTERM');
      });
      // Only the client that spawned the daemon removes its socket.
      if (this.endpoint.startsWith('unix://') && fs.existsSync(this.endpoint.slice(7))) {
        try { fs.unlinkSync(this.endpoint.slice(7)); } catch { /* ignore */ }
      }
    }
  }
}

/** Retry a call on retryable errors with exponential backoff. */
async function retry(fn, attempts = 3, delayMs = 250) {
  let last;
  for (let i = 0; i < attempts; i++) {
    try {
      return await fn();
    } catch (err) {
      if (!(err instanceof BridgeError) || !err.retryable) throw err;
      last = err;
      await new Promise((r) => setTimeout(r, delayMs * 2 ** i));
    }
  }
  throw last;
}

module.exports = { Bridge, BridgeError, retry, ERR, FT, PROTOCOL, VERSION };
