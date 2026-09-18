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
#include <vector>
#include <memory>
#include <string>

#include "core/errors.hpp"
#include "core/flags.hpp"
#include "core/version.hpp"
#include "load/closed_loop.hpp"
#include "load/http11_get.hpp"
#include "load/open_loop.hpp"
#include "load/raw_echo.hpp"
#include "stats/csv.hpp"
#include "stats/timeseries.hpp"

using namespace dariyanaap;
using namespace std;

namespace {

constexpr const char* kUsage =
    "usage: dariyanaap --target HOST:PORT [options]\n"
    "  --rate RPS          open-loop at this offered rate; omit for closed-loop\n"
    "  --connections N     connections held open (default 1)\n"
    "  --duration MS       measured window in ms (default 10000)\n"
    "  --warmup MS         discarded window before it (default 0)\n"
    "  --protocol NAME     raw | http (default raw)\n"
    "  --payload N         raw echo bytes each way (default 64)\n"
    "  --path P            http path (default /)\n"
    "  --read-timeout MS   (default 1000)\n"
    "  --connect-timeout MS (default 1000)\n"
    "  --csv-dir DIR       write summary.csv, histogram.csv and timeseries.csv here\n"
    "  --timeseries 1      bucket latency by wall-clock second as well\n";

double as_ms(Nanos value) {
    return static_cast<double>(value.count()) / 1e6;
}

// Takes the pieces rather than a mode-specific struct, so the two modes cannot
// drift into reporting different things — which would make the one comparison
// this repo exists for (E3) a comparison of two report formats.
void report(const RunResult& result, const ClosedLoopPlan& plan, const char* mode,
            size_t requested, size_t started, uint64_t opened, Nanos resolution) {
    printf("%s  %s  target %s  %zu/%zu connections  %llu opens\n", version().data(), mode,
           plan.target.str().c_str(), started, requested,
           static_cast<unsigned long long>(opened));

    if (started < requested) {
        // The rig hit a limit, not the target. Said loudly, because comparing
        // this run's throughput against one that got all its threads is
        // comparing two different experiments.
        printf("  WARNING: only %zu of %zu connections started — the RIG is the limit here\n",
               started, requested);
    }

    const ErrorCounts& errors = result.errors;
    printf("  attempted %llu  errors: connect %llu write %llu read %llu "
           "timeout %llu protocol %llu rejected %llu\n",
           static_cast<unsigned long long>(result.attempted),
           static_cast<unsigned long long>(errors.connect),
           static_cast<unsigned long long>(errors.write),
           static_cast<unsigned long long>(errors.read),
           static_cast<unsigned long long>(errors.timeout),
           static_cast<unsigned long long>(errors.protocol),
           static_cast<unsigned long long>(errors.rejected));

    if (!result.consistent()) {
        // Requests that went nowhere. Reported rather than hidden: a rig that
        // loses requests silently reports a throughput it never achieved.
        printf("  WARNING: accounting does not balance — %llu attempted, "
               "%llu timed, %llu untimed\n",
               static_cast<unsigned long long>(result.attempted),
               static_cast<unsigned long long>(result.timed()),
               static_cast<unsigned long long>(result.untimed()));
    }

    if (!result.latency.has_value()) {
        printf("  no latency distribution: nothing produced a duration\n");
        return;
    }
    const Summary& s = *result.latency;
    printf("  %.0f req/s   p50 %.3fms  p90 %.3fms  p99 %.3fms  p999 %.3fms  max %.3fms\n",
           s.per_second(), as_ms(s.p50), as_ms(s.p90), as_ms(s.p99), as_ms(s.p999),
           as_ms(s.max));
    printf("  rig floor: clock resolution %lld ns%s\n",
           static_cast<long long>(resolution.count()),
           s.percentiles_bounded() ? "" : "   (percentiles UNBOUNDED: samples past 60s)");
    // No mean, anywhere. DESIGN.md decision 3.
}

void write_csv(const string& directory, const RunResult& result, const Histogram& histogram,
               const ClosedLoopPlan& plan, const Flags& flags, const char* mode,
               size_t started, uint64_t opened, Nanos resolution,
               const vector<Histogram>& per_second) {
    const string summary_path = directory + "/summary.csv";

    // Append, and write the header only for a new file, so a sweep's six steps
    // land in one file that plots directly.
    const bool fresh = !ifstream(summary_path).good();
    ofstream summary(summary_path, ios::app);
    if (!summary) {
        throw IoError("cannot write " + summary_path);
    }
    if (fresh) {
        summary << "target,protocol,mode,connections_requested,connections_started,"
                   "connections_opened,clock_resolution_ns,";
        csv::write_summary_header(summary);
    }
    summary << csv::quoted(plan.target.str()) << ','
            << csv::quoted(flags.text("protocol", "raw")) << ','
            << csv::quoted(mode) << ','
            << plan.connections << ',' << started << ','
            << opened << ',' << resolution.count() << ',';
    if (result.latency.has_value()) {
        csv::write_summary_row(summary, *result.latency);
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
    ofstream histogram_file(histogram_path);
    if (!histogram_file) {
        throw IoError("cannot write " + histogram_path);
    }
    csv::write_histogram(histogram_file, histogram);
    printf("  wrote %s and %s\n", summary_path.c_str(), histogram_path.c_str());

    if (!per_second.empty()) {
        const string series_path = directory + "/timeseries.csv";
        ofstream series(series_path);
        if (!series) {
            throw IoError("cannot write " + series_path);
        }
        csv::write_timeseries(series, per_second);
        printf("  wrote %s (%zu seconds)\n", series_path.c_str(), per_second.size());
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (Flags::wants_help(argc, argv)) {
            fputs(kUsage, stdout);
            return 0;   // asking for help is not a failure
        }
        const Flags flags = Flags::parse(
            argc, argv,
            {"target", "connections", "duration", "warmup", "protocol", "payload", "path",
             "read-timeout", "connect-timeout", "csv-dir", "rate", "timeseries"});

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
        plan.timeseries = flags.number("timeseries", 0) != 0;

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

        if (!flags.has("rate")) {
            const ClosedLoopRun run = run_closed_loop(*protocol, plan);
            report(run.result, plan, "closed-loop", run.connections_requested,
                   run.connections_started, run.connections_opened, run.clock_resolution);
            if (flags.has("csv-dir")) {
                write_csv(flags.text("csv-dir", "."), run.result, run.histogram, plan,
                          flags, "closed-loop", run.connections_started,
                          run.connections_opened, run.clock_resolution, run.per_second);
            }
            return run.result.consistent() ? 0 : 1;
        }

        OpenLoopPlan open;
        open.target = plan.target;
        open.rate = Rate::per_second(flags.real("rate", 1000.0));
        open.connections = plan.connections;
        open.duration = plan.duration;
        open.warmup = plan.warmup;
        open.worker = plan.worker;

        const OpenLoopRun run = run_open_loop(*protocol, open);
        report(run.result, plan, "open-loop", run.connections_requested,
               run.connections_started, run.connections_opened, run.clock_resolution);
        printf("  schedule: %llu of %llu slots claimed%s",
               static_cast<unsigned long long>(run.slots_claimed),
               static_cast<unsigned long long>(run.slots_due),
               run.kept_up() ? "\n" : "");
        if (!run.kept_up()) {
            // Decision 7's second self-check. The offered load was not what
            // was configured, so the numbers describe a rate nobody asked for.
            printf("   —  WARNING: THE RIG COULD NOT KEEP UP; THIS RUN IS VOID\n");
        }
        if (run.schedule_lag.count() > 0) {
            // Total first, then the split, because the split is the finding and
            // the total is what the total was before anyone knew to ask.
            printf("  schedule lag: p50 %.3fms  p99 %.3fms  max %.3fms\n",
                   as_ms(run.schedule_lag.percentile(50.0).value()),
                   as_ms(run.schedule_lag.percentile(99.0).value()),
                   as_ms(run.schedule_lag.max()));
            printf("    of which:   waiting for a connection p50 %.3fms"
                   "   |   the rig itself p50 %.3fms\n",
                   as_ms(run.connection_wait.percentile(50.0).value()),
                   as_ms(run.rig_lag.percentile(50.0).value()));
        }
        if (run.connections_saturated()) {
            // Not a warning and not void. Every connection carries one request
            // at a time, so `connections / service_time` is a hard ceiling on
            // what can be offered, and past it the rate configured is not the
            // rate delivered. The latencies are honest — the wait is in them.
            printf("   —  NOTE: connections, not --rate, limited this run. %zu connections\n"
                   "      against this target's service time could not offer %.0f rps;\n"
                   "      %.0f rps was achieved. Raise --connections to offer the rate.\n",
                   run.connections_started, run.rate.rps(),
                   run.result.latency.has_value() ? run.result.latency->per_second() : 0.0);
        }
        if (flags.has("csv-dir")) {
            write_csv(flags.text("csv-dir", "."), run.result, run.histogram, plan, flags,
                      "open-loop", run.connections_started, run.connections_opened,
                      run.clock_resolution, {});
        }
        // kept_up() is now a verdict on the rig alone, so a connection-starved
        // run exits 0: it is a valid measurement of a target that is slower
        // than the pool can cover, which is the ordinary case in unit 1 and
        // needs to be scriptable in a sweep.
        return (run.result.consistent() && run.kept_up()) ? 0 : 1;
    } catch (const UsageError& e) {
        fprintf(stderr, "dariyanaap: %s\n", e.what());
        return 2;
    } catch (const Error& e) {
        fprintf(stderr, "dariyanaap: %s\n", e.what());
        return 1;
    }
}
