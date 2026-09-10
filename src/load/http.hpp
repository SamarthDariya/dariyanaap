#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace dariyanaap::http {

// The minimal HTTP/1.1 this track needs. Free functions rather than a class,
// because parsing is worth testing without a Protocol wrapped around it —
// Http11Get at chunk 2.7 composes these.
//
// Out of scope permanently, per DESIGN.md decision 11: chunked encoding, TLS,
// HTTP/2, redirects, cookies, authentication. Every target in the track is
// `GET /` over cleartext.

// One GET request, ready to write.
//
// Deliberately absent: `Connection: close`. HTTP/1.1 keeps the connection
// alive by default, and closed-loop mode depends on that — a rig that closed
// after every request would measure connection setup, not request handling,
// and unit 1's thread-per-request collapse would be hidden behind TCP
// handshakes.
//
// Also absent: `Accept-Encoding`. Offering gzip invites a compressed response,
// and decompressing it would put the rig's CPU on the measured path and report
// it as the target's latency.
//
// Throws UsageError if the host or path could not produce a well-formed
// request. A malformed request is not a target failure, but that is exactly
// how it would be counted.
std::string build_get(const std::string& host, const std::string& path);

// The status line's code, or nullopt if the bytes are not yet, or not, a
// status line.
//
// nullopt covers two different situations — too few bytes so far, and bytes
// that can never be a status line. The caller distinguishes them by whether
// the response is complete: incomplete plus nullopt means read again, complete
// plus nullopt means the target is not speaking HTTP, which decision 6 counts
// as a protocol error rather than a read failure.
std::optional<int> status_code(std::span<const char> response);

}  // namespace dariyanaap::http
