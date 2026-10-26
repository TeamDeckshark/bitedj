#include <gtest/gtest.h>

#include <QTest>
#include <algorithm>
#include <cmath>
#include <vector>

#include "analyzer/analyzerbeats.h"
#include "analyzer/analyzertrack.h"
#include "analyzer/constants.h"
#include "test/mixxxtest.h"
#include "track/track.h"

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);
constexpr double kBpm = 120.0;
constexpr double kTrackSeconds = 70.0;
constexpr SINT kTrackFrames = static_cast<SINT>(kTrackSeconds * 44100);
constexpr SINT kChunkFrames = mixxx::kAnalysisFramesPerChunk;

bool isProvisional(const mixxx::BeatsPointer& pBeats) {
    return pBeats &&
            pBeats->getSubVersion().split(QChar('|')).contains(
                    QStringLiteral("provisional=1"));
}

class AnalyzerBeatsProvisionalTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pTrack = Track::newTemporary();
        m_pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(mixxx::kAnalysisChannels),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(kTrackSeconds));

        // A click track: a short decaying 1 kHz burst on every beat.
        m_samples.assign(kTrackFrames * mixxx::kAnalysisChannels, 0.0f);
        const double beatFrames = kSampleRate.value() * 60.0 / kBpm;
        const SINT clickFrames = static_cast<SINT>(0.02 * kSampleRate.value());
        for (double beat = 0; beat < kTrackFrames; beat += beatFrames) {
            const SINT start = static_cast<SINT>(beat);
            for (SINT i = 0; i < clickFrames && start + i < kTrackFrames; ++i) {
                const auto value = static_cast<CSAMPLE>(
                        std::sin(2.0 * M_PI * 1000.0 * i / kSampleRate.value()) *
                        std::exp(-5.0 * i / clickFrames));
                m_samples[(start + i) * 2] = value;
                m_samples[(start + i) * 2 + 1] = value;
            }
        }
    }

    /// Feeds frames [from, to) in analysis-sized chunks.
    void process(AnalyzerBeats* pAnalyzer, SINT fromFrame, SINT toFrame) {
        for (SINT frame = fromFrame; frame < toFrame; frame += kChunkFrames) {
            const SINT frames = std::min(kChunkFrames, toFrame - frame);
            ASSERT_TRUE(pAnalyzer->processSamples(
                    &m_samples[frame * 2], frames * mixxx::kAnalysisChannels));
        }
    }

    SINT framesAt(double seconds) const {
        // Rounded to whole chunks, so process() calls line up.
        return static_cast<SINT>(seconds * kSampleRate.value()) / kChunkFrames * kChunkFrames;
    }

    TrackPointer m_pTrack;
    std::vector<CSAMPLE> m_samples;
};

TEST_F(AnalyzerBeatsProvisionalTest, GridGrowsWhileAnalyzing) {
    AnalyzerBeats analyzer(config(), true, true);
    ASSERT_TRUE(analyzer.initialize(AnalyzerTrack(m_pTrack), kSampleRate, kTrackFrames));

    process(&analyzer, 0, framesAt(25));
    EXPECT_FALSE(m_pTrack->getBeats());

    process(&analyzer, framesAt(25), framesAt(35));
    const mixxx::BeatsPointer pFirst = m_pTrack->getBeats();
    ASSERT_TRUE(pFirst);
    EXPECT_TRUE(isProvisional(pFirst));
    EXPECT_NEAR(kBpm, m_pTrack->getBpm(), 1.0);
    EXPECT_FALSE(m_pTrack->canUndoBeatsChange());

    process(&analyzer, framesAt(35), framesAt(65));
    const mixxx::BeatsPointer pSecond = m_pTrack->getBeats();
    EXPECT_NE(pFirst, pSecond);
    EXPECT_TRUE(isProvisional(pSecond));
    EXPECT_FALSE(m_pTrack->canUndoBeatsChange());

    process(&analyzer, framesAt(65), kTrackFrames);
    analyzer.storeResults(m_pTrack);
    analyzer.cleanup();
    const mixxx::BeatsPointer pFinal = m_pTrack->getBeats();
    ASSERT_TRUE(pFinal);
    EXPECT_FALSE(isProvisional(pFinal));
    EXPECT_NEAR(kBpm, m_pTrack->getBpm(), 1.0);
    // Undo never steps back into a provisional grid.
    EXPECT_FALSE(m_pTrack->canUndoBeatsChange());
}

TEST_F(AnalyzerBeatsProvisionalTest, BatchAnalysisPublishesOnlyTheResult) {
    AnalyzerBeats analyzer(config(), true, false);
    ASSERT_TRUE(analyzer.initialize(AnalyzerTrack(m_pTrack), kSampleRate, kTrackFrames));
    process(&analyzer, 0, framesAt(65));
    EXPECT_FALSE(m_pTrack->getBeats());
    process(&analyzer, framesAt(65), kTrackFrames);
    analyzer.storeResults(m_pTrack);
    analyzer.cleanup();
    EXPECT_FALSE(isProvisional(m_pTrack->getBeats()));
}

TEST_F(AnalyzerBeatsProvisionalTest, EditOfProvisionalGridIsKept) {
    AnalyzerBeats analyzer(config(), true, true);
    ASSERT_TRUE(analyzer.initialize(AnalyzerTrack(m_pTrack), kSampleRate, kTrackFrames));
    process(&analyzer, 0, framesAt(35));
    const mixxx::BeatsPointer pProvisional = m_pTrack->getBeats();
    ASSERT_TRUE(isProvisional(pProvisional));

    // The DJ hits Align on the provisional grid.
    const auto edited = pProvisional->tryTranslate(1000);
    ASSERT_TRUE(edited);
    ASSERT_TRUE(m_pTrack->trySetBeats(*edited));
    const auto editedFirstBeat = (*edited)->firstBeat();

    // Further provisional grids leave the edit alone...
    process(&analyzer, framesAt(35), framesAt(65));
    EXPECT_EQ(*edited, m_pTrack->getBeats());

    // ...and so does the result, which only becomes what Undo restores.
    process(&analyzer, framesAt(65), kTrackFrames);
    // Past the undo stack's 800 ms "quick succession" window since the edit.
    QTest::qSleep(810);
    analyzer.storeResults(m_pTrack);
    analyzer.cleanup();
    const mixxx::BeatsPointer pKept = m_pTrack->getBeats();
    EXPECT_FALSE(isProvisional(pKept));
    EXPECT_DOUBLE_EQ(editedFirstBeat.value(), pKept->firstBeat().value());
    ASSERT_TRUE(m_pTrack->canUndoBeatsChange());
    m_pTrack->undoBeatsChange();
    EXPECT_FALSE(isProvisional(m_pTrack->getBeats()));
    EXPECT_NE(pKept, m_pTrack->getBeats());
}

TEST_F(AnalyzerBeatsProvisionalTest, InterruptedAnalysisIsRedone) {
    {
        AnalyzerBeats analyzer(config(), true, true);
        ASSERT_TRUE(analyzer.initialize(AnalyzerTrack(m_pTrack), kSampleRate, kTrackFrames));
        process(&analyzer, 0, framesAt(35));
        // The track is ejected mid-analysis.
        analyzer.cleanup();
    }
    ASSERT_TRUE(isProvisional(m_pTrack->getBeats()));

    AnalyzerBeats analyzer(config(), true, true);
    EXPECT_TRUE(analyzer.initialize(AnalyzerTrack(m_pTrack), kSampleRate, kTrackFrames));
    analyzer.cleanup();
}

} // namespace
