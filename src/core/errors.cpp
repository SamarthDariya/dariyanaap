#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

// Out of line so each class has one translation unit holding its vtable,
// rather than a copy in every includer.
Error::Error(const string& what) : runtime_error(what) {}

UsageError::UsageError(const string& what) : Error(what) {}

InvalidEndpoint::InvalidEndpoint(const string& what) : UsageError(what) {}

IoError::IoError(const string& what) : Error(what) {}

TimedOut::TimedOut(const string& what) : IoError(what) {}

}  // namespace dariyanaap
