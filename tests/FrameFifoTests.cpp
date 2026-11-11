#include <catch2/catch_test_macros.hpp>

#include "dsp/FrameFifo.h"

using fixomic::FrameFifo;

TEST_CASE("FrameFifo pushes and pops in order", "[fifo]") {
    FrameFifo<float> fifo(8);
    const float in[5] = {1, 2, 3, 4, 5};
    REQUIRE(fifo.push(in, 5));
    REQUIRE(fifo.size() == 5);

    float out[3] = {};
    REQUIRE(fifo.pop(out, 3));
    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 2.0f);
    REQUIRE(out[2] == 3.0f);
    REQUIRE(fifo.size() == 2);

    // Wrap around the end of the ring.
    const float more[5] = {6, 7, 8, 9, 10};
    REQUIRE(fifo.push(more, 5));
    float rest[7] = {};
    REQUIRE(fifo.pop(rest, 7));
    for (int i = 0; i < 7; ++i) REQUIRE(rest[i] == static_cast<float>(i + 4));
    REQUIRE(fifo.size() == 0);
}

TEST_CASE("FrameFifo push beyond capacity fails without partial write", "[fifo]") {
    FrameFifo<float> fifo(4);
    const float in[3] = {1, 2, 3};
    REQUIRE(fifo.push(in, 3));
    REQUIRE_FALSE(fifo.push(in, 2));
    REQUIRE(fifo.size() == 3);

    float out[3] = {};
    REQUIRE(fifo.pop(out, 3));
    REQUIRE(out[2] == 3.0f);
}

TEST_CASE("FrameFifo pop of more than available fails and keeps content", "[fifo]") {
    FrameFifo<float> fifo(4);
    const float in[2] = {1, 2};
    REQUIRE(fifo.push(in, 2));

    float out[4] = {};
    REQUIRE_FALSE(fifo.pop(out, 3));
    REQUIRE(fifo.size() == 2);
    REQUIRE(fifo.pop(out, 2));
    REQUIRE(out[0] == 1.0f);
}

TEST_CASE("FrameFifo pushZeros and clear", "[fifo]") {
    FrameFifo<float> fifo(6);
    const float in[2] = {7, 8};
    REQUIRE(fifo.push(in, 2));
    fifo.pushZeros(3);
    REQUIRE(fifo.size() == 5);

    float out[5] = {};
    REQUIRE(fifo.pop(out, 5));
    REQUIRE(out[0] == 7.0f);
    REQUIRE(out[1] == 8.0f);
    REQUIRE(out[2] == 0.0f);
    REQUIRE(out[4] == 0.0f);

    fifo.pushZeros(2);
    fifo.clear();
    REQUIRE(fifo.size() == 0);
    REQUIRE_FALSE(fifo.pop(out, 1));
}
