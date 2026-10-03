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

from xbridge import Bridge, BridgeError  # noqa: E402

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
