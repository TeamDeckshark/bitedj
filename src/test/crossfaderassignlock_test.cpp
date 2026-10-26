// A deck's crossfader assignment can't be changed while any deck is audible:
// re-assigning mid-mix jumps the level of whatever is on the speakers.
#include "mixer/crossfaderassignlock.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <memory>

#include "control/controlobject.h"
#include "engine/channels/enginechannel.h"
#include "soundio/soundmanager.h"
#include "test/mixxxtest.h"

namespace {

const QString kDeck1 = QStringLiteral("[Channel1]");
const QString kDeck2 = QStringLiteral("[Channel2]");

class CrossfaderAssignLockTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_soundStatus.set(SOUNDMANAGER_CONNECTED);
        m_numDecks.set(2);
        m_xfaderCurve.set(1.0);
        m_xfaderCalibration.set(1.0);
        m_xfaderMode.set(0.0);
        for (const QString& group : {kDeck1, kDeck2}) {
            addDeckControl(group, QStringLiteral("play"), 0.0);
            addDeckControl(group, QStringLiteral("track_loaded"), 1.0);
            addDeckControl(group, QStringLiteral("mute"), 0.0);
            addDeckControl(group, QStringLiteral("volume"), 1.0);
            addDeckControl(group, QStringLiteral("pregain"), 1.0);
            addDeckControl(group, QStringLiteral("orientation"), EngineChannel::CENTER);
        }
        m_pLock = std::make_unique<CrossfaderAssignLock>();
    }

    void TearDown() override {
        m_pLock.reset();
    }

    void addDeckControl(const QString& group, const QString& item, double value) {
        m_deckControls.push_back(std::make_unique<ControlObject>(ConfigKey(group, item)));
        m_deckControls.back()->set(value);
    }

    static ControlObject* co(const QString& group, const QString& item) {
        return ControlObject::getControl(ConfigKey(group, item));
    }

    // What the skin binds the Settings page buttons' enabled to.
    static double assignEnabled() {
        QCoreApplication::processEvents();
        return ControlObject::get(ConfigKey(
                QStringLiteral("[BiteDJ]"), QStringLiteral("xfader_assign_enabled")));
    }

    ControlObject m_soundStatus{
            ConfigKey(QStringLiteral("[SoundManager]"), QStringLiteral("status"))};
    ControlObject m_numDecks{ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks"))};
    ControlObject m_crossfader{
            ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader"))};
    ControlObject m_xfaderCurve{
            ConfigKey(QStringLiteral("[Mixer Profile]"), QStringLiteral("xFaderCurve"))};
    ControlObject m_xfaderCalibration{ConfigKey(
            QStringLiteral("[Mixer Profile]"), QStringLiteral("xFaderCalibration"))};
    ControlObject m_xfaderMode{
            ConfigKey(QStringLiteral("[Mixer Profile]"), QStringLiteral("xFaderMode"))};
    ControlObject m_xfaderReverse{
            ConfigKey(QStringLiteral("[Mixer Profile]"), QStringLiteral("xFaderReverse"))};
    std::vector<std::unique_ptr<ControlObject>> m_deckControls;
    std::unique_ptr<CrossfaderAssignLock> m_pLock;
};

TEST_F(CrossfaderAssignLockTest, ChangeAllowedWhileNothingPlays) {
    EXPECT_EQ(1.0, assignEnabled());
    co(kDeck1, QStringLiteral("orientation"))->set(EngineChannel::LEFT);
    EXPECT_EQ(EngineChannel::LEFT, co(kDeck1, QStringLiteral("orientation"))->get());
}

TEST_F(CrossfaderAssignLockTest, AudibleDeckLocksEveryDeck) {
    co(kDeck1, QStringLiteral("play"))->set(1.0);
    EXPECT_EQ(0.0, assignEnabled());
    co(kDeck1, QStringLiteral("orientation"))->set(EngineChannel::LEFT);
    co(kDeck2, QStringLiteral("orientation"))->set(EngineChannel::RIGHT);
    EXPECT_EQ(EngineChannel::CENTER, co(kDeck1, QStringLiteral("orientation"))->get());
    EXPECT_EQ(EngineChannel::CENTER, co(kDeck2, QStringLiteral("orientation"))->get());

    co(kDeck1, QStringLiteral("play"))->set(0.0);
    EXPECT_EQ(1.0, assignEnabled());
    co(kDeck2, QStringLiteral("orientation"))->set(EngineChannel::RIGHT);
    EXPECT_EQ(EngineChannel::RIGHT, co(kDeck2, QStringLiteral("orientation"))->get());
}

TEST_F(CrossfaderAssignLockTest, SilentPlayingDeckDoesNotLock) {
    // Cueing with the level fader down (the fork's default) is not audible.
    co(kDeck1, QStringLiteral("play"))->set(1.0);
    co(kDeck1, QStringLiteral("volume"))->set(0.0);
    EXPECT_EQ(1.0, assignEnabled());
    co(kDeck1, QStringLiteral("volume"))->set(1.0);
    co(kDeck1, QStringLiteral("mute"))->set(1.0);
    EXPECT_EQ(1.0, assignEnabled());
    co(kDeck1, QStringLiteral("mute"))->set(0.0);
    co(kDeck1, QStringLiteral("track_loaded"))->set(0.0);
    EXPECT_EQ(1.0, assignEnabled());
}

TEST_F(CrossfaderAssignLockTest, CrossfaderCutDeckDoesNotLock) {
    co(kDeck1, QStringLiteral("orientation"))->set(EngineChannel::LEFT);
    co(kDeck1, QStringLiteral("play"))->set(1.0);
    EXPECT_EQ(0.0, assignEnabled());

    // Fader all the way to B: side A is silent.
    m_crossfader.set(1.0);
    EXPECT_EQ(1.0, assignEnabled());
    co(kDeck2, QStringLiteral("orientation"))->set(EngineChannel::RIGHT);
    EXPECT_EQ(EngineChannel::RIGHT, co(kDeck2, QStringLiteral("orientation"))->get());

    // Reversed, the same fader position opens side A again.
    m_xfaderReverse.set(1.0);
    EXPECT_EQ(0.0, assignEnabled());
}

TEST_F(CrossfaderAssignLockTest, NoSoundDeviceDoesNotLock) {
    // A dead output device leaves play latched with nothing to clear it.
    co(kDeck1, QStringLiteral("play"))->set(1.0);
    m_soundStatus.set(SOUNDMANAGER_DISCONNECTED);
    EXPECT_EQ(1.0, assignEnabled());
}

} // namespace
