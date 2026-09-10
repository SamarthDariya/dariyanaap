#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/errors.hpp"
#include "core/listener.hpp"
#include "core/socket.hpp"
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
