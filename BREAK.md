# BREAK.md — dariyanaap

Track rule 4: three lines per experiment — what I **predicted**, what I **measured**, and what I got
**wrong**. This file is the learning artifact; the code is just how it gets produced.

Rule: **the prediction is written before the run.** A prediction filled in afterwards is worth
nothing, and the prediction error shrinking across twelve repos is the whole skill being trained.

Unit 0 has one trade-off — **closed-loop vs open-loop** — so it has one headline experiment (E3) and
three supporting ones that exist to make E3 trustworthy.

---

## E1 — Histogram accuracy (M1)

Feed 1M samples from a known distribution (lognormal, plus a deliberate 1-in-1000 spike at 500ms) and
compare the histogram's percentiles against exact percentiles from the sorted samples.

- **Predicted:** p99 error ≤ ___% · p999 error ≤ ___% · memory ___ KB
- **Measured:**
- **Wrong about:**

> Why this one is first: every later number is read out of this data structure. If the histogram is
> off at p999, twelve repos inherit the error and nothing downstream can detect it.

---

## E2 — The rig's own ceiling (M2)

`dariyanaap` (closed-loop) against `dariyanaap-null`, which replies immediately. Ramp connections
1 → 10 → 50 → 100 → 500 → 1000.

- **Predicted:** max throughput ___ rps · p99 floor ___ µs · rig becomes the bottleneck at ___ conns
- **Measured:**
- **Wrong about:**

Also worth predicting first: at what connection count does the *rig's* p99 detach from its p50? The
rig is a thread-per-connection program, and unit 1 is about to break a thread-per-request server on
exactly that. Expect to see the same curve in the tool as in the thing it measures.

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

- **Predicted:** the p99 ratio open/closed is about ___× · p999 ratio about ___×
- **Measured:**
- **Wrong about:**

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
| Rig max throughput (null target) | | E2 |
| Rig p99 floor | | E2 |
| Rig bottleneck concurrency | | E2 |
| Histogram p99 error bound | | E1 |
