#include <gtest/gtest.h>

#include "engine/controls/ratecontrol.h"
#include "util/rotary.h"

TEST(RotaryTest, FilterLengthIsClamped) {
    Rotary rotary;

    rotary.setFilterLength(0);
    EXPECT_EQ(1, rotary.getFilterLength());

    rotary.setFilterLength(65);
    EXPECT_EQ(64, rotary.getFilterLength());
}

TEST(RotaryTest, ChangingLengthClearsOldSamples) {
    Rotary rotary;
    rotary.setFilterLength(2);

    EXPECT_DOUBLE_EQ(1.0, rotary.filter(2.0));
    EXPECT_DOUBLE_EQ(2.0, rotary.filter(2.0));

    rotary.setFilterLength(4);
    EXPECT_DOUBLE_EQ(1.0, rotary.filter(4.0));
}

TEST(RateControlTest, JogFilterLengthIsClamped) {
    RateControl::setJogFilterLength(0);
    EXPECT_EQ(RateControl::kDefaultJogFilterLength,
            RateControl::getJogFilterLength());

    RateControl::setJogFilterLength(100);
    EXPECT_EQ(RateControl::kDefaultJogFilterLength,
            RateControl::getJogFilterLength());

    RateControl::setJogFilterLength(RateControl::kMaxJogFilterLength);
    EXPECT_EQ(RateControl::kMaxJogFilterLength,
            RateControl::getJogFilterLength());

    RateControl::setJogFilterLength(RateControl::kDefaultJogFilterLength);
}
