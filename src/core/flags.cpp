#include "core/flags.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

Flags Flags::parse(int argc, char** argv, const vector<string>& known) {
    Flags flags;
    for (int i = 1; i < argc; ++i) {
        const string argument = argv[i];
        if (argument.rfind("--", 0) != 0) {
            throw UsageError("expected a --flag, got \"" + argument + "\"");
        }
        const string name = argument.substr(2);
        if (name.empty()) {
            throw UsageError("\"--\" is not a flag name");
        }
        if (find(known.begin(), known.end(), name) == known.end()) {
            // Listing the accepted names beats "unknown flag": the usual cause
            // is a near miss, and the fix is visible in the message.
            ostringstream out;
            out << "unknown flag --" << name << "; accepted flags are";
            for (const string& accepted : known) {
                out << " --" << accepted;
            }
            throw UsageError(out.str());
        }
        if (i + 1 >= argc) {
            throw UsageError("--" + name + " needs a value");
        }
        flags.values_[name] = argv[++i];
    }
    return flags;
}

bool Flags::has(const string& name) const {
    return values_.find(name) != values_.end();
}

string Flags::text(const string& name, const string& fallback) const {
    const auto found = values_.find(name);
    return found == values_.end() ? fallback : found->second;
}

uint64_t Flags::number(const string& name, uint64_t fallback) const {
    const auto found = values_.find(name);
    if (found == values_.end()) {
        return fallback;
    }
    const string& value = found->second;
    if (value.empty()) {
        throw UsageError("--" + name + " needs a number, got an empty value");
    }
    uint64_t parsed = 0;
    for (const char digit : value) {
        // Hand-rolled for the same reason Endpoint's port is (chunk 0.9):
        // stoull accepts "64abc", " 64" and "+64", and reports failure with a
        // different exception type than this file promises.
        if (digit < '0' || digit > '9') {
            throw UsageError("--" + name + " must be a whole number, got \"" + value + "\"");
        }
        if (parsed > (UINT64_MAX - static_cast<uint64_t>(digit - '0')) / 10) {
            throw UsageError("--" + name + " is too large: \"" + value + "\"");
        }
        parsed = parsed * 10 + static_cast<uint64_t>(digit - '0');
    }
    return parsed;
}

double Flags::real(const string& name, double fallback) const {
    const auto found = values_.find(name);
    if (found == values_.end()) {
        return fallback;
    }
    // istringstream rather than stod so trailing junk is rejected: stod("1.5x")
    // returns 1.5 and says nothing.
    istringstream in(found->second);
    double parsed = 0.0;
    in >> parsed;
    if (in.fail() || !in.eof() || !isfinite(parsed)) {
        throw UsageError("--" + name + " must be a number, got \"" + found->second + "\"");
    }
    return parsed;
}

}  // namespace dariyanaap
