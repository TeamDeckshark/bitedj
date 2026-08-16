#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

class Track;

/// Portable, per-filesystem store for the cues a DJ sets on this unit.
///
/// Hot cues and memory cues edited while a track plays are written back to the
/// drive the track came from, into a self-contained SQLite database next to the
/// analysis cache: `<mountRoot>/.bitedj/cues.sqlite`. Entries are keyed by the
/// track's path relative to the mount root and hold positions in seconds, so
/// the cues travel with the stick and reload on any other Bite DJ unit.
///
/// A stored entry is an *override*: it is the whole picture of a track's hot
/// cue bank, memory cue bank and main cue, and it is applied after the
/// rekordbox ANLZ import, so it wins over whatever rekordbox exported. An entry
/// with no cues in it is meaningful — that is a track whose cues the DJ deleted
/// — which is why the store distinguishes "no entry" from "an empty entry".
///
/// Unlike FsAnalysisCache this store does not keep its connections open: every
/// operation opens the database, runs one statement and closes it again. The
/// writes are a few hundred bytes each and rare (only when the cues actually
/// changed), while a lingering file descriptor on a USB stick makes `umount`
/// fail with EBUSY on eject — the eject path closes the analysis caches *before*
/// pumping the event loop that evicts (and thereby saves) the track, so a store
/// that held connections could be reopened behind the eject's back.
///
/// All members are static: the baselines below are process-wide state guarded
/// by an internal mutex, and every entry point is safe to call from any thread.
class FsCueOverrideStore {
  public:
    /// Apply the stored cue override for `pTrack`, if the track's filesystem
    /// has one, and remember the resulting cue set as the baseline against
    /// which a later flushIfChanged() detects DJ edits.
    ///
    /// Cues are updated in place wherever a slot survives, because this runs on
    /// the same Track object a deck may already be playing; recreating them
    /// would blank that deck's pads and drop an active saved loop mid-set.
    ///
    /// Does nothing (and records no baseline, so a later load can retry) while
    /// the track has no valid sample rate to convert stored seconds with.
    static void applyOverrides(Track* pTrack);

    /// Write the track's cues to its filesystem if they differ from the
    /// baseline remembered by applyOverrides(), i.e. if the DJ added, moved or
    /// deleted a cue since the track was loaded. Touches no database at all
    /// when nothing changed, which is what keeps eject free of EBUSY.
    ///
    /// A track that was never seen by applyOverrides() has an empty baseline,
    /// so its cues are stored the first time it is saved with any cue set.
    static void flushIfChanged(const Track& track);

    /// Delete the cue override database of the filesystem mounted at
    /// `mountPoint` (`<mountPoint>/.bitedj/cues.sqlite`). Returns false only
    /// when a database exists but could not be deleted; a drive without one
    /// counts as success.
    static bool clearFilesystemOverrides(const QString& mountPoint);

    /// Stop flushIfChanged() from re-creating an override for a track that is
    /// still loaded (typically one in a deck) after the overrides were cleared.
    /// The next load of the track re-baselines it and saves again.
    static void suppressPendingSaves();

    /// Locations of the tracks this store has actually put an override on
    /// since startup. Paired with the global track cache to reach the ones
    /// that are still in a deck when the overrides are cleared.
    ///
    /// Deliberately *not* every track it has baselined: a track the drive held
    /// no override for carries nothing of this unit's, so clearing has nothing
    /// to take off it and must not touch it at all.
    static QStringList overriddenLocations();

    /// Put `pTrack` back to the cues its source library exported — the state
    /// captured by applyOverrides() before this unit's override went on top.
    /// Returns false, changing nothing, for a track that never had one.
    ///
    /// This is what makes Settings → Clear take off the DJ's own edits without
    /// taking the rekordbox ANLZ / Serato marker cues with them. Removing every
    /// managed cue instead would blank the pads of a loaded track down to the
    /// imported cues it never owned — and for a Serato track that is permanent,
    /// since markers are imported from the file's tags once and the library
    /// database is authoritative from then on.
    ///
    /// Cues are updated in place wherever a slot survives, for the same reason
    /// applyOverrides() does it: this runs on a track a deck may be playing.
    static bool restoreImportedCues(Track* pTrack);

    /// The stored payload: the track's hot cues, memory cues and main cue as a
    /// compact JSON array with positions in seconds. Also serves as the
    /// baseline to compare a later cue set against, so the same cues always
    /// have to serialize to the same bytes.
    ///
    /// Public so the codec can be exercised without a removable drive under
    /// the test; the store itself only ever reaches it through the calls above.
    static QByteArray serializeCues(const Track& track);

    /// Replace the track's hot cue bank, memory cue bank and main cue with the
    /// ones in `payload`, leaving every other cue (intro, outro, the
    /// analyzer's own) alone.
    static void applyPayload(Track* pTrack, const QByteArray& payload);

  private:
    // Returns false if the track's filesystem is unavailable. `pFound` reports
    // whether it holds an override for the track.
    static bool readOverride(
            const QString& trackLocation, QByteArray* pPayload, bool* pFound);
    static bool writeOverride(const QString& trackLocation, const QByteArray& payload);

    static QMutex s_baselineMutex;
    // Maps a track's location to the cue set it was loaded with (or last saved
    // with), or to a suppression marker set by suppressPendingSaves().
    static QHash<QString, QByteArray> s_baselines;
    // Maps a track's location to the cue set its source library exported, as it
    // stood just before an override was applied over it. Only holds the tracks
    // that actually got one, which is what restoreImportedCues() keys off.
    static QHash<QString, QByteArray> s_importedCues;
};
