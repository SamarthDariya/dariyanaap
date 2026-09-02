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

// A rate, duration, or connection count that cannot describe a real run.
class InvalidArgument : public Error {
public:
    explicit InvalidArgument(const std::string& what);
};

}  // namespace dariyanaap
