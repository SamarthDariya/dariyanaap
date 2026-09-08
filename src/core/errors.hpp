#pragma once

#include <stdexcept>
#include <string>

namespace dariyanaap {

// Errors in core are exceptions rather than return codes, and that is a
// decision about *where* they happen: every one is raised while parsing
// arguments or opening a run — once, before any measurement starts.
//
// The hot path is the opposite. Nothing in stats or load will throw per
// request, because a run with a 30% error rate must not spend 30% of itself
// unwinding, or the rig would be measuring its own error handling.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what);
};

// A human typed something that cannot describe a real run: a negative rate, a
// port of zero, a duration of "soon".
//
// Named for the human, not for the callee. std::invalid_argument — the obvious
// name — derives from std::logic_error, which the standard reserves for a
// caller violating a precondition, i.e. a bug. This is the opposite: the
// program is fine and the input was wrong, so it derives from runtime_error
// and the CLI turns it into a usage message rather than a crash.
class UsageError : public Error {
public:
    explicit UsageError(const std::string& what);
};

// A target could not be read from its textual form.
//
// Derived from UsageError, not beside it: an endpoint reaches the rig as a
// command-line flag a human typed, so one `catch (const UsageError&)` in the
// CLI prints a usage message for every way the invocation can be wrong.
class InvalidEndpoint : public UsageError {
public:
    explicit InvalidEndpoint(const std::string& what);
};

}  // namespace dariyanaap
