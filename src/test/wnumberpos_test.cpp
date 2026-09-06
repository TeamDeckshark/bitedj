// Tests for the Bite DJ deck time readout: tapping it flips that deck between
// elapsed and remaining time, and only that deck. The display mode lives in the
// player's own [ChannelN],show_duration_remaining, so the two decks can disagree
// and the REMAIN / TIME indicators in the skin (which bind to the same control)
// follow along.
//
// The taps go through the button-less mouse events a touch is synthesized into
// on the appliance's touchscreen.
#include "widget/wnumberpos.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "test/mixxxtest.h"

namespace {

const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");

constexpr double kElapsed = 0;
constexpr double kRemaining = 1;
constexpr double kElapsedAndRemaining = 2;

// Controls a player normally publishes, one set per deck.
struct DeckControls {
    explicit DeckControls(const QString& group)
            : timeElapsed(ConfigKey(group, QStringLiteral("time_elapsed"))),
              timeRemaining(ConfigKey(group, QStringLiteral("time_remaining"))),
              trackLoaded(ConfigKey(group, QStringLiteral("track_loaded"))),
              showDurationRemaining(
                      ConfigKey(group, QStringLiteral("show_duration_remaining"))) {
        trackLoaded.set(1);
    }

    void setPosition(double elapsed, double remaining) {
        timeRemaining.set(remaining);
        timeElapsed.set(elapsed);
    }

    ControlObject timeElapsed;
    ControlObject timeRemaining;
    ControlObject trackLoaded;
    ControlObject showDurationRemaining;
};

class WNumberPosTest : public MixxxTest {
  protected:
    void SetUp() override {
        // Every WWidget holds a proxy on this one.
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));
        m_pTimeFormat = std::make_unique<ControlObject>(
                ConfigKey("[Controls]", "TimeFormat"));

        m_pDeck1Controls = std::make_unique<DeckControls>(kDeck1);
        m_pDeck2Controls = std::make_unique<DeckControls>(kDeck2);

        m_pDeck1 = std::make_unique<WNumberPos>(kDeck1);
        m_pDeck2 = std::make_unique<WNumberPos>(kDeck2);
        QCoreApplication::processEvents();
    }

    // Mimics the touch translation: the press carries the button in button()
    // but leaves buttons() empty.
    void tap(WNumberPos* pWidget) {
        const QPointF pos(1, 1);
        QMouseEvent press(QEvent::MouseButtonPress,
                pos,
                pos,
                pos,
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                QPointingDevice::primaryPointingDevice());
        QCoreApplication::sendEvent(pWidget, &press);
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<ControlObject> m_pTimeFormat;
    std::unique_ptr<DeckControls> m_pDeck1Controls;
    std::unique_ptr<DeckControls> m_pDeck2Controls;
    std::unique_ptr<WNumberPos> m_pDeck1;
    std::unique_ptr<WNumberPos> m_pDeck2;
};

TEST_F(WNumberPosTest, TapTogglesElapsedAndRemaining) {
    m_pDeck1Controls->showDurationRemaining.set(kElapsed);
    m_pDeck1Controls->setPosition(12.0, 34.0);
    EXPECT_EQ(QStringLiteral("0:12.00"), m_pDeck1->text());

    tap(m_pDeck1.get());
    EXPECT_DOUBLE_EQ(kRemaining, m_pDeck1Controls->showDurationRemaining.get());
    EXPECT_EQ(QStringLiteral("-0:34.00"), m_pDeck1->text());

    tap(m_pDeck1.get());
    EXPECT_DOUBLE_EQ(kElapsed, m_pDeck1Controls->showDurationRemaining.get());
    EXPECT_EQ(QStringLiteral("0:12.00"), m_pDeck1->text());
}

// The combined mode is unreachable by tapping: there is a single time field in
// the skin, and it renders blank in that mode.
TEST_F(WNumberPosTest, TapLeavesElapsedAndRemainingMode) {
    m_pDeck1Controls->showDurationRemaining.set(kElapsedAndRemaining);
    m_pDeck1Controls->setPosition(12.0, 34.0);

    tap(m_pDeck1.get());
    EXPECT_DOUBLE_EQ(kElapsed, m_pDeck1Controls->showDurationRemaining.get());
}

TEST_F(WNumberPosTest, TapAffectsOnlyItsOwnDeck) {
    m_pDeck1Controls->showDurationRemaining.set(kElapsed);
    m_pDeck2Controls->showDurationRemaining.set(kElapsed);
    m_pDeck1Controls->setPosition(12.0, 34.0);
    m_pDeck2Controls->setPosition(56.0, 78.0);

    tap(m_pDeck1.get());

    EXPECT_DOUBLE_EQ(kRemaining, m_pDeck1Controls->showDurationRemaining.get());
    EXPECT_DOUBLE_EQ(kElapsed, m_pDeck2Controls->showDurationRemaining.get());
    EXPECT_EQ(QStringLiteral("-0:34.00"), m_pDeck1->text());
    EXPECT_EQ(QStringLiteral("0:56.00"), m_pDeck2->text());
}

// Every readout of a deck (the play page and the deck header both have one)
// follows the control, so a tap on either one flips them together.
TEST_F(WNumberPosTest, ControlChangeUpdatesReadoutWithoutATap) {
    m_pDeck1Controls->showDurationRemaining.set(kElapsed);
    m_pDeck1Controls->setPosition(12.0, 34.0);

    m_pDeck1Controls->showDurationRemaining.set(kRemaining);
    EXPECT_EQ(QStringLiteral("-0:34.00"), m_pDeck1->text());
}

// time_elapsed/time_remaining keep the last position of an ejected track, so
// the readout has to fall back on track_loaded rather than show a stale time.
TEST_F(WNumberPosTest, UnloadedDeckReadsZero) {
    m_pDeck1Controls->showDurationRemaining.set(kElapsed);
    m_pDeck1Controls->setPosition(12.0, 34.0);
    EXPECT_EQ(QStringLiteral("0:12.00"), m_pDeck1->text());

    m_pDeck1Controls->trackLoaded.set(0);
    EXPECT_EQ(QStringLiteral("0:00.00"), m_pDeck1->text());

    tap(m_pDeck1.get());
    EXPECT_EQ(QStringLiteral("-0:00.00"), m_pDeck1->text());

    m_pDeck1Controls->trackLoaded.set(1);
    EXPECT_EQ(QStringLiteral("-0:34.00"), m_pDeck1->text());
}

} // namespace
