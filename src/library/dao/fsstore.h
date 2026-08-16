#pragma once

#include <QSqlDatabase>
#include <QString>

/// Where one per-filesystem store lives: a self-contained SQLite database in
/// the `.bitedj` directory at the root of a removable drive, so that whatever
/// it holds travels with the stick.
///
/// Resolving is deliberately restricted to drives the DJ can pull out. Cues and
/// sampler banks only need to travel when the media does, and a store written
/// anywhere else would be out of reach of the settings actions that clear them
/// (which walk the same removable mounts). The *mount root* has to be the
/// removable one, not just the path: a plain directory sitting under /mnt
/// belongs to the filesystem it is a folder on, and a store rooted there would
/// land on the boot volume.
struct FsStoreTarget {
    /// Absolute path of the database file.
    QString dbPath;
    /// Absolute path of the `.bitedj` directory holding it.
    QString storeDirPath;
    /// Mount root of the filesystem the store lives on.
    QString rootPath;
    /// Path of the resolved file relative to `rootPath`. Only set by
    /// resolveForFile(); empty for a store keyed by something other than a file.
    QString relPath;
    /// False on a write-protected stick, which is an expected case rather than
    /// a failure worth logging on every save.
    bool writable = false;

    /// Locate the store `dbName` on the removable filesystem holding
    /// `fileLocation`, and record where that file sits inside it. Returns false
    /// when the filesystem is unavailable or is not a removable one.
    static bool resolveForFile(const QString& fileLocation,
            const QString& dbName,
            FsStoreTarget* pTarget);

    /// Locate the store `dbName` on the removable filesystem mounted at
    /// `mountRoot`. Returns false unless `mountRoot` is the mount point of a
    /// removable filesystem that is currently there — in particular a stale
    /// path left behind by an eject resolves to nothing rather than to the
    /// boot volume it is now a plain directory on.
    static bool resolveForMount(const QString& mountRoot,
            const QString& dbName,
            FsStoreTarget* pTarget);
};

/// A store connection that is opened for one operation and dropped again,
/// leaving no file descriptor behind on the drive.
///
/// The stores that use this do not keep their connections open: their writes
/// are a few hundred bytes each and rare, while a lingering file descriptor on
/// a USB stick makes `umount` fail with EBUSY on eject — and the eject path
/// closes the analysis caches *before* pumping the event loop that evicts (and
/// thereby saves) a track, so a store that held connections could be reopened
/// behind the eject's back.
class ScopedFsStore {
  public:
    /// `logTag` names the owning store in the warnings this emits.
    explicit ScopedFsStore(const char* logTag);
    ~ScopedFsStore();

    ScopedFsStore(const ScopedFsStore&) = delete;
    ScopedFsStore& operator=(const ScopedFsStore&) = delete;

    /// Open the store at `target`. With a non-empty `createTableDdl` the
    /// statement is executed (and the `.bitedj` directory created) as needed;
    /// with an empty one an absent database is simply "nothing stored here"
    /// and nothing at all is written to the drive.
    ///
    /// Recovers once from a database file that is corrupt or not SQLite at all
    /// by replacing it — what was in it is unreadable either way.
    bool open(const FsStoreTarget& target, const QString& createTableDdl);

    QSqlDatabase database() const;

  private:
    const char* m_logTag;
    QString m_connectionName;
};

/// Delete `dbName` and the journal siblings a crash may have left beside it
/// from the `.bitedj` directory of the filesystem mounted at `mountPoint`.
/// Returns false only when a file exists but could not be deleted; a drive
/// without the store counts as success.
bool fsStoreRemove(const QString& mountPoint, const QString& dbName, const char* logTag);
