# dariyanaap — Design

The measuring rig for the build-first HLD track. *"Dariya nāp"* — Dariya measures.

Unit 0 of [`builds/`](../../../hld/builds/README.md). Written before any other unit because every
other unit's verifier is a number this tool produces.

---

## The thesis

> **A load generator that lies is worse than no load generator.**

Every conclusion in the next twelve repos — "least-connections beat round-robin by 4× at p99",
"consistent hashing moved 1/N of keys, mod-N moved 7/8", "the retry storm tripled offered load" — is
a number this rig reported. If the rig is wrong, the conclusions are wrong, and worse: they'll *feel*
earned, because a measured number is far more convincing than a guessed one.

So the design goal is not throughput and not features. It is **honesty**, and specifically three
kinds of it:

1. **Distributional honesty.** No means. A mean latency is an average of a bimodal distribution where
   one mode is "the fast path" and the other is "the problem", and it reports neither.
2. **Sampling honesty.** Measure the requests you *failed to make*, not only the ones you completed.
3. **Self-honesty.** Know and publish the rig's own ceiling, so its limits are never mistaken for the
   target's.

Most decisions below are consequences of one of those three.

---

## Part I — Conceptual design

### 0. Language and style

**C++20**, matching [dariyakyu](../dariyakyu). The rig is vendored into repos in both C++ (units 1, 2,
5, 6, 7, 8, 9, 10, 11, 12) and Go (units 3, 4). The C++ ones link the libraries directly; the Go ones
drive the CLI as a subprocess and read its CSV. That asymmetry is fine — it's why the CLI's output
format is a designed artifact and not a debug print.

House style follows dariyakyu: `.hpp`/`.cpp` split, includes rooted at `src/`,
`using namespace std;` in `.cpp` only, RAII for anything owning a file descriptor, no Boost, no
template metaprogramming, and nothing that makes a reader stop and squint.

### 1. Two halves, not one tool

`load` (the client) and `fault` (linked *into* the target) are separate libraries in one repo.

The obvious alternative is a load generator plus a network proxy that injects faults — the toxiproxy
shape. It was rejected because it cannot express the faults this track actually needs:

- Unit 1's "fake DB call that sleeps 20ms" is a function call inside the service, not a hop.
- Unit 3's cache stampede needs the *origin* to be slow, behind the cache, not the edge.
- Unit 8's `partition(a, b)` must drop messages between two peers of a three-node cluster while
  leaving both reachable from the client. A proxy in front of the cluster sees none of that traffic.
- Unit 9's circuit breaker has to observe a dependency that is **slow, not down** — and "slow" is a
  property of the callee's handler, not of the wire.

They share a repo because they share the clock, the units, and a release: a fault knob and the
histogram that measures its effect must agree on what a microsecond is.

### 2. Closed-loop and open-loop are both first-class

Closed-loop: fire, wait for the reply, fire again. C connections in flight, offered load determined
by the target's own speed.

Open-loop: request *i* is due at `start + i/R`, whether or not reply *i-1* has arrived. Offered load
is determined by you.

Almost every hand-rolled load generator is closed-loop, because it's twenty lines. And closed-loop
**cannot** measure the cost of a stall: when the target hangs for 200ms, the client stops sending, so
the requests that *should* have been issued during that window are never issued and never timed. The
recorded p99 is the p99 of the requests the target was healthy enough to accept. That is
**coordinated omission**, and it is why load tests report a p99 of 8ms for a service whose users see
400ms.

The fix is one line and it is the whole point of the unit: in open-loop mode, latency is measured from
the **intended** send time, so a request the rig couldn't send on schedule accrues latency while it
waits. A stall shows up as the fat tail it is.

Both modes ship, because the *comparison* is the lesson. One rig, one target, one offered load, two
numbers that disagree by an order of magnitude.

### 3. Never report a mean

`Histogram` exposes `percentile()`, `max()` and `count()`. It does **not** expose `mean()`, and the
omission is deliberate rather than an oversight — the header says so, so nobody adds it back.

Rejected: "report it alongside p99, it's free". It isn't free. It's the number a reader's eye goes to
first, and in every scenario in this track (a slow backend in three, one hot key in a cache, one
contended row) the mean moves by a few percent while p99 moves by 50×.

### 4. HDR-style buckets, not samples, not sketches

Log-linear buckets: **nanoseconds, from 1ns to 60s**, with **128 sub-buckets per power of two**.
Fixed memory (3,808 counters, 29.75KB), constant-time `record()`, no allocation on the hot path, and an
error bound that is derived rather than hoped for.

*Amended after M0.* This decision originally said "two significant digits of precision from 1µs to
60s", and both halves of that were wrong in a way that only showed up once there were numbers.

**The unit is nanoseconds, not microseconds.** Decision 7 requires the rig to publish its own p99
floor, and M0 measured the clock tick at 42ns. A histogram whose bottom bucket is 1µs cannot express
the number decision 7 exists to report — it would have had to say "the rig's floor is under 1µs",
which is the kind of non-answer this repo is meant to avoid. Extending the bottom to 1ns costs 10KB
(2,528 counters → 3,808) and nothing else: values below the clock tick never occur, so those counters
stay empty. Latency is still *reported* in microseconds; only the recording unit changed.

**"Two significant digits" was a name, not a parameter.** The actual knob is the sub-bucket count,
and the error bound is exactly `1/sub_buckets` when a bucket reports its high edge. Measured over a
sweep to 6e10:

| sub-buckets | counters | memory | worst over-report |
|---|---|---|---|
| 64 | 1,968 | 15.4KB | 1.562% — fails the ≤1% claim |
| **128** | **3,808** | **29.8KB** | **0.781%** |
| 256 | 7,360 | 57.5KB | 0.391% — twice the memory for accuracy nothing needs |

The counter count is derived from the layout by `index_of(60s) + 1`, not from the octave-ceiling
formula, which would over-allocate by 32 slots for a top octave that stops at 60s rather than 68.7s.

So 128 is not inherited from HdrHistogram's defaults; it is the only value that satisfies the ≤1%
bound without paying for precision no experiment in the track can use. The index arithmetic was
verified monotone and gapless over a dense sweep of 1..3e6 and a sparse sweep to 6e10.

Rejected alternatives:

- **Keep every sample and sort at the end.** Exact, and it works — until a 60-second run at 100k rps
  wants 6M samples per connection, at which point the rig's allocator is in the measurement.
- **Reservoir sampling.** Bounded memory, but it throws away tail samples, which are the only samples
  that matter here. Sampling the tail to measure the tail is circular.
- **t-digest / DDSketch.** Better accuracy per byte, more code, and the accuracy was never the
  constraint. Rule 5 of the track applies to the rig too.

The raw bucket counts go into the CSV, not just the computed percentiles, so a run can be
re-percentiled or merged with another run months later without being re-run.

### 5. One histogram per thread, merged at the end

The hot path touches only thread-local state. Merging happens once, after the run, single-threaded.

Rejected: a shared histogram with atomic increments. Cheap-looking and wrong — at high rates the
counter for the modal bucket becomes a contended cache line, and the rig's own measurement machinery
starts costing more than the syscall it's timing. The whole repo exists to avoid exactly this class of
mistake.

This is also the main claim TSan is here to check.

### 6. Errors are counted by kind, never as one rate

Connect failure, write failure, read failure, timeout, protocol error, and non-2xx response are six
separate counters.

A single "error rate: 3%" cannot distinguish "the backend refused connections" from "the backend
returned 503 instantly" from "the backend hung until we timed out" — and those three have opposite
implications for a load balancer, which is unit 2's entire subject. Timed-out requests are recorded in
the histogram at their timeout value, not dropped: dropping them is coordinated omission wearing
another hat.

### 7. The rig measures itself first

M2 ships `dariyanaap-null`, a target that replies immediately with a fixed byte string, and the rig is
run against it to establish three numbers: max throughput, p99 floor, and the connection count at
which the rig itself becomes the bottleneck.

Those three go into `BREAK.md` and are stamped into every CSV thereafter. Without them, the first time
a target flatlines there is no way to tell whose ceiling was hit — and the wrong answer there
invalidates a whole unit's conclusion.

In open-loop mode there is a second self-check with teeth: **schedule lag**. If the sender is
persistently behind its own plan, it is saturated and the run is void. The rig says so loudly rather
than printing plausible numbers.

### 8. Warm-up is excluded, and the exclusion is visible

Runs are fixed-duration with a warm-up window whose samples are discarded — JIT-free C++ still has a
cold page cache, an empty connection pool, and a target whose caches are empty.

But unit 2 is specifically about a **backend restarting with a cold cache** and getting slammed. So
warm-up is a flag with a default, printed in the CSV header, never a hidden constant. Any number this
rig prints must be attributable to a configuration a reader can see.

The rig's *self*-measurement sits inside the same window, which was not obvious until it was
measured. `MonotonicClock::measured_resolution()` reports **90ns** as the first thing a process does
and **35–42ns** once the core has clocked up — and 42ns is one tick of the 24MHz timebase, so the warm
number is the hardware floor and the cold one is an artifact of when it was taken. Measuring at
startup would publish a p99 floor more than twice too high, and decision 7 would then be quoting a
wrong number into every later repo. So resolution is measured at the *end* of warm-up, not at
process start.

### 9. Fault checks must be cheap enough to ship in the hot path

A target links `fault` and calls into it on its request path unconditionally. So a disabled check is
one relaxed atomic load against a `bool`, benchmarked in M4, with the "all faults off ≈ unlinked
build" claim verified rather than asserted.

If it weren't cheap, targets would guard it with `#ifdef`, the fault-injecting build would differ from
the measured build, and the numbers would be from a different program than the one being reasoned
about.

### 10. Faults are settable mid-run

Knobs are read from the environment at startup **and** changeable at runtime over a small control
socket.

Unit 2 makes a healthy backend `hang_forever()` in the middle of a run. Unit 8 partitions a cluster,
writes to both sides, then heals it. Unit 9 makes a dependency slow for ten seconds and watches the
breaker trip and recover. None of those are expressible as startup configuration; all of them are the
interesting part of their unit.

Env-only would be the two-day version of this repo. The control socket is what makes it the
three-day version, and it earns the day.

### 11. Protocols are an interface, and there are exactly two

`Protocol` does two things: build a request, and decide when a response is complete.

Two implementations ship: `RawEcho` (fixed-size ping/pong, for units 2, 6 and 8) and minimal
HTTP/1.1 `GET` with `Content-Length` (for units 1, 3, 7, 9, 11, 12). No chunked encoding, no
keep-alive negotiation, no HTTP/2, no TLS. The track's targets are `GET /` and raw TCP; anything more
is scope the stop-here line forbids.

### 12. `steady_clock`, once, in one wrapper

All timing goes through `MonotonicClock`. `system_clock` appears nowhere in the repo — it can step
backwards, and unit 10 (`dariyabarf`) deliberately moves the clock backwards to break a Snowflake
generator. The rig measuring that experiment must not be affected by it.

Durations are strong types (`Micros`, `Rate`), because a function taking `(int timeout, int rate)` is
a bug waiting for the day you're tired.

---

## Part II — Structure

```
src/
├── core/          units, MonotonicClock, Target/Endpoint, errors      → dariyanaap::core
├── stats/         Histogram, Summary, CsvWriter                       → dariyanaap::stats
├── load/          Protocol, Connection, ClosedLoopRunner,             → dariyanaap::load
│                  OpenLoopRunner, EventLoop (kqueue), LoadPlan
└── fault/         knobs, control socket, the four primitives          → dariyanaap::fault
apps/
├── dariyanaap.cpp        the CLI
└── dariyanaap_null.cpp   the calibration target
tools/plot.py             throwaway; latency CDF and per-second p99
```

Layering is enforced by the build: `core` links nothing, `stats` links `core`, `load` links `stats`,
`fault` links `core` only. If `core` ever includes from `load`, it will not link — the same trick
dariyakyu uses, and the reason a layering violation is a build error rather than a code review.

`fault` deliberately does **not** depend on `stats`. It gets linked into other people's services, and
a fault knob that drags a histogram implementation into a Go-adjacent build is a nuisance nobody asked
for.

### The concurrency model, and why it changes at M3

**M2, closed-loop: thread-per-connection, blocking I/O.** Naive on purpose, per the track's rule 1. It
is also correct for closed-loop, where a connection is genuinely blocked waiting for its reply, and it
is fifty lines instead of four hundred.

Its ceiling is real and will be measured: at ~500 connections the rig pays the same
context-switching cost that unit 1's thread-per-request service is about to be broken on. That is a
pleasing irony and a genuine hazard, which is exactly why decision 7 exists.

**M3, open-loop: `kqueue`, K event-loop threads owning C/K connections.** Open-loop needs a socket
that never blocks the scheduler, because the schedule is the measurement. This is where the rig stops
being naive, and doing it second means the cost of the event loop is a measured delta rather than an
assumption.

### Output schema

Three files per run, all stamped with rig version, target, mode, offered rate, duration, warm-up, and
the rig's calibrated ceiling:

| File | One row per | Why it exists |
|---|---|---|
| `summary.csv` | run | the number that goes in `BREAK.md` |
| `histogram.csv` | bucket | so a run can be re-percentiled or merged without re-running |
| `timeseries.csv` | second | so "when did it degrade" is answerable, which matters for units 2 and 9 |

---

## Open questions

Recorded rather than resolved, to be settled by the first unit that needs them:

- **Connection re-establishment on error.** Reconnect immediately, with backoff, or run the
  connection count down? Unit 9 (`dariyadhaal`) is about retry policy, and a rig with an opinionated
  retry policy of its own would contaminate it. Leaning: no reconnect by default, and count the lost
  connection.
- **Open-loop overflow.** When every connection is mid-request and the schedule says send now: queue
  the request (keeping its intended timestamp), or open a new connection? Queueing measures the
  target; opening connections measures the target *plus* its accept path. Probably a flag, defaulting
  to queue.
- **Whether `fault::partition` needs peer identity.** Unit 8's three-node store must name its peers.
  Passing that identity in may be the one place this library needs to know something about its host.
