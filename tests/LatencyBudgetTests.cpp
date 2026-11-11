#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/LatencyBudget.h"

using fixomic::LatencyBreakdown;

TEST_CASE("LatencyBreakdown totalMs sums every stage", "[latency]") {
    const LatencyBreakdown b{48000.0, 24, 24, 128, 480, 0};
    REQUIRE(b.totalMs() == Catch::Approx(13.67).margin(0.01));
}

TEST_CASE("LatencyBreakdown totalMs includes plugin latency", "[latency]") {
    const LatencyBreakdown b{48000.0, 0, 0, 0, 0, 480};
    REQUIRE(b.totalMs() == Catch::Approx(10.0).margin(1e-9));
}

TEST_CASE("LatencyBreakdown totalMs is 0 without a valid sample rate", "[latency]") {
    REQUIRE(LatencyBreakdown{0.0, 24, 24, 128, 480, 0}.totalMs() == 0.0);
    REQUIRE(LatencyBreakdown{-1.0, 24, 24, 128, 480, 0}.totalMs() == 0.0);
}
