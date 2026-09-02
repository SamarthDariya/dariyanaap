#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "core/errors.hpp"
#include "core/version.hpp"

using namespace dariyanaap;

TEST_CASE("version is reported and non-empty") {
    CHECK_FALSE(version().empty());
}

TEST_CASE("every core error is catchable as Error") {
    CHECK_THROWS_AS(throw InvalidArgument("bad rate"), Error);
    CHECK_THROWS_AS(throw InvalidArgument("bad rate"), std::runtime_error);
}
