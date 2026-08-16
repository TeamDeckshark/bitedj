#include <gtest/gtest.h>

#include <memory>

#include "control/controlproxy.h"
#include "test/signalpathtest.h"
#include "track/keyutils.h"

// Covers the fork-added [ChannelN],key_shifted flag. The BiteDJ skin binds it
// to `highlight` on the deck key badge and on the KEY panel's per-deck key, so
// it must be 1 exactly while the playing key renders as something other than
// the file's own key.
class KeyControlTest : public BaseSignalPathTest {
  protected:
    void SetUp() override {
        BaseSignalPathTest::SetUp();

        m_pFileKey = std::make_unique<ControlProxy>(m_sGroup1, "file_key");
        m_pKeyShifted = std::make_unique<ControlProxy>(m_sGroup1, "key_shifted");
        m_pPitch = std::make_unique<ControlProxy>(m_sGroup1, "pitch");
        m_pPitchUp2 = std::make_unique<ControlProxy>(m_sGroup1, "pitch_up_2");
        m_pPitchDown2 = std::make_unique<ControlProxy>(m_sGroup1, "pitch_down_2");
        m_pResetKey = std::make_unique<ControlProxy>(m_sGroup1, "reset_key");
    }

    std::unique_ptr<ControlProxy> m_pFileKey;
    std::unique_ptr<ControlProxy> m_pKeyShifted;
    std::unique_ptr<ControlProxy> m_pPitch;
    std::unique_ptr<ControlProxy> m_pPitchUp2;
    std::unique_ptr<ControlProxy> m_pPitchDown2;
    std::unique_ptr<ControlProxy> m_pResetKey;
};

TEST_F(KeyControlTest, KeyShiftedTracksPitchNudgesAndReset) {
    m_pFileKey->set(KeyUtils::keyToNumericValue(mixxx::track::io::key::A_MINOR));
    EXPECT_DOUBLE_EQ(0.0, m_pKeyShifted->get());

    m_pPitchUp2->set(1.0);
    EXPECT_DOUBLE_EQ(2.0, m_pPitch->get());
    EXPECT_DOUBLE_EQ(1.0, m_pKeyShifted->get());

    // Nudging back to the file key clears the flag without a reset.
    m_pPitchDown2->set(1.0);
    EXPECT_DOUBLE_EQ(0.0, m_pKeyShifted->get());

    m_pPitchDown2->set(1.0);
    EXPECT_DOUBLE_EQ(1.0, m_pKeyShifted->get());

    m_pResetKey->set(1.0);
    EXPECT_DOUBLE_EQ(0.0, m_pPitch->get());
    EXPECT_DOUBLE_EQ(0.0, m_pKeyShifted->get());
}

TEST_F(KeyControlTest, KeyShiftedStaysClearWithoutAFileKey) {
    // No detected key: the badge has nothing to disagree with.
    m_pFileKey->set(KeyUtils::keyToNumericValue(mixxx::track::io::key::INVALID));
    m_pPitchUp2->set(1.0);
    EXPECT_DOUBLE_EQ(0.0, m_pKeyShifted->get());
}
