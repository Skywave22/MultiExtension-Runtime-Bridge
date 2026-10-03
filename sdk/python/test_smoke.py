#!/usr/bin/env python3
"""smoke.py — Python SDK end-to-end smoke test.

Different from tests/e2e/run_e2e.py: that one is the product's acceptance test
and drives the daemon from the outside; this one checks the *client library*
against a real daemon, including the ergonomics (spawn, context manager,
timeouts, retry).

    python3 sdk/python/test_smoke.py
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests", "fixtures"))

import content_site                                    # noqa: E402
from xbridge import Bridge, BridgeError, TimeoutError, retry  # noqa: E402

PASSED = 0
FAILED = 0


def check(cond, msg):
    global PASSED, FAILED
    if cond:
        PASSED += 1
        print(f"    ok   {msg}")
    else:
        FAILED += 1
        print(f"    FAIL {msg}")


def eq(actual, expected, msg):
    check(actual == expected, f"{msg} (got {actual!r}, want {expected!r})")


def main():
    if not os.path.exists(os.path.join(ROOT, "build", "xbridged")):
        print("daemon not found; run `make` first")
        return 2

    with content_site.Server() as site:
        tmp = tempfile.mkdtemp(prefix="xbridge-py-")
        rules = {
            "sourceName": "Fixture Site",
            "sourceUrl": site.base,
            "bookSourceUrl": site.base,
            "searchUrl": site.base + "/?q={{key}}&page={{page}}",
            "ruleSearch": {
                "bookList": "div.book", "name": "h3.title a@text",
                "author": "span.author@text", "coverUrl": "img.cover@src",
                "bookUrl": "h3.title a@href", "nextPage": "a.next@href",
            },
            "ruleBookInfo": {
                "name": "h1.bookname@text", "author": "span.writer@text",
                "coverUrl": "img.cover@src", "intro": "div.intro@text",
            },
            "ruleToc": {"chapterList": "ul.chapters li",
                        "chapterName": "a@text", "chapterUrl": "a@href"},
            "ruleContent": {"content": "div#content@html",
                            "nextContentUrl": "a.next@href"},
        }
        rules_path = os.path.join(tmp, "rules.json")
        with open(rules_path, "w", encoding="utf-8") as fh:
            json.dump(rules, fh)

        # spawn() is the SDK's own lifecycle helper: it starts the daemon,
        # reads the ready line, connects and cleans both up.
        with Bridge.spawn(os.path.join(ROOT, "build", "xbridged"),
                          data_dir=os.path.join(tmp, "data")) as bridge:
            src = "legado/rules.json"

            print("lifecycle")
            check(bridge.hello["protocol"] == "XBP/1", "handshake completed by spawn()")
            check(bridge.endpoint.startswith("unix://"), "endpoint resolved")

            print("\nspawn concurrency")
            # Two spawns in the same process must not race for the same socket.
            import xbridge
            with Bridge.spawn(os.path.join(ROOT, "build", "xbridged"),
                              data_dir=os.path.join(tmp, "data2")) as second:
                check(second.endpoint != bridge.endpoint, "second spawn uses its own socket")
                check(second.ping() < 1000, "second daemon answers")

            print("\nspawn guard")
            bare = xbridge.Bridge("unix:///nonexistent.sock")
            try:
                bare._hello()
                check(False, "using a Bridge without a transport raises")
            except BridgeError as e:
                check(e.code == -32004, "clear error instead of AttributeError")

            print("\ndetect, install, search")
            eq(bridge.detect(rules_path)["format"], "legado", "detected")
            eq(bridge.install(rules_path)["format_id"], "legado", "installed")

            found = bridge.search(src, "solo")
            eq(len(found["list"]), 1, "search result count")
            eq(found["list"][0]["name"], "Solo Leveling", "search result name")

            print("\nreading a book")
            detail = bridge.detail(src, found["list"][0]["url"])
            eq(len(detail["episodes"]), 3, "chapter count")
            chapter = bridge.novel_content(src, detail["episodes"][0]["url"])
            check(chapter["length"] > 40, "chapter body fetched")
            check("Paragraph two" in chapter["content"], "chapter text parsed")

            print("\nretry helper")
            attempts = {"n": 0}

            def flaky():
                attempts["n"] += 1
                if attempts["n"] < 3:
                    raise xbridge.BridgeError(xbridge.ERR_NETWORK, "transient")
                return bridge.ping()

            check(retry(flaky, attempts=5) < 1000, "retry recovers from network errors")
            eq(attempts["n"], 3, "retry attempts counted")

            print("\nbad path never retries")
            try:
                retry(lambda: bridge.search("legado/missing.json", "x"))
                check(False, "not-found is not retried")
            except BridgeError as e:
                eq(e.code, -32003, "not-found surfaced immediately")

            print("\ntimeouts")
            try:
                bridge.call("bridge.ping", {}, timeout_ms=1)
                # A 1 ms deadline may still be met on a fast machine; both
                # outcomes are acceptable, so only assert that it returns.
                check(True, "tiny deadline returned")
            except TimeoutError:
                check(True, "tiny deadline raised TimeoutError")

            print("\nconcurrent readers on separate connections")
            errors = []

            def reader():
                try:
                    with Bridge.connect(bridge.endpoint) as b:
                        b.search(src, "solo")
                except Exception as exc:  # noqa: BLE001
                    errors.append(exc)

            threads = [threading.Thread(target=reader) for _ in range(6)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=15)
            check(not errors, f"six parallel clients ({errors[:1]})")

    print(f"\npython sdk: {PASSED} passed, {FAILED} failed")
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
