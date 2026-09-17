#pragma once

#include <string>
#include <string_view>

#include "core/units.hpp"

namespace dariyanaap::fault {

// Failure injection, linked INTO the service under test.
//
// The other half of this repo. A load generator alone cannot create the
// failures this track needs: unit 1's "fake DB call that sleeps 20ms" is a
// function call inside the service, unit 3's stampede needs the origin slow
// behind the cache, and unit 8's partition must drop messages between two
// peers of a three-node cluster while leaving both reachable from the client.
// A proxy in front sees none of that (DESIGN.md decision 1).
//
// Depends on core only. Never on stats: this gets linked into other people's
// services, and a fault knob that drags a histogram implementation along is a
// nuisance nobody asked for.
//
// THE COST WHEN OFF, which is decision 9 and the reason this is usable at all:
// every check below starts with one relaxed load of a single bool. A target
// can therefore call these unconditionally on its hot path, which matters more
// than it sounds — if the checks were expensive, targets would guard them with
// #ifdef, the fault-injecting build would differ from the measured build, and
// the numbers would come from a different program than the one being reasoned
// about. E4 measures the claim.

// ---------------------------------------------------------------------------
// What a target calls
// ---------------------------------------------------------------------------

// Call immediately before sending a response. Applies injected latency, and
// blocks for as long as hang_forever is set.
//
// Returns after the hang ends rather than never returning, so healing works: a
// run that could not be un-hung would make unit 2's "restart a backend and
// watch it get slammed" impossible to stage.
void before_response();

// Should this response be dropped? The caller decides what dropping means —
// close the connection, or return nothing and let the client time out — and
// which it picks matters, so the library does not choose.
bool should_drop();

// Is traffic between this node and `peer` currently cut?
//
// For unit 8. Both names come from the cluster's own configuration; this
// library has no opinion about what a node is called.
bool blocked(std::string_view peer);

// ---------------------------------------------------------------------------
// What a test, an env var, or the control socket calls
// ---------------------------------------------------------------------------

// Mean added latency, plus uniform jitter either side of it.
//
// Jitter is not decoration. Injecting a fixed delay makes every affected
// request finish in lockstep, which produces a thundering herd the experiment
// did not ask for and a latency histogram with one enormous spike instead of a
// distribution.
void set_latency(Millis mean, Millis jitter);

// Fraction of responses to drop, 0.0 to 1.0.
void set_drop_probability(double probability);

// Accept and never answer. The failure mode that is worse than being down,
// and the one unit 9's circuit breaker exists for.
void set_hang_forever(bool hanging);

// This node's name, for partition decisions.
void set_identity(std::string name);

// Cut traffic between two nodes, symmetrically. Naming this node as one of the
// pair is the usual case; naming two others is legal and does nothing here.
void partition(std::string_view a, std::string_view b);
void heal(std::string_view a, std::string_view b);

// Every FAULT back to off. What a test calls between cases, and what the
// control socket's "reset" does.
//
// Identity survives, deliberately: a node's name is configuration set once at
// startup, not a fault, and a reset that made a cluster forget who its members
// were would be a worse failure than the one being staged. Tests that need a
// clean identity call set_identity("") — which blocked() reads as "nobody told
// us who we are", and answers no to everything.
void clear();

// Whether anything at all is enabled — the single bool every check reads.
bool any_enabled();

}  // namespace dariyanaap::fault
