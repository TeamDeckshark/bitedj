#include "waveform/renderers/waveformmarkset.h"

#include <gtest/gtest.h>

#include <QDomDocument>
#include <QString>
#include <iterator>
#include <memory>

#include "control/controlobject.h"
#include "engine/controls/cuecontrol.h"
#include "skin/legacy/skincontext.h"
#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/cueinfo.h"
#include "waveform/renderers/waveformsignalcolors.h"

namespace {

const QString kGroup = QStringLiteral("[Test]");

class WaveformMarkSetTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        // WaveformMark binds a proxy per slot it is pointed at, so the
        // controls have to exist before the marks are built.
        for (int i = 1; i <= NUM_HOT_CUES; ++i) {
            const QString prefix = QStringLiteral("hotcue_") + QString::number(i);
            createControl(prefix + QStringLiteral("_position"));
            createControl(prefix + QStringLiteral("_endposition"));
            createControl(prefix + QStringLiteral("_type"));
        }
        createControl(QStringLiteral("cue_point"));
    }

    void createControl(const QString& item) {
        m_controls.push_back(std::make_unique<ControlObject>(ConfigKey(kGroup, item)));
    }

    /// Builds a mark set from the children of a <Visual>-like element.
    void setupMarks(const QString& markupXml) {
        QDomDocument doc;
        ASSERT_TRUE(doc.setContent(
                QStringLiteral("<Visual>") + markupXml + QStringLiteral("</Visual>")));
        m_pContext = std::make_unique<SkinContext>(config(), QString());
        m_marks.setup(kGroup, doc.documentElement(), *m_pContext, m_signalColors);
    }

    /// Iterating a mark set walks the marks it would render, which setup()
    /// alone does not populate.
    int markCount() {
        m_marks.update();
        return static_cast<int>(std::distance(m_marks.cbegin(), m_marks.cend()));
    }

    WaveformMarkPointer firstMark() {
        m_marks.update();
        return *m_marks.cbegin();
    }

    std::vector<std::unique_ptr<ControlObject>> m_controls;
    std::unique_ptr<SkinContext> m_pContext;
    WaveformSignalColors m_signalColors;
    WaveformMarkSet m_marks;
};

// A mark can name a hotcue slot instead of a position control. That binding is
// what gets it the cue's end position and type as well, so a saved loop draws
// its range.
TEST_F(WaveformMarkSetTest, HotcueMarkBindsSlot) {
    setupMarks(QStringLiteral(
            "<Mark><Hotcue>3</Hotcue><Text>C</Text><Color>#ff0000</Color></Mark>"));

    const WaveformMarkPointer pMark = m_marks.getHotCueMark(2);
    ASSERT_FALSE(pMark.isNull());
    EXPECT_TRUE(pMark->isValid());
    EXPECT_EQ(2, pMark->getHotCue());
    EXPECT_EQ(QStringLiteral("hotcue_3_position"), pMark->getItem());
    EXPECT_TRUE(pMark->isShowUntilNext());
}

// <Text> on such a mark names the slot, in place of the stock hotcue number.
TEST_F(WaveformMarkSetTest, HotcueMarkTextNamesTheSlot) {
    setupMarks(QStringLiteral(
            "<Mark><Hotcue>1</Hotcue><Text>A</Text><Color>#ff0000</Color></Mark>"
            "<Mark><Hotcue>17</Hotcue><Text>M1</Text><Color>#ff0000</Color></Mark>"));

    EXPECT_EQ(QStringLiteral("A"), m_marks.getHotCueMark(0)->hotcueLabelPrefix());
    EXPECT_EQ(QStringLiteral("M1"), m_marks.getHotCueMark(16)->hotcueLabelPrefix());
}

// Without <Text> the stock hotcue number is used.
TEST_F(WaveformMarkSetTest, HotcueMarkWithoutTextFallsBackToNumber) {
    setupMarks(QStringLiteral("<Mark><Hotcue>5</Hotcue><Color>#ff0000</Color></Mark>"));

    EXPECT_EQ(QStringLiteral("5"), m_marks.getHotCueMark(4)->hotcueLabelPrefix());
}

// A DefaultMark stands in for every slot it generates, so its <Text> cannot
// name any one of them — those marks must keep the number. Skins in the wild
// put a placeholder there, and taking it as a name would label every hotcue
// with the same literal string.
TEST_F(WaveformMarkSetTest, DefaultMarkTextIsNotUsedAsSlotName) {
    setupMarks(QStringLiteral(
            "<DefaultMark><Text> %1 </Text><Color>#ff0000</Color></DefaultMark>"));

    EXPECT_EQ(QStringLiteral("1"), m_marks.getHotCueMark(0)->hotcueLabelPrefix());
    EXPECT_EQ(QStringLiteral("9"), m_marks.getHotCueMark(8)->hotcueLabelPrefix());
}

// An explicit hotcue mark takes the slot; the DefaultMark only fills the rest.
TEST_F(WaveformMarkSetTest, ExplicitHotcueMarkOverridesDefaultMark) {
    setupMarks(QStringLiteral(
            "<DefaultMark><Color>#ff0000</Color></DefaultMark>"
            "<Mark><Hotcue>2</Hotcue><Text>B</Text><Color>#00ff00</Color></Mark>"));

    EXPECT_EQ(QStringLiteral("B"), m_marks.getHotCueMark(1)->hotcueLabelPrefix());
    // Every slot still has exactly one mark.
    EXPECT_EQ(NUM_HOT_CUES, markCount());
    for (int i = 0; i < NUM_HOT_CUES; ++i) {
        EXPECT_FALSE(m_marks.getHotCueMark(i).isNull()) << "missing mark for slot " << i;
    }
}

// Out of range indices are rejected rather than binding a control that does
// not exist. The skin counts from 1, so 0 is out of range too.
TEST_F(WaveformMarkSetTest, OutOfRangeHotcueIndexIsRejected) {
    setupMarks(QStringLiteral(
            "<Mark><Hotcue>0</Hotcue><Color>#ff0000</Color></Mark>"
            "<Mark><Hotcue>%1</Hotcue><Color>#ff0000</Color></Mark>")
                    .arg(NUM_HOT_CUES + 1));

    EXPECT_EQ(0, markCount());
}

// Marks that name a position control keep working, and take no slot.
TEST_F(WaveformMarkSetTest, ControlMarkStillWorks) {
    setupMarks(QStringLiteral(
            "<Mark><Control>cue_point</Control><Text>CUE</Text><Color>#ff0000</Color></Mark>"));

    ASSERT_EQ(1, markCount());
    const WaveformMarkPointer pMark = firstMark();
    EXPECT_TRUE(pMark->isValid());
    EXPECT_EQ(Cue::kNoHotCue, pMark->getHotCue());
    EXPECT_EQ(QStringLiteral("CUE"), pMark->m_text);
    EXPECT_EQ(QStringLiteral("cue_point"), pMark->getItem());
}

// The two banks the skin lays out are both addressable, and land on the
// controls the pads use.
TEST_F(WaveformMarkSetTest, BothCueBanksAreAddressable) {
    setupMarks(QStringLiteral(
            "<Mark><Hotcue>%1</Hotcue><Text>A</Text><Color>#ff0000</Color></Mark>"
            "<Mark><Hotcue>%2</Hotcue><Text>M1</Text><Color>#ff0000</Color></Mark>")
                    .arg(mixxx::kHotCueBankStart + 1)
                    .arg(mixxx::kMemoryCueBankStart + 1));

    EXPECT_EQ(QStringLiteral("hotcue_1_position"),
            m_marks.getHotCueMark(mixxx::kHotCueBankStart)->getItem());
    EXPECT_EQ(QStringLiteral("hotcue_17_position"),
            m_marks.getHotCueMark(mixxx::kMemoryCueBankStart)->getItem());
}

} // namespace
