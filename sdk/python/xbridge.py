"""xbridge — Python client for the Multi-Extension Runtime Bridge (XBP/1).

Works on Linux, macOS, Windows and BSD with no third-party dependency.

    from xbridge import Bridge

    with Bridge.spawn("./build/xbridged") as bridge:
        print(bridge.formats())
        for ext in bridge.extensions():
            print(ext["id"], ext["manager_id"])
        results = bridge.search(ext["id"], "one piece")
        detail = bridge.detail(ext["id"], results["list"][0])
        pages = bridge.page_list(ext["id"], detail["episodes"][0])

The bridge daemon owns deadlines, caching and cancellation, so this client stays
thin: framing, correlation and streaming.
"""

from __future__ import annotations

import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Callable, Dict, Iterator, List, Optional

__all__ = ["Bridge", "BridgeError", "TimeoutError", "Frame", "SPAWN_LOCK"]
__version__ = "1.0.0"

PROTOCOL = "XBP/1"

# Frame types — keep in sync with core/include/bridge/server.h
HELLO = 0x01
HELLO_ACK = 0x02
REQUEST = 0x03
RESPONSE = 0x04
STREAM_CHUNK = 0x05
STREAM_END = 0x06
STREAM_ERR = 0x07
CANCEL = 0x08
PING = 0x09
PONG = 0x0A
ERROR = 0x0B
BYE = 0x0C

# Error codes — stable across SDKs.
ERR_PARSE = -32700
ERR_INVALID = -32600
ERR_NO_METHOD = -32601
ERR_PARAMS = -32602
ERR_ENGINE = -32000
ERR_DEADLINE = -32001
ERR_CANCELLED = -32002
ERR_NOT_FOUND = -32003
ERR_UNAVAILABLE = -32004
ERR_NETWORK = -32005
ERR_REJECTED = -32006
ERR_LIMIT = -32007

_RETRYABLE = {ERR_ENGINE, ERR_DEADLINE, ERR_UNAVAILABLE, ERR_NETWORK}

# Serialises endpoint creation across threads in the same process.
SPAWN_LOCK = threading.Lock()


class BridgeError(Exception):
    """A structured error returned by the daemon."""

    def __init__(self, code: int, message: str, meta: Optional[dict] = None):
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.message = message
        self.meta = meta or {}

    @property
    def retryable(self) -> bool:
        return self.code in _RETRYABLE

    @property
    def not_found(self) -> bool:
        return self.code == ERR_NOT_FOUND

    @property
    def unavailable(self) -> bool:
        return self.code == ERR_UNAVAILABLE


class TimeoutError(BridgeError):
    def __init__(self, message: str = "deadline exceeded", meta: Optional[dict] = None):
        super().__init__(ERR_DEADLINE, message, meta)


class Frame:
    __slots__ = ("type", "payload")

    def __init__(self, ftype: int, payload: dict):
        self.type = ftype
        self.payload = payload

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"Frame(0x{self.type:02x}, {self.payload!r})"


# --------------------------------------------------------------------------- #
# transports
# --------------------------------------------------------------------------- #


def _connect_unix(path: str, abstract: bool = False, timeout: float = 10.0) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    if abstract:
        s.connect("\0" + path)
    else:
        s.connect(path)
    return s


def _connect_tcp(host: str, port: int, timeout: float = 10.0) -> socket.socket:
    s = socket.create_connection((host or "127.0.0.1", port), timeout=timeout)
    s.settimeout(None)
    return s


def _connect_npipe(name: str, timeout: float = 10.0) -> socket.socket:
    if sys.platform != "win32":  # pragma: no cover
        raise BridgeError(ERR_INVALID, "named pipes are only available on Windows")
    import pywintypes  # type: ignore  # pragma: no cover
    import win32file  # type: ignore  # pragma: no cover
    import win32pipe  # type: ignore  # pragma: no cover

    deadline = time.time() + timeout
    while True:  # pragma: no cover - Windows only
        try:
            handle = win32file.CreateFile(
                name,
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0, None, win32file.OPEN_EXISTING, 0, None,
            )
            win32pipe.SetNamedPipeHandleState(
                handle, win32pipe.PIPE_READMODE_BYTE, None, None
            )
            return _HandleSocket(handle)
        except pywintypes.error:
            if time.time() > deadline:
                raise BridgeError(ERR_UNAVAILABLE, f"cannot connect to {name}")
            time.sleep(0.02)


class _HandleSocket:
    """Minimal socket-like wrapper over a Win32 pipe handle."""  # pragma: no cover

    def __init__(self, handle):
        self._h = handle

    def sendall(self, data: bytes) -> None:  # pragma: no cover
        import win32file

        win32file.WriteFile(self._h, data)

    def recv(self, n: int) -> bytes:  # pragma: no cover
        import win32file

        _, data = win32file.ReadFile(self._h, n)
        return data

    def close(self) -> None:  # pragma: no cover
        try:
            self._h.Close()
        except Exception:
            pass


def connect(endpoint: str, timeout: float = 10.0):
    """Connect to an endpoint URI: unix://, unix-abstract://, npipe://, tcp://."""
    if endpoint.startswith("unix-abstract://"):
        return _connect_unix(endpoint[len("unix-abstract://"):], abstract=True, timeout=timeout)
    if endpoint.startswith("unix://"):
        return _connect_unix(endpoint[len("unix://"):], timeout=timeout)
    if endpoint.startswith("npipe://"):
        return _connect_npipe(endpoint[len("npipe://"):], timeout=timeout)
    if endpoint.startswith("tcp://"):
        rest = endpoint[len("tcp://"):]
        host, _, port = rest.rpartition(":")
        return _connect_tcp(host, int(port), timeout=timeout)
    # Bare path
    return _connect_unix(endpoint, timeout=timeout)


# --------------------------------------------------------------------------- #
# client
# --------------------------------------------------------------------------- #


class Bridge:
    """A connection to an xbridged daemon.

    Use it as a context manager. `Bridge.spawn()` starts a daemon and connects
    to it, cleaning both up on exit.
    """

    def __init__(self, endpoint: str = "", timeout: float = 60.0, socket=None):
        self.endpoint = endpoint
        self.timeout = timeout
        self._sock = socket
        self._lock = threading.Lock()
        self._counter = 0
        self._closed = False
        self.hello: Dict[str, Any] = {}
        self._proc: Optional[subprocess.Popen] = None
        self._proc_log: Optional[Any] = None

    # -- lifecycle --------------------------------------------------------- #

    @classmethod
    def spawn(
        cls,
        binary: str = "xbridged",
        *,
        data_dir: Optional[str] = None,
        endpoint: Optional[str] = None,
        extra_args: Optional[List[str]] = None,
        timeout: float = 20.0,
        log_file: Optional[str] = None,
    ) -> "Bridge":
        """Start a daemon and connect to it.

        The endpoint is resolved before starting so that parallel spawns in the
        same process never race for the same socket path.
        """
        with SPAWN_LOCK:
            if endpoint is None:
                fd, path = tempfile.mkstemp(prefix="xbridge-", suffix=".sock")
                os.close(fd)
                os.unlink(path)
                endpoint = f"unix://{path}"

            args = [binary, "--endpoint", endpoint]
            if data_dir:
                args += ["--data-dir", data_dir]
            args += extra_args or []

            env = dict(os.environ)
            env.setdefault("RUST_BACKTRACE", "0")
            log = open(log_file, "wb") if log_file else subprocess.DEVNULL
            proc = subprocess.Popen(
                args, stdout=subprocess.PIPE, stderr=log, env=env
            )

        ready = None
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = proc.stdout.readline()  # type: ignore[union-attr]
            if not line:
                break
            try:
                msg = json.loads(line.decode("utf-8", "replace"))
            except ValueError:
                continue
            if isinstance(msg, dict) and msg.get("event") == "ready":
                ready = msg
                break

        if ready is None:
            proc.kill()
            raise BridgeError(ERR_UNAVAILABLE, f"daemon did not become ready ({binary})")

        sock = connect(ready["endpoint"], timeout=timeout)
        bridge = cls(endpoint=ready["endpoint"], socket=sock)
        bridge._proc = proc
        bridge._proc_log = log
        bridge._hello()
        return bridge

    @classmethod
    def connect(cls, endpoint: str, timeout: float = 10.0) -> "Bridge":
        """Connect to an already-running daemon and complete the handshake."""
        sock = connect(endpoint, timeout=timeout)
        bridge = cls(endpoint=endpoint, timeout=timeout, socket=sock)
        try:
            bridge._hello()
        except Exception:
            bridge.close()
            raise
        return bridge

    @property
    def handshake(self) -> Dict[str, Any]:
        """Alias for `hello`, for hosts that call the response a handshake."""
        return self.hello

    def _hello(self) -> None:
        if self._sock is None:
            raise BridgeError(
                ERR_UNAVAILABLE,
                "this Bridge has no transport: use Bridge.connect(endpoint) "
                "or Bridge.spawn(...)",
            )
        self.hello = self._call_raw(HELLO, {
            "protocol": PROTOCOL,
            "client": f"python-sdk/{__version__}",
            "features": ["stream", "cancel"],
        }, expect=HELLO_ACK)

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._send_frame(BYE, {"reason": "client closing"})
        except Exception:
            pass
        try:
            self._sock.close()
        except Exception:
            pass
        if self._proc is not None:
            try:
                self._proc.wait(timeout=5)
            except Exception:
                self._proc.kill()
        if self._proc_log is not None:
            try:
                self._proc_log.close()
            except Exception:
                pass
        # Only a client that *spawned* the daemon may remove its socket. A
        # plain Bridge.connect() must leave the endpoint alone, otherwise
        # closing one connection would take the daemon's address away from
        # every other client on the machine.
        if self._proc is not None and self.endpoint.startswith("unix://") \
                and os.path.exists(self.endpoint[7:]):
            try:
                os.unlink(self.endpoint[7:])
            except OSError:
                pass

    def __enter__(self) -> "Bridge":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    # -- framing ----------------------------------------------------------- #

    def _send_frame(self, ftype: int, payload: dict) -> None:
        body = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        header = struct.pack(">IB", len(body) + 1, ftype)
        with self._lock:
            self._sock.sendall(header + body)

    def _recv_exact(self, n: int) -> bytes:
        chunks = []
        remaining = n
        while remaining > 0:
            chunk = self._sock.recv(remaining)
            if not chunk:
                raise BridgeError(ERR_UNAVAILABLE, "connection closed by daemon")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def read_frame(self) -> Frame:
        head = self._recv_exact(5)
        length, ftype = struct.unpack(">IB", head)
        if length < 1:
            raise BridgeError(ERR_PARSE, "zero-length frame")
        body = self._recv_exact(length - 1) if length > 1 else b""
        try:
            payload = json.loads(body.decode("utf-8")) if body else {}
        except ValueError:
            raise BridgeError(ERR_PARSE, "daemon sent invalid JSON")
        return Frame(ftype, payload)

    # -- request plumbing -------------------------------------------------- #

    def next_id(self, prefix: str = "py") -> str:
        with self._lock:
            self._counter += 1
            return f"{prefix}-{self._counter}"

    def _call_raw(self, ftype: int, payload: dict, expect: int) -> dict:
        self._send_frame(ftype, payload)
        frame = self.read_frame()
        if frame.type != expect:
            if frame.type == ERROR:
                raise BridgeError(
                    frame.payload.get("code", ERR_INVALID),
                    frame.payload.get("message", "protocol error"),
                )
            raise BridgeError(ERR_INVALID, f"expected frame 0x{expect:02x}, got 0x{frame.type:02x}")
        return frame.payload

    def call(
        self,
        method: str,
        params: Optional[dict] = None,
        *,
        timeout_ms: int = 0,
        cache_ttl_ms: Optional[int] = None,
        coalesce: bool = True,
    ) -> Any:
        """Invoke a method and return its result.

        Raises BridgeError (or TimeoutError) on failure.
        """
        req_id = self.next_id()
        payload: Dict[str, Any] = {
            "id": req_id,
            "method": method,
            "params": params or {},
        }
        if timeout_ms:
            payload["deadline_ms"] = timeout_ms
        if cache_ttl_ms is not None:
            payload["cache"] = {"ttl_ms": cache_ttl_ms}
        if not coalesce:
            payload["coalesce"] = False

        self._send_frame(REQUEST, payload)

        while True:
            frame = self.read_frame()
            if frame.type == RESPONSE and frame.payload.get("id") == req_id:
                if frame.payload.get("ok"):
                    return frame.payload.get("result")
                err = frame.payload.get("error", {})
                code = err.get("code", ERR_ENGINE)
                msg = err.get("message", "engine error")
                if code == ERR_DEADLINE:
                    raise TimeoutError(msg, frame.payload.get("meta"))
                raise BridgeError(code, msg, frame.payload.get("meta"))
            if frame.type == ERROR:
                raise BridgeError(
                    frame.payload.get("code", ERR_INVALID),
                    frame.payload.get("message", "protocol error"),
                )
            if frame.type == PING:
                self._send_frame(PONG, frame.payload)
            # Anything else (a chunk for a concurrent stream, a stray response)
            # is not ours: keep reading rather than desynchronising the stream.

    def stream(
        self, method: str, params: Optional[dict] = None, *, timeout_ms: int = 0
    ) -> Iterator[Any]:
        """Invoke a streaming method, yielding each chunk."""
        req_id = self.next_id("stream")
        payload: Dict[str, Any] = {
            "id": req_id,
            "method": method,
            "params": params or {},
            "stream": True,
        }
        if timeout_ms:
            payload["deadline_ms"] = timeout_ms
        self._send_frame(REQUEST, payload)

        while True:
            frame = self.read_frame()
            if frame.type == STREAM_CHUNK and frame.payload.get("id") == req_id:
                yield frame.payload.get("chunk")
            elif frame.type == STREAM_END and frame.payload.get("id") == req_id:
                return
            elif frame.type == STREAM_ERR and frame.payload.get("id") == req_id:
                err = frame.payload.get("error", {})
                raise BridgeError(err.get("code", ERR_ENGINE), err.get("message", "stream error"))
            elif frame.type == PING:
                self._send_frame(PONG, frame.payload)

    def cancel(self, request_id: str) -> None:
        self._send_frame(CANCEL, {"id": request_id})

    def ping(self) -> float:
        t0 = time.time()
        self.call("bridge.ping")
        return (time.time() - t0) * 1000.0

    # -- convenience wrappers ---------------------------------------------- #

    def metrics(self) -> dict:
        return self.call("bridge.metrics")

    def formats(self) -> List[dict]:
        return self.call("format.list")["formats"]

    def detect(self, path: str) -> dict:
        return self.call("format.detect", {"path": path})

    def repositories(self) -> List[dict]:
        return self.call("repo.list")["repos"]

    def add_repo(self, url: str, manager_id: str = "aniyomi", name: str = "") -> dict:
        params = {"url": url, "manager_id": manager_id}
        if name:
            params["name"] = name
        return self.call("repo.add", params)

    def remove_repo(self, repo_id: str) -> dict:
        return self.call("repo.remove", {"id": repo_id})

    def refresh_repo(self, repo_id: str = "", url: str = "") -> dict:
        params = {}
        if repo_id:
            params["id"] = repo_id
        if url:
            params["url"] = url
        return self.call("repo.refresh", params)

    def extensions(self, manager_id: str = "") -> List[dict]:
        params = {"manager_id": manager_id} if manager_id else {}
        return self.call("extension.list", params)["extensions"]

    def install(self, path: str = "", url: str = "", manager_id: str = "") -> dict:
        params: Dict[str, Any] = {}
        if path:
            params["path"] = path
        if url:
            params["url"] = url
        if manager_id:
            params["manager_id"] = manager_id
        return self.call("extension.install", params)

    def uninstall(self, ext_id: str) -> dict:
        return self.call("extension.uninstall", {"id": ext_id})

    def source_methods(self, source_id: str) -> dict:
        return self.call("source.methods", {"source_id": source_id})

    def popular(self, source_id: str, page: int = 1) -> dict:
        return self.call("source.getPopular", {"source_id": source_id, "page": page})

    def latest(self, source_id: str, page: int = 1) -> dict:
        return self.call("source.getLatestUpdates", {"source_id": source_id, "page": page})

    def search(self, source_id: str, query: str, page: int = 1, filters=None) -> dict:
        return self.call(
            "source.search",
            {"source_id": source_id, "query": query, "page": page, "filters": filters or []},
        )

    def detail(self, source_id: str, media) -> dict:
        media = {"url": media} if isinstance(media, str) else dict(media)
        return self.call("source.getDetail", {"source_id": source_id, "media": media})

    def video_list(self, source_id: str, episode) -> dict:
        episode = {"url": episode} if isinstance(episode, str) else dict(episode)
        return self.call("source.getVideoList", {"source_id": source_id, "episode": episode})

    def video_list_stream(self, source_id: str, episode: dict) -> Iterator[Any]:
        return self.stream(
            "source.getVideoListStream", {"source_id": source_id, "episode": episode}
        )

    def page_list(self, source_id: str, episode: dict) -> dict:
        return self.call("source.getPageList", {"source_id": source_id, "episode": episode})

    def novel_content(self, source_id: str, title: str, chapter_id: str = "") -> dict:
        """Fetch a chapter.

        `novel_content(src, url)` is the two-argument short form used by most
        hosts: the chapter id *is* its URL for every rule and web engine.
        """
        if not chapter_id:
            return self.call("source.getNovelContent",
                             {"source_id": source_id, "id": title})
        return self.call(
            "source.getNovelContent",
            {"source_id": source_id, "title": title, "id": chapter_id},
        )

    def get_preference(self, source_id: str) -> dict:
        return self.call("source.getPreference", {"source_id": source_id})

    def set_preference(self, source_id: str, key: str, value: Any) -> dict:
        return self.call(
            "source.setPreference", {"source_id": source_id, "key": key, "value": value}
        )


def retry(callable_: Callable[[], Any], attempts: int = 3, delay: float = 0.25) -> Any:
    """Retry a bridge call on retryable errors with exponential backoff."""
    last: Optional[Exception] = None
    for i in range(attempts):
        try:
            return callable_()
        except BridgeError as exc:
            if not exc.retryable:
                raise
            last = exc
            time.sleep(delay * (2 ** i))
    assert last is not None
    raise last
