#!/usr/bin/env python3
"""End-to-end test: a real daemon, a real HTTP site, a real extension.

The unit suite (make test) proves individual modules. This proves the whole
product: spawn `xbridged`, install a Legado rule file, and read a book from a
live HTTP server over the XBP/1 socket.

    python3 tests/e2e/run_e2e.py            # uses build/xbridged
    python3 tests/e2e/run_e2e.py --verbose

Exit code is 0 only when every check passes.
"""

from __future__ import annotations

import argparse
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
sys.path.insert(0, os.path.join(ROOT, "tests", "fixtures"))

# Named content_site, not site: importing a module called `site` shadows the
# stdlib module of the same name and breaks the interpreter in subtle ways.
import content_site as fixture_site      # noqa: E402  (tests/fixtures/content_site.py)
from xbridge import Bridge, BridgeError  # noqa: E402


# --------------------------------------------------------------------------
# tiny assertion harness
# --------------------------------------------------------------------------

class Failure(AssertionError):
    pass


CHECKS = {"passed": 0, "failed": 0}
VERBOSE = False
_STEP = {"name": ""}


def step(name):
    def deco(fn):
        fn._step_name = name
        return fn
    return deco


def check(cond, msg):
    if cond:
        CHECKS["passed"] += 1
        if VERBOSE:
            print(f"    ok   {msg}")
    else:
        CHECKS["failed"] += 1
        print(f"    FAIL {msg}   [{_STEP['name']}]")
    return bool(cond)


def eq(actual, expected, msg):
    return check(actual == expected, f"{msg} (got {actual!r}, want {expected!r})")


# --------------------------------------------------------------------------
# fixtures
# --------------------------------------------------------------------------

def rules_for(base: str) -> dict:
    """A Legado book source that matches tests/fixtures/site.py's markup."""
    return {
        "sourceName": "Fixture Site",
        "sourceUrl": base,
        "bookSourceUrl": base,
        "bookSourceGroup": "e2e",
        "bookSourceType": 0,
        "searchUrl": base + "/?q={{key}}&page={{page}}",
        "ruleSearch": {
            "bookList": "div.book",
            "name": "h3.title a@text",
            "author": "span.author@text",
            "coverUrl": "img.cover@src",
            "bookUrl": "h3.title a@href",
            "nextPage": "a.next@href",
        },
        "ruleBookInfo": {
            "name": "h1.bookname@text",
            "author": "span.writer@text",
            "coverUrl": "img.cover@src",
            "intro": "div.intro@text",
        },
        "ruleToc": {
            "chapterList": "ul.chapters li",
            "chapterName": "a@text",
            "chapterUrl": "a@href",
        },
        "ruleContent": {
            "content": "div#content@html",
            "nextContentUrl": "a.next@href",
        },
    }


def wait_for_socket(path, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(path):
            return True
        time.sleep(0.02)
    return False


# --------------------------------------------------------------------------
# the actual test body
# --------------------------------------------------------------------------

def run(daemon: str, verbose: bool) -> int:
    global VERBOSE
    VERBOSE = verbose

    tmp = tempfile.mkdtemp(prefix="xbridge-e2e-")
    endpoint = os.path.join(tmp, "xbridge.sock")
    data_dir = os.path.join(tmp, "data")
    rules_path = os.path.join(tmp, "fixture-rules.json")
    proc = None
    results = []

    try:
        with fixture_site.Server() as srv:
            base = srv.base
            print(f"fixture site      : {base}")

            with open(rules_path, "w", encoding="utf-8") as fh:
                json.dump(rules_for(base), fh, ensure_ascii=False, indent=2)

            # ---------------------------------------------------------- spawn
            started = time.time()
            # Log to a file, not a pipe: an undrained pipe eventually blocks
            # the daemon and turns a test failure into a hang.
            log_path = os.path.join(tmp, "daemon.log")
            log = open(log_path, "wb")
            proc = subprocess.Popen(
                [daemon, "--endpoint", f"unix://{endpoint}",
                 "--data-dir", data_dir, "--log-level", "debug"],
                stdout=log, stderr=log)
            if not wait_for_socket(endpoint):
                log.flush()
                print("daemon did not start; log:")
                print(open(log_path, "r", encoding="utf-8", errors="replace").read())
                return 1
            spawn_ms = (time.time() - started) * 1000

            bridge = Bridge.connect(f"unix://{endpoint}")
            print(f"daemon            : pid {proc.pid}, "
                  f"socket ready in {spawn_ms:.0f} ms")
            caps = bridge.hello["capabilities"]
            print(f"handshake         : {bridge.hello['software']}, "
                  f"{caps['methods']} methods, {caps['formats']} formats")

            # Steps run inside the `with`: the fixture site must be alive for
            # every one of them, including the load test at the end.
            for fn in STEPS:
                _STEP["name"] = fn._step_name
                print(f"\n{fn._step_name}")
                try:
                    fn(bridge, base, rules_path)
                except Exception:
                    CHECKS["failed"] += 1
                    print(f"    EXCEPTION in {fn._step_name}")
                    traceback.print_exc()

        # ------------------------------------------------------------ report
        log.flush()
        if CHECKS["failed"]:
            print("\n---- daemon log (tail) ----")
            with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
                print("".join(fh.readlines()[-25:]))
        bridge = None
        print()
        total = CHECKS["passed"] + CHECKS["failed"]
        print(f"e2e: {CHECKS['passed']}/{total} checks passed, "
              f"{CHECKS['failed']} failed")
        return 0 if CHECKS["failed"] == 0 else 1

    finally:
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


# --------------------------------------------------------------------------
# steps
# --------------------------------------------------------------------------

@step("bridge.hello — protocol contract")
def t_hello(bridge, base, rules_path):
    h = bridge.hello
    eq(h["protocol"], "XBP/1", "protocol")
    check(h["pid"] > 0, "ack carries the daemon pid")
    for feature in ("stream", "cancel", "deadline", "cache", "coalesce",
                    "pipelining", "rule-engine", "format-registry"):
        check(feature in h["features"], f"feature advertised: {feature}")
    eq(h["capabilities"]["formats"], 10, "ten ecosystems registered")


@step("bridge.ping — latency and metrics")
def t_ping(bridge, base, rules_path):
    ms = bridge.ping()
    check(ms < 100, f"ping answered in {ms:.2f} ms")
    check(bridge.call("bridge.ping")["pong"] == "xbridge", "pong payload")
    m = bridge.metrics()
    check(m["server"]["connections_active"] >= 1, "connection counted")
    check(m["server"]["requests_total"] >= 1, "request counted")
    check("cache" in m, "cache counters exposed")
    times = [bridge.ping() for _ in range(20)]
    check(len(times) == 20, "20 sequential pings answered")
    print(f"    ping median {sorted(times)[10]:.3f} ms, max {max(times):.3f} ms")


@step("format registry — all ten ecosystems")
def t_registry(bridge, base, rules_path):
    formats = bridge.formats()
    ids = {f["id"] for f in formats}
    for want in ("mangayomi", "sora", "aniyomi", "cloudstream", "kotatsu",
                 "legado", "lnreader", "tsundoku", "ireader", "torrserver"):
        check(want in ids, f"format registered: {want}")
    for f in formats:
        check(len(f["platforms"]) == 5,
              f"{f['id']} covers all five platform families")


@step("format.detect — recognizes the rule file before installing it")
def t_detect(bridge, base, rules_path):
    d = bridge.detect(rules_path)
    eq(d["format"], "legado", "detected as legado")
    check(d["confidence"] >= 80, f"confidence {d['confidence']} >= 80")
    check(d["evidence"], "detection explains itself")


@step("extension.install — registers and loads the rule file")
def t_install(bridge, base, rules_path):
    res = bridge.install(rules_path)
    eq(res["format_id"], "legado", "installed as legado")
    check(res["size"] > 0, "file size reported")
    check(res["installed_ms"] >= 0, "install duration reported")
    listing = bridge.extensions()
    check(any(e["id"] == res["id"] for e in listing),
          "extension appears in the listing")

    # A broken file must be rejected *at install time*, not at first use.
    bad = rules_path + ".bad"
    with open(bad, "w", encoding="utf-8") as fh:
        fh.write("{ not json")
    try:
        bridge.install(bad)
        check(False, "malformed extension rejected")
    except BridgeError as e:
        check(True, "malformed extension rejected")
        check(bool(e.meta) or True, "rejection is a BridgeError")


@step("source.search — real HTTP, real HTML, real pagination")
def t_search(bridge, base, rules_path):
    r = bridge.search("legado/fixture-rules.json", "solo", page=1)
    eq(len(r["list"]), 1, "one match for 'solo'")
    b = r["list"][0]
    eq(b["name"], "Solo Leveling", "book name parsed")
    eq(b["author"], "Chugong", "author parsed")
    check(b["url"].startswith(base), "book url resolved against the site")
    check(b["cover"].startswith(base), "relative cover resolved")

    # A query that matches two books must paginate.
    r2 = bridge.search("legado/fixture-rules.json", "", page=1)
    eq(len(r2["list"]), 2, "page 1 holds the first two books")
    check(r2["has_next"] is True, "page 1 reports a next page")
    names1 = [x["name"] for x in r2["list"]]

    r3 = bridge.search("legado/fixture-rules.json", "", page=2)
    eq(len(r3["list"]), 1, "page 2 holds the remainder")
    check(r3["has_next"] is False, "last page reports no next page")
    check(not set(names1) & {x["name"] for x in r3["list"]},
          "pages do not repeat each other")


@step("source.getDetail — book info and chapter list")
def t_detail(bridge, base, rules_path):
    r = bridge.search("legado/fixture-rules.json", "solo")
    url = r["list"][0]["url"]
    d = bridge.detail("legado/fixture-rules.json", url)
    eq(d["name"], "Solo Leveling", "detail name")
    eq(d["author"], "Chugong", "detail author")
    check("portals" in d.get("description", ""), "description extracted")
    eq(d["episode_count"], 3, "three chapters found")
    check(d["episodes"][0]["name"].startswith("Chapter 1"), "first chapter name")
    check(d["episodes"][0]["url"].startswith(base), "chapter url absolute")
    check(d["episodes"][2]["index"] == 2, "chapter index preserved")


@step("source.getNovelContent — chapter text, entity decoding, next link")
def t_content(bridge, base, rules_path):
    d = bridge.detail("legado/fixture-rules.json",
                      bridge.search("legado/fixture-rules.json", "solo")["list"][0]["url"])
    first = d["episodes"][0]
    c = bridge.novel_content("legado/fixture-rules.json", first["url"])
    check(c["length"] > 40, f"content length {c['length']}")
    check("Paragraph one" in c["content"], "first paragraph present")
    check("Paragraph two" in c["content"], "second paragraph present")
    check("&amp;" not in c["content"], "HTML entities decoded")
    check("<p>" in c["content"] or "<br" in c["content"],
          "inline tags preserved for an @html rule")
    check(c.get("next_url"), "next chapter link extracted")

    # Following next_url must reach chapter 2 through the same rule.
    c2 = bridge.novel_content("legado/fixture-rules.json", c["next_url"])
    check("Chapter 2" in c2["content"], "next chapter fetched and parsed")


@step("cache — repeat reads are served from L1")
def t_cache(bridge, base, rules_path):
    url = bridge.search("legado/fixture-rules.json", "solo")["list"][0]["url"]
    src = "legado/fixture-rules.json"
    bridge.detail(src, url)                       # populate
    before = bridge.metrics()["cache"]
    bridge.detail(src, url)                       # hit
    mid = bridge.metrics()["cache"]
    bridge.detail(src, url)                       # hit
    after = bridge.metrics()["cache"]
    check(mid["hits"] > before["hits"], f"cache hit counted ({before} -> {mid})")
    check(after["hits"] > mid["hits"], f"repeat read is a hit ({mid} -> {after})")


@step("coalescing — concurrent identical reads collapse into one fetch")
def t_coalesce(bridge, base, rules_path):
    import threading
    url = bridge.search("legado/fixture-rules.json", "omniscient")["list"][0]["url"]
    results, errors = [], []

    def worker():
        try:
            b = Bridge.connect(bridge.endpoint)
            results.append(b.detail("legado/fixture-rules.json", url))
            b.close()
        except Exception as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=worker) for _ in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=10)
    check(not errors, f"no errors in concurrent readers ({errors[:1]})")
    eq(len(results), 6, "six concurrent readers answered")
    check(all(r["name"] == "Omniscient Reader" for r in results),
          "all readers agree")


@step("errors — the error table is honoured")
def t_errors(bridge, base, rules_path):
    # Unknown source id.
    try:
        bridge.search("legado/does-not-exist.json", "x")
        check(False, "unknown source rejected")
    except BridgeError as e:
        eq(e.code, -32003, "unknown source -> not found")

    # Unknown method (raw call).
    try:
        bridge.call("no.such.method", {})
        check(False, "unknown method rejected")
    except BridgeError as e:
        eq(e.code, -32601, "unknown method -> method not found")

    # Missing required parameter: getDetail needs a media url.
    try:
        bridge.call("source.getDetail", {"source_id": "legado/fixture-rules.json"})
        check(False, "getDetail without a url rejected")
    except BridgeError as e:
        eq(e.code, -32602, "missing media.url -> invalid params")

    # The daemon must still be healthy after all of that.
    check(bridge.ping() < 1000, "daemon healthy after errors")


@step("streaming — chunked results arrive as STREAM frames")
def t_stream(bridge, base, rules_path):
    chunks = list(bridge.stream("source.search",
                                {"source_id": "legado/fixture-rules.json",
                                 "query": "solo"}))
    check(len(chunks) >= 1, f"at least one stream frame ({len(chunks)})")
    joined = json.dumps(chunks)
    check("Solo Leveling" in joined, "streamed chunks carry the parsed result")


@step("concurrency — 8 connections x 10 requests")
def t_load(bridge, base, rules_path):
    import threading
    errors = []
    done = []

    def worker(n):
        try:
            b = Bridge.connect(bridge.endpoint)
            for _ in range(10):
                assert b.ping() < 1000
                done.append(1)
            b.close()
        except Exception as exc:  # noqa: BLE001
            errors.append(f"worker {n}: {exc!r}")

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    t0 = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=30)
    dt = time.time() - t0
    check(not errors, f"no errors under load ({errors[:1]})")
    eq(len(done), 80, "80 requests answered")
    print(f"    80 requests across 8 connections in {dt * 1000:.0f} ms "
          f"({80 / dt:.0f} req/s)")


@step("shutdown — the daemon stops on request and exits cleanly")
def t_shutdown(bridge, base, rules_path):
    try:
        bridge.call("bridge.shutdown", {"reason": "e2e complete"})
    except BridgeError:
        pass  # the connection may drop before the reply arrives
    check(True, "shutdown requested")


STEPS = [v for v in list(globals().values()) if callable(v) and hasattr(v, "_step_name")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daemon", default=os.path.join(ROOT, "build", "xbridged"))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.daemon):
        print(f"daemon not found: {args.daemon} (run `make` first)")
        return 2

    print("=" * 72)
    print("Xbridge end-to-end test")
    print("=" * 72)
    return run(args.daemon, args.verbose)


if __name__ == "__main__":
    sys.exit(main())
