#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/errors.hpp"
#include "core/listener.hpp"
#include "core/socket.hpp"
#include "load/http.hpp"
#include "load/http11_get.hpp"
#include "load/raw_echo.hpp"
#include "load/run_result.hpp"

using namespace dariyanaap;

TEST_CASE("a raw echo request is the size asked for, and stable") {
    const RawEcho protocol(64);
    CHECK(protocol.size() == 64);
    CHECK(protocol.request().size() == 64);
    // Returned by reference, not rebuilt: the send path must do no work per
    // request beyond the write itself.
    CHECK(protocol.request().data() == protocol.request().data());
}

TEST_CASE("a zero-byte payload is refused, because it would measure nothing") {
    // consume() would report Complete on an empty buffer, so every read would
    // look like a finished response and the run would spin at full speed
    // recording latencies for replies that never came.
    CHECK_THROWS_AS(RawEcho(0), UsageError);
    CHECK_NOTHROW(RawEcho(1));
}

TEST_CASE("the payload is a recognisable pattern, not zeros") {
    // A target that replies with a zeroed buffer of the right length would
    // otherwise be indistinguishable from one that echoed correctly.
    const RawEcho protocol(30);
    const std::span<const char> request = protocol.request();
    CHECK(request[0] == 'a');
    CHECK(request[25] == 'z');
    CHECK(request[26] == 'a');  // wraps
}

TEST_CASE("completeness is decided by length alone") {
    const RawEcho protocol(8);
    const std::vector<char> buffer(16, 'a');

    CHECK(protocol.consume({buffer.data(), 0}) == ResponseState::NeedMore);
    CHECK(protocol.consume({buffer.data(), 7}) == ResponseState::NeedMore);
    CHECK(protocol.consume({buffer.data(), 8}) == ResponseState::Complete);
    // More bytes than asked for is a desynchronised stream, not a long
    // response: timings for reply N would be reported against request N+1.
    CHECK(protocol.consume({buffer.data(), 9}) == ResponseState::Malformed);
}

TEST_CASE("a complete raw echo response is a success") {
    const RawEcho protocol(4);
    const std::vector<char> buffer(4, 'a');
    CHECK(protocol.succeeded({buffer.data(), 4}));
}

TEST_CASE("one protocol object serves many threads at once") {
    // The object is shared across the run rather than copied per connection,
    // so every method is const and nothing here may mutate. TSan checks it.
    const RawEcho protocol(128);
    std::vector<std::thread> threads;
    // std::vector<char>, NOT std::vector<bool>. vector<bool> is the bitfield
    // specialisation, so eight threads writing eight "separate" elements are
    // writing bits of the same byte — a genuine data race, which TSan caught
    // here and which showed up in the plain build as an intermittent failure.
    std::vector<char> ok(8, 0);
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&protocol, &ok, t] {
            for (int i = 0; i < 5'000; ++i) {
                const std::span<const char> request = protocol.request();
                if (request.size() != 128) return;
                if (protocol.consume(request) != ResponseState::Complete) return;
            }
            ok[static_cast<std::size_t>(t)] = 1;
        });
    }
    for (std::thread& t : threads) t.join();
    for (const char passed : ok) CHECK(passed == 1);
}

TEST_CASE("a request goes out and an echo comes back through real sockets") {
    // End to end over loopback, using the protocol the way chunk 2.9 will:
    // write request(), read until consume() says Complete.
    const RawEcho protocol(200);
    Listener listener = Listener::bind_ephemeral("127.0.0.1");

    std::thread echo_server([&listener] {
        Socket served = listener.accept();
        std::vector<char> buffer(4096);
        const std::size_t got = served.read_some(std::span<char>(buffer.data(), buffer.size()));
        served.write_all(std::span<const char>(buffer.data(), got));
    });

    Socket client = Socket::connect_any(
        resolve(Endpoint("127.0.0.1", listener.port())), Millis(1000));
    client.set_timeouts(Millis(1000), Millis(1000));
    client.write_all(protocol.request());

    std::vector<char> received;
    received.reserve(protocol.size());
    ResponseState state = ResponseState::NeedMore;
    while (state == ResponseState::NeedMore) {
        char chunk[64] = {};
        const std::size_t got = client.read_some(std::span<char>(chunk, sizeof(chunk)));
        REQUIRE(got > 0);  // 0 would be the peer closing mid-response
        received.insert(received.end(), chunk, chunk + got);
        state = protocol.consume({received.data(), received.size()});
    }

    CHECK(state == ResponseState::Complete);
    CHECK(protocol.succeeded({received.data(), received.size()}));
    // Content verified here, once, rather than on the measured path.
    CHECK(std::string(received.begin(), received.end()) ==
          std::string(protocol.request().begin(), protocol.request().end()));
    echo_server.join();
}

// ---------------------------------------------------------------------------
// Chunk 2.6 — http::build_get and http::status_code
// ---------------------------------------------------------------------------

namespace {

std::span<const char> bytes_of(const std::string& text) {
    return {text.data(), text.size()};
}

}  // namespace

TEST_CASE("a GET request is exactly the bytes a server expects") {
    const std::string request = http::build_get("target.internal", "/health");
    CHECK(request ==
          "GET /health HTTP/1.1\r\n"
          "Host: target.internal\r\n"
          "User-Agent: dariyanaap 0.1.0\r\n"
          "Accept: */*\r\n"
          "\r\n");
    // A request not terminated by a blank line leaves the server waiting for
    // more headers, and the run reports a read timeout against a healthy target.
    CHECK(request.ends_with("\r\n\r\n"));
}

TEST_CASE("the request does not close the connection or offer compression") {
    const std::string request = http::build_get("h", "/");
    // Closing per request would measure TCP setup instead of request handling,
    // and would hide unit 1's thread-per-request collapse behind handshakes.
    CHECK(request.find("Connection:") == std::string::npos);
    // Offering gzip invites a compressed response, and decompressing it would
    // put the rig's CPU on the measured path as though the target caused it.
    CHECK(request.find("Accept-Encoding") == std::string::npos);
}

TEST_CASE("a request that could not be well-formed is refused before the run") {
    CHECK_THROWS_AS(http::build_get("", "/"), UsageError);
    CHECK_THROWS_AS(http::build_get("host", ""), UsageError);
    CHECK_THROWS_AS(http::build_get("host", "health"), UsageError);  // no leading slash

    // Header injection: a CR or LF ends the request line early, so the target
    // receives something other than what was asked for and its complaint gets
    // counted against the target.
    CHECK_THROWS_AS(http::build_get("host\r\nX: y", "/"), UsageError);
    CHECK_THROWS_AS(http::build_get("host", "/\r\nX: y"), UsageError);
    CHECK_THROWS_AS(http::build_get("host", "/\npath"), UsageError);
}

TEST_CASE("status codes are read off the status line") {
    CHECK(http::status_code(bytes_of("HTTP/1.1 200 OK\r\n\r\n")) == 200);
    CHECK(http::status_code(bytes_of("HTTP/1.1 404 Not Found\r\n\r\n")) == 404);
    CHECK(http::status_code(bytes_of("HTTP/1.1 503 Service Unavailable\r\n\r\n")) == 503);
    CHECK(http::status_code(bytes_of("HTTP/1.0 200 OK\r\n\r\n")) == 200);
    // The shortest thing that can carry a code, with no reason phrase.
    CHECK(http::status_code(bytes_of("HTTP/1.1 204")) == 204);
}

TEST_CASE("too few bytes yields nothing rather than a guess") {
    CHECK_FALSE(http::status_code(bytes_of("")).has_value());
    CHECK_FALSE(http::status_code(bytes_of("HTTP/1.1 2")).has_value());
    CHECK_FALSE(http::status_code(bytes_of("HTTP/1.1 20")).has_value());
    // A partial code must not be read as a smaller one: "HTTP/1.1 20" is not 20.
}

TEST_CASE("bytes that are not HTTP yield nothing") {
    CHECK_FALSE(http::status_code(bytes_of("PONGPONGPONG")).has_value());
    CHECK_FALSE(http::status_code(bytes_of("HTTP/2.0 200 OK\r\n\r\n")).has_value());
    CHECK_FALSE(http::status_code(bytes_of("HTTP/1.1 2xx OK\r\n")).has_value());
    CHECK_FALSE(http::status_code(bytes_of("HTTP/1.1 2000 OK\r\n")).has_value());
    // The caller separates "not yet" from "never" by whether the response is
    // complete: complete plus nothing here is a protocol error, and decision 6
    // counts that apart from a read failure.
}

TEST_CASE("the status line is found even with a body attached") {
    const std::string full =
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
    CHECK(http::status_code(bytes_of(full)) == 200);
}

// ---------------------------------------------------------------------------
// Chunk 2.7a — http::header_value and http::content_length
// ---------------------------------------------------------------------------

namespace {

// Everything before the blank line, status line included.
constexpr std::string_view kHeaders =
    "HTTP/1.1 200 OK\r\n"
    "Server: nginx/1.25.3\r\n"
    "Content-Length: 1234\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "X-Not-Content-Length: 9\r\n"
    "Empty:";

}  // namespace

TEST_CASE("a header is found by name and its value trimmed") {
    CHECK(http::header_value(kHeaders, "Content-Length") == "1234");
    CHECK(http::header_value(kHeaders, "Server") == "nginx/1.25.3");
    CHECK(http::header_value(kHeaders, "Content-Type") == "text/plain; charset=utf-8");
}

TEST_CASE("header names match case-insensitively, as the RFC requires") {
    // nginx sends "Content-Length", some frameworks send "content-length". A
    // rig that matched one spelling would report a well-framed response as
    // unparseable and count it as a protocol error.
    CHECK(http::header_value(kHeaders, "content-length") == "1234");
    CHECK(http::header_value(kHeaders, "CONTENT-LENGTH") == "1234");
    CHECK(http::header_value(kHeaders, "cOnTeNt-LeNgTh") == "1234");
}

TEST_CASE("a similar header name is not mistaken for the one asked for") {
    // A substring search for "Content-Length:" would have matched
    // "X-Not-Content-Length: 9" and framed the body at 9 bytes.
    CHECK(http::header_value(kHeaders, "Content-Length") == "1234");
    CHECK(http::header_value(kHeaders, "Not-Content-Length") == std::nullopt);
    CHECK(http::header_value(kHeaders, "Length") == std::nullopt);
    CHECK(http::header_value(kHeaders, "X-Not-Content-Length") == "9");
}

TEST_CASE("an absent header, an empty value, and no headers at all") {
    CHECK(http::header_value(kHeaders, "Transfer-Encoding") == std::nullopt);
    CHECK(http::header_value(kHeaders, "Empty") == "");
    // Status line only: there are no headers to walk.
    CHECK(http::header_value("HTTP/1.1 204 No Content", "Content-Length") == std::nullopt);
}

TEST_CASE("the status line is never matched as a header") {
    // It has no colon before a space, but it does contain "HTTP/1.1", and a
    // naive line loop that did not skip it could still match something.
    CHECK(http::header_value("HTTP/1.1 200 OK\r\nA: b", "HTTP/1.1 200 OK") == std::nullopt);
    CHECK(http::header_value("HTTP/1.1 200 OK\r\nA: b", "A") == "b");
}

TEST_CASE("content length reads a plain decimal count") {
    CHECK(http::content_length(kHeaders) == 1234u);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length: 0") == 0u);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length:   42  ") == 42u);
}

TEST_CASE("a content length that cannot be read is the same as absent") {
    // Both mean the body cannot be framed, and the caller treats an
    // unframeable response as a protocol error either way.
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nServer: x") == std::nullopt);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length:") == std::nullopt);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length: abc") == std::nullopt);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length: +5") == std::nullopt);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length: -5") == std::nullopt);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length: 1 2") == std::nullopt);
    CHECK(http::content_length("HTTP/1.1 200 OK\r\nContent-Length: 0x10") == std::nullopt);
}

TEST_CASE("a content length too large to be a body is refused, not wrapped") {
    // 20 nines overflows uint64. Wrapping would produce a small length, frame
    // the response early, and desynchronise every request after it.
    CHECK(http::content_length(
              "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999") == std::nullopt);
    CHECK(http::content_length(
              "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551615") == 18446744073709551615ull);
}

// ---------------------------------------------------------------------------
// Chunk 2.7b — Http11Get
// ---------------------------------------------------------------------------

namespace {

std::string ok_response(const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

}  // namespace

TEST_CASE("the request is the one http::build_get produces") {
    const Http11Get protocol("target.internal", "/health");
    const std::span<const char> request = protocol.request();
    CHECK(std::string(request.begin(), request.end()) ==
          http::build_get("target.internal", "/health"));
    CHECK(protocol.request().data() == protocol.request().data());  // not rebuilt
}

TEST_CASE("a bad host or path is refused at construction, before the run") {
    CHECK_THROWS_AS((Http11Get("", "/")), UsageError);
    CHECK_THROWS_AS((Http11Get("host", "no-slash")), UsageError);
    CHECK_THROWS_AS((Http11Get("host\r\nX: y", "/")), UsageError);
}

TEST_CASE("a response is complete only when the declared body has all arrived") {
    const Http11Get protocol("h", "/");
    const std::string full = ok_response("hello");

    CHECK(protocol.consume(bytes_of("HTTP/1.1 200 OK\r\n")) == ResponseState::NeedMore);
    CHECK(protocol.consume(bytes_of("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n")) ==
          ResponseState::NeedMore);
    CHECK(protocol.consume(bytes_of(full.substr(0, full.size() - 1))) ==
          ResponseState::NeedMore);
    CHECK(protocol.consume(bytes_of(full)) == ResponseState::Complete);
    CHECK(protocol.succeeded(bytes_of(full)));
}

TEST_CASE("feeding one byte at a time completes at exactly the right total") {
    // What TCP actually does when a response spans segments. Complete must
    // happen on the last byte and not one before or after it.
    const Http11Get protocol("h", "/");
    const std::string full = ok_response("a body of some length");

    std::string received;
    for (std::size_t i = 0; i < full.size(); ++i) {
        received.push_back(full[i]);
        const ResponseState state = protocol.consume(bytes_of(received));
        if (i + 1 < full.size()) {
            REQUIRE(state == ResponseState::NeedMore);
        } else {
            REQUIRE(state == ResponseState::Complete);
        }
    }
}

TEST_CASE("bytes past the declared body are a desynchronised stream") {
    // Not a long response. The extra bytes are the head of a second reply, so
    // continuing would attribute reply N's timing to request N+1.
    const Http11Get protocol("h", "/");
    CHECK(protocol.consume(bytes_of(ok_response("hello") + "H")) ==
          ResponseState::Malformed);
    CHECK(protocol.consume(bytes_of(ok_response("hello") + ok_response("world"))) ==
          ResponseState::Malformed);
}

TEST_CASE("a 200 with no Content-Length cannot be framed, so it is a protocol error") {
    // Guessing where the body ends is what produces silently wrong numbers.
    const Http11Get protocol("h", "/");
    CHECK(protocol.consume(bytes_of("HTTP/1.1 200 OK\r\nServer: x\r\n\r\nbody")) ==
          ResponseState::Malformed);
    CHECK(protocol.consume(bytes_of("HTTP/1.1 200 OK\r\nServer: x\r\n\r\n")) ==
          ResponseState::Malformed);
}

TEST_CASE("statuses defined to carry no body complete at the end of their headers") {
    // Without this a target legitimately answering 204 would be counted as a
    // protocol error, which is exactly the misattribution decision 6 forbids.
    const Http11Get protocol("h", "/");
    CHECK(protocol.consume(bytes_of("HTTP/1.1 204 No Content\r\nServer: x\r\n\r\n")) ==
          ResponseState::Complete);
    CHECK(protocol.consume(bytes_of("HTTP/1.1 304 Not Modified\r\n\r\n")) ==
          ResponseState::Complete);
    CHECK(protocol.consume(bytes_of("HTTP/1.1 100 Continue\r\n\r\n")) ==
          ResponseState::Complete);

    // A 204 is still a success, and bytes after it are still a desync.
    CHECK(protocol.succeeded(bytes_of("HTTP/1.1 204 No Content\r\n\r\n")));
    CHECK(protocol.consume(bytes_of("HTTP/1.1 204 No Content\r\n\r\nx")) ==
          ResponseState::Malformed);
}

TEST_CASE("chunked encoding is refused by name rather than mis-framed") {
    const Http11Get protocol("h", "/");
    CHECK(protocol.consume(bytes_of(
              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n")) ==
          ResponseState::Malformed);
    // Even with a Content-Length alongside it, which is itself a broken
    // response: RFC 9112 says Transfer-Encoding wins and the message is
    // suspect. Refusing is the only reading that cannot desynchronise.
    CHECK(protocol.consume(bytes_of(
              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\nhello")) ==
          ResponseState::Malformed);
}

TEST_CASE("a target that never sends a blank line is bounded, not accumulated") {
    const Http11Get protocol("h", "/");
    std::string babble = "HTTP/1.1 200 OK\r\n";
    babble += std::string(Http11Get::kMaxHeaderBytes / 2, 'x');
    CHECK(protocol.consume(bytes_of(babble)) == ResponseState::NeedMore);

    babble += std::string(Http11Get::kMaxHeaderBytes, 'x');
    CHECK(protocol.consume(bytes_of(babble)) == ResponseState::Malformed);
}

TEST_CASE("success is 2xx, and a redirect is a complete response that failed") {
    const Http11Get protocol("h", "/");
    auto complete = [](int status, const std::string& body) {
        return "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n\r\n" + body;
    };

    for (const int status : {200, 201, 204, 299}) {
        const std::string response = status == 204 ? "HTTP/1.1 204 X\r\n\r\n"
                                                   : complete(status, "b");
        REQUIRE(protocol.consume(bytes_of(response)) == ResponseState::Complete);
        REQUIRE(protocol.succeeded(bytes_of(response)));
    }
    // 3xx: decision 11 excludes redirects, so following one is not on the
    // table, and counting it as a success would report a target that never
    // served the request as having served it.
    for (const int status : {300, 301, 400, 404, 500, 503}) {
        const std::string response = complete(status, "b");
        REQUIRE(protocol.consume(bytes_of(response)) == ResponseState::Complete);
        REQUIRE_FALSE(protocol.succeeded(bytes_of(response)));
    }
}

TEST_CASE("Http11Get is usable through the Protocol interface, like the runner will") {
    const Http11Get concrete("h", "/");
    const Protocol& protocol = concrete;
    const std::string full = ok_response("hi");
    CHECK(protocol.request().size() > 0);
    CHECK(protocol.consume(bytes_of(full)) == ResponseState::Complete);
    CHECK(protocol.succeeded(bytes_of(full)));
}

// ---------------------------------------------------------------------------
// Chunk 2.8 — ErrorCounts and RunResult
// ---------------------------------------------------------------------------

// Declared here as well as in stats_tests.cpp. Duplicated deliberately: the
// two suites link different libraries, and a shared test header for one
// three-line concept would be more machinery than the duplication costs.
template <class T>
concept HasErrorRate = requires(const T& t) { t.error_rate(); };

TEST_CASE("the six failure kinds are separate counters, not one rate") {
    // Decision 6. Enforced the same way the absent mean is: a static_assert,
    // because a comment would not stop anyone adding a convenience accessor.
    static_assert(!HasErrorRate<ErrorCounts>,
                  "DESIGN.md decision 6: never one error rate");
    static_assert(!HasErrorRate<RunResult>);

    ErrorCounts counts;
    CHECK(counts.total() == 0);

    counts.connect = 1;
    counts.write = 2;
    counts.read = 4;
    counts.timeout = 8;
    counts.protocol = 16;
    counts.rejected = 32;
    CHECK(counts.total() == 63);  // distinct powers of two: nothing aliases
}

TEST_CASE("merging error counts sums each kind independently") {
    ErrorCounts a;
    a.connect = 3;
    a.timeout = 5;

    ErrorCounts b;
    b.timeout = 7;
    b.rejected = 11;

    a.merge(b);
    CHECK(a.connect == 3);
    CHECK(a.timeout == 12);
    CHECK(a.rejected == 11);
    CHECK(a.write == 0);
    CHECK(a.total() == 26);
}

TEST_CASE("a run where every connect was refused has counts and no distribution") {
    // The case M1's optional<Summary> was built for. Reporting p99 = 0 here
    // would be the most flattering possible under-report.
    RunResult result;
    result.attempted = 500;
    result.errors.connect = 500;
    result.duration = Secs(10);

    CHECK_FALSE(result.latency.has_value());
    CHECK(result.timed() == 0);
    CHECK(result.untimed() == 500);
    CHECK(result.consistent());
}

TEST_CASE("timeouts and non-2xx replies are in the histogram, so not untimed") {
    // Timeouts because leaving them out is coordinated omission: the slowest
    // requests of the run would be the only ones missing. Non-2xx because
    // latency is a property of time, not of semantics — and unit 9 needs a
    // tripped breaker's fast failures to show up in the caller's p99.
    Histogram histogram;
    for (int i = 0; i < 90; ++i) histogram.record(Micros(100));   // successes
    for (int i = 0; i < 7; ++i) histogram.record(Micros(50));     // fast 503s
    for (int i = 0; i < 3; ++i) histogram.record(Secs(1));        // timeouts

    RunResult result;
    result.attempted = 105;
    result.errors.timeout = 3;
    result.errors.rejected = 7;
    result.errors.connect = 5;   // these five never got a connection
    result.latency = Summary::of(histogram, Secs(2));
    result.duration = Secs(2);

    CHECK(result.timed() == 100);
    CHECK(result.untimed() == 5);       // timeout and rejected are NOT counted here
    CHECK(result.consistent());
    CHECK(result.errors.total() == 15);  // but they are still errors
}

TEST_CASE("a request that vanished is caught by the accounting") {
    // The runner increments these from several places. A request counted as
    // attempted but landing in neither the histogram nor an untimed error is a
    // request that disappeared, and a rig that loses requests silently reports
    // a throughput it never achieved.
    Histogram histogram;
    histogram.record(Micros(10));

    RunResult result;
    result.attempted = 10;
    result.latency = Summary::of(histogram, Secs(1));
    result.errors.connect = 2;
    result.duration = Secs(1);

    CHECK(result.timed() == 1);
    CHECK(result.untimed() == 2);
    CHECK_FALSE(result.consistent());   // 10 != 1 + 2

    result.attempted = 3;
    CHECK(result.consistent());
}

TEST_CASE("an empty run is consistent, rather than a special case") {
    const RunResult result;
    CHECK(result.attempted == 0);
    CHECK(result.consistent());
    CHECK(result.errors.total() == 0);
    CHECK_FALSE(result.latency.has_value());
}
