#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

// Out of line so each class has one translation unit holding its vtable,
// rather than a copy in every includer.
Error::Error(const string& what) : runtime_error(what) {}

InvalidArgument::InvalidArgument(const string& what) : Error(what) {}

}  // namespace dariyanaap
