// The Bite DJ Beat FX unit only exposes the unit mix on the unshifted
// DDJ-400 LEVEL/DEPTH knob, so an effect picked for the first time must start
// with its metaknob somewhere audible. Inheriting the slot's meta of 0 drove
// linked knobs like Echo's send and Flanger's mix to zero, leaving the effect
// silent until the user found SHIFT + LEVEL/DEPTH.
#include <gtest/gtest.h>

#include "control/controlobject.h"
#include "effects/backends/effectsbackendmanager.h"
#include "effects/effectchain.h"
#include "effects/effectslot.h"
#include "effects/effectsmanager.h"
#include "engine/channelhandle.h"
#include "test/mixxxtest.h"

namespace {

class EffectMetaknobDefaultTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_unique<EffectsManager>(config(), m_pFactory);
        ChannelHandleAndGroup output(
                m_pFactory->getOrCreateHandle("[MasterOutput]"), "[MasterOutput]");
        ChannelHandleAndGroup deck(m_pFactory->getOrCreateHandle("[Channel1]"), "[Channel1]");
        m_pEffectsManager->registerOutputChannel(output);
        m_pEffectsManager->registerInputChannel(output);
        m_pEffectsManager->registerInputChannel(deck);
        m_pEffectsManager->setup();
        m_pSlot = m_pEffectsManager->getStandardEffectChain(0)->getEffectSlot(0);
        ASSERT_TRUE(m_pSlot);
    }

    void TearDown() override {
        m_pSlot.reset();
        m_pEffectsManager.reset();
    }

    void pick(const QString& effectId) {
        const auto pManifest = m_pEffectsManager->getBackendManager()->getManifest(
                effectId, EffectBackendType::BuiltIn);
        ASSERT_TRUE(pManifest) << effectId.toStdString();
        m_pSlot->switchEffectRemembering(pManifest);
    }

    /// Normalized position of the first metaknob-linked knob.
    double linkedKnobParameter() const {
        for (int i = 1; i <= 16; ++i) {
            const QString prefix = QStringLiteral("parameter%1").arg(i);
            if (get(prefix + "_loaded") == 1 && get(prefix + "_link_type") != 0) {
                return ControlObject::getControl(ConfigKey(m_pSlot->getGroup(), prefix))
                        ->getParameter();
            }
        }
        ADD_FAILURE() << "no linked knob loaded";
        return -1;
    }

    double get(const QString& item) const {
        return ControlObject::get(ConfigKey(m_pSlot->getGroup(), item));
    }

    void set(const QString& item, double value) {
        ControlObject::set(ConfigKey(m_pSlot->getGroup(), item), value);
    }

    std::shared_ptr<ChannelHandleFactory> m_pFactory;
    std::unique_ptr<EffectsManager> m_pEffectsManager;
    EffectSlotPointer m_pSlot;
};

TEST_F(EffectMetaknobDefaultTest, FirstPickedEchoIsAudible) {
    ASSERT_EQ(0.0, get("meta"));
    pick(QStringLiteral("org.mixxx.effects.echo"));
    EXPECT_GT(get("meta"), 0.5);
    EXPECT_NEAR(get("meta"), linkedKnobParameter(), 1e-6);
}

TEST_F(EffectMetaknobDefaultTest, FirstPickDoesNotInheritPreviousMeta) {
    pick(QStringLiteral("org.mixxx.effects.echo"));
    set("meta", 0.0);
    pick(QStringLiteral("org.mixxx.effects.flanger"));
    EXPECT_DOUBLE_EQ(1.0, get("meta"));
    EXPECT_DOUBLE_EQ(1.0, linkedKnobParameter());
}

TEST_F(EffectMetaknobDefaultTest, RememberedMetaWinsOnLaterPicks) {
    pick(QStringLiteral("org.mixxx.effects.echo"));
    set("meta", 0.2);
    pick(QStringLiteral("org.mixxx.effects.flanger"));
    pick(QStringLiteral("org.mixxx.effects.echo"));
    EXPECT_DOUBLE_EQ(0.2, get("meta"));
    EXPECT_NEAR(0.2, linkedKnobParameter(), 1e-6);
}

} // namespace
