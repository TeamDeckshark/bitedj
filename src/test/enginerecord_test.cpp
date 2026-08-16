// End-to-end cover for the recorder's file path: EngineRecord driven the way
// the sidechain thread drives it, writing a real file to a real filesystem.
//
// What it is here to catch is the write path growing a step that quietly
// damages the recording — the page-cache limiter that keeps a long set from
// filling memory with dirty pages issues its syscalls between the encoder's
// writes, and the file it leaves behind still has to be complete and playable.
#include "engine/sidechain/enginerecord.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "engine/engine.h"
#include "recording/defs_recording.h"
#include "test/mixxxtest.h"
#include "util/types.h"

namespace {

constexpr int kSampleRate = 44100;
constexpr int kFramesPerBuffer = 1024;
// Enough to write several megabytes, so the page-cache limiter's window rolls
// over more than once during the recording instead of never being reached.
constexpr int kBuffers = 768;
// WAV: 44 byte canonical header, 16 bit stereo frames.
constexpr int kWavHeaderBytes = 44;
constexpr int kBytesPerFrame = 4;

class EngineRecordTest : public MixxxTest {
  protected:
    void SetUp() override {
        // The controls EngineRecord reaches for; RecordingManager and the
        // engine own these in the running app.
        m_pRecStatus = std::make_unique<ControlObject>(
                ConfigKey(RECORDING_PREF_KEY, "status"));
        m_pSampleRate = std::make_unique<ControlObject>(
                ConfigKey(QStringLiteral("[App]"), QStringLiteral("samplerate")));
        m_pSampleRate->set(kSampleRate);

        m_recordingPath = getTestDataDir().filePath(QStringLiteral("enginerecord_test.wav"));
        QFile::remove(m_recordingPath);
        config()->set(ConfigKey(RECORDING_PREF_KEY, "Path"), ConfigValue(m_recordingPath));
        config()->set(ConfigKey(RECORDING_PREF_KEY, "Encoding"), ConfigValue(ENCODING_WAVE));
        config()->set(ConfigKey(RECORDING_PREF_KEY, "CueEnabled"), ConfigValue(0));

        m_pRecord = std::make_unique<EngineRecord>(config());
        m_buffer.assign(kFramesPerBuffer * mixxx::kEngineChannelCount, 0.25f);
    }

    void TearDown() override {
        m_pRecord.reset();
        QFile::remove(m_recordingPath);
    }

    // One turn of the sidechain's drain loop.
    void process() {
        m_pRecord->process(m_buffer.data(),
                static_cast<int>(m_buffer.size()));
    }

    std::unique_ptr<ControlObject> m_pRecStatus;
    std::unique_ptr<ControlObject> m_pSampleRate;
    std::unique_ptr<EngineRecord> m_pRecord;
    std::vector<CSAMPLE> m_buffer;
    QString m_recordingPath;
};

TEST_F(EngineRecordTest, writesACompleteFile) {
    QSignalSpy recordingSpy(m_pRecord.get(), &EngineRecord::isRecording);

    m_pRecStatus->set(RECORD_READY);
    for (int i = 0; i < kBuffers; ++i) {
        process();
    }
    ASSERT_EQ(RECORD_ON, m_pRecStatus->get()) << "recording never started";

    // The stop the DJ asks for: status goes off, the next turn of the loop
    // flushes the encoder and closes the file.
    m_pRecStatus->set(RECORD_OFF);
    process();

    ASSERT_GE(recordingSpy.count(), 2);
    // Started, then stopped, and neither carried the error flag.
    EXPECT_TRUE(recordingSpy.first().at(0).toBool());
    EXPECT_FALSE(recordingSpy.first().at(1).toBool());
    EXPECT_FALSE(recordingSpy.last().at(0).toBool());
    EXPECT_FALSE(recordingSpy.last().at(1).toBool());

    const QFileInfo recorded(m_recordingPath);
    ASSERT_TRUE(recorded.exists()) << "no file was written";
    // Every buffer that went in is in the file: nothing the limiter did to the
    // page cache cost the recording its tail.
    const qint64 expectedBytes = static_cast<qint64>(kWavHeaderBytes) +
            static_cast<qint64>(kBuffers) * kFramesPerBuffer * kBytesPerFrame;
    EXPECT_EQ(expectedBytes, recorded.size());

    QFile file(m_recordingPath);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray header = file.read(12);
    // A header that was patched on close, over a range the limiter had already
    // handed to the kernel, still reads back as a RIFF/WAVE file.
    EXPECT_EQ(QByteArray("RIFF"), header.left(4));
    EXPECT_EQ(QByteArray("WAVE"), header.mid(8, 4));
}

TEST_F(EngineRecordTest, splittingStartsANewFileAndKeepsTheOldOne) {
    m_pRecStatus->set(RECORD_READY);
    for (int i = 0; i < kBuffers; ++i) {
        process();
    }
    ASSERT_EQ(RECORD_ON, m_pRecStatus->get());

    // RecordingManager splits by pointing Path at the next part and asking for
    // a continue; the limiter has to start over with the new file.
    const QString partTwoPath =
            getTestDataDir().filePath(QStringLiteral("enginerecord_test_part2.wav"));
    QFile::remove(partTwoPath);
    config()->set(ConfigKey(RECORDING_PREF_KEY, "Path"), ConfigValue(partTwoPath));
    m_pRecStatus->set(RECORD_SPLIT_CONTINUE);
    process();
    ASSERT_EQ(RECORD_ON, m_pRecStatus->get()) << "split did not resume recording";

    for (int i = 0; i < kBuffers; ++i) {
        process();
    }
    m_pRecStatus->set(RECORD_OFF);
    process();

    const qint64 expectedFirst = static_cast<qint64>(kWavHeaderBytes) +
            static_cast<qint64>(kBuffers) * kFramesPerBuffer * kBytesPerFrame;
    // The part that was split away is closed and complete...
    EXPECT_EQ(expectedFirst, QFileInfo(m_recordingPath).size());
    // ...and the second part holds everything after the split (one buffer of
    // which was the process() call that performed it).
    const qint64 expectedSecond = static_cast<qint64>(kWavHeaderBytes) +
            static_cast<qint64>(kBuffers + 1) * kFramesPerBuffer * kBytesPerFrame;
    EXPECT_EQ(expectedSecond, QFileInfo(partTwoPath).size());
    QFile::remove(partTwoPath);
}

} // namespace
