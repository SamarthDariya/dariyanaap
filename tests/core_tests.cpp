#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/version.hpp"

using namespace dariyanaap;

TEST_CASE("version is reported and non-empty") {
    CHECK_FALSE(version().empty());
}
