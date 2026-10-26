#include "library/dao/fsanalysiscache.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QStorageInfo>
#include <QStringList>
#include <memory>
#include <thread>

#include "test/mixxxtest.h"
#include "track/keyfactory.h"

namespace {

// Same removable-drive stand-in as rekordboxanalysis_test.cpp; see there for
// how to mount it without root. The tests skip themselves without it.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

QStringList s_warnings;
QtMessageHandler s_previousHandler = nullptr;

void recordWarnings(QtMsgType type, const QMessageLogContext& context, const QString& msg) {
    if (type == QtWarningMsg) {
        s_warnings.append(msg);
    }
    s_previousHandler(type, context, msg);
}

/// Whether this process still holds the drive's cache database open.
bool cacheFileIsOpen() {
    const QString cacheDb = kFakeUsb + QStringLiteral("/.bitedj/analysis.sqlite");
    const QDir fds(QStringLiteral("/proc/self/fd"));
    for (const QString& fd : fds.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
        if (QFile::symLinkTarget(fds.filePath(fd)) == cacheDb) {
            return true;
        }
    }
    return false;
}

class FsAnalysisCacheTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        if (usbIsMounted()) {
            FsAnalysisCache::clearFilesystemCache(kFakeUsb);
        }
    }

    void TearDown() override {
        if (usbIsMounted()) {
            FsAnalysisCache::clearFilesystemCache(kFakeUsb);
        }
        MixxxTest::TearDown();
    }

    static bool usbIsMounted() {
        const QStorageInfo usb(kFakeUsb);
        return usb.isValid() && usb.isReady() && usb.rootPath() == kFakeUsb;
    }
};

// An eject closes the cache connections from the GUI thread, but they were
// opened wherever the cache is used: an analyzer worker, for one. Closing one
// must neither trip Qt's thread check on connection lookup nor leave the file
// open, which would keep the drive busy and fail the unmount.
TEST_F(FsAnalysisCacheTest, EjectClosesConnectionOpenedOnAnotherThread) {
    if (!usbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    // The cache finds its drive from the track file, so the file must exist.
    const QString trackPath = kFakeUsb + QStringLiteral("/track.wav");
    QFile::remove(trackPath);
    ASSERT_TRUE(QFile::copy(getTestDir().filePath(QStringLiteral("sine-30.wav")), trackPath));

    std::unique_ptr<FsAnalysisCache> pCache;
    std::thread worker([&] {
        pCache = std::make_unique<FsAnalysisCache>(config());
        EXPECT_TRUE(pCache->saveTrackKeys(trackPath,
                KeyFactory::makeBasicKeys(
                        mixxx::track::io::key::A_MINOR, mixxx::track::io::key::ANALYZER)));
    });
    worker.join();
    ASSERT_TRUE(cacheFileIsOpen());

    s_warnings.clear();
    s_previousHandler = qInstallMessageHandler(recordWarnings);
    FsAnalysisCache::closeFilesystemConnections(kFakeUsb);
    qInstallMessageHandler(s_previousHandler);

    EXPECT_TRUE(s_warnings.isEmpty()) << qPrintable(s_warnings.join('\n'));
    EXPECT_FALSE(cacheFileIsOpen());

    // The next access reopens the drive's cache and finds what was saved.
    const auto keys = pCache->getTrackKeys(trackPath);
    ASSERT_TRUE(keys.has_value());
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, keys->getGlobalKey());
}

} // namespace
