#include "effects/eqmode.h"

#include <gtest/gtest.h>

#include "util/math.h"

namespace {

constexpr double kMaxCutDb = EqCurve::kDeckMaxCutDb;

double cutDb(EqMode mode, double cut) {
    return ratio2db(EqCurve::cutGain(mode, cut, kMaxCutDb));
}

// Both curves start at unity: nothing is being asked for at the detent.
TEST(EqModeTest, NeutralIsUnityInBothModes) {
    EXPECT_DOUBLE_EQ(1.0, EqCurve::cutGain(EqMode::Eq, 0.0, kMaxCutDb));
    EXPECT_DOUBLE_EQ(1.0, EqCurve::cutGain(EqMode::Isolator, 0.0, kMaxCutDb));
}

// EQ mode bottoms out at a finite shelf; it never silences the band.
TEST(EqModeTest, EqBottomsOutAtMaxCut) {
    EXPECT_NEAR(kMaxCutDb, cutDb(EqMode::Eq, 1.0), 1e-9);
    EXPECT_GT(EqCurve::cutGain(EqMode::Eq, 1.0, kMaxCutDb), 0.0);
}

// Isolator mode reaches actual silence, not nearly silence.
TEST(EqModeTest, IsolatorReachesFullKill) {
    EXPECT_DOUBLE_EQ(0.0, EqCurve::cutGain(EqMode::Isolator, 1.0, kMaxCutDb));
}

// Over the first half of the travel EQ is the more responsive of the two;
// over the second half the isolator is, because it has to get to zero.
TEST(EqModeTest, IsolatorOvertakesEqAtHalfTravel) {
    EXPECT_NEAR(cutDb(EqMode::Eq, 0.5), cutDb(EqMode::Isolator, 0.5), 1e-9);

    for (int step = 1; step < 10; ++step) {
        const double cut = step / 20.0;
        EXPECT_LT(cutDb(EqMode::Eq, cut), cutDb(EqMode::Isolator, cut))
                << "EQ should cut harder than the isolator at cut=" << cut;
    }
    for (int step = 11; step < 20; ++step) {
        const double cut = step / 20.0;
        EXPECT_GT(cutDb(EqMode::Eq, cut), cutDb(EqMode::Isolator, cut))
                << "the isolator should cut harder than EQ at cut=" << cut;
    }
}

// Turning further down never gives back level, in either mode.
TEST(EqModeTest, BothCurvesAreMonotonic) {
    for (EqMode mode : {EqMode::Eq, EqMode::Isolator}) {
        double previous = EqCurve::cutGain(mode, 0.0, kMaxCutDb);
        for (int step = 1; step <= 100; ++step) {
            const double cut = step / 100.0;
            const double gain = EqCurve::cutGain(mode, cut, kMaxCutDb);
            EXPECT_LE(gain, previous) << "at cut=" << cut;
            previous = gain;
        }
    }
}

// The dB form is a plain wrapper in EQ mode and only the isolator's last
// sliver of travel — where the true curve is heading for -inf — is floored.
TEST(EqModeTest, DbFormFloorsOnlyTheIsolatorTail) {
    for (int step = 0; step <= 20; ++step) {
        const double cut = step / 20.0;
        EXPECT_NEAR(kMaxCutDb * cut,
                EqCurve::cutGainDb(EqMode::Eq, cut, kMaxCutDb),
                1e-9);
    }
    EXPECT_DOUBLE_EQ(EqCurve::kMinCutDb,
            EqCurve::cutGainDb(EqMode::Isolator, 1.0, kMaxCutDb));
    EXPECT_GT(EqCurve::cutGainDb(EqMode::Isolator, 0.5, kMaxCutDb),
            EqCurve::kMinCutDb);
}

// Out-of-range requests are clamped rather than extrapolated - std::pow of a
// negative base would be NaN.
TEST(EqModeTest, CutIsClamped) {
    EXPECT_DOUBLE_EQ(1.0, EqCurve::cutGain(EqMode::Isolator, -0.5, kMaxCutDb));
    EXPECT_DOUBLE_EQ(0.0, EqCurve::cutGain(EqMode::Isolator, 1.5, kMaxCutDb));
}

// A deck knob's parameter is its travel: 0 at the stop, 1 at the detent, 2 at
// full boost. Boost is the same either way; only the cut side has two
// characters.
TEST(EqModeTest, DeckBandGainMapsKnobTravel) {
    for (EqMode mode : {EqMode::Eq, EqMode::Isolator}) {
        EXPECT_DOUBLE_EQ(1.0, EqCurve::deckBandGain(mode, 1.0));
        EXPECT_NEAR(EqCurve::kDeckBoostDb,
                ratio2db(EqCurve::deckBandGain(mode, 2.0)),
                1e-9);
        EXPECT_NEAR(EqCurve::kDeckBoostDb / 2,
                ratio2db(EqCurve::deckBandGain(mode, 1.5)),
                1e-9);
    }

    EXPECT_NEAR(kMaxCutDb, ratio2db(EqCurve::deckBandGain(EqMode::Eq, 0.0)), 1e-9);
    EXPECT_DOUBLE_EQ(0.0, EqCurve::deckBandGain(EqMode::Isolator, 0.0));
}

TEST(EqModeTest, ModeFromControlValue) {
    EXPECT_EQ(EqMode::Eq, EqCurve::modeFromControlValue(0.0));
    EXPECT_EQ(EqMode::Isolator, EqCurve::modeFromControlValue(1.0));
}

} // namespace
