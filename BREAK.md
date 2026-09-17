# BREAK.md — dariyanaap

Track rule 4: three lines per experiment — what I **predicted**, what I **measured**, and what I got
**wrong**. This file is the learning artifact; the code is just how it gets produced.

Rule: **the prediction is written before the run.** A prediction filled in afterwards is worth
nothing, and the prediction error shrinking across twelve repos is the whole skill being trained.

Unit 0 has one trade-off — **closed-loop vs open-loop** — so it has one headline experiment (E3) and
three supporting ones that exist to make E3 trustworthy.

---

## E0 — Clock resolution, cold vs warm (M0)

Not a planned experiment. `MonotonicClock::measured_resolution()` was written to be called at
startup, and the number it returned turned out to depend on when it was called.

- **Predicted:** nothing — this was not predicted, which is the point of recording it.
- **Measured:** 90 ns as the first thing the process does · 39 ns after ~200M busy iterations ·
  35 ns immediately after that. Inside the test suite, after other cases have run: 42 ns.
- **Wrong about:** the cause. The code discards one clock read on the assumption that the *first
  read* is slow (cold cache, first call into the commpage). That is real but minor. The dominant
  effect is **CPU frequency scaling** — the core is idling at a low clock and takes time to boost,
  and no amount of discarding single reads fixes that. 42 ns is one tick of the 24 MHz timebase,
  so the warm figure is the hardware floor and the cold one is 2.5x too pessimistic.

**Consequence:** resolution must be measured at the *end* of the warm-up window, not at process
start, or decision 7 publishes a p99 floor more than twice too high into every later repo. Recorded
in `DESIGN.md` decision 8. There is no caller yet — this becomes real code at M2.

---

## E1 — Histogram accuracy (M1)

Feed 1M samples from a known distribution (lognormal, plus a deliberate 1-in-1000 spike at 500ms) and
compare the histogram's percentiles against exact percentiles from the sorted samples.

- **Predicted:** *not recorded before the run.* Rule 4 says a prediction filled in afterwards is
  worth nothing, so this stays empty rather than being back-filled — the empty field is the honest
  record. E2 and E3 get predicted first.
- **Measured:** 1,000,000 samples, seed 20260902, lognormal(log 200µs, 0.6) + 1-in-1000 spike at 500ms.

  | percentile | exact ns | reported ns | error |
  |---|---|---|---|
  | p50 | 200,031 | 200,703 | 0.3359% |
  | p90 | 432,586 | 434,175 | 0.3673% |
  | p99 | 827,686 | 831,487 | 0.4592% |
  | p99.9 | 2,362,840 | 2,375,679 | **0.5434%** |
  | p100 | 500,000,000 | 501,219,327 | 0.2439% |

  Worst error **0.5434%** against the structural bound of 0.7812%. Every percentile reported at or
  above the true value — the never-under-report guarantee held at all five. `record()` cost
  **2.3 ns/op**. Histogram **30,488 bytes**, fixed, against 8,000,000 bytes to keep every sample:
  **262× less memory**. Zero overflow, as expected — 500ms is well inside the 60s range.

- **Wrong about:** nothing that invalidates the design, but two things worth noting.

  **`record()` got faster with more samples** — 3.0 ns/op at 2,000 samples, 2.3 ns/op at 1,000,000.
  Same effect as E0's clock resolution: a cold core and a cold branch predictor. Any per-operation
  cost this rig measures has to come from a warm run or it is 30% pessimistic.

  **p99.9 is the most sensitive percentile in this distribution, and that was luck rather than
  design.** With a 1-in-1000 spike over 1M samples there are ~1,000 spikes, so they occupy exactly
  the top 0.1% — which puts p99.9 precisely on the boundary between the lognormal body and the spike
  population. It is the hardest place to be accurate and it is where the worst error landed. The
  distribution in this experiment was written before that was understood; it turned out to probe the
  right spot for the wrong reason.

> Why this one is first: every later number is read out of this data structure. If the histogram is
> off at p999, twelve repos inherit the error and nothing downstream can detect it.

---

## E2 — The rig's own ceiling (M2)

`dariyanaap` (closed-loop) against `dariyanaap-null`, which replies immediately. Ramp connections
1 → 10 → 50 → 100 → 500 → 1000.

**Platform constraint measured before the run (M2, chunk 2.4b):**
`kern.ipc.somaxconn` on this machine is **128**, and macOS clamps `listen(backlog)` to it silently —
asking for 1024 is not an error and produces no warning, it just gets you 128. So a ramp to 1000
connections offers far more pending connects than the accept queue holds, and the surplus is refused
or dropped. **That is a rig artifact, not a target failure**, and counting it as one would corrupt
the very number this experiment exists to establish. Either ramp connections gradually, or raise the
limit with `sudo sysctl -w kern.ipc.somaxconn=2048` and say in the results which was done.

- **Predicted:** *not recorded before the run,* same as E1 — a 4-connection smoke test during chunk
  2.12 had already shown ~97k rps, so an honest prediction was no longer available. E3 gets
  predicted first; it is the one that matters, and this is now twice.
- **Measured:** `./scripts/e2-sweep.sh`, RawEcho 64 bytes each way, loopback, 3s measured after
  500ms warm-up per step, one `dariyanaap-null` for the whole sweep.

  | conns | req/s | p50 µs | p99 µs | p999 µs | max µs | clock res ns |
  |---|---|---|---|---|---|---|
  | 1 | 43,866 | 21.4 | **51.7** | 87.0 | 177.3 | 41 |
  | 2 | 72,569 | 25.6 | 63.0 | 95.7 | 1,519.6 | 51 |
  | 4 | 95,487 | 40.7 | 73.7 | 102.9 | 239.7 | 56 |
  | 8 | 118,267 | 62.2 | 128.5 | 203.8 | 1,453.8 | 47 |
  | 16 | 131,424 | 116.2 | 184.3 | 224.3 | 499.7 | 42 |
  | **32** | **132,834** | 229.4 | 350.2 | 397.3 | 498.3 | 61 |
  | 64 | 127,229 | 460.8 | 876.5 | 2,162.7 | 4,461.9 | 47 |
  | 128 | 112,269 | 1,065.0 | 1,884.2 | 2,244.6 | 2,697.9 | 58 |
  | 256 | 112,103 | 2,146.3 | 3,293.2 | 17,301.5 | 32,619.3 | 43 |
  | 500 | 107,441 | 4,554.8 | 6,029.3 | 6,488.1 | 6,714.5 | 47 |
  | 1000 | 85,743 | 11,730.9 | 19,398.7 | 53,739.5 | 132,687.6 | 47 |

  All 11 steps started every connection they asked for, including 1000, and every step's accounting
  balanced with zero errors of any kind.

- **Wrong about:** two things, and the second is the more interesting.

  **The clock resolution does not rise with concurrency.** Chunk 2.10 measures it at the warm-up
  boundary while the workers run, on the argument that the floor during a 500-connection run
  genuinely includes scheduler contention, and predicted it would climb. It does not: 41–61ns across
  the whole sweep, with no trend — 41ns at 1 connection and 47ns at 1000. The hardware tick dominates
  and contention is invisible in it. The decision was harmless but the reasoning behind it was wrong.

  **The rig's p99 does not "detach" from its p50 at all.** The prediction in this file was that a
  thread-per-connection program would show the same p99-detaches-from-p50 curve unit 1 is about to
  find in a thread-per-request server. The p99/p50 ratio instead stays between 1.3× and 2.5× at every
  concurrency, with no trend. What happens past saturation is that p50 and p99 rise *together*,
  because the latency is not a tail effect — it is queueing, and queueing delays every request
  equally.

  That is Little's law, and it holds almost exactly:

  | conns | concurrency ÷ throughput | measured p50 |
  |---|---|---|
  | 32 | 241 µs | 229 µs |
  | 500 | 4,654 µs | 4,555 µs |
  | 1000 | 11,663 µs | 11,731 µs |

  So past 32 connections the rig is not degrading, it is *saturated*, and every extra connection
  buys latency instead of throughput. Unit 1 will derive Little's law deliberately; it turned up here
  uninvited, in the tool, on day one.

**These three numbers get stamped into every CSV from here on.** Without them there is no way to tell
a target's ceiling from the rig's.

---

## E3 — Coordinated omission — the headline (M3)

One target (`dariyaraah` from unit 1, or `dariyanaap-null` with `fault::inject_latency`), one offered
load, two modes. Then, mid-run, stall the target for 200ms with `fault::hang_forever()` on a timer.

| | closed-loop | open-loop |
|---|---|---|
| throughput | | |
| p50 | | |
| p99 | | |
| p999 | | |
| max | | |
| requests issued | | |

- **Predicted (by Claude, before the run, committed ahead of the code that measures it —
  Samarth's own prediction slot is below and still his to fill):**

  Offered load 40,000 rps, 32 connections, against a target stalled for 200ms once per second.

  | | closed-loop | open-loop |
  |---|---|---|
  | throughput | ~39,000 | ~39,000 |
  | p50 | ~250 µs | ~300 µs |
  | p99 | ~2 ms | ~90 ms |
  | p999 | ~5 ms | ~180 ms |
  | requests issued | ~117,000 | ~120,000 |

  Reasoning: each 200ms stall blocks all 32 connections. Closed-loop simply stops sending, so it
  records 32 requests at ~200ms each and nothing else — about 0.05% of a 3-second run, which lands
  at p999 and leaves p99 almost untouched. Open-loop keeps the schedule running, so the ~8,000
  requests due during each stall all accrue latency from their intended send time, with the earliest
  waiting nearly the full 200ms. Three stalls in three seconds is ~24,000 of ~120,000 requests, or
  20% — which pushes the stall into p99, not just p999.

  **So the headline prediction: p99 ratio about 45×, p999 ratio about 36×, and throughput nearly
  identical.** The throughput agreeing while p99 differs by more than an order of magnitude is the
  point — it is why a closed-loop load test can report a healthy service that users experience as
  broken.

- **Predicted (Samarth's, unfilled):** p99 ratio ___× · p999 ratio ___×

- **Measured:** `./scripts/e3-omission.sh`. One `dariyanaap-null` stalled 200ms every 1000ms, 32
  connections, 3s measured after 500ms warm-up.

  | mode | req/s | p50 ms | p99 ms | max ms | lag p50 ms | verdict |
  |---|---|---|---|---|---|---|
  | closed-loop | 100,641 | 0.241 | **0.414** | 206.124 | — | n/a |
  | open, 40,000 rps | 37,324 | 0.123 | 201.327 | 206.146 | 0.065 | valid |
  | open, 60,000 rps | 60,000 | 0.093 | 204.472 | 206.458 | 0.036 | valid |
  | open, 80,000 rps | 80,000 | 0.104 | **208.667** | 209.657 | 0.042 | valid |
  | open, 98,000 rps | 93,036 | 105.382 | 210.764 | 212.600 | 104.858 | **VOID** |

  **p99 ratio: 504×** (208.667 / 0.414, at the highest offered rate the rig could sustain).
  Zero errors of any kind in every row; every run's accounting balanced.

  Three things beyond the headline:

  **Closed-loop does see the stall — it just buries it.** Its max is 206ms, so the information is
  there. What differs is the stall's *weight*: closed-loop freezes all 32 connections and therefore
  records 32 slow requests per stall, about 96 of 300,000, or 0.03% — below p999, so it surfaces only
  in max. Open-loop records every request the schedule demanded during the stall, about 16,000 of
  80,000 per stall, or 20%. Coordinated omission does not hide the stall; it reduces its share of the
  distribution by roughly 600×, which moves it from p90 to beyond p999.

  **Open-loop's p99 is ~200ms at every valid rate**, from 40k to 80k. It has to be: the stall is
  200ms and it catches a fifth of all requests, so the 99th percentile lands near the top of the
  stalled population whatever the rate. A number that stable is the signature of measuring the target
  rather than the client.

  **Closed-loop overstates capacity as well as understating latency.** It reports 100,641 rps, but
  the highest rate the rig can actually sustain against this target is 80,000 — 98,000 goes void.
  Closed-loop's 100k is an average that includes bursting to catch up after each stall; it is not a
  rate the service could be offered continuously. So the closed-loop row overstates throughput by 26%
  *and* understates p99 by 504×, in the same run.

- **Wrong about:** the size of the gap, by a factor of 11, and for a reason this repo should have
  made me immune to.

  I predicted a p99 ratio of about 45×; it is 504×. The closed-loop half of the prediction was right
  (0.414ms measured against "~2ms, almost untouched", and the stall landing in max). The error was
  all in the open-loop estimate, which I put at ~90ms and is 208ms.

  The reasoning behind ~90ms: "the ~8,000 requests due during each stall all accrue latency from
  their intended send time, with the earliest waiting nearly the full 200ms" — and then I averaged
  that population to get a middle value. **That is the mean of the stalled requests, not the 99th
  percentile of the whole distribution.** With a fifth of all requests stalled, the worst 1% overall
  is the worst 5% of the stalled ones, which sits near the *top* of the stall window, not its middle.

  So I under-predicted by 11× by reasoning about an average, in the repo whose third design decision
  is that the mean "averages a bimodal distribution whose two modes are the fast path and the problem,
  and reports neither". The distribution here is exactly that, and I used the mean on it anyway.

  Also wrong, more simply: I predicted the two modes would show "nearly identical" throughput. They
  cannot be made to, because **closed-loop has no offered-load knob at all** — its throughput is an
  output. "The same offered load in both modes" is not a thing that can be configured, which is
  itself the reason closed-loop cannot answer "what is the p99 at 80,000 rps?"

  One correction to the rig came out of this. `kept_up()` first compared slots claimed against slots
  due, and that declared the 60,000 rps run void while 80,000 passed against the same target — the
  slot count fails whenever a stall merely overlaps the end of the measured window, through no fault
  of the rig's. The lag distribution said so plainly (p50 of 36µs at 60k against 104,858µs at 98k),
  so the verdict now rests on the median lag against ten schedule intervals, and the slot shortfall
  is reported as information rather than as a judgement.

The specific thing to look for: closed-loop **issues fewer requests** during the stall and never times
the ones it skipped. Count them. The gap between "requests the schedule demanded" and "requests
actually issued" is coordinated omission expressed as an integer, which is a better memory of the
concept than any definition.

---

## E4 — Cost of a disabled fault check (M4)

`dariyanaap-null` in three builds: no `fault` linked · `fault` linked, all knobs off · `fault` linked,
`drop_probability(0.0)` explicitly set.

- **Predicted:** throughput delta ≤ ___% · p99 delta ≤ ___ µs
- **Measured:**
- **Wrong about:**

If this fails, decision 9 in `DESIGN.md` is wrong and targets will need `#ifdef` guards — which means
the build being measured differs from the build being reasoned about, and that has to be fixed here
rather than lived with for twelve repos.

---

## Carried forward

Numbers established here that later units quote rather than re-derive:

| Number | Value | Established in |
|---|---|---|
| Rig max throughput (null target) | **132,834 rps** at 32 connections, RawEcho 64B, loopback | E2 |
| Rig p99 floor | **51.7 µs** at 1 connection (p50 21.4 µs) | E2 |
| Rig bottleneck concurrency | **32 connections** — past it, throughput falls and latency is pure queueing | E2 |
| Rig behaviour past saturation | Little's law, within 3%: p50 ≈ connections ÷ throughput | E2 |
| Histogram p99 error bound | 0.4592% measured, 0.7812% structural | E1 |
| Histogram p999 error bound | 0.5434% measured — the worst of any percentile | E1 |
| `record()` cost | 2.3 ns/op warm (3.0 ns cold) | E1 |
| Histogram memory | 30,488 bytes, independent of sample count | E1 |
| Clock resolution (warm) | **42 ns** — one tick of the 24 MHz timebase | M0 |
| Clock resolution (cold) | 90 ns — an artifact of CPU frequency scaling, not granularity | M0 |
