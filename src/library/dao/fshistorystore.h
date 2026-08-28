#pragma once

#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

/// One session a DJ played off a drive: a name unique on that drive, the time
/// the first track went out, and the totals the sidebar labels it with.
struct FsHistorySession {
    QString name;
    QDateTime startedAt;
    int trackCount = 0;
    int durationSeconds = 0;

    /// So a re-read of a drive can be recognised as having found nothing new.
    friend bool operator==(const FsHistorySession& lhs, const FsHistorySession& rhs) {
        return lhs.name == rhs.name && lhs.startedAt == rhs.startedAt &&
                lhs.trackCount == rhs.trackCount &&
                lhs.durationSeconds == rhs.durationSeconds;
    }
    friend bool operator!=(const FsHistorySession& lhs, const FsHistorySession& rhs) {
        return !(lhs == rhs);
    }
};

// Handed from the writer thread back to the GUI thread as a queued signal
// argument; see FsHistoryWorker::trackLogged().
Q_DECLARE_METATYPE(FsHistorySession)

/// Portable, per-filesystem store for the history of what was played off a
/// drive.
///
/// The history a DJ cares about is the history of a stick, not of a box: the
/// same crate of tracks gets played on whatever unit is at the venue, and the
/// set that was played off it last night should come back with it. So sessions
/// live in a self-contained SQLite database at
/// `<mountRoot>/.bitedj/history.sqlite`, next to the analysis cache, the cue
/// overrides and the sampler banks, keyed by each track's path relative to the
/// mount root — the same key FsMetaOverrideStore uses, and for the same reason:
/// the automounter names mount points after the volume label, so nothing
/// absolute survives a re-plug.
///
/// Tracks played from somewhere other than a removable drive have no stick to
/// be written to, and are not recorded at all: this unit keeps no history of
/// its own (see SetlogFeature).
///
/// Like the other .bitedj stores this one does not keep its connections open;
/// see ScopedFsStore for why a lingering file descriptor would break eject.
class FsHistoryStore {
  public:
    /// Read every session stored on the drive mounted at `mountRoot`, newest
    /// first. Returns false when the drive is unavailable or holds no history,
    /// leaving `pSessions` untouched.
    static bool readSessions(const QString& mountRoot, QList<FsHistorySession>* pSessions);

    /// Read the tracks of one session in play order into `pLocations`, as
    /// absolute paths under `mountRoot`. Returns false when the drive is
    /// unavailable or has no such session.
    static bool readSessionTracks(const QString& mountRoot,
            const QString& sessionName,
            QStringList* pLocations);

    /// The totals of one session, for relabelling a single sidebar item without
    /// re-reading the whole store. Returns false when there is no such session.
    static bool readSessionSummary(const QString& mountRoot,
            const QString& sessionName,
            FsHistorySession* pSession);

    /// A session name for a set starting now on the drive mounted at
    /// `mountRoot`: today's date, suffixed "#2", "#3", ... when the drive
    /// already carries a session from today. Nothing is written — a session
    /// exists once its first track is appended — so an unused name costs the
    /// drive nothing.
    ///
    /// Empty when the drive is unavailable or not writable, which is the
    /// caller's signal that this drive cannot record history.
    static QString newSessionName(const QString& mountRoot);

    /// Append `trackLocation` (an absolute path on that drive) to `sessionName`,
    /// creating the session on first use. `durationSeconds` is stored alongside
    /// so a session can be labelled with its running time without resolving
    /// every track against a library.
    static bool appendTrack(const QString& mountRoot,
            const QString& sessionName,
            const QString& trackLocation,
            int durationSeconds);

    /// Delete one session from the drive.
    static bool deleteSession(const QString& mountRoot, const QString& sessionName);

    /// Delete the whole history database of the filesystem mounted at
    /// `mountPoint`. Returns false only when one exists but could not be
    /// deleted; a drive without history counts as success.
    static bool clearFilesystemHistory(const QString& mountPoint);
};
