#include "library/dao/fshistoryworker.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <QtDebug>
#include <utility>

#include "moc_fshistoryworker.cpp"
#include "util/rtscheduling.h"

namespace {

const char kLogTag[] = "FsHistoryWorker";

/// How long a flush waits for the drive. Generous next to the ~3s a healthy
/// stick takes for one append, short enough that a drive that has stopped
/// answering does not hold the eject (and with it the whole GUI) forever.
constexpr int kFlushTimeoutMillis = 10000;

} // anonymous namespace

// static
QMutex FsHistoryWorker::s_registryMutex;
// static
QSet<FsHistoryWorker*> FsHistoryWorker::s_instances;

FsHistoryWorker::FsHistoryWorker(QObject* parent)
        : QObject(parent),
          m_pThread(new QThread(this)),
          m_pWorkerContext(new QObject()),
          m_pendingTasks(0) {
    m_pThread->setObjectName(QStringLiteral("FsHistoryWorker"));
    m_pWorkerContext->moveToThread(m_pThread);
    connect(m_pThread, &QThread::finished, m_pWorkerContext, &QObject::deleteLater);
    connect(m_pThread, &QThread::started, m_pWorkerContext, []() {
        // Writing history is the definition of work that can wait: it records
        // a track that is already playing, and it spends its time blocked on a
        // USB stick. The demotion is done from inside the thread rather than
        // left to the priority passed to start() below, for the same reasons
        // the analyzer threads do it: this thread is created by the SCHED_FIFO
        // GUI thread (see the caution in main()), so it states its policy
        // outright instead of relying on what Qt derives from its creator's,
        // and a QThread priority cannot express the other half of the demotion
        // — the I/O priority, which is what keeps this append from queueing in
        // front of a deck reading the track it is about to play.
        mixxx::demoteCurrentThreadToBackground(kLogTag);
    });
    m_pThread->start(QThread::LowPriority);

    QMutexLocker locker(&s_registryMutex);
    s_instances.insert(this);
}

FsHistoryWorker::~FsHistoryWorker() {
    {
        QMutexLocker locker(&s_registryMutex);
        s_instances.remove(this);
    }
    // Shutting down is not a reason to lose the last track of the set, so the
    // queue is drained first — quit() stops the event loop at its next turn and
    // anything still posted then is dropped, which only happens on a drive that
    // has stopped answering. Dropping a task is harmless (one that never runs
    // never touches this object); a task that is *running* is not, which is why
    // the wait() below is unbounded where the flush is not. It is what
    // guarantees no write is still inside a member of an object that has gone.
    flush(QString());
    m_pThread->quit();
    m_pThread->wait();
}

void FsHistoryWorker::enqueue(std::function<void()> task) {
    {
        QMutexLocker locker(&m_pendingMutex);
        ++m_pendingTasks;
    }
    QMetaObject::invokeMethod(
            m_pWorkerContext,
            [this, task = std::move(task)]() {
                task();
                {
                    QMutexLocker locker(&m_pendingMutex);
                    --m_pendingTasks;
                }
                m_pendingDone.wakeAll();
            },
            Qt::QueuedConnection);
}

void FsHistoryWorker::logTrack(const QString& mountRoot,
        const QString& trackLocation,
        int durationSeconds,
        TrackId trackId) {
    enqueue([this, mountRoot, trackLocation, durationSeconds, trackId]() {
        QString sessionName = m_sessionByMount.value(mountRoot);
        if (sessionName.isEmpty()) {
            // First track off this drive since it was plugged in: that is where
            // one set ends and the next begins.
            sessionName = FsHistoryStore::newSessionName(mountRoot);
            if (sessionName.isEmpty()) {
                // Unavailable or write-protected. Nothing to log to, and
                // nothing worth saying about it on every track change.
                return;
            }
            m_sessionByMount.insert(mountRoot, sessionName);
        }
        if (!FsHistoryStore::appendTrack(
                    mountRoot, sessionName, trackLocation, durationSeconds)) {
            return;
        }
        // Read back here rather than making the receiver do it: it is the same
        // store on the same slow drive, and it is already open in this thread's
        // page cache.
        FsHistorySession session;
        if (!FsHistoryStore::readSessionSummary(mountRoot, sessionName, &session)) {
            return;
        }
        emit trackLogged(mountRoot, session, trackId);
    });
}

void FsHistoryWorker::forgetSession(const QString& mountRoot) {
    enqueue([this, mountRoot]() {
        m_sessionByMount.remove(mountRoot);
        emit sessionClosed(mountRoot);
    });
}

void FsHistoryWorker::deleteSession(
        const QString& mountRoot, const QString& sessionName) {
    enqueue([this, mountRoot, sessionName]() {
        FsHistoryStore::deleteSession(mountRoot, sessionName);
        // Deleting the set that is being recorded ends it: the next track off
        // the drive opens a new one rather than refilling what was just
        // removed. Reported before the list below, so the caller drops the
        // "current session" marker before it redraws the sessions.
        closeSession(mountRoot, sessionName);
        // Answered with the drive's remaining sessions whether or not the
        // delete took: either way that list is the truth the caller wanted, and
        // a failure it cannot act on is not worth a signal of its own.
        emitSessions(mountRoot);
    });
}

void FsHistoryWorker::clearHistory(const QString& mountRoot) {
    enqueue([this, mountRoot]() {
        FsHistoryStore::clearFilesystemHistory(mountRoot);
        closeSession(mountRoot, QString());
        emitSessions(mountRoot);
    });
}

void FsHistoryWorker::closeSession(
        const QString& mountRoot, const QString& sessionName) {
    const auto it = m_sessionByMount.constFind(mountRoot);
    if (it == m_sessionByMount.constEnd()) {
        return;
    }
    if (!sessionName.isEmpty() && it.value() != sessionName) {
        // Some older set on the drive; the one being recorded carries on.
        return;
    }
    m_sessionByMount.erase(it);
    emit sessionClosed(mountRoot);
}

void FsHistoryWorker::readSessions(const QString& mountRoot) {
    enqueue([this, mountRoot]() {
        emitSessions(mountRoot);
    });
}

void FsHistoryWorker::readSessionTracks(
        const QString& mountRoot, const QString& sessionName) {
    enqueue([this, mountRoot, sessionName]() {
        QStringList locations;
        // A session that is gone, on a drive that is gone, and one that is
        // simply empty are the same answer to the caller: nothing to show.
        FsHistoryStore::readSessionTracks(mountRoot, sessionName, &locations);
        emit sessionTracksRead(mountRoot, sessionName, locations);
    });
}

void FsHistoryWorker::emitSessions(const QString& mountRoot) {
    QList<FsHistorySession> sessions;
    FsHistoryStore::readSessions(mountRoot, &sessions);
    emit sessionsRead(mountRoot, sessions);
}

void FsHistoryWorker::flush(const QString& mountPoint) {
    QMutexLocker locker(&m_pendingMutex);
    if (m_pendingTasks == 0) {
        return;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (m_pendingTasks > 0) {
        const qint64 remaining = kFlushTimeoutMillis - elapsed.elapsed();
        if (remaining <= 0 ||
                !m_pendingDone.wait(
                        &m_pendingMutex, static_cast<unsigned long>(remaining))) {
            qWarning() << kLogTag << ": history is still being written"
                       << (mountPoint.isEmpty() ? QStringLiteral("(shutdown)")
                                                : QStringLiteral("to ") + mountPoint)
                       << "after" << kFlushTimeoutMillis << "ms; carrying on without it";
            return;
        }
    }
}

// static
void FsHistoryWorker::flushFilesystem(const QString& mountPoint) {
    // Held across the flush so a worker cannot be destroyed under us; its
    // destructor takes the same mutex before tearing the thread down, and the
    // worker thread never takes it at all, so this cannot deadlock.
    QMutexLocker locker(&s_registryMutex);
    for (FsHistoryWorker* pWorker : std::as_const(s_instances)) {
        pWorker->flush(mountPoint);
    }
}
