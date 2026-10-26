// The Levels page (output gains + master Graphic EQ) has to come back as the
// DJ left it after the unit is switched off at the wall, where no clean
// shutdown ever writes effects.xml. Every change goes to mixxx.cfg instead, and
// the stored value seeds the control on the next start.
#include <gtest/gtest.h>

#include <memory>

#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "effects/effectsmanager.h"
#include "engine/channelhandle.h"
#include "engine/enginemixer.h"
#include "test/mixxxtest.h"

namespace {

const QString kMainGroup = QStringLiteral("[Master]");
const QString kOutputEqGroup = QStringLiteral("[OutputEffectRack_[Master]_Effect1]");

ConfigKey stored(const QString& item) {
    return ConfigKey(QStringLiteral("[BiteDJ]"), item);
}

class LevelsPersistenceTest : public MixxxTest {
  protected:
    void start() {
        m_pFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_unique<EffectsManager>(config(), m_pFactory);
        m_pEngineMixer = std::make_unique<EngineMixer>(
                config(), kMainGroup, m_pEffectsManager.get(), m_pFactory, false);
        m_pEffectsManager->setup();
    }

    void stop() {
        m_pEngineMixer.reset();
        m_pEffectsManager.reset();
    }

    void TearDown() override {
        stop();
    }

    mixxx::ControlIndicatorTimer m_indicatorTimer;
    ControlObject m_numDecks{ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks"))};
    ControlObject m_numSamplers{
            ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_samplers"))};
    ControlObject m_numPreviewDecks{
            ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_preview_decks"))};
    std::shared_ptr<ChannelHandleFactory> m_pFactory;
    std::unique_ptr<EffectsManager> m_pEffectsManager;
    std::unique_ptr<EngineMixer> m_pEngineMixer;
};

TEST_F(LevelsPersistenceTest, GainChangesAreStored) {
    start();
    ControlProxy(kMainGroup, QStringLiteral("gain")).set(0.5);
    ControlProxy(kMainGroup, QStringLiteral("booth_gain")).set(2.0);
    ControlProxy(kMainGroup, QStringLiteral("headGain")).set(0.25);
    EXPECT_DOUBLE_EQ(0.5, config()->getValue(stored(QStringLiteral("master_gain")), 0.0));
    EXPECT_DOUBLE_EQ(2.0, config()->getValue(stored(QStringLiteral("booth_gain")), 0.0));
    EXPECT_DOUBLE_EQ(0.25, config()->getValue(stored(QStringLiteral("head_gain")), 0.0));
}

TEST_F(LevelsPersistenceTest, StoredGainsAreRestored) {
    config()->setValue(stored(QStringLiteral("master_gain")), 0.5);
    config()->setValue(stored(QStringLiteral("booth_gain")), 2.0);
    config()->setValue(stored(QStringLiteral("head_gain")), 0.25);
    start();
    EXPECT_DOUBLE_EQ(0.5, ControlObject::get(ConfigKey(kMainGroup, QStringLiteral("gain"))));
    EXPECT_DOUBLE_EQ(2.0,
            ControlObject::get(ConfigKey(kMainGroup, QStringLiteral("booth_gain"))));
    EXPECT_DOUBLE_EQ(0.25,
            ControlObject::get(ConfigKey(kMainGroup, QStringLiteral("headGain"))));
}

TEST_F(LevelsPersistenceTest, NothingStoredLeavesUnity) {
    start();
    EXPECT_DOUBLE_EQ(1.0, ControlObject::get(ConfigKey(kMainGroup, QStringLiteral("gain"))));
    EXPECT_FALSE(config()->exists(stored(QStringLiteral("master_gain"))));
}

TEST_F(LevelsPersistenceTest, MasterEqBandChangesAreStored) {
    start();
    ControlProxy(kOutputEqGroup, QStringLiteral("parameter3")).set(4.5);
    EXPECT_DOUBLE_EQ(4.5, config()->getValue(stored(QStringLiteral("master_eq_3")), 0.0));
    EXPECT_FALSE(config()->exists(stored(QStringLiteral("master_eq_4"))));
}

TEST_F(LevelsPersistenceTest, StoredMasterEqBandsAreRestored) {
    config()->setValue(stored(QStringLiteral("master_eq_1")), -6.0);
    config()->setValue(stored(QStringLiteral("master_eq_8")), 3.0);
    start();
    EXPECT_DOUBLE_EQ(-6.0,
            ControlObject::get(ConfigKey(kOutputEqGroup, QStringLiteral("parameter1"))));
    EXPECT_DOUBLE_EQ(3.0,
            ControlObject::get(ConfigKey(kOutputEqGroup, QStringLiteral("parameter8"))));
    EXPECT_DOUBLE_EQ(0.0,
            ControlObject::get(ConfigKey(kOutputEqGroup, QStringLiteral("parameter4"))));
}

// A power cut means effects.xml is whatever the last clean shutdown left, so
// the per-change copy in mixxx.cfg must win over it.
TEST_F(LevelsPersistenceTest, StoredBandWinsOverEffectsXml) {
    start();
    ControlProxy(kOutputEqGroup, QStringLiteral("parameter2")).set(5.0);
    stop(); // clean shutdown: effects.xml now holds +5
    config()->setValue(stored(QStringLiteral("master_eq_2")), -2.0);
    start();
    EXPECT_DOUBLE_EQ(-2.0,
            ControlObject::get(ConfigKey(kOutputEqGroup, QStringLiteral("parameter2"))));
}

} // namespace
