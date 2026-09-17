#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dariyanaap {

// `--name value` pairs, and nothing more.
//
// DESIGN.md's stop-here line forbids a config DSL, and this is what that looks
// like in practice: no positional arguments, no short forms, no `=`, no
// grouping. The rig's flags are read once at startup, so the only properties
// that matter are that a typo is refused rather than ignored, and that every
// value ends up printed in the CSV header.
//
// An UNKNOWN flag is an error, not a warning. A run invoked with --connection
// instead of --connections would otherwise use the default silently, and a
// sweep would produce a row that looks like every other row and describes a
// different experiment.
class Flags {
public:
    // Throws UsageError for a malformed argument, an unknown name, or a
    // missing value. `known` is every name this program accepts.
    static Flags parse(int argc, char** argv, const std::vector<std::string>& known);

    bool has(const std::string& name) const;

    // Throw UsageError if present but unparseable; return the default if
    // absent. A flag that is present and wrong must never fall back to a
    // default — that turns a typo into a silently different run.
    std::string text(const std::string& name, const std::string& fallback) const;
    std::uint64_t number(const std::string& name, std::uint64_t fallback) const;
    double real(const std::string& name, double fallback) const;

    // Every flag, in name order, for stamping into a CSV header so a run is
    // reproducible from its own output rather than from shell history.
    const std::map<std::string, std::string>& all() const { return values_; }

private:
    std::map<std::string, std::string> values_;
};

}  // namespace dariyanaap
