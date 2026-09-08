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

## Status

**Design: drafted.** **Implementation: M0 complete, M1 next.**

| Milestone | What lands | Effort | Status |
|---|---|---|---|
| M0 — Skeleton | build, sanitizers, ctest, units, clock | 0.5d | ✅ |
| M1 — Histogram | HDR buckets, percentiles, merge, CSV | 0.5d | 🔸 next |
| M2 — Closed-loop driver | thread-per-conn, HTTP + raw TCP, **self-calibration** | 1d | ⬜ |
| M3 — Open-loop driver | `kqueue`, intended-send-time, **coordinated omission demo** | 1d | ⬜ |
| M4 — Fault injection | the four primitives + a runtime control channel | 0.5d | ⬜ |
| M5 — Output | CSV schema, plot script, submodule smoke test | 0.5d | ⬜ |

~4 days. The track's estimate is 2–3; if it needs to be 2, cut M4's control channel to environment
variables read at startup and skip the per-second timeseries in M5.

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

### M1 — The histogram 🔸
- [ ] log-linear buckets, 128 sub-buckets per octave, **1ns → 60s**, 3,808 counters (29.75KB)
- [ ] `record()` on the hot path is branch-light and allocation-free
- [ ] `percentile()` reporting each slot's **high edge**, `max()`, `count()` — **and no `mean()`**
- [ ] `merge()` — one histogram per thread, merged once at the end of the run
- [ ] `Summary`: throughput, p50/p90/p99/p999/max, duration, overflow — **no error rate**,
      because decision 6 counts errors by kind and four of the six kinds are HTTP facts
      `stats` must not know; they live on `load`'s run result beside an `optional<Summary>`
- [ ] CSV: one summary row + the raw bucket counts, so a run can be re-percentiled later
- [ ] **verifier:** 1M samples from a known distribution — p99 within 1% of the exact value,
      memory flat, and the error bound *stated* rather than assumed

### M2 — Closed-loop driver ⬜
- [ ] `Protocol` interface: build a request, decide when a response is complete
- [ ] `RawEcho` protocol (fixed-size ping/pong) and minimal HTTP/1.1 `GET` with `Content-Length`
- [ ] thread-per-connection, blocking I/O, per-thread histogram
- [ ] connect errors, read errors, timeouts and non-2xx counted **separately** — an error rate that
      lumps them together hides which failure you caused
- [ ] fixed-duration runs with a warm-up window excluded from the histogram
- [ ] `dariyanaap-null` — an in-repo target that replies immediately, for calibrating the rig
- [ ] **verifier:** against `dariyanaap-null`, the rig's own max throughput and p99 floor, and the
      connection count at which the *rig* becomes the bottleneck. These three numbers go in
      `BREAK.md` and get quoted in every later repo.

### M3 — Open-loop driver ⬜
- [ ] non-blocking sockets + `kqueue`, K event-loop threads owning C/K connections
- [ ] a fixed-rate schedule: request *i* is due at `start + i/R`, and that is its start timestamp
- [ ] latency measured from **intended** send time — the one line that kills coordinated omission
- [ ] schedule-lag metric: how far behind its own plan the sender fell (if this is large, the rig is
      saturated and the run is void — say so loudly rather than reporting the numbers)
- [ ] optional: extra connections opened when every existing one is mid-request
- [ ] **verifier:** the same target, at the same offered load, closed-loop vs open-loop. Predict the
      p99 gap first, in `BREAK.md`, then measure it.

### M4 — Fault injection ⬜
- [ ] `fault::inject_latency(ms, jitter)` — sleep before responding, jitter to avoid lockstep
- [ ] `fault::drop_probability(p)` — drop the response, or the connection, and say which
- [ ] `fault::partition(node_a, node_b)` — symmetric message drop between two named peers (unit 8)
- [ ] `fault::hang_forever()` — accept and never reply, the failure mode worse than being down
- [ ] knobs readable at startup from env, **and settable mid-run** over a tiny control socket —
      units 2, 8 and 9 all need to flip a knob without restarting the target
- [ ] disabled fault checks cost one relaxed atomic load, verified by benchmark, so a target can ship
      them in the hot path without apology
- [ ] **verifier:** with all faults off, target throughput is within noise of an unlinked build

### M5 — Output and ergonomics ⬜
- [ ] CSV schema: `summary.csv`, `histogram.csv`, optional `timeseries.csv` (per-second p99)
- [ ] every file stamped with rig version, target, mode, offered rate, duration, and rig ceiling
- [ ] `tools/plot.py` — throwaway matplotlib; latency CDF and per-second p99. Not a product.
- [ ] `dariyanaap --help` that reads like the flags an actual run needs
- [ ] a scratch parent repo that adds this as a submodule and links both halves, so the vendoring
      claim in M0 is tested rather than believed
- [ ] `BREAK.md` complete: predictions, measurements, and what was wrong

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

### Using it from another repo

```sh
git submodule add https://github.com/SamarthDariya/dariyanaap.git vendor/dariyanaap
```

```cmake
add_subdirectory(vendor/dariyanaap)
target_link_libraries(my_service PRIVATE dariyanaap::fault)   # the target's half
```

Tests and the CLI only build when dariyanaap is the top-level project, so vendoring it adds two
static libraries and nothing else.

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
