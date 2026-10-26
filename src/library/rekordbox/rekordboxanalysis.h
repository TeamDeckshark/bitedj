#pragma once

#include <QString>
#include <functional>
#include <optional>

#include "preferences/usersettings.h"
#include "track/beats.h"
#include "track/track_decl.h"

/// Whose analysis a track a rekordbox export on its drive has analysed plays
/// with: rekordbox's (the beat grid in the ANLZ file and the key in the PDB) or
/// Bite DJ's own detection. The DJ's pick is stored per track on its drive
/// (FsAnalysisCache::saveAnalysisSource()), so the values must not move.
enum class AnalysisSource {
    BiteDJ = 0,
    Rekordbox = 1,
};

/// For a track the DJ has not picked a source for. Only ever used when
/// rekordbox's grid can actually be read; otherwise the track plays with Bite
/// DJ's analysis.
constexpr AnalysisSource kAnalysisSourceDefault = AnalysisSource::Rekordbox;

/// Puts rekordbox's beat grid and key on a track, or takes them off again, for
/// whichever player is loading it — whatever view the track came from. A track
/// on a drive with a rekordbox export is the same track whether it is picked
/// from the Rekordbox view, from Browse or from the library, so the analysis
/// it plays with must not depend on that.
///
/// Both analyses are kept, side by side, and neither is ever thrown away to
/// make room for the other:
///
///  - Rekordbox's comes from the drive itself every time: the ANLZ `.DAT` for
///    the grid, the PDB (via the scanned copy of it) for the key. Both are
///    tagged with the rekordbox sub-version (rekordboxconstants), which is
///    what everything downstream keys on. FsAnalysisCache never stores such a
///    grid or key, and the analyzers never replace one (AnalyzerBeats /
///    AnalyzerKey). Rekordbox's grid is read-only here: BpmControl refuses to
///    edit it, since there is nowhere to write a change back to.
///  - Bite DJ's lives where it always has: the per-drive analysis cache
///    (`.bitedj/analysis.sqlite`), edits made on a deck included. Nothing
///    about its format changes, so a stick written by an older build reads
///    the same way. Rows an older build wrote from a rekordbox import are
///    recognised and ignored rather than shown as Bite DJ's (see apply()).
///
/// Which of the two a track plays with is the DJ's pick for that track, stored
/// on its drive next to Bite DJ's analysis: rekordbox's until the DJ picks,
/// and Bite DJ's whenever rekordbox's grid is missing or unreadable.
///
/// All of it runs on the GUI thread, like the player loads that call it.
class RekordboxAnalysis {
  public:
    /// What the scanned copy of a drive's rekordbox export knows about one
    /// audio file.
    struct Entry {
        /// The track's ANLZ `.DAT`. May name a file that does not exist: a
        /// track rekordbox never analysed has an empty analyze path.
        QString anlzPath;
        /// The key exactly as the PDB spells it.
        QString keyText;
    };

    using Lookup = std::function<std::optional<Entry>(const QString& location)>;

    /// RekordboxFeature owns the scanned tables and answers lookups from them.
    /// It registers here on construction and clears this (with an empty
    /// function) on destruction.
    static void setLookup(Lookup lookup);

    /// The rekordbox export entry for the audio file at `location`, if a
    /// scanned drive has one.
    static std::optional<Entry> lookup(const QString& location);

    struct Result {
        /// Rekordbox's grid for the track could be read, so there is a choice
        /// to make. Without it the track plays with Bite DJ's analysis.
        bool rekordboxAvailable = false;
        /// Whose analysis the track carries now.
        AnalysisSource source = AnalysisSource::BiteDJ;
        /// The source picked has no grid or no key for the track yet, and the
        /// analyzer has to produce it. Only ever true for AnalysisSource::BiteDJ.
        bool needsAnalysis = false;
    };

    /// Give `pTrack` the grid and key of the source the DJ picked for it (see
    /// AnalysisSource), as a player loading it does. A track no export knows
    /// is left exactly as it was.
    ///
    /// For AnalysisSource::Rekordbox: rekordbox's grid and key replace whatever
    /// the track has. An export holding no key leaves the track its own key.
    ///
    /// For AnalysisSource::BiteDJ: a rekordbox grid or key on the track is
    /// replaced with Bite DJ's from the drive's analysis cache, or cleared for
    /// the analyzer to fill in when there is none yet (needsAnalysis).
    ///
    /// Replacing the grid forgets its undo history: Undo never crosses from
    /// one source to the other. A BPM-locked grid is the DJ's and is never
    /// replaced either way.
    static Result applySource(const TrackPointer& pTrack, UserSettingsPointer pConfig);

    /// The DJ picked `source` for `pTrack`: store that on its drive and apply
    /// it. Rekordbox's is refused (and nothing stored) when its grid cannot
    /// be read; the result says what the track carries.
    static Result chooseSource(const TrackPointer& pTrack,
            AnalysisSource source,
            UserSettingsPointer pConfig);

    static bool isRekordboxBeats(const mixxx::BeatsPointer& pBeats);

  private:
    static Result apply(const TrackPointer& pTrack,
            std::optional<AnalysisSource> chosen,
            UserSettingsPointer pConfig);
};
