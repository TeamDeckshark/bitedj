#include <gtest/gtest.h>

#include "engine/cachingreader/cachingreader.h"
#include "engine/engineworkerscheduler.h"
#include "test/mixxxtest.h"

// Bite DJ: a fast search (SHIFT+jog) must not queue a seek-and-decode for
// every chunk the playhead skips past. No scheduler is attached, so the worker
// never wakes and every request stays in the FIFO where the test can count it.
// (A scheduler is attached but never started: workReady() only marks it.)
class CachingReaderFastSearchTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pReader = std::make_unique<CachingReader>(
                QStringLiteral("[Test]"), config());
        m_pReader->setScheduler(&m_scheduler);
        // Pretend a 100-chunk track is loaded without running the worker.
        m_pReader->m_state = CachingReader::STATE_TRACK_LOADED;
        m_pReader->m_readableFrameIndexRange = mixxx::IndexRange::forward(
                0, 100 * CachingReaderChunk::kFrames);
    }

    void TearDown() override {
        // Hand every queued chunk back the way the worker would, so the reader
        // owns all of its chunks again before it is destroyed.
        while (pendingRequests() > 0) {
            completeOldestRequest();
        }
        m_pReader->process();
        m_pReader.reset();
    }

    // Hints the three chunks starting at firstChunk, as three distinct hints.
    void hintChunks(SINT firstChunk, bool fastSearch) {
        HintVector hints;
        for (SINT i = 0; i < 3; ++i) {
            Hint hint;
            hint.frame = (firstChunk + i) * CachingReaderChunk::kFrames;
            hint.frameCount = CachingReaderChunk::kFrames;
            hint.type = Hint::Type::CurrentPosition;
            hints.append(hint);
        }
        m_pReader->hintAndMaybeWake(hints, fastSearch);
    }

    int pendingRequests() const {
        return m_pReader->m_chunkReadRequestFIFO.readAvailable();
    }

    CachingReaderChunkReadRequest takeOldestRequest() {
        CachingReaderChunkReadRequest request;
        EXPECT_EQ(1, m_pReader->m_chunkReadRequestFIFO.read(&request, 1));
        return request;
    }

    void giveBack(const CachingReaderChunkReadRequest& request) {
        const auto update = ReaderStatusUpdate::readDiscarded(request.chunk);
        m_pReader->m_readerStatusUpdateFIFO.writeBlocking(&update, 1);
    }

    void completeOldestRequest() {
        giveBack(takeOldestRequest());
    }

    EngineWorkerScheduler m_scheduler;
    std::unique_ptr<CachingReader> m_pReader;
};

TEST_F(CachingReaderFastSearchTest, NormalPlaybackRequestsEveryHintedChunk) {
    hintChunks(10, false);
    ASSERT_EQ(3, pendingRequests());
    for (int i = 0; i < 3; ++i) {
        const auto request = takeOldestRequest();
        EXPECT_FALSE(request.speculative);
        EXPECT_EQ(10 + i, request.chunk->getIndex());
        giveBack(request);
    }
}

TEST_F(CachingReaderFastSearchTest, FastSearchKeepsOneSpeculativeRequestInFlight) {
    hintChunks(10, true);
    ASSERT_EQ(1, pendingRequests());

    // The playhead races on while the first chunk is still being decoded:
    // nothing new is queued behind it.
    hintChunks(20, true);
    hintChunks(30, true);
    EXPECT_EQ(1, pendingRequests());

    // Once it comes back, the next request starts from wherever the playhead
    // is now, not from where it was.
    const auto first = takeOldestRequest();
    EXPECT_TRUE(first.speculative);
    EXPECT_EQ(10, first.chunk->getIndex());
    giveBack(first);

    hintChunks(40, true);
    ASSERT_EQ(1, pendingRequests());
    const auto second = takeOldestRequest();
    EXPECT_TRUE(second.speculative);
    EXPECT_EQ(40, second.chunk->getIndex());
    giveBack(second);
}

TEST_F(CachingReaderFastSearchTest, SearchWaitsForRequestsFromNormalPlayback) {
    hintChunks(10, false);
    ASSERT_EQ(3, pendingRequests());
    hintChunks(50, true);
    EXPECT_EQ(3, pendingRequests());
}
