#!/usr/bin/env python3
"""js_engine.py — prove that a *worker-hosted* engine works end to end.

The rest of the suite exercises the built-in rule engine, which lives in the
daemon's own process. This one starts the daemon with a real Node.js worker
(`core/workers/js_worker.js`), loads an extension into it, and drives the
unified `source.*` surface through it:

    daemon  ──XBP/1 over stdio──▶  node js_worker.js  ──▶  the extension module

That is the path every JavaScript ecosystem (LnReader, Mangayomi, Sora,
Tsundoku, iReader, Kotatsu's JS parsers) takes, so it is the one worth proving.

    python3 tests/e2e/js_engine.py           # skips cleanly when node is absent
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "sdk", "python"))

from xbridge import (  # noqa: E402
    REQUEST, RESPONSE, STREAM_CHUNK, STREAM_END, STREAM_ERR,
    Bridge, BridgeError,
)

PASSED = 0
FAILED = 0
STEP = {"name": ""}

EXTENSION = """\
/* A source in the shape the js worker expects: one module, async methods. */
module.exports = {
  name: 'Fixture JS Source',
  version: '1.0.2',
  async search(query, page) {
    const all = [
      { name: 'Solo Leveling', url: 'https://example.tld/book/1', author: 'Chugong' },
      { name: 'Omniscient Reader', url: 'https://example.tld/book/2', author: 'Sing Shong' },
      { name: 'The Beginning After The End', url: 'https://example.tld/book/3', author: 'TurtleMe' },
    ];
    const hits = query ? all.filter((b) => b.name.toLowerCase().includes(query.toLowerCase())) : all;
    return { list: hits, has_next: false, page: page || 1, total: hits.length };
  },
  async getDetail(media) {
    if (media && media.slow) await new Promise((r) => setTimeout(r, 1200));
    return {
      ...media,
      name: 'Solo Leveling',
      author: 'Chugong',
      description: 'served by the JavaScript engine',
      episodes: [
        { name: 'Chapter 1', url: media.url + '/1', index: 0 },
        { name: 'Chapter 2', url: media.url + '/2', index: 1 },
      ],
      episode_count: 2,
    };
  },
  async *getVideoListStream(episode) {
    if (episode && episode.slow) {
      /* One item every 25 ms, long enough to be cancelled or to time out. */
      for (let i = 0; i < 30; i++) {
        await new Promise((r) => setTimeout(r, 25));
        yield { url: 'https://cdn.example.tld/part-' + i + '.mp4', quality: 'part-' + i };
      }
      return;
    }
    yield { url: 'https://cdn.example.tld/720.mp4', quality: '720p' };
    yield { url: 'https://cdn.example.tld/1080.mp4', quality: '1080p' };
  },
  async getNovelContent(id) {
    return { content: 'Text produced by the JavaScript engine for ' + id, url: id, length: 46 };
  },
};
"""


def check(cond, msg):
    global PASSED, FAILED
    if cond:
        PASSED += 1
        print(f"    ok   {msg}")
    else:
        FAILED += 1
        print(f"    FAIL {msg}   [{STEP['name']}]")


def eq(actual, expected, msg):
    check(actual == expected, f"{msg} (got {actual!r}, want {expected!r})")


def raw_request(bridge, method, params, **extra):
    """Send a REQUEST frame and return the RESPONSE payload.

    The SDK hides the request id; cancellation and deadlines are wire-level
    contracts, so these tests speak the protocol directly."""
    rid = bridge.next_id("raw")
    payload = {"id": rid, "method": method, "params": params}
    payload.update(extra)
    bridge._send_frame(REQUEST, payload)
    while True:
        frame = bridge.read_frame()
        if frame.type == RESPONSE:
            return frame.payload


def raw_stream_cancel(bridge, method, params, cancel_after=1):
    """Start a stream, cancel it after N chunks, report what happened.

    Returns (terminal_kind, terminal_payload, chunks_seen, seconds_from_cancel).
    """
    rid = bridge.next_id("rawc")
    bridge._send_frame(REQUEST, {"id": rid, "method": method, "params": params,
                                 "stream": True})
    seen = 0
    cancelled_at = None
    t0 = time.time()
    while time.time() - t0 < 15:
        frame = bridge.read_frame()
        if frame.payload.get("id") != rid:
            continue                      # a stale frame from an earlier test
        if frame.type == STREAM_CHUNK:
            seen += 1
            if seen == cancel_after and cancelled_at is None:
                bridge.cancel(rid)
                cancelled_at = time.time()
        elif frame.type in (STREAM_END, STREAM_ERR):
            took = (time.time() - cancelled_at) if cancelled_at else 0.0
            return frame.type, frame.payload, seen, took
    return None, {}, seen, 0.0


def wait_for_socket(path, timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.01)
    return False


def main():
    global PASSED, FAILED
    if shutil.which("node") is None:
        print("node not installed; skipping the js engine test")
        return 0

    daemon = os.path.join(ROOT, "build", "xbridged")
    if not os.path.exists(daemon):
        print(f"daemon not found: {daemon} (run `make` first)")
        return 2

    tmp = tempfile.mkdtemp(prefix="xbridge-js-")
    endpoint = os.path.join(tmp, "x.sock")
    worker = os.path.join(ROOT, "core", "workers", "js_worker.js")
    ext_path = os.path.join(tmp, "fixture-source.js")
    with open(ext_path, "w", encoding="utf-8") as fh:
        fh.write(EXTENSION)

    env = dict(os.environ)
    # The daemon takes a whole command line here, so a worker that needs
    # arguments works without a wrapper script.
    env["XBRIDGE_JS_WORKER"] = f"node {worker} {{}}"

    log = open(os.path.join(tmp, "daemon.log"), "wb")
    proc = subprocess.Popen(
        [daemon, "--endpoint", f"unix://{endpoint}", "--data-dir",
         os.path.join(tmp, "data"), "--workers", "1", "--log-level", "info"],
        stdout=log, stderr=log, env=env)

    if not wait_for_socket(endpoint):
        print("daemon did not start with a js worker")
        return 1

    src = "lnreader/fixture"
    try:
        bridge = Bridge.connect(f"unix://{endpoint}")

        STEP["name"] = "engine discovery"
        print("\nengine discovery")
        engines = {e["name"]: e for e in bridge.hello["engines"]}
        check("js" in engines, "js engine advertised in the handshake")
        eq(engines["js"]["status"], "ready", "js worker started")
        listing = bridge.call("engine.list")
        names = [e["name"] for e in listing["engines"]]
        check("js" in names and "rule" in names, f"engine.list reports {names}")
        js = next(e for e in listing["engines"] if e["name"] == "js")
        check(not js["in_process"], "js engine is out of process")
        check("lnreader" in js["formats"], "js engine covers lnreader")

        STEP["name"] = "loading an extension into the worker"
        print("\nloading an extension into the worker")
        loaded = bridge.call("engine.load", {"id": src, "path": ext_path})
        eq(loaded["id"], src, "extension id")
        eq(loaded["version"], "1.0.2", "extension version read from the module")
        check("search" in loaded["methods"], f"methods detected: {loaded['methods']}")
        exts = bridge.extensions()
        check(any(e["id"] == src for e in exts), "worker-hosted source appears in extension.list")

        STEP["name"] = "source.*"
        print("\nsource.* through the worker")
        found = bridge.search(src, "solo")
        eq(found["total"], 1, "search filtered by query")
        eq(found["list"][0]["name"], "Solo Leveling", "book name from the JS module")

        every = bridge.search(src, "")
        eq(len(every["list"]), 3, "empty query returns everything")

        detail = bridge.detail(src, found["list"][0]["url"])
        eq(detail["name"], "Solo Leveling", "detail name")
        eq(detail["description"], "served by the JavaScript engine", "detail description")
        eq(len(detail["episodes"]), 2, "chapter list")

        content = bridge.novel_content(src, detail["episodes"][0]["url"])
        check("JavaScript engine" in content["content"], "chapter text from the JS module")

        STEP["name"] = "streaming from an async iterator"
        print("\nstreaming from an async iterator")
        chunks = list(bridge.stream("source.getVideoListStream",
                                    {"source_id": src, "episode": {"url": "x"}}))
        eq(len(chunks), 2, "two async-iterator chunks forwarded")
        eq(chunks[1]["quality"], "1080p", "chunk payload preserved")

        streamed = list(bridge.stream("source.search", {"source_id": src, "query": ""}))
        check(any(c.get("summary") for c in streamed), "one-shot results are chunked with a summary")

        STEP["name"] = "cancelling a running stream"
        print("\ncancelling a running stream")
        kind, payload, seen, took = raw_stream_cancel(
            bridge, "source.getVideoListStream",
            {"source_id": src, "episode": {"url": "x", "slow": True}})
        eq(kind, STREAM_ERR, "a cancelled stream ends with STREAM_ERR, not a clean end")
        eq((payload.get("error") or {}).get("code"), -32002, "cancel reported as -32002")
        check(took < 1.0, f"cancel took effect promptly ({took * 1000:.0f} ms)")
        check(seen <= 3, f"production stopped early ({seen} chunks before the terminal frame)")

        # A cancel for an id that is not in flight must be a no-op.
        bridge.cancel("no-such-request")
        check(bridge.ping() < 1000, "daemon healthy after cancelling an unknown id")

        # Cancelling a plain (non-streaming) call terminates it with -32002.
        rid = bridge.next_id("rawns")
        bridge._send_frame(REQUEST, {
            "id": rid, "method": "source.getDetail",
            "params": {"source_id": src, "media": {"url": "x", "slow": True}}})
        time.sleep(0.15)
        t0 = time.time()
        bridge.cancel(rid)
        resp = None
        while time.time() - t0 < 10:
            frame = bridge.read_frame()
            if frame.type == RESPONSE and frame.payload.get("id") == rid:
                resp = frame.payload
                break
        waited = time.time() - t0
        check(resp is not None, "a cancelled call still gets exactly one RESPONSE")
        if resp is not None:
            eq((resp.get("error") or {}).get("code"), -32002, "non-stream cancel -> -32002")
        check(waited < 1.0, f"non-stream cancel took effect promptly ({waited * 1000:.0f} ms)")

        STEP["name"] = "deadlines"
        print("\ndeadlines")
        t0 = time.time()
        resp = raw_request(bridge, "source.getDetail",
                           {"source_id": src, "media": {"url": "deadline", "slow": True}},
                           deadline_ms=250)
        elapsed = time.time() - t0
        check(resp.get("ok") is False, "a call past its deadline is not reported as ok")
        eq((resp.get("error") or {}).get("code"), -32001, "deadline -> -32001")
        check(elapsed < 0.9, f"deadline fired on time ({elapsed * 1000:.0f} ms, work takes 1200 ms)")

        t0 = time.time()
        try:
            list(bridge.stream("source.getVideoListStream",
                               {"source_id": src, "episode": {"url": "x", "slow": True}},
                               timeout_ms=250))
            check(False, "a slow stream must not outlive its deadline")
        except (BridgeError, TimeoutError) as e:
            eq(getattr(e, "code", -32001), -32001, "stream deadline -> -32001")
        elapsed = time.time() - t0
        check(elapsed < 0.9, f"stream deadline fired on time ({elapsed * 1000:.0f} ms)")

        check(bridge.ping() < 1000, "daemon healthy after a deadline")
        found = bridge.search(src, "solo")
        eq(found["total"], 1, "the worker still serves requests after cancel and deadlines")

        STEP["name"] = "coalescing"
        print("\ncoalescing")
        # Two byte-identical calls collapse into one engine job. Cancelling that
        # job must end both with the same error code, not a generic -32000.
        params = {"source_id": src, "media": {"url": "coalesce", "slow": True}}
        rid_a = bridge.next_id("coalesce")
        bridge._send_frame(REQUEST, {"id": rid_a, "method": "source.getDetail",
                                     "params": params})
        time.sleep(0.1)
        rid_b = bridge.next_id("coalesce")
        bridge._send_frame(REQUEST, {"id": rid_b, "method": "source.getDetail",
                                     "params": params})
        time.sleep(0.1)
        bridge.cancel(rid_a)
        answers = {}
        t0 = time.time()
        while len(answers) < 2 and time.time() - t0 < 10:
            frame = bridge.read_frame()
            if frame.type == RESPONSE and frame.payload.get("id") in (rid_a, rid_b):
                answers[frame.payload["id"]] = frame.payload
        eq(len(answers), 2, "both coalesced calls answered")
        for rid in (rid_a, rid_b):
            payload = answers.get(rid) or {}
            eq((payload.get("error") or {}).get("code"), -32002,
               f"coalesced call {rid} reports the owner's cancel")
        eq((answers.get(rid_b) or {}).get("meta", {}).get("coalesced"), True,
           "the second call joined the in-flight one")

        STEP["name"] = "cancellation and errors"
        print("\ncancellation and errors")
        try:
            bridge.search("lnreader/never-loaded", "x")
            check(False, "unknown source rejected")
        except BridgeError as e:
            eq(e.code, -32003, "unknown source -> not found")

        try:
            bridge.call("engine.load", {"path": os.path.join(tmp, "missing.js")})
            check(False, "broken extension rejected")
        except BridgeError as e:
            check(e.code in (-32000, -32003, -32006), f"load failure -> {e.code}")

        check(bridge.ping() < 1000, "daemon healthy after worker errors")

        STEP["name"] = "unload"
        print("\nunload")
        removed = bridge.call("engine.unload", {"id": src})
        check(removed.get("removed") is True, "extension unloaded from the worker")
        try:
            bridge.search(src, "solo")
            check(True, "search after unload still routed (source record persists)")
        except BridgeError as e:
            check(e.code == -32000, f"unloaded source reports an engine error ({e.code})")

        bridge.close()
    except Exception:
        FAILED += 1
        print(f"    EXCEPTION in {STEP['name']}")
        traceback.print_exc()
    finally:
        log.flush()
        if FAILED:
            print("\n---- daemon log (tail) ----")
            with open(os.path.join(tmp, "daemon.log"), "r",
                      encoding="utf-8", errors="replace") as fh:
                print("".join(fh.readlines()[-20:]))
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\njs engine: {PASSED} passed, {FAILED} failed")
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
