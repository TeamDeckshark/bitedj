#include "library/dao/fshistoryworker.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStorageInfo>
#include <QStringList>
#include <QThread>
#include <QVariant>

#include "library/dao/fshistorystore.h"
#include "test/mixxxtest.h"

namespace {

// Like FsHistoryStoreTest, these need a filesystem mounted under one of the
// removable roots, which `unshare` provides without root:
//
//   unshare -Umr --propagation private sh -c \
//     'mount -t tmpfs tmpfs /mnt && mkdir -p /mnt/usbtest && \
//      mount -t tmpfs tmpfs /mnt/usbtest && \
//      QT_QPA_PLATFORM=offscreen ./mixxx-test \
//        --gtest_filter="FsHistoryWorkerTest.*"'
//
// The cases skip when that mount is not there, so an ordinary run stays green.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

constexpr int kSignalTimeoutMillis = 10000;

class FsHistoryWorkerTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        if (haveFakeUsb()) {
            ASSERT_TRUE(FsHistoryStore::clearFilesystemHistory(kFakeUsb));
        }
    }

    static bool haveFakeUsb() {
        const QStorageInfo usb(kFakeUsb);
        return usb.isValid() && usb.isReady() && usb.rootPath() == kFakeUsb;
    }

    static QString onUsb(const QString& relPath) {
        return kFakeUsb + QLatin1Char('/') + relPath;
    }
};

} // anonymous namespace

// The point of the class: the SQLite work happens somewhere other than the
// thread that played the track. Proven rather than timed — the slot is
// connected directly, so it runs wherever the append ran.
TEST_F(FsHistoryWorkerTest, WritesOnItsOwnThread) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QThread* pWriteThread = nullptr;
    QObject::connect(
            &worker,
            &FsHistoryWorker::trackLogged,
            &worker,
            [&pWriteThread](const QString&, const FsHistorySession&, TrackId) {
                pWriteThread = QThread::currentThread();
            },
            Qt::DirectConnection);

    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("House/first.mp3")), 300, TrackId());
    FsHistoryWorker::flushFilesystem(kFakeUsb);

    ASSERT_NE(nullptr, pWriteThread);
    EXPECT_NE(QThread::currentThread(), pWriteThread);
}

// A flush is what the eject and the delete paths rely on: once it returns, what
// was queued is on the drive, without the caller having run an event loop.
TEST_F(FsHistoryWorkerTest, FlushLandsWhatWasQueued) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("House/first.mp3")), 300, TrackId());
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("House/second.mp3")), 240, TrackId());
    FsHistoryWorker::flushFilesystem(kFakeUsb);

    QList<FsHistorySession> sessions;
    ASSERT_TRUE(FsHistoryStore::readSessions(kFakeUsb, &sessions));
    ASSERT_EQ(1, sessions.size());
    EXPECT_EQ(2, sessions.first().trackCount);
    EXPECT_EQ(540, sessions.first().durationSeconds);

    QStringList locations;
    ASSERT_TRUE(FsHistoryStore::readSessionTracks(
            kFakeUsb, sessions.first().name, &locations));
    EXPECT_EQ(QStringList({onUsb(QStringLiteral("House/first.mp3")),
                      onUsb(QStringLiteral("House/second.mp3"))}),
            locations);
}

// The answer carries everything the sidebar needs — the session's running
// totals and the track it was asked about — so the GUI thread never reads the
// drive itself.
TEST_F(FsHistoryWorkerTest, ReportsTheSessionTotalsBack) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QSignalSpy logged(&worker, &FsHistoryWorker::trackLogged);

    const TrackId firstId(QVariant(1));
    const TrackId secondId(QVariant(2));
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("first.mp3")), 300, firstId);
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("second.mp3")), 240, secondId);

    while (logged.count() < 2) {
        ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    }
    ASSERT_EQ(2, logged.count());

    const auto first = logged.at(0);
    EXPECT_EQ(kFakeUsb, first.at(0).toString());
    EXPECT_EQ(1, first.at(1).value<FsHistorySession>().trackCount);
    EXPECT_EQ(300, first.at(1).value<FsHistorySession>().durationSeconds);
    EXPECT_EQ(firstId, first.at(2).value<TrackId>());

    const auto second = logged.at(1);
    // Both tracks belong to the set that started with the first one.
    EXPECT_EQ(first.at(1).value<FsHistorySession>().name,
            second.at(1).value<FsHistorySession>().name);
    EXPECT_EQ(2, second.at(1).value<FsHistorySession>().trackCount);
    EXPECT_EQ(540, second.at(1).value<FsHistorySession>().durationSeconds);
    EXPECT_EQ(secondId, second.at(2).value<TrackId>());
}

// Closing a session is ordered against the appends around it: a track handed
// over before the request still belongs to the session being closed, and only
// what comes after starts the next one.
TEST_F(FsHistoryWorkerTest, ForgettingASessionWaitsForWhatWasAlreadyQueued) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QSignalSpy logged(&worker, &FsHistoryWorker::trackLogged);
    QSignalSpy closed(&worker, &FsHistoryWorker::sessionClosed);

    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("first.mp3")), 300, TrackId());
    worker.forgetSession(kFakeUsb);
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("second.mp3")), 240, TrackId());

    while (logged.count() < 2) {
        ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    }
    ASSERT_EQ(1, closed.count());
    EXPECT_EQ(kFakeUsb, closed.at(0).at(0).toString());

    const QString firstSession = logged.at(0).at(1).value<FsHistorySession>().name;
    const QString secondSession = logged.at(1).at(1).value<FsHistorySession>().name;
    EXPECT_NE(firstSession, secondSession);
    // The second set of the day, named as such on the drive.
    EXPECT_EQ(firstSession + QStringLiteral(" #2"), secondSession);
    EXPECT_EQ(1, logged.at(0).at(1).value<FsHistorySession>().trackCount);
    EXPECT_EQ(1, logged.at(1).at(1).value<FsHistorySession>().trackCount);
}

// The sidebar's view of a drive: what it holds, read off it and handed back
// without the caller ever touching the filesystem.
TEST_F(FsHistoryWorkerTest, ReadsBackWhatADriveHolds) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QSignalSpy read(&worker, &FsHistoryWorker::sessionsRead);

    // Also received the way SetlogFeature receives it — an ordinary connection
    // to this thread, which is a queued one across the worker's thread
    // boundary, and silently drops the call if the session list is not a type
    // Qt can put in a queue.
    QList<FsHistorySession> delivered;
    int deliveries = 0;
    QObject receiver;
    QObject::connect(&worker,
            &FsHistoryWorker::sessionsRead,
            &receiver,
            [&delivered, &deliveries](
                    const QString&, const QList<FsHistorySession>& sessions) {
                delivered = sessions;
                ++deliveries;
            });

    // A drive that was never played from answers with an empty list rather
    // than not answering: "nothing here" is a result the sidebar can draw.
    worker.readSessions(kFakeUsb);
    ASSERT_TRUE(read.wait(kSignalTimeoutMillis));
    EXPECT_TRUE(read.takeFirst().at(1).value<QList<FsHistorySession>>().isEmpty());

    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("first.mp3")), 300, TrackId());
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("second.mp3")), 240, TrackId());
    worker.readSessions(kFakeUsb);
    ASSERT_TRUE(read.wait(kSignalTimeoutMillis));

    const auto sessions = read.takeFirst().at(1).value<QList<FsHistorySession>>();
    ASSERT_EQ(1, sessions.size());
    EXPECT_EQ(2, sessions.first().trackCount);
    EXPECT_EQ(540, sessions.first().durationSeconds);

    EXPECT_EQ(2, deliveries);
    EXPECT_EQ(sessions, delivered);
}

// Opening a session in the track view: the paths come back resolved against
// the drive, in play order.
TEST_F(FsHistoryWorkerTest, ReadsBackTheTracksOfASession) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QSignalSpy logged(&worker, &FsHistoryWorker::trackLogged);
    QSignalSpy read(&worker, &FsHistoryWorker::sessionTracksRead);

    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("House/first.mp3")), 300, TrackId());
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("House/second.mp3")), 240, TrackId());
    while (logged.count() < 2) {
        ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    }
    const QString session = logged.at(0).at(1).value<FsHistorySession>().name;

    worker.readSessionTracks(kFakeUsb, session);
    ASSERT_TRUE(read.wait(kSignalTimeoutMillis));
    const auto answer = read.takeFirst();
    EXPECT_EQ(kFakeUsb, answer.at(0).toString());
    EXPECT_EQ(session, answer.at(1).toString());
    EXPECT_EQ(QStringList({onUsb(QStringLiteral("House/first.mp3")),
                      onUsb(QStringLiteral("House/second.mp3"))}),
            answer.at(2).toStringList());

    // A session that is not on the drive is an empty answer, not a silence:
    // the view it was asked for still has to be emptied.
    worker.readSessionTracks(kFakeUsb, QStringLiteral("never happened"));
    ASSERT_TRUE(read.wait(kSignalTimeoutMillis));
    EXPECT_TRUE(read.takeFirst().at(2).toStringList().isEmpty());
}

// Deleting the set being recorded ends it as well as removing it, and says so
// before it reports the list the sidebar is to draw.
TEST_F(FsHistoryWorkerTest, DeletingTheOpenSessionClosesIt) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QSignalSpy logged(&worker, &FsHistoryWorker::trackLogged);
    QSignalSpy closed(&worker, &FsHistoryWorker::sessionClosed);
    QSignalSpy read(&worker, &FsHistoryWorker::sessionsRead);

    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("first.mp3")), 300, TrackId());
    ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    const QString session = logged.at(0).at(1).value<FsHistorySession>().name;

    worker.deleteSession(kFakeUsb, session);
    ASSERT_TRUE(read.wait(kSignalTimeoutMillis));
    EXPECT_EQ(1, closed.count());
    EXPECT_TRUE(read.takeFirst().at(1).value<QList<FsHistorySession>>().isEmpty());

    // The set really is over: the next track opens a new one rather than
    // carrying on appending to the session that was just deleted. (It is free
    // to take the same name — nothing on the drive is called that any more —
    // so what says it is a new set is that it starts from one track.)
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("second.mp3")), 240, TrackId());
    while (logged.count() < 2) {
        ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    }
    const auto reopened = logged.at(1).at(1).value<FsHistorySession>();
    EXPECT_EQ(1, reopened.trackCount);
    EXPECT_EQ(240, reopened.durationSeconds);

    QStringList locations;
    ASSERT_TRUE(FsHistoryStore::readSessionTracks(kFakeUsb, reopened.name, &locations));
    EXPECT_EQ(QStringList({onUsb(QStringLiteral("second.mp3"))}), locations);
}

// Deleting some older set leaves the one being recorded alone.
TEST_F(FsHistoryWorkerTest, DeletingAnotherSessionLeavesTheOpenOneOpen) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QSignalSpy logged(&worker, &FsHistoryWorker::trackLogged);
    QSignalSpy closed(&worker, &FsHistoryWorker::sessionClosed);
    QSignalSpy read(&worker, &FsHistoryWorker::sessionsRead);

    // An earlier set on the drive, then the one being recorded now.
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("old.mp3")), 300, TrackId());
    worker.forgetSession(kFakeUsb);
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("new.mp3")), 240, TrackId());
    while (logged.count() < 2) {
        ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    }
    const QString oldSession = logged.at(0).at(1).value<FsHistorySession>().name;
    const QString openSession = logged.at(1).at(1).value<FsHistorySession>().name;
    closed.clear();

    worker.deleteSession(kFakeUsb, oldSession);
    ASSERT_TRUE(read.wait(kSignalTimeoutMillis));
    EXPECT_EQ(0, closed.count());
    const auto sessions = read.takeFirst().at(1).value<QList<FsHistorySession>>();
    ASSERT_EQ(1, sessions.size());
    EXPECT_EQ(openSession, sessions.first().name);

    // Still recording into it.
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("another.mp3")), 60, TrackId());
    while (logged.count() < 3) {
        ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    }
    EXPECT_EQ(openSession, logged.at(2).at(1).value<FsHistorySession>().name);
    EXPECT_EQ(2, logged.at(2).at(1).value<FsHistorySession>().trackCount);
}

// Clearing a drive takes the whole store with it, open session included.
TEST_F(FsHistoryWorkerTest, ClearingADriveEmptiesItAndEndsTheSet) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    FsHistoryWorker worker;
    QSignalSpy logged(&worker, &FsHistoryWorker::trackLogged);
    QSignalSpy closed(&worker, &FsHistoryWorker::sessionClosed);
    QSignalSpy read(&worker, &FsHistoryWorker::sessionsRead);

    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("first.mp3")), 300, TrackId());
    worker.forgetSession(kFakeUsb);
    worker.logTrack(kFakeUsb, onUsb(QStringLiteral("second.mp3")), 240, TrackId());
    while (logged.count() < 2) {
        ASSERT_TRUE(logged.wait(kSignalTimeoutMillis));
    }
    closed.clear();

    worker.clearHistory(kFakeUsb);
    ASSERT_TRUE(read.wait(kSignalTimeoutMillis));
    EXPECT_EQ(1, closed.count());
    EXPECT_TRUE(read.takeFirst().at(1).value<QList<FsHistorySession>>().isEmpty());

    QList<FsHistorySession> sessions;
    EXPECT_FALSE(FsHistoryStore::readSessions(kFakeUsb, &sessions));
}

// A drive that is not there is not an error the DJ hears about on every track
// change: the request is dropped and nothing is reported back.
TEST_F(FsHistoryWorkerTest, SaysNothingAboutADriveThatIsNotThere) {
    FsHistoryWorker worker;
    QSignalSpy logged(&worker, &FsHistoryWorker::trackLogged);

    const QString absent = QStringLiteral("/mnt/usb-that-was-pulled");
    worker.logTrack(absent, absent + QStringLiteral("/first.mp3"), 300, TrackId());
    FsHistoryWorker::flushFilesystem(absent);

    EXPECT_EQ(0, logged.count());
}
