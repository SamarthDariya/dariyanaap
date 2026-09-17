# dariyanaap

The measuring rig for the [`builds/`](../../../hld/builds/README.md) HLD track: a load generator,
a latency histogram, and a fault-injection library — written from scratch in C++.

*"Dariya nāp"* — Dariya measures. Unit **0** of the track, and the only unit that is a tool rather
than a lesson. Every other repo vendors it as a submodule and reports its numbers through it.

Sibling projects: [dariyanache](https://github.com/SamarthDariya/DariyanAche) (Redis clone, Go),
[dariyakyu](../dariyakyu) (Kafka-style commit log, C++).

---

## What it is

Three libraries and a CLI:

| | What | Linked into |
|---|---|---|
| `dariyanaap::stats` | HDR-style latency histogram → p50/p90/p99/p999, throughput, error rate | the rig, and any target that wants to self-report |
| `dariyanaap::load` | the driver: N connections, fixed duration, **closed-loop and open-loop** | the `dariyanaap` CLI |
| `dariyanaap::fault` | `inject_latency`, `drop_probability`, `partition`, `hang_forever` | **the targets** — this half runs inside the service under test |

That split is the thing to notice. A load generator alone cannot create the failures this track needs:
"make one backend 10× slower mid-run" (unit 2), "drop messages between node A and node B, then heal"
(unit 8), and "this DB call sleeps 20ms" (unit 1) all live *inside* the service. So `fault` is a
library the targets link, driven at runtime, not a proxy in front of them.

---

**Where to look:** [Build](#build) · [**Using it**](#using-it) · [The numbers](#the-numbers) ·
[Status](#status) · [Checkpoints](#checkpoints) — and [DESIGN.md](DESIGN.md) for why anything is the
way it is, [BREAK.md](BREAK.md) for what got measured and what the predictions got wrong.

---

## The thesis

> **A load generator that lies is worse than no load generator.**

Twelve repos' worth of conclusions get derived from numbers this tool produces. So its honesty is
the actual product, and three rules follow:

1. **Never report a mean.** The mean is what hides every problem this track is about. p99 or nothing.
2. **Measure the requests you failed to make.** A closed-loop client that stalls stops sending, so it
   never records the latency of the requests it skipped, and reports a p99 that flatters the target by
   an order of magnitude. That's **coordinated omission**. Open-loop mode fixes it by timestamping from
   the *intended* send time, not the actual one.
3. **Know your own ceiling.** The rig competes with the target for the same CPU. Its own max
   throughput and its own p99 floor get measured against a null target and printed in every report, so
   "the service flatlined at 12k rps" can never be confused with "the rig flatlined at 12k rps".

Rule 2 is the lesson of this unit — see [BREAK.md](BREAK.md). Learning it on day one is what makes
the next twelve sets of numbers worth collecting.

Full reasoning, with the rejected alternatives, in **[DESIGN.md](DESIGN.md)**.

---

## Build

```sh
brew install cmake            # one-time

cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Or in one command, which is what the inner loop actually uses:

```sh
./scripts/check.sh          # build + test
./scripts/check.sh --all    # also ASan/UBSan and TSan — the gate for "done"
```

It also enforces DESIGN.md decision 12 (`steady_clock` only) with a grep, so a
`system_clock` creeping into `src/` fails the check rather than a review.

With sanitizers by hand:

```sh
cmake -B build-tsan -DDARIYANAAP_TSAN=ON && cmake --build build-tsan -j
cmake -B build-asan -DDARIYANAAP_ASAN=ON && cmake --build build-asan -j
```

Requires a C++20 compiler (developed against Apple Clang 17). doctest is fetched at configure time
and is the only dependency.

**Platform:** developed on macOS, so `kqueue` rather than `epoll`. The units that need cgroup memory
limits or `tc`/`netem` run in Docker; this rig does not, because its faults are in-process by design.

Vendoring it into another repo is under [Using it](#using-it).

---

## Using it

Two halves, and the split is the thing to understand first:

```
   ┌─────────────────┐                        ┌────────────────────────┐
   │   dariyanaap    │ ───── requests ──────► │   your service         │
   │   the client    │                        │                        │
   │                 │ ◄──── replies ──────── │   links                │
   │   measures      │                        │   dariyanaap::fault    │
   └─────────────────┘                        └────────────────────────┘
          │                                              ▲
     summary.csv                                  control socket
   histogram.csv                              break it while it runs
  timeseries.csv
```

The left half generates load and measures. The right half is a library your **target** links, so it
can be made slow, made to drop replies, made to hang, or cut off from its peers. Decision 1 explains
why that cannot be one program: unit 1's "DB call that sleeps 20ms" is a function inside the service,
and unit 8 needs node A cut from node B while both stay reachable from the client. A proxy in front
sees neither.

### The client: three questions it can answer

**"How fast can this go?"** — closed-loop. Omit `--rate` and each connection sends, waits for the
reply, sends again.

```sh
./build/dariyanaap --target 127.0.0.1:9000 --connections 32 --duration 10000 --warmup 500
```

Good for peak throughput and for finding the concurrency where a service saturates.

**"What is the p99 at 50,000 rps?"** — open-loop, and the only mode that can answer it.

```sh
./build/dariyanaap --target 127.0.0.1:9000 --connections 32 --rate 50000 \
                   --duration 10000 --warmup 500
```

With `--rate`, request *i* is due at `start + i/rate` and goes out on schedule whether or not reply
*i-1* has arrived — and latency is measured from when it was **due**, not when it was sent.
Closed-loop cannot answer this question at all: its throughput is an output, not an input, so "the
p99 at 50,000 rps" is not a thing you can ask it. E3 measured what the difference costs: against the
same stalling target, closed-loop reported p99 = 0.41ms and open-loop 208ms. **504×.**

**"What happens when it breaks?"** — the fault half, below.

### Talking HTTP

```sh
./build/dariyanaap --target 127.0.0.1:8080 --protocol http --path /health \
                   --connections 64 --rate 20000 --duration 10000
```

Minimal HTTP/1.1 `GET` framed by `Content-Length`. No TLS, no chunked encoding, no redirects — a
chunked response is reported as a protocol error by name rather than mis-framed.

### Getting numbers out

```sh
./build/dariyanaap ... --csv-dir runs/today --timeseries 1
python3 tools/plot.py runs/today            # needs matplotlib
```

| File | Shape | Why |
|---|---|---|
| `summary.csv` | **appended**, one row per run | a concurrency sweep accumulates into one file that plots directly |
| `histogram.csv` | one row per non-empty slot | raw counts, so a run can be re-percentiled months later without this binary |
| `timeseries.csv` | one row per elapsed second | *when* it degraded, not just by how much. A second with no samples gets a row of zeros, because "the file has a gap" and "the service was down" are different findings |

Values are nanoseconds. A microsecond column would record the rig's own 42ns floor as `0`.

### Every flag

```
--target HOST:PORT     required. "[::1]:8080" for IPv6
--rate RPS             open-loop at this offered rate; omit for closed-loop
--connections N        connections held open (default 1)
--duration MS          measured window (default 10000)
--warmup MS            discarded window before it (default 0)
--protocol raw|http    default raw
--payload N            raw echo bytes each way (default 64)
--path P               http path (default /)
--read-timeout MS      also the write timeout (default 1000)
--connect-timeout MS   default 1000
--csv-dir DIR          write the three files here
--timeseries 1         bucket by wall-clock second as well
--help
```

A flag that is misspelled, or present with an unreadable value, is an error rather than a fallback to
its default. `--connection 64` silently running at 1 connection would produce a sweep row that looks
like all the others and describes a different experiment.

### The target half: what your service calls

```cpp
#include "fault/knobs.hpp"
using namespace dariyanaap;

int main() {
    fault::load_from_env();                            // knobs at startup
    fault::set_identity("node-a");                     // only for partitions
    fault::ControlServer control("127.0.0.1", 7777);   // knobs mid-run
    // ... your accept loop
}

void handle_request(Connection& client) {
    do_the_actual_work();

    fault::before_response();             // applies latency; blocks while hung
    if (fault::should_drop()) return;     // YOU decide what dropping means
    client.send(reply);
}

void send_to_peer(const std::string& peer, const Message& message) {
    if (fault::blocked(peer)) return;     // partition — unit 8
    actually_send(peer, message);
}
```

Those calls go in **unconditionally**, with no `#ifdef`. E4 measured the cost with every knob off:
**+2.66 ns per request**, roughly one thousandth of the rig's own p99 floor. That is the whole point
of decision 9 — if the checks were expensive you would guard them, the fault-injecting build would
differ from the measured build, and your numbers would come from a different program than the one you
are reasoning about.

`should_drop()` deliberately does not act. Closing the connection and returning nothing look very
different to a client, and which one you pick is part of the experiment.

### Breaking things at startup

```sh
DARIYANAAP_FAULT_LATENCY_MS=20 DARIYANAAP_FAULT_JITTER_MS=5 ./my-service
DARIYANAAP_FAULT_DROP=0.05 ./my-service
DARIYANAAP_FAULT_IDENTITY=node-a DARIYANAAP_FAULT_PARTITION=node-a:node-b ./my-service
```

Jitter is not decoration: a fixed delay releases every affected request in lockstep, giving you a
thundering herd you did not ask for and a histogram with one spike instead of a distribution.

### Breaking things mid-run — the part that matters

```sh
$ nc 127.0.0.1 7777
status
no faults
latency 200 20
ok
hang 1
ok
hang 0
ok
partition node-a node-b
ok
heal node-a node-b
ok
clear
ok
```

This is what makes units 2, 8 and 9 possible at all (decision 10). "Kill a backend halfway through a
run and watch it get slammed when it rejoins" is not expressible as startup configuration. A
malformed command is answered with its error rather than closing the connection, so you can retype it
instead of reconnecting.

### Reading a run without fooling yourself

```
dariyanaap 0.1.0  closed-loop  target 127.0.0.1:57450  32/32 connections  32 opens
  attempted 293867  errors: connect 0 write 0 read 0 timeout 0 protocol 0 rejected 0
  97956 req/s   p50 0.242ms  p90 0.330ms  p99 0.459ms  p999 0.664ms  max 205.584ms
  rig floor: clock resolution 57 ns
```

| What you see | What to do about it |
|---|---|
| `32/32 connections` | if the first number is smaller, **the rig hit a limit, not your target** — that row's throughput is not comparable to the others |
| `32 opens` | more opens than connections means connections died and were replaced. Explain the churn before trusting the run |
| six error counters | never one rate. "Refused connections" and "hung until we gave up" call for opposite responses, and unit 2 turns on exactly that difference |
| `max` above `p999` | not a bug. `max` is exact; percentiles round **up** to a slot edge, so p999 can exceed max by up to 0.78% |
| no mean | deliberate, and enforced by a `static_assert`. The mean averages "the fast path" and "the problem" and reports neither |
| `THIS RUN IS VOID` | open-loop only. The rig could not offer the rate you asked for, so every other number describes a rate nobody requested. Lower the rate and rerun |

**Know these two before trusting any measurement** (both from E2):

- **Rig peak: 132,834 rps at 32 connections.** Past 32 the rig is saturated rather than degrading —
  it obeys Little's law within 3%, so extra connections buy latency, not throughput. A target
  measuring near 130k is measuring the rig.
- **Rig p99 floor: 51.7 µs.** Nothing here can be measured as faster than that.

### Vendoring it into the next repo

```sh
git submodule add https://github.com/SamarthDariya/dariyanaap.git vendor/dariyanaap
```

```cmake
add_subdirectory(vendor/dariyanaap)
target_link_libraries(my_service PRIVATE dariyanaap::fault)   # the target's half
target_link_libraries(my_bench   PRIVATE dariyanaap::load)    # the driver's half
```

Tests, the CLI and doctest only build when `dariyanaap` is the top-level project, and `-Werror` is
not forced on you. `./scripts/vendor-smoke-test.sh` asserts all four of those against a throwaway
parent project rather than leaving them as claims.

### The scripts

```sh
./scripts/check.sh              # build + test
./scripts/check.sh --all        # also ASan/UBSan and TSan — run before committing
./scripts/e2-sweep.sh           # the rig's own ceiling, 1 → 1000 connections
./scripts/e3-omission.sh        # closed-loop vs open-loop against a stalling target
./scripts/vendor-smoke-test.sh  # prove the submodule claims
./build/hist_verify             # histogram accuracy, 1M samples
./build/fault_cost              # cost of a disabled fault check
```

### One line to remember

**Closed-loop answers "how fast?". Open-loop answers "how slow, at this rate?". Only the second
question has an honest answer, and 504× is what the difference costs.**

---

## The numbers

Everything later repos quote, in one place. Full working in [BREAK.md](BREAK.md).

| | | |
|---|---|---|
| Rig peak throughput | **132,834 rps** at 32 connections | E2 |
| Rig p99 floor | **51.7 µs** at 1 connection | E2 |
| Rig bottleneck concurrency | **32 connections** | E2 |
| Past saturation | Little's law within 3% | E2 |
| **Coordinated omission** | **p99 504× worse open-loop than closed-loop, same target** | E3 |
| Closed-loop capacity overstatement | **26%** | E3 |
| Histogram error | 0.54% worst measured, 0.78% structural | E1 |
| `record()` cost | 2.3 ns/op | E1 |
| Disabled fault check | +2.66 ns per request | E4 |
| Clock resolution | 42 ns warm, 90 ns cold | E0 |

---

## Status

**Design: drafted. Implementation: complete — M0 through M5, E0 through E4.**

| Milestone | What lands | Effort | Status |
|---|---|---|---|
| M0 — Skeleton | build, sanitizers, ctest, units, clock | 0.5d | ✅ |
| M1 — Histogram | HDR buckets, percentiles, merge, CSV | 0.5d | ✅ |
| M2 — Closed-loop driver | thread-per-conn, HTTP + raw TCP, **self-calibration** | 1d | ✅ |
| M3 — Open-loop driver | intended-send-time, **coordinated omission demo** | 1d | ✅ |
| M4 — Fault injection | the four primitives + a runtime control channel | 0.5d | ✅ |
| M5 — Output | CSV schema, plot script, submodule smoke test | 0.5d | ✅ |

The track budgeted 2–3 days and this plan said 4. What it actually took was longer, and the honest
reason is that the socket layer (M2's chunks 2.1–2.4) was four chunks of plumbing before anything
measured anything, and three of the five milestones produced a correction to `DESIGN.md` rather than
just code. Those corrections are in the design doc, marked *Amended*, with the measurement that
forced each one.

---

## Checkpoints

Each milestone ends in something runnable or measurable, lives on its own `feature/*` branch, and is
merged by PR.

### M0 — Skeleton ✅
- [x] CMake, C++20, warnings on by default
- [x] ASan/UBSan and TSan build options, mutually exclusive
- [x] doctest wired into `ctest` (header-only, no subproject build)
- [x] builds as a submodule without forcing tests, CLI, or flags on the parent
- [x] `Error` / `UsageError` / `InvalidEndpoint` — startup throws, the hot path will count
- [x] durations are `chrono` aliases (`Nanos`/`Micros`/`Millis`/`Secs`), not hand-rolled wrappers
- [x] `Rate` — always positive, no default; `due_at(i) = i/rate` computed, never accumulated
- [x] `MonotonicClock` + `Stopwatch` — `steady_clock` only, no `system_clock` in `src/`
- [x] `MonotonicClock::measured_resolution()` — **42 ns warm**, 90 ns cold
- [x] `Endpoint` — validating constructor, strict `parse()` for `host:port` and `[::1]:8080`
- [x] `-Werror` when top-level, off when vendored
- [x] `scripts/check.sh` — build, test, sanitizers, and the `system_clock` ban in one command
- [x] suite green: 23 cases, clean under ASan/UBSan **and** TSan

Two things on this list were not planned and were added because the work produced them: `-Werror`
(a warning that only prints is invisible on the next incremental build, so it has to fail the
compile) and the `Rate`/`Endpoint` rule that a value which exists is always valid — no default
constructors, absence expressed as `std::optional`.

### M1 — The histogram ✅
- [x] log-linear buckets, 128 sub-buckets per octave, **1ns → 60s**, 3,808 counters (29.75KB)
- [x] `record()` allocation-free, one branch for overflow — **2.3 ns/op** measured
- [x] `percentile()` reporting each slot's **high edge**, `max()` exact, `count()` — no `mean()`,
      enforced by a `static_assert` on a concept rather than by a comment
- [x] `merge()` — per-thread histograms, merged once; TSan verified to catch a shared one
- [x] `Summary` — `optional`, because a run whose every request failed has no distribution.
      **No error rate**: decision 6 counts errors by kind, four of the six kinds are HTTP facts
      `stats` must not know, so they live on `load`'s run result beside an `optional<Summary>`
- [x] CSV in **nanoseconds** — a µs column would record the rig's own floor as `0`. Raw slot
      counts, and a test that re-derives p99 from the file text alone
- [x] **verifier** (`apps/hist_verify.cpp`) — 1M samples, worst error **0.5434%** against a
      0.7812% structural bound, memory flat at 30,488 bytes vs 8MB of samples (262×)
- [x] suite green: 38 cases, clean under ASan/UBSan **and** TSan

Corrections this milestone made to the design, both recorded in `DESIGN.md` decision 4: the
recording unit is nanoseconds, not microseconds (decision 7 needs a floor a µs layout cannot
express), and "two significant digits" was a name rather than a parameter — the knob is sub-bucket
count, and 128 is the only value meeting the ≤1% claim.

### M2 — Closed-loop driver ✅
- [x] `core::Socket` / `Listener` — RAII, non-blocking connect + `poll`, `SO_NOSIGPIPE`, `SO_REUSEADDR`
- [x] `Protocol` interface: build a request, decide when a response is complete, say whether it passed
- [x] `RawEcho` and minimal HTTP/1.1 `GET` framed by `Content-Length`, chunked refused by name
- [x] thread-per-connection, blocking I/O, per-thread histogram merged once
- [x] all six error kinds counted separately, with `consistent()` catching a request that vanished
- [x] fixed-duration runs; warm-up excluded from **everything**, not just the histogram
- [x] `dariyanaap-null` — RawEcho only, so E2 measures the rig rather than a server
- [x] `dariyanaap` CLI + CSV with RFC 4180 quoting
- [x] **E2 done:** **132,834 rps** peak at **32 connections**, p99 floor **51.7 µs**, and past
      saturation the rig obeys Little's law within 3% — see `BREAK.md`

The prediction fields for E1 and E2 are both empty, because both were run before being predicted.
E3 is the one that matters and gets predicted first.

### M3 — Open-loop driver ✅
- [x] `Schedule` — one lock-free counter hands out "request *i*, due at `start + i/R`"
- [x] **no `kqueue`** — `DESIGN.md` amended: what open-loop needs is that no single connection can
      delay the schedule, and moving the schedule out of the connections achieves that. E2 showed
      thread count is not the binding constraint at any concurrency this track uses
- [x] latency measured from **intended** send time — the two lines that kill coordinated omission
- [x] schedule-lag metric, and the run declared **VOID** when the sender is persistently behind
- [x] `--rate` on the CLI; `--stall-every` / `--stall-for` on the null target
- [x] **E3 done: p99 ratio 504×** — closed-loop 0.414ms, open-loop 208.667ms, same target, and
      closed-loop *also* overstates throughput by 26%. Prediction was wrong by 11×, by reasoning
      about a mean. See `BREAK.md`.

### M4 — Fault injection ✅
- [x] `set_latency(mean, jitter)` — jitter refused past the mean, since the draw would go negative
      and clamping it would mean the configured mean was not the mean
- [x] `set_drop_probability(p)` — the caller decides what dropping *means* (close the connection, or
      answer nothing), because which it picks matters and the library should not choose
- [x] `partition(a, b)` — symmetric, stored name-ordered; a node with no identity blocks nothing,
      which is safer than guessing
- [x] `set_hang_forever` — checked in a loop, so healing takes effect at once and unit 2 can stage a
      mid-run restart. The name describes the failure, not a promise about the process
- [x] env at startup **and** a line-oriented control socket for mid-run changes (decision 10)
- [x] one relaxed load of a single bool gates every check; `blocked()` takes two gates so the
      partition mutex is never reached on a healthy cluster
- [x] **E4 done: +2.66 ns per request** with everything off — about one thousandth of the rig's own
      51.7 µs p99 floor. Decision 9 holds, so a target can call these unconditionally
- [x] 4 suites green under ASan/UBSan and TSan, including six threads reading knobs while they change

### M5 — Output and ergonomics ✅
- [x] CSV schema: `summary.csv` (appended, one row per run), `histogram.csv` (raw slot counts),
      `timeseries.csv` (per-second p50/p90/p99/max)
- [x] every row stamped with rig version, target, protocol, mode, connections asked for **and**
      started, and the measured clock resolution
- [x] `tools/plot.py` — throughput vs concurrency, the latency CDF built from raw slot counts, and
      p99 per second. Throwaway, and it says so
- [x] `dariyanaap --help` and `dariyanaap-null --help`
- [x] `scripts/vendor-smoke-test.sh` — builds a throwaway parent that links both halves and asserts
      no tests, no CLI, no doctest fetch, and **no `-Werror` forced on the parent**
- [x] `BREAK.md` complete: E0–E4, with the two predictions that were wrong and why

---

## Deliberately out of scope

The track's rule 5 is "cap the scope", and unit 0's stop-here line is short:

| | Why |
|---|---|
| Distributed load generation | one laptop's rig, honestly calibrated, beats a cluster of liars |
| Web UI / live dashboard | CSV plus a plot script answers every question this track asks |
| Config DSL | flags and env vars. A config language is a project |
| HTTP/2, TLS, gRPC | the targets in this track are `GET /` and raw TCP |
| A proxy-based fault injector | `partition(a, b)` and "this DB call sleeps" are inside the app |
| Percentile estimation cleverness (t-digest, sketches) | HDR buckets are exact enough and simpler |

---

## Track rules this repo has to satisfy

From [`builds/README.md`](../../../hld/builds/README.md):

- **Ships a benchmark** — the rig's own ceiling, measured in M2, quoted forever after.
- **Ships a `BREAK.md`** — predicted vs measured vs what was wrong. See [BREAK.md](BREAK.md).
- **One trade-off per repo** — closed-loop vs open-loop. Everything else here is plumbing.
