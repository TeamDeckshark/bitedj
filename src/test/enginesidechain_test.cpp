// The sidechain is how the audio callback hands off work it must not do
// itself — encoding and writing a recording, broadcasting. Two properties of
// that handoff are load-bearing and neither is visible from the recording it
// produces, so they are pinned here:
//
//  - the callback's side is a lock-free FIFO write and nothing else. It does
//    not wake the sidechain, so the sidechain has to find the samples on its
//    own; if it ever stopped polling, a recording would simply stop advancing.
//  - a FIFO that has run full drops samples instead of blocking the callback,
//    which is what bounds the sidechain's memory to the FIFO itself however
//    far behind the device it writes to has fallen.
#include "engine/sidechain/enginesidechain.h"

#include <gtest/gtest.h>

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QThread>
#include <memory>
#include <vector>

#include "engine/engine.h"
#include "engine/sidechain/sidechainworker.h"
#include "test/mixxxtest.h"
#include "util/types.h"

namespace {

// Generous compared with the sidechain's own drain interval: this is a "does
// it ever arrive" bound, not a latency measurement, and the thread it waits on
// runs in the idle scheduling class.
constexpr int kDrainTimeoutMs = 5000;

class CountingSideChainWorker : public SideChainWorker {
  public:
    // Called on the sidechain thread.
    void process(const CSAMPLE* pBuffer, const int iBufferSize) override {
        Q_UNUSED(pBuffer);
        m_samples.fetchAndAddRelease(iBufferSize);
    }

    void shutdown() override {
    }

    int samplesProcessed() const {
        return m_samples.loadAcquire();
    }

  private:
    QAtomicInt m_samples;
};

class EngineSideChainTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_sidechainMix.assign(mixxx::kEngineChannelCount * kFramesPerWrite, 0.0f);
        m_pSideChain = std::make_unique<EngineSideChain>(config(), m_sidechainMix.data());
        // Ownership passes to the sidechain, which deletes its workers when it
        // shuts down.
        m_pWorker = new CountingSideChainWorker();
        m_pSideChain->addSideChainWorker(m_pWorker);
    }

    void TearDown() override {
        m_pSideChain.reset();
        m_pWorker = nullptr;
    }

    // Waits for the sidechain to have processed at least `samples`, returning
    // what it had processed when the wait ended.
    int waitForSamples(int samples) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kDrainTimeoutMs) {
            if (m_pWorker->samplesProcessed() >= samples) {
                break;
            }
            QThread::msleep(10);
        }
        return m_pWorker->samplesProcessed();
    }

    static constexpr int kFramesPerWrite = 1024;

    std::vector<CSAMPLE> m_sidechainMix;
    std::unique_ptr<EngineSideChain> m_pSideChain;
    CountingSideChainWorker* m_pWorker = nullptr;
};

TEST_F(EngineSideChainTest, samplesReachTheWorkerWithoutBeingWokenByTheCallback) {
    constexpr int kExpected = kFramesPerWrite * mixxx::kEngineChannelCount;
    m_pSideChain->writeSamples(m_sidechainMix.data(), kFramesPerWrite);
    EXPECT_GE(waitForSamples(kExpected), kExpected);
}

TEST_F(EngineSideChainTest, keepsDrainingAcrossIntervals) {
    // A recording is a long succession of these; the drain has to come back
    // round every time, not just for the first buffer after the thread starts.
    constexpr int kWrites = 20;
    int written = 0;
    for (int i = 0; i < kWrites; ++i) {
        m_pSideChain->writeSamples(m_sidechainMix.data(), kFramesPerWrite);
        written += kFramesPerWrite * mixxx::kEngineChannelCount;
        QThread::msleep(5);
    }
    EXPECT_GE(waitForSamples(written), written);
}

TEST_F(EngineSideChainTest, anOverrunDropsSamplesRatherThanBlockingTheCallback) {
    // Nothing drains between these writes fast enough to make room for all of
    // them, so the FIFO fills and the rest go on the floor. What matters is
    // that every call returns: the caller is the audio callback.
    const int fifoFrames = EngineSideChain::SIDECHAIN_BUFFER_SIZE /
            mixxx::kEngineChannelCount;
    const int writes = (fifoFrames / kFramesPerWrite) * 4;
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < writes; ++i) {
        m_pSideChain->writeSamples(m_sidechainMix.data(), kFramesPerWrite);
    }
    // Wildly loose — the point is "did not park on the sidechain", and this
    // runs on whatever machine CI happens to be.
    EXPECT_LT(timer.elapsed(), kDrainTimeoutMs);
}

} // namespace
