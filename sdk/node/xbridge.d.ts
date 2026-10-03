/**
 * Type definitions for the Xbridge Node client.
 *
 * These mirror sdk/node/xbridge.js exactly. Where the daemon's answer shape is
 * loose (`[key: string]: unknown` on extension payloads), that is deliberate:
 * the ten ecosystems disagree about their own fields, so the bridge passes them
 * through rather than pretending they are uniform.
 */

export const PROTOCOL: 'XBP/1';
export const VERSION: string;

export declare const FT: {
  readonly HELLO: 0x01;
  readonly HELLO_ACK: 0x02;
  readonly REQUEST: 0x03;
  readonly RESPONSE: 0x04;
  readonly STREAM_CHUNK: 0x05;
  readonly STREAM_END: 0x06;
  readonly STREAM_ERR: 0x07;
  readonly CANCEL: 0x08;
  readonly PING: 0x09;
  readonly PONG: 0x0a;
  readonly ERROR: 0x0b;
  readonly BYE: 0x0c;
};

export declare const ERR: {
  readonly PARSE: -32700;
  readonly INVALID: -32600;
  readonly NO_METHOD: -32601;
  readonly PARAMS: -32602;
  readonly ENGINE: -32000;
  readonly DEADLINE: -32001;
  readonly CANCELLED: -32002;
  readonly NOT_FOUND: -32003;
  readonly UNAVAILABLE: -32004;
  readonly NETWORK: -32005;
  readonly REJECTED: -32006;
  readonly LIMIT: -32007;
};

export type ErrorCode = (typeof ERR)[keyof typeof ERR];

export declare class BridgeError extends Error {
  readonly code: number;
  readonly meta?: Record<string, unknown>;
  /** Worth retrying: engine, deadline, unavailable, network. */
  readonly retryable: boolean;
  readonly notFound: boolean;
  readonly unavailable: boolean;
}

export interface Handshake {
  protocol: string;
  software: string;
  protocol_version: number;
  pid: number;
  started_ms: number;
  max_frame: number;
  task_threads: number;
  cache_entries: number;
  cache_ttl_ms: number;
  endpoint: string;
  data_dir: string;
  features: string[];
  capabilities: {
    tls: boolean;
    tls_backend: string;
    formats: number;
    methods: number;
    installed_extensions: number;
    repositories: number;
    allow_shutdown: boolean;
  };
  engines: Array<{
    name: string;
    status: string;
    workers: number;
    workers_configured: number;
    formats: string[];
  }>;
}

export interface FormatInfo {
  id: string;
  label: string;
  manager_id: string;
  engine: 'jvm' | 'js' | 'rule' | 'native' | string;
  artifact_kinds: string[];
  identify_markers: string[];
  capabilities: string[];
  platforms: string[];
  notes?: string;
}

export interface DetectResult {
  format: string;
  manager_id: string;
  kind: string;
  confidence: number;
  evidence: string;
  entry_point?: string;
}

export interface ExtensionInfo {
  id: string;
  name: string;
  version: string;
  manager_id: string;
  format_id: string;
  path: string;
  kind: string;
  installed_ms: number;
  size: number;
  installed: boolean;
}

export interface MediaItem {
  name: string;
  url: string;
  cover?: string;
  author?: string;
  [key: string]: unknown;
}

export interface ListResult {
  list: MediaItem[];
  has_next: boolean;
  page: number;
  total: number;
}

export interface Episode {
  name: string;
  url: string;
  index: number;
  [key: string]: unknown;
}

export interface DetailResult {
  name: string;
  url: string;
  cover?: string;
  author?: string;
  description?: string;
  episodes: Episode[];
  episode_count: number;
  [key: string]: unknown;
}

export interface ContentResult {
  content: string;
  url: string;
  length: number;
  next_url?: string;
}

export interface Metrics {
  uptime_ms: number;
  server: Record<string, number>;
  cache: Record<string, number>;
  engines: Array<Record<string, unknown>>;
  memory: {
    allocated_bytes: number;
    allocated_blocks: number;
    rss_bytes: number;
    peak_rss_bytes: number;
  };
}

export interface CallOptions {
  /** Milliseconds; 0 means "no deadline". */
  timeoutMs?: number;
  /** Cache lifetime for this call; 0 bypasses the cache. */
  cacheTtlMs?: number;
  /** Join an identical in-flight call instead of starting a second one. */
  coalesce?: boolean;
}

export interface SpawnOptions {
  dataDir?: string;
  endpoint?: string;
  extraArgs?: string[];
  /** How long to wait for the daemon's ready line. */
  timeoutMs?: number;
}

export declare class Bridge {
  constructor(endpoint?: string, timeoutMs?: number);

  /** Handshake response, available after connect()/spawn(). */
  readonly hello: Handshake;
  readonly endpoint: string;
  readonly timeoutMs: number;

  static connect(endpoint: string, timeoutMs?: number): Promise<Bridge>;
  static spawn(binary?: string, options?: SpawnOptions): Promise<Bridge>;

  call<T = unknown>(method: string, params?: object, options?: CallOptions): Promise<T>;
  stream<T = unknown>(method: string, params?: object, options?: { timeoutMs?: number }): AsyncGenerator<T, void, void>;
  cancel(requestId: string): void;
  /** Round-trip time of one `bridge.ping`, in milliseconds. */
  ping(): Promise<number>;

  metrics(): Promise<Metrics>;
  formats(): Promise<FormatInfo[]>;
  detect(path: string): Promise<DetectResult>;
  repositories(): Promise<Array<Record<string, unknown>>>;
  addRepo(url: string, managerId?: string, name?: string): Promise<Record<string, unknown>>;
  removeRepo(repoId: string): Promise<Record<string, unknown>>;
  refreshRepo(repoId?: string, url?: string): Promise<Record<string, unknown>>;
  extensions(managerId?: string): Promise<ExtensionInfo[]>;
  install(options: { path?: string; url?: string; managerId?: string }): Promise<ExtensionInfo>;
  uninstall(extId: string): Promise<Record<string, unknown>>;
  sourceMethods(sourceId: string): Promise<Record<string, unknown>>;
  popular(sourceId: string, page?: number): Promise<ListResult>;
  latest(sourceId: string, page?: number): Promise<ListResult>;
  search(sourceId: string, query: string, page?: number, filters?: unknown[]): Promise<ListResult>;
  detail(sourceId: string, media: string | { url: string }): Promise<DetailResult>;
  videoList(sourceId: string, episode: string | { url: string }): Promise<Record<string, unknown>>;
  videoListStream(sourceId: string, episode: string | { url: string }): AsyncGenerator<unknown, void, void>;
  pageList(sourceId: string, episode: string | { url: string }): Promise<Record<string, unknown>>;
  /** `novelContent(src, url)` is the short form used by most hosts. */
  novelContent(sourceId: string, title: string, chapterId?: string): Promise<ContentResult>;

  readonly admin: {
    stats(): Promise<Record<string, unknown>>;
    logs(limit?: number): Promise<Record<string, unknown>>;
  };

  close(): Promise<void>;
}

/** Retry a call on retryable errors with exponential backoff. */
export declare function retry<T>(fn: () => Promise<T>, attempts?: number, delayMs?: number): Promise<T>;
