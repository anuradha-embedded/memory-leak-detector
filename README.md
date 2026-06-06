# Memory Leak Detector

> A lightweight C++ allocation tracker that captures call sites and reports leaks at exit.

[![CI](https://github.com/anuradha-embedded/memory-leak-detector/actions/workflows/ci.yml/badge.svg)](https://github.com/anuradha-embedded/memory-leak-detector/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)](#building)

## Overview

`libmemleak` is a small, dependency-free leak detector for C++ programs. It
replaces the global `operator new` / `operator delete` family, records every
heap allocation together with a snapshot of the call site that produced it, and
prints a report of everything still outstanding when the process exits.

It is deliberately simple to adopt: link the static library, and from that point
on the tracker observes all heap traffic with no code changes required. A small
public API lets you query live allocations and bytes programmatically, scope
detection to a region of interest, or print a report on demand.

### Motivation

Full instrumentation tools such as Valgrind or the LeakSanitizer are excellent,
but they are not always available: cross-compiled targets, restricted CI images,
or third-party build environments may ship without them. This project fills that
gap with a self-contained, standard-library-only detector that builds anywhere a
C++17 compiler does, links in seconds, and adds no runtime dependencies. It is
also a compact, readable reference for how the global allocation operators can be
replaced safely without tripping over re-entrancy or static-destruction ordering.

## Features

- Intercepts the **complete** C++17 set of global allocation operators: throwing
  and `nothrow` `new` / `new[]`, plain and `nothrow` `delete` / `delete[]`, and
  the sized `delete` / `delete[]` overloads.
- Records pointer, request size, a monotonically increasing allocation serial,
  and a captured call stack for every allocation.
- Symbolizes call sites with `backtrace_symbols` and demangles C++ names through
  the Itanium ABI where available; degrades gracefully to raw addresses where the
  platform offers no backtrace facility.
- Thread-safe registry built on an open-addressing hash table with
  backward-shift deletion, all of it allocated through an **untracked** path so
  the detector never recurses into itself.
- Re-entrancy is fenced with a thread-local guard, so the tracker's own
  bookkeeping (and code it calls, such as the symbolizer) is never recorded.
- Automatic leak report on program exit, plus an explicit `report()` and
  programmatic accessors (`liveAllocationCount()`, `liveBytes()`, `summary()`,
  `liveAllocations()`).
- Zero third-party dependencies; standard library only; one static library
  (`libmemleak`) and one demo binary.

## Design / Architecture

```
                 +-------------------------------+
   new / delete  |        Overrides.cpp          |
  ───────────────▶  global operator new/delete   │
                 |  (malloc/free + record hook)   |
                 +---------------+---------------+
                                 │ recordAllocation / recordDeallocation
                                 ▼
                 +-------------------------------+
                 |          Tracker              |   process-wide singleton
                 |  - std::mutex guarded         |
                 |  - thread-local reentry guard │
                 |  - atexit report handler      │
                 +-------+---------------+-------+
                         │               │
            captureBacktrace        Registry (open-addressing
            symbolizeFrame          hash table, malloc-backed,
                 │                  invisible to the tracker)
                 ▼
        +-----------------------+
        |    Backtrace.cpp      |  execinfo.h + cxxabi.h,
        |  (guarded fallback)   |  with a no-op fallback
        +-----------------------+
```

Components:

- **Overrides.cpp** — the replaced global allocation operators. Each allocating
  form backs onto `std::malloc` and notifies the tracker; each deallocating form
  notifies the tracker and calls `std::free`. Placement forms are left untouched.
- **Tracker** (`Tracker.cpp`) — the singleton registry. It owns the live-record
  table, the mutex, the lifetime serial counter, and the `atexit` handler. The
  singleton is constructed into static storage and intentionally never destroyed
  so it outlives every other static object during shutdown.
- **Registry** — an open-addressing hash table keyed by pointer. All storage is
  obtained via `malloc`/`free` so the table is invisible to the operators that
  would otherwise try to track it.
- **Backtrace.cpp** — call-site capture and symbolization, isolated behind
  `#ifdef` probes so the library builds and links even where `<execinfo.h>` is
  absent.

The two defenses against infinite recursion are (1) a thread-local guard that
short-circuits recording while the tracker is on its own stack, and (2) an
untracked allocator for all internal bookkeeping.

## Building

The project builds with either CMake or a plain Makefile. It requires only a
C++17 compiler and a threading library.

### CMake

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

This produces `build/libmemleak.a`, the `build/memleak_demo` executable, the
`build/track_workload` example, and the `build/tracker_tests` test binary.

### Make

```sh
make            # build libmemleak.a, the demo, and the tests
make test       # build and run the test suite
make demo       # build and run the demonstration program
make clean      # remove build artifacts
```

To build with GCC explicitly: `make CXX=g++`.

## Usage

Link your program against `libmemleak` and the operators are replaced
automatically. The minimal integration is simply to link the library and let the
at-exit handler do its work:

```cpp
#include "memleak/Tracker.hpp"

int main() {
    int* leaked = new int[100];   // never freed
    (void)leaked;
    return 0;                     // leak report prints on exit
}
```

Querying the tracker programmatically:

```cpp
#include "memleak/Tracker.hpp"
#include <iostream>

int main() {
    auto& t = memleak::Tracker::instance();
    int* a = new int[64];
    std::cout << t.liveAllocationCount() << " live, "
              << t.liveBytes() << " bytes\n";   // 1 live, 256 bytes
    delete[] a;
    std::cout << t.liveAllocationCount() << " live\n"; // 0 live
}
```

Running the bundled demo:

```sh
$ ./build/memleak_demo
memleak demo: backtrace support is enabled
after well-behaved phase: 0 live allocation(s), 0 live byte(s)
after leaking phase:      4 live allocation(s), 320 live byte(s)

Explicit report follows (the at-exit handler will print another copy):
================ memleak: leak report ================
Detected 4 live allocation(s), 320 byte(s) leaked.
(lifetime: 7 tracked, 3 freed)
------------------------------------------------------
#1  32 byte(s)  @ 0x102d49e10  (alloc #4)
      at (anonymous namespace)::leakOnPurpose()
      at main
#2  128 byte(s)  @ 0x102d49c10  (alloc #5)
      at (anonymous namespace)::leakOnPurpose()
      at main
...
======================================================
```

A second example, `examples/track_workload.cpp`, models a cache with a buggy
`clear()` that leaks one node per run; see `examples/sample_report.txt` for its
captured output.

## Project Structure

```
memory-leak-detector/
├── CMakeLists.txt
├── Makefile
├── LICENSE
├── README.md
├── include/
│   └── memleak/
│       ├── Tracker.hpp        # public API: Tracker singleton, records, queries
│       └── Backtrace.hpp      # internal call-site capture / symbolization
├── src/
│   ├── Tracker.cpp            # registry, mutex, atexit handler, reporting
│   ├── Backtrace.cpp          # execinfo + cxxabi with guarded fallback
│   ├── Overrides.cpp          # global operator new/delete replacements
│   └── demo.cpp               # memleak_demo: deliberate leaks + report
├── tests/
│   └── tracker_tests.cpp      # assert-based suite, wired into ctest
├── examples/
│   ├── track_workload.cpp     # realistic leaking-cache example
│   └── sample_report.txt      # captured example output
└── .github/workflows/ci.yml   # Ubuntu + macOS CI
```

## Testing

Tests live in `tests/tracker_tests.cpp` and use `<cassert>` only — there is no
external test framework. They query the tracker's API directly rather than
parsing any printed output, and cover:

- A balanced allocate/free sequence leaving zero live allocations and bytes.
- A leak of a precisely known count and byte total, asserted exactly against
  `liveAllocationCount()`, `liveBytes()`, and the `liveAllocations()` snapshot.
- Record removal on free, and that freeing a pointer the tracker never saw is a
  harmless no-op.
- A high-volume allocate/free churn that forces the registry to rehash and then
  drains it completely, verifying table consistency through growth and deletion.

Run them via CTest:

```sh
ctest --test-dir build --output-on-failure
```

or directly with `make test`. The suite forces assertions to stay enabled even
in Release builds and defeats the compiler's new/delete elision so the operators
are genuinely exercised.

## Roadmap / Future Work

- Optional peak-usage and high-water-mark statistics.
- Per-call-site aggregation in the report (group identical stacks, show counts).
- A JSON report mode for machine consumption and CI gating.
- Windows support via a CRT allocation-hook backend.
- Optional bounded ring buffer of recent frees to help diagnose double-frees.

## License

Released under the MIT License. See [LICENSE](LICENSE) for details.
