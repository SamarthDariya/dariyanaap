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
#include "load/raw_echo.hpp"

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
