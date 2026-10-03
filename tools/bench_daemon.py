#!/usr/bin/env python3
"""bench_daemon.py — end-to-end numbers for a real daemon.

Micro-benchmarks live in tests/bench/bench.c; this measures the product: how
long the daemon takes to become usable, how fast a call round-trips, what a
cached read saves, and how much memory it holds.

    python3 tools/bench_daemon.py [--json] [--requests N]

Every number is measured here and now. Nothing is estimated.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "sdk", "python"))
sys.path.insert(0, os.path.join(ROOT, "tests", "fixtures"))

import content_site                                    # noqa: E402
from xbridge import Bridge                             # noqa: E402


def rules_for(base):
    return {
        "sourceName": "Bench",
        "sourceUrl": base,
        "bookSourceUrl": base,
        "searchUrl": base + "/?q={{key}}&page={{page}}",
        "ruleSearch": {"bookList": "div.book", "name": "h3.title a@text",
                       "author": "span.author@text", "coverUrl": "img.cover@src",
                       "bookUrl": "h3.title a@href", "nextPage": "a.next@href"},
        "ruleBookInfo": {"name": "h1.bookname@text", "author": "span.writer@text",
                         "coverUrl": "img.cover@src", "intro": "div.intro@text"},
        "ruleToc": {"chapterList": "ul.chapters li", "chapterName": "a@text",
                    "chapterUrl": "a@href"},
        "ruleContent": {"content": "div#content@html", "nextContentUrl": "a.next@href"},
    }


def percentile(values, pct):
    if not values:
        return 0.0
    ordered = sorted(values)
    k = max(0, min(len(ordered) - 1, int(round((pct / 100.0) * len(ordered) + 0.5)) - 1))
    return ordered[k]


def measure_latencies(bridge, method, params, n):
    latencies = []
    for _ in range(n):
        t0 = time.perf_counter()
        bridge.call(method, params)
        latencies.append((time.perf_counter() - t0) * 1000.0)
    return latencies


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--requests", type=int, default=200)
    ap.add_argument("--daemon", default=os.path.join(ROOT, "build", "xbridged"))
    args = ap.parse_args()

    if not os.path.exists(args.daemon):
        print(f"daemon not found: {args.daemon} (run `make`)", file=sys.stderr)
        return 2

    report = {}
    tmp = tempfile.mkdtemp(prefix="xbridge-bench-")

    with content_site.Server() as site:
        rules_path = os.path.join(tmp, "rules.json")
        with open(rules_path, "w", encoding="utf-8") as fh:
            json.dump(rules_for(site.base), fh)

        # ---- cold start ------------------------------------------------ #
        endpoint = os.path.join(tmp, "x.sock")
        log = open(os.path.join(tmp, "daemon.log"), "wb")
        t0 = time.perf_counter()
        proc = subprocess.Popen(
            [args.daemon, "--endpoint", f"unix://{endpoint}",
             "--data-dir", os.path.join(tmp, "data"), "--log-level", "error"],
            stdout=log, stderr=log)
        while not os.path.exists(endpoint):
            if time.perf_counter() - t0 > 20:
                raise SystemExit("daemon never became ready")
            time.sleep(0.0005)
        ready_ms = (time.perf_counter() - t0) * 1000.0

        bridge = Bridge.connect(f"unix://{endpoint}")
        connect_ms = (time.perf_counter() - t0) * 1000.0 - ready_ms
        report["cold_start_ms"] = round(ready_ms, 2)
        report["connect_and_handshake_ms"] = round(connect_ms, 3)

        # ---- control-plane latency ------------------------------------- #
        for _ in range(20):        # warm up
            bridge.call("bridge.ping")
        ping = measure_latencies(bridge, "bridge.ping", {}, args.requests)
        report["ping_ms"] = {
            "p50": round(statistics.median(ping), 4),
            "p95": round(percentile(ping, 95), 4),
            "p99": round(percentile(ping, 99), 4),
            "mean": round(statistics.mean(ping), 4),
            "n": len(ping),
        }

        # ---- real work: install then search ---------------------------- #
        bridge.install(rules_path)
        src = "legado/rules.json"
        bridge.search(src, "solo")          # warm the rule + HTTP paths

        # Uncached: unique query each time so the cache cannot help.
        uncached = []
        for i in range(60):
            t = time.perf_counter()
            bridge.call("source.search",
                        {"source_id": src, "query": f"solo {i}"}, cache_ttl_ms=0)
            uncached.append((time.perf_counter() - t) * 1000.0)
        report["search_uncached_ms"] = {
            "p50": round(statistics.median(uncached), 3),
            "p95": round(percentile(uncached, 95), 3),
        }

        cached = []
        for _ in range(200):
            t = time.perf_counter()
            bridge.search(src, "solo")
            cached.append((time.perf_counter() - t) * 1000.0)
        report["search_cached_ms"] = {
            "p50": round(statistics.median(cached), 4),
            "p95": round(percentile(cached, 95), 4),
        }
        speedup = statistics.median(uncached) / max(statistics.median(cached), 1e-6)
        report["cache_speedup_x"] = round(speedup, 1)

        # ---- throughput over several connections ----------------------- #
        conns = 8
        per_conn = max(1, args.requests // conns)
        def worker(out, i):
            b = Bridge.connect(f"unix://{endpoint}")
            n = 0
            t = time.perf_counter()
            for _ in range(per_conn):
                b.call("bridge.ping")
                n += 1
            out.append((n, time.perf_counter() - t))
            b.close()

        results = []
        threads = [threading.Thread(target=worker, args=(results, i)) for i in range(conns)]
        t0 = time.perf_counter()
        for t in threads:
            t.start()
        for t in threads:
            t.join(timeout=60)
        wall = time.perf_counter() - t0
        total = sum(n for n, _ in results)
        report["throughput_rps"] = round(total / wall, 0)
        report["throughput_connections"] = conns
        report["throughput_requests"] = total

        # ---- footprint -------------------------------------------------- #
        m = bridge.metrics()
        report["memory_allocated_bytes"] = m["memory"].get("allocated_bytes", 0)
        report["memory_rss_kb"] = m["memory"].get("rss_bytes", 0) // 1024
        report["cache_entries"] = m["cache"]["entries"]
        report["threads"] = m["server"].get("connections_active")

        bridge.close()
        proc.terminate()
        proc.wait(timeout=5)
        log.close()

    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    def row(label, value):
        print(f"  {label:<34} {value}")

    print("\nXbridge daemon benchmarks")
    print("  " + "-" * 58)
    print("  cold start")
    row("spawn -> socket accepting", f"{report['cold_start_ms']:.1f} ms")
    row("connect + HELLO/HELLO_ACK", f"{report['connect_and_handshake_ms']:.2f} ms")
    print("  request latency (bridge.ping)")
    for k in ("p50", "p95", "p99", "mean"):
        row(k, f"{report['ping_ms'][k]:.4f} ms")
    print("  source.search (HTTP + HTML + parse)")
    row("uncached p50", f"{report['search_uncached_ms']['p50']:.2f} ms")
    row("cached p50", f"{report['search_cached_ms']['p50']:.3f} ms")
    row("cache speedup", f"{report['cache_speedup_x']:.0f}x")
    print("  throughput")
    row(f"{conns} connections, {total} requests", f"{report['throughput_rps']:.0f} req/s")
    print("  footprint")
    row("allocated by the bridge", f"{report['memory_allocated_bytes'] / 1024:.0f} KiB")
    row("resident set", f"{report['memory_rss_kb']} KiB")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
