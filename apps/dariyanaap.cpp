// dariyanaap — the load generator.
//
//   dariyanaap --target 127.0.0.1:9000 --connections 64 --duration 10
//   dariyanaap --target 127.0.0.1:8080 --protocol http --path /health
//
// Writes a one-line human summary to stdout and, with --csv-dir, the three
// files DESIGN.md's output schema describes. Everything the run was configured
// with ends up in the CSV header, so a row is reproducible from its own output
// rather than from shell history.

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#include "core/errors.hpp"
#include "core/flags.hpp"
#include "core/version.hpp"
#include "load/closed_loop.hpp"
#include "load/http11_get.hpp"
#include "load/raw_echo.hpp"
#include "stats/csv.hpp"

using namespace dariyanaap;
using namespace std;

namespace {

constexpr const char* kUsage =
    "usage: dariyanaap --target HOST:PORT [options]\n"
    "  --connections N     connections held open (default 1)\n"
    "  --duration MS       measured window in ms (default 10000)\n"
    "  --warmup MS         discarded window before it (default 0)\n"
    "  --protocol NAME     raw | http (default raw)\n"
    "  --payload N         raw echo bytes each way (default 64)\n"
    "  --path P            http path (default /)\n"
    "  --read-timeout MS   (default 1000)\n"
    "  --connect-timeout MS (default 1000)\n"
    "  --csv-dir DIR       write summary.csv and histogram.csv here\n";

double as_ms(Nanos value) {
    return static_cast<double>(value.count()) / 1e6;
}

void report(const ClosedLoopRun& run, const ClosedLoopPlan& plan) {
    printf("%s  target %s  %zu/%zu connections  %llu opens\n", version().data(),
           plan.target.str().c_str(), run.connections_started,
           run.connections_requested,
           static_cast<unsigned long long>(run.connections_opened));

    if (run.connections_started < run.connections_requested) {
        // The rig hit a limit, not the target. Said loudly, because comparing
        // this run's throughput against one that got all its threads is
        // comparing two different experiments.
        printf("  WARNING: only %zu of %zu connections started — the RIG is the limit here\n",
               run.connections_started, run.connections_requested);
    }

    const ErrorCounts& errors = run.result.errors;
    printf("  attempted %llu  errors: connect %llu write %llu read %llu "
           "timeout %llu protocol %llu rejected %llu\n",
           static_cast<unsigned long long>(run.result.attempted),
           static_cast<unsigned long long>(errors.connect),
           static_cast<unsigned long long>(errors.write),
           static_cast<unsigned long long>(errors.read),
           static_cast<unsigned long long>(errors.timeout),
           static_cast<unsigned long long>(errors.protocol),
           static_cast<unsigned long long>(errors.rejected));

    if (!run.result.consistent()) {
        // Requests that went nowhere. Reported rather than hidden: a rig that
        // loses requests silently reports a throughput it never achieved.
        printf("  WARNING: accounting does not balance — %llu attempted, "
               "%llu timed, %llu untimed\n",
               static_cast<unsigned long long>(run.result.attempted),
               static_cast<unsigned long long>(run.result.timed()),
               static_cast<unsigned long long>(run.result.untimed()));
    }

    if (!run.result.latency.has_value()) {
        printf("  no latency distribution: nothing produced a duration\n");
        return;
    }
    const Summary& s = *run.result.latency;
    printf("  %.0f req/s   p50 %.3fms  p90 %.3fms  p99 %.3fms  p999 %.3fms  max %.3fms\n",
           s.per_second(), as_ms(s.p50), as_ms(s.p90), as_ms(s.p99), as_ms(s.p999),
           as_ms(s.max));
    printf("  rig floor: clock resolution %lld ns%s\n",
           static_cast<long long>(run.clock_resolution.count()),
           s.percentiles_bounded() ? "" : "   (percentiles UNBOUNDED: samples past 60s)");
    // No mean, anywhere. DESIGN.md decision 3.
}

void write_csv(const string& directory, const ClosedLoopRun& run,
               const ClosedLoopPlan& plan, const Flags& flags) {
    const string summary_path = directory + "/summary.csv";

    // Append, and write the header only for a new file, so a sweep's six steps
    // land in one file that plots directly.
    const bool fresh = !ifstream(summary_path).good();
    ofstream summary(summary_path, ios::app);
    if (!summary) {
        throw IoError("cannot write " + summary_path);
    }
    if (fresh) {
        summary << "target,protocol,connections_requested,connections_started,"
                   "connections_opened,clock_resolution_ns,";
        csv::write_summary_header(summary);
    }
    summary << csv::quoted(plan.target.str()) << ','
            << csv::quoted(flags.text("protocol", "raw")) << ','
            << plan.connections << ',' << run.connections_started << ','
            << run.connections_opened << ',' << run.clock_resolution.count() << ',';
    if (run.result.latency.has_value()) {
        csv::write_summary_row(summary, *run.result.latency);
    } else {
        // A run with no distribution still gets a row: its error counts are
        // the result. Omitting it would leave a gap in a sweep that looks like
        // a missing step rather than a failed one.
        csv::write_absent_summary_row(summary);
    }

    // Per-run and overwritten, unlike summary.csv: raw slot counts cannot be
    // rebuilt from a summary, and they are what lets a run be re-percentiled
    // later (decision 4).
    const string histogram_path = directory + "/histogram.csv";
    ofstream histogram(histogram_path);
    if (!histogram) {
        throw IoError("cannot write " + histogram_path);
    }
    csv::write_histogram(histogram, run.histogram);
    printf("  wrote %s and %s\n", summary_path.c_str(), histogram_path.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Flags flags = Flags::parse(
            argc, argv,
            {"target", "connections", "duration", "warmup", "protocol", "payload", "path",
             "read-timeout", "connect-timeout", "csv-dir"});

        if (!flags.has("target")) {
            fputs(kUsage, stderr);
            throw UsageError("--target is required");
        }

        ClosedLoopPlan plan;
        plan.target = Endpoint::parse(flags.text("target", ""));
        plan.connections = static_cast<size_t>(flags.number("connections", 1));
        plan.duration = Millis(static_cast<int64_t>(flags.number("duration", 10'000)));
        plan.warmup = Millis(static_cast<int64_t>(flags.number("warmup", 0)));
        plan.worker.read_timeout = Millis(static_cast<int64_t>(flags.number("read-timeout", 1000)));
        plan.worker.write_timeout = plan.worker.read_timeout;
        plan.worker.connect_timeout =
            Millis(static_cast<int64_t>(flags.number("connect-timeout", 1000)));

        if (plan.connections == 0) {
            throw UsageError("--connections must be at least 1");
        }
        if (plan.duration.count() <= 0) {
            throw UsageError("--duration must be at least 1ms");
        }

        const string protocol_name = flags.text("protocol", "raw");
        unique_ptr<Protocol> protocol;
        if (protocol_name == "raw") {
            protocol = make_unique<RawEcho>(static_cast<size_t>(flags.number("payload", 64)));
        } else if (protocol_name == "http") {
            protocol = make_unique<Http11Get>(plan.target.host(), flags.text("path", "/"));
        } else {
            throw UsageError("--protocol must be raw or http, got \"" + protocol_name + "\"");
        }

        const ClosedLoopRun run = run_closed_loop(*protocol, plan);
        report(run, plan);
        if (flags.has("csv-dir")) {
            write_csv(flags.text("csv-dir", "."), run, plan, flags);
        }
        return run.result.consistent() ? 0 : 1;
    } catch (const UsageError& e) {
        fprintf(stderr, "dariyanaap: %s\n", e.what());
        return 2;
    } catch (const Error& e) {
        fprintf(stderr, "dariyanaap: %s\n", e.what());
        return 1;
    }
}
