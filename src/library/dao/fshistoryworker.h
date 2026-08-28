#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QWaitCondition>
#include <functional>

#include "library/dao/fshistorystore.h"
#include "track/trackid.h"

class QThread;

/// FsHistoryStore on a thread of its own: every access to a drive's history
/// goes through here, and none of them runs on the GUI thread.
///
/// A history append is three SQLite statements of a few hundred bytes each, but
/// they go to a USB stick: opening the store, growing the file and fsyncing the
/// journal have been measured at over three seconds on a real drive, and the
/// call used to be made straight from PlayerInfo::currentPlayingTrackChanged on
/// the GUI thread — so starting a track froze the UI, jog wheel included, for
/// as long as the stick took to accept the row. The reads are the same story on
/// a slower scale: the sidebar lists every session on every mounted drive, and
/// it rebuilt itself (synchronously, off the sticks) whenever one was plugged
/// in, ejected or written to. None of this is work the event loop should be
/// waiting for — a history records what already happened.
///
/// So requests are queued to a background thread and answered by signals that
/// arrive back on the caller's thread. Nothing here returns a result directly,
/// including the deletes: the queue is what orders an operation against the
/// appends around it, so a session cannot be deleted out from under a track
/// that is still being written, or recreated by one. The caller keeps its own
/// copy of what is on each drive and follows the answers; where it needs to
/// know that the drive itself is quiet — the eject — it calls
/// flushFilesystem().
///
/// Which session a track lands in is decided here too, rather than by the
/// caller: naming a new session reads the store (see
/// FsHistoryStore::newSessionName), so the map of "the set currently being
/// recorded on each drive" has to live on the thread doing the reading.
class FsHistoryWorker : public QObject {
    Q_OBJECT

  public:
    explicit FsHistoryWorker(QObject* parent = nullptr);
    ~FsHistoryWorker() override;

    /// Queue `trackLocation` (an absolute path on the drive mounted at
    /// `mountRoot`) to be appended to that drive's history, opening a session
    /// on it if this is the first track since it was plugged in. Returns
    /// immediately; trackLogged() reports the row once it is on the drive.
    /// `trackId` is not stored — it is handed back untouched so the caller can
    /// match the answer to the track it played.
    void logTrack(const QString& mountRoot,
            const QString& trackLocation,
            int durationSeconds,
            TrackId trackId);

    /// Close the session open on `mountRoot`, so the next track played off that
    /// drive starts a new one. Queued behind any pending append, which is what
    /// makes it safe to call right after deleting the session those appends
    /// were going to, and answered by sessionClosed().
    void forgetSession(const QString& mountRoot);

    /// Delete one session from `mountRoot`, then re-read the drive: the answer
    /// is a sessionsRead() carrying what is left, so a caller that keeps a list
    /// needs no separate completion to act on. Deleting the set currently being
    /// recorded also closes it, reported by sessionClosed() first.
    void deleteSession(const QString& mountRoot, const QString& sessionName);

    /// Delete every session stored on `mountRoot`, answered like
    /// deleteSession().
    void clearHistory(const QString& mountRoot);

    /// Read the sessions on `mountRoot`, newest first, into a sessionsRead().
    void readSessions(const QString& mountRoot);

    /// Read the tracks of one session in play order, into a
    /// sessionTracksRead().
    void readSessionTracks(const QString& mountRoot, const QString& sessionName);

    /// Block until everything queued for the filesystem mounted at `mountPoint`
    /// has been carried out. The one caller that needs this is the eject, which
    /// must not pull a drive out from under a row that is still being written;
    /// everything else gets its ordering from the queue and never has to wait.
    /// There is one queue for all drives, so this waits out whatever else is in
    /// it too — `mountPoint` says what is being waited for, not how much.
    ///
    /// Gives up after a few seconds rather than holding the GUI thread on a
    /// drive that has stopped answering: a stuck write is not a reason to brick
    /// the screen, and a write that lands after its mount is gone is refused by
    /// FsStoreTarget::resolveForMount rather than written somewhere wrong.
    ///
    /// Safe to call from any thread, and a no-op when no worker exists.
    static void flushFilesystem(const QString& mountPoint);

  signals:
    /// A track reached the drive: `session` carries the totals the sidebar
    /// labels the session with, so the receiver needs no read of its own.
    /// Emitted on the worker thread, i.e. delivered to the receiver's thread as
    /// a queued call.
    void trackLogged(const QString& mountRoot,
            const FsHistorySession& session,
            TrackId trackId);

    /// A forgetSession() request has been carried out. It comes back rather
    /// than being assumed because the queue is what orders it against the
    /// appends around it: a track logged just before the request still belongs
    /// to the session being closed, and its trackLogged() is delivered first.
    void sessionClosed(const QString& mountRoot);

    /// What `mountRoot` now holds, newest session first — the answer to
    /// readSessions() and to either delete. Empty for a drive that carries no
    /// history, or none any more, which is a result and not a failure: the
    /// receiver should take it as the drive's list either way.
    void sessionsRead(const QString& mountRoot, const QList<FsHistorySession>& sessions);

    /// The tracks of one session in play order, as absolute paths under
    /// `mountRoot`. Empty when the session is gone (or the drive is), which the
    /// receiver shows as an empty view.
    void sessionTracksRead(const QString& mountRoot,
            const QString& sessionName,
            const QStringList& locations);

  private:
    /// Post `task` to the worker thread and count it as pending until it has
    /// run, which is what flush() waits on.
    void enqueue(std::function<void()> task);
    /// Worker thread: read the drive and answer with sessionsRead().
    void emitSessions(const QString& mountRoot);
    /// Worker thread: end the set being recorded on `mountRoot` and say so,
    /// if it is `sessionName` (or, with an empty `sessionName`, whichever it
    /// is). Unlike forgetSession() this is silent when there is nothing open.
    void closeSession(const QString& mountRoot, const QString& sessionName);
    void flush(const QString& mountPoint);

    QThread* m_pThread;
    /// Lives on m_pThread; the object queued tasks are posted to.
    QObject* m_pWorkerContext;

    /// Mount root -> the session being recorded on that drive. Worker thread
    /// only.
    QHash<QString, QString> m_sessionByMount;

    QMutex m_pendingMutex;
    QWaitCondition m_pendingDone;
    int m_pendingTasks;

    /// Every live worker, so the eject path can reach one without being handed
    /// a pointer through the library. Mirrors FsAnalysisCache's registry, and
    /// for the same reason.
    static QMutex s_registryMutex;
    static QSet<FsHistoryWorker*> s_instances;
};
