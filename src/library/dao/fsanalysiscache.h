#pragma once

#include <QHash>
#include <QMutex>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

#include "library/dao/analysisdao.h"
#include "preferences/usersettings.h"
#include "waveform/waveform.h"

class QSqlError;

/// Portable, per-filesystem waveform analysis cache.
///
/// Unlike AnalysisDao, which stores waveform blobs in the home settings dir keyed
/// by the global autoincrement analysisId of the home mixxxdb.sqlite, this cache
/// stores everything in a self-contained SQLite database that lives on the same
/// filesystem as the track (e.g. the USB stick): `<mountRoot>/.bitedj/analysis.sqlite`.
/// Entries are keyed by the track's path relative to the mount root, so the cache
/// is reusable on any Bite DJ unit without re-analysis.
///
/// Enabled via the `[Library]/AnalysisCacheOnTrackFs` config key (default true).
/// When the track's filesystem is read-only or otherwise not writable, saving is
/// skipped (logged), matching the "always target the track's own FS" behaviour.
///
/// Each instance opens its own QSqlDatabase connections and is therefore bound to
/// the thread that created it, mirroring AnalysisDao's per-thread usage.
class FsAnalysisCache {
  public:
    explicit FsAnalysisCache(UserSettingsPointer pConfig);
    ~FsAnalysisCache();

    /// Whether the per-filesystem cache mode is enabled in the config.
    /// Controlled by `[Library]/AnalysisCacheOnTrackFs` and defaults to true
    /// (this fork plays from USB and keeps analysis on the stick by default).
    bool isEnabled() const;

    /// Whether the legacy home-directory analysis cache (AnalysisDao waveform
    /// blobs in `~/.bitedj`) is enabled. Controlled by `[Library]/AnalysisCacheInHome`
    /// and defaults to false. When the per-filesystem cache is enabled it replaces
    /// the home cache regardless of this setting; this toggle only matters when the
    /// FS cache is off, where leaving it false disables waveform caching entirely so
    /// nothing is read from or written to the home settings dir.
    bool isHomeCacheEnabled() const;

    /// Load the cached waveform analyses for the track at `trackLocation`.
    /// Returns an empty list on a cache miss or if the filesystem is unavailable.
    QList<AnalysisDao::AnalysisInfo> getAnalysesForTrack(const QString& trackLocation);

    /// Persist the waveform and waveform summary for the track at `trackLocation`
    /// to its filesystem's cache. Returns false (and logs) if the filesystem is
    /// not writable; pending blobs are saved on success.
    bool saveTrackAnalyses(
            const QString& trackLocation,
            ConstWaveformPointer pWaveform,
            ConstWaveformPointer pWaveSummary);

    /// Close every open cache connection (across all FsAnalysisCache instances on
    /// any thread) that lives on the filesystem mounted at `mountPoint`, freeing
    /// the SQLite file descriptors that would otherwise keep the device busy and
    /// block `umount` on eject. `mountPoint` is matched against each connection's
    /// filesystem mount root (cleaned). Safe to call from any thread: each
    /// instance serializes the close against its own in-flight cache operations.
    /// A subsequent cache access transparently reopens a fresh connection.
    static void closeFilesystemConnections(const QString& mountPoint);

    /// Delete the on-disk analysis cache database of the filesystem mounted at
    /// `mountPoint` (`<mountPoint>/.bitedj/analysis.sqlite`), first closing every
    /// open connection to it via closeFilesystemConnections(). Returns false only
    /// when a cache file exists but could not be deleted; a drive without a cache
    /// counts as success. A later cache access transparently recreates the file.
    static bool clearFilesystemCache(const QString& mountPoint);

  private:
    // Open (lazily creating) connection + cache dir for the filesystem containing
    // `trackLocation`. `relPath` is set to the track path relative to the mount root.
    // `pWritable`, if given, reports whether the filesystem accepts writes.
    // Returns an invalid QSqlDatabase if the filesystem is unavailable.
    QSqlDatabase databaseForTrack(
            const QString& trackLocation, QString* pRelPath, bool* pWritable = nullptr);

    // Open `db` and, on a writable filesystem, ensure the cache schema exists.
    // Returns false and reports the failure via `pError` (used to distinguish a
    // corrupt/undeserializable file from other open errors).
    bool openConnection(QSqlDatabase& db, bool writable, QSqlError* pError);

    // Close and forget this instance's connection for the filesystem mounted at
    // `mountPoint`, if any. Takes m_handlesMutex so it cannot run while one of
    // this instance's own operations is using the connection on its home thread.
    void releaseFilesystem(const QString& mountPoint);

    bool saveAnalysis(
            QSqlDatabase& db,
            const QString& relPath,
            AnalysisDao::AnalysisType type,
            const QString& description,
            const QString& version,
            const QByteArray& data);

    struct FsHandle {
        QString connectionName;
        bool writable = false;
    };

    const UserSettingsPointer m_pConfig;
    // Maps a filesystem mount root to its open cache connection (the fs->cache mapping).
    QHash<QString, FsHandle> m_handles;
    // Serializes access to m_handles and its connections between this instance's
    // home thread (cache reads/writes) and any thread calling releaseFilesystem()
    // via closeFilesystemConnections() (e.g. the eject path).
    QMutex m_handlesMutex;

    // Registry of all live instances, so closeFilesystemConnections() can reach
    // every cache regardless of which thread/owner created it.
    static QMutex s_registryMutex;
    static QSet<FsAnalysisCache*> s_instances;
};
