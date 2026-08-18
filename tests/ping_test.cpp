#include "plugins/ping.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace seraph;

TEST(PingReply, RoundtripMatchesScriptFormula) {
    // Тот же XOR, что скрипт-тест повторит на своей стороне.
    EXPECT_EQ(ping_reply(0), kMagic);
    EXPECT_EQ(ping_reply(1234), 1234 ^ kMagic);
    EXPECT_EQ(ping_reply(kMagic), 0);  // token ^ magic ^ magic
}

TEST(PingReply, Involution) {
    // Двойное применение возвращает исходное — свойство XOR.
    for (std::int32_t t : {0, 1, -1, 42, 1 << 30}) {
        EXPECT_EQ(ping_reply(ping_reply(t)), t);
    }
}

TEST(ProofLine, HasTokenAndReply) {
    EXPECT_EQ(proof_line(1234, ping_reply(1234)),
              "SERAPH_GRAFT token=1234 reply=" + std::to_string(1234 ^ kMagic));
}
