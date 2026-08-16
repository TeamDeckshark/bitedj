#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

class Track;

/// Announces every rating this unit writes to a drive, so the views that show a
/// rating from somewhere other than the track itself can follow it.
///
/// The Rekordbox playlist view is the one that needs it: its ratings come from
/// the device's exported database (mirrored into a temporary table at scan
/// time), not from the Track object, so a rating set on a deck would otherwise
/// not show in the playlist until the drive is scanned again.
class FsMetaOverrideNotifier : public QObject {
    Q_OBJECT

  public:
    /// Lazily created on first use. Callers on the GUI thread (the features
    /// that connect to it) touch it first, so it takes their affinity and a
    /// store write from a worker thread is delivered queued.
    static FsMetaOverrideNotifier& instance();

  signals:
    /// `trackLocation` now has `rating` stored on its drive.
    void ratingStored(const QString& trackLocation, int rating);

  private:
    FsMetaOverrideNotifier() = default;

    friend class FsMetaOverrideStore;
};

/// Portable, per-filesystem store for the track metadata a DJ edits on this
/// unit. Currently that is the star rating.
///
/// A rating changed here is written back to the drive the track came from, into
/// a self-contained SQLite database next to the analysis cache and the cue
/// overrides: `<mountRoot>/.bitedj/meta.sqlite`. Entries are keyed by the
/// track's path relative to the mount root, so the rating travels with the
/// stick and comes back on the next insertion — on this unit or any other Bite
/// DJ one — rather than living in this box's library, which the drive's own
/// playlists are never read from.
///
/// A stored entry is an *override*: it wins over the rating the source library
/// exported (a rekordbox `DJ Rating`), and it is applied both to the Track
/// object and to the scanned copy of that library. An entry holding no rating
/// at all is meaningful — that is a track whose stars the DJ took off — which
/// is why the store distinguishes "no entry" from "an entry of zero".
///
/// Like FsCueOverrideStore this store does not keep its connections open: every
/// operation opens the database, runs one statement and closes it again, so no
/// file descriptor lingers to make `umount` fail with EBUSY on eject.
///
/// All members are static: the baselines below are process-wide state guarded
/// by an internal mutex, and every entry point is safe to call from any thread.
class FsMetaOverrideStore {
  public:
    /// Every rating stored for one drive, read in a single pass.
    ///
    /// The rekordbox scan needs the whole picture at once — it walks thousands
    /// of tracks and cannot open the store per track — so it takes one of these
    /// and asks it per file.
    struct MountRatings {
        /// Mount root the relative paths below are keyed against. Empty when
        /// the drive has no store (or is not a removable one), which makes
        /// ratingFor() a pass-through.
        QString rootPath;
        QHash<QString, int> byRelPath;

        bool isEmpty() const {
            return byRelPath.isEmpty();
        }

        /// The stored rating for the file at `trackLocation`, or `fallback`
        /// (what the source library says) when the DJ never rated it here.
        int ratingFor(const QString& trackLocation, int fallback) const;
    };

    /// Apply the stored rating for `pTrack`, if the track's filesystem has one,
    /// and remember the resulting rating as the baseline against which a later
    /// flushIfChanged() detects DJ edits.
    static void applyOverrides(Track* pTrack);

    /// Write the track's rating to its filesystem if it differs from the
    /// baseline remembered by applyOverrides(), i.e. if the DJ changed it since
    /// the track was loaded. Touches no database at all when nothing changed,
    /// which is what keeps eject free of EBUSY.
    ///
    /// A track that was never seen by applyOverrides() has no baseline, so its
    /// rating is stored the first time it is saved with any stars on it.
    static void flushIfChanged(const Track& track);

    /// Store `rating` for `trackLocation` outright, for an edit made somewhere
    /// the Track object is not the thing being edited — the rating cell of a
    /// Rekordbox playlist, whose stars come from the device's own database.
    /// Returns false when the drive cannot be written to.
    static bool storeRating(const QString& trackLocation, int rating);

    /// Remember `rating` as what `trackLocation` carried before this unit's
    /// first override went on it, unless something is already remembered.
    /// Callers that edit a rating without going through applyOverrides() use
    /// this so that clearing can put the track back rather than blank it.
    static void noteImportedRating(const QString& trackLocation, int rating);

    /// Every rating stored on the filesystem mounted at `mountRoot`. Returns an
    /// empty (pass-through) result for a drive with no store.
    static MountRatings readMountRatings(const QString& mountRoot);

    /// Delete the metadata override database of the filesystem mounted at
    /// `mountPoint` (`<mountPoint>/.bitedj/meta.sqlite`). Returns false only
    /// when a database exists but could not be deleted; a drive without one
    /// counts as success.
    static bool clearFilesystemOverrides(const QString& mountPoint);

    /// Stop flushIfChanged() from re-creating an override for a track that is
    /// still loaded (typically one in a deck) after the overrides were cleared.
    /// The next load of the track re-baselines it and saves again.
    static void suppressPendingSaves();

    /// Locations of the tracks this store has actually put a rating on since
    /// startup. Paired with the global track cache to reach the ones that are
    /// still in a deck when the overrides are cleared.
    ///
    /// Deliberately *not* every track it has baselined: a track the drive held
    /// no override for carries nothing of this unit's, so clearing has nothing
    /// to take off it and must not touch it at all.
    static QStringList overriddenLocations();

    /// Put `pTrack` back to the rating its source library exported — what it
    /// carried before this unit's override went on top. Returns false, changing
    /// nothing, for a track that never had one.
    static bool restoreImportedRating(Track* pTrack);

  private:
    // Returns false if the track's filesystem is unavailable. `pFound` reports
    // whether it holds an override for the track.
    static bool readOverride(const QString& trackLocation, int* pRating, bool* pFound);
    static bool writeOverride(const QString& trackLocation, int rating);

    static QMutex s_baselineMutex;
    // Maps a track's location to the rating it was loaded with (or last saved
    // with), or to a suppression marker set by suppressPendingSaves().
    static QHash<QString, int> s_baselines;
    // Maps a track's location to the rating its source library exported, as it
    // stood just before an override was applied over it. Only holds the tracks
    // that actually got one, which is what restoreImportedRating() keys off.
    static QHash<QString, int> s_importedRatings;
};
