# Clean-room statement

## What "clean room" means here

This repository is a **new implementation**. It was written by reading the
public contract of [`RyanYuuki/AnymeXExtensionRuntimeBridge`](https://github.com/RyanYuuki/AnymeXExtensionRuntimeBridge)
and the *ecosystem* formats it targets — and then implementing everything from
scratch, without copying.

Concretely, for the reference repository:

| Allowed (and done) | Not allowed (and not done) |
|---|---|
| Reading the repository to understand what it does | Copying any file, in whole or in part |
| Recording its architecture, file counts, dependency list and observable behaviour | Copying or transliterating a method, function, class, comment or algorithm |
| Adopting the public **vocabulary** it is a consumer of: manager ids, method names, extension file layouts | Reusing its internal APIs, class names, field names or data structures |
| Measuring its documented startup and transport behaviour | Reusing its code under any licence, including GPL-compatible ones |

The distinction that matters: **manager ids** (`aniyomi`, `legado`, …),
**method names** (`search`, `getDetail`, `getNovelContent`, …) and **extension
formats** (a Legado book-source JSON, a CloudStream `.jar`, an Aniyomi ext-lib)
are ecosystem interfaces owned by the Aniyomi / CloudStream / Kotatsu /
Mangayomi / LnReader / Legado projects and their users. Any bridge that wants
to run those extensions must use them; they are not the reference project's
invention and using them is not copying.

## Provenance of this code base

Every C, Python and JavaScript file in this repository was written for this
project. The evidence is in the history and the shape of the result:

- **Language and structure differ.** The reference is 158 Dart files plus
  Kotlin/Java/Swift hosts with a JVM sidecar. This is a C11 daemon of ~9 500
  lines with no runtime dependencies, plus thin SDKs.
- **Transport differs.** Reference: newline-delimited JSON over stdio.
  Here: length-prefixed binary framing over UDS / abstract UDS / named pipe /
  TCP, specified in `docs/PROTOCOL.md`, with a conformance suite that
  re-implements the framing independently of the server.
- **The algorithm differences are visible in the test suite.** The HTML
  selector engine, the rule interpreter, the cache with single-flight
  coalescing and the worker supervisor were each designed here, and each has a
  unit suite that pins its behaviour (`tests/unit/`, `tests/protocol/`).
- **No reference artifact is vendored.** There is no copied `.jar`, `.dex`,
  `.dart` or `.kt` file; `git log --stat` contains only new files.

## How the boundary was maintained

1. The reference was cloned **outside** the workspace (`/tmp/ref-bridge`) and
   only ever read. It is not a submodule, not a dependency, and nothing from it
   is committed here.
2. Observations were recorded as *behaviour*, in prose, in `docs/ANALYSIS.md`
   — never as code excerpts.
3. Implementation started from `docs/PROTOCOL.md`, which was written as an
   independent specification of what a bridge must do, before the daemon was
   written against it.
4. Where the reference's design was found to be a liability (JVM sidecar,
   stderr-grep readiness, dropped streaming chunks, client-only timeouts,
   per-platform duplicated service classes), this project deliberately chose a
   different mechanism, so even the *shape* of the solution is unrelated.

## If you are reviewing this for licence compliance

The new work is GPL-3.0 (`LICENSE`), which is compatible with the reference's
own GPL-3.0 licensing — but compatibility is not the reason for the clean-room
method; the reason is that a from-scratch implementation is what was asked for.
No file in this repository is derived from that project, so no attribution is
owed to it, and none is claimed.
