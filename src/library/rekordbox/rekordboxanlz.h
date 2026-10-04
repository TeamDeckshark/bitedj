#pragma once

#include <QString>
#include <QStringList>

#include "audio/types.h"
#include "track/track_decl.h"

namespace mixxx {
namespace rekordbox {

/// Import beats and/or cues from a rekordbox ANLZ file onto a track.
///
/// `ignoreCues` picks which half of the file is read: beats when true, cues
/// when false. Both halves are never wanted from the same file — the beat grid
/// is only correct in the legacy `.DAT`, while cues are preferred from the
/// `.EXT` when one exists.
///
/// The cue pass treats the ANLZ file as the authority for the whole track:
/// hot cues land in the hot cue bank, memory cues in the memory cue bank (see
/// `kHotCueBankStart` / `kMemoryCueBankStart` in `track/cueinfo.h`), and any
/// hotcue slot the file does not describe is cleared. Cues that survive are
/// updated in place, because this runs on every load of a track that a deck
/// may already be playing.
///
/// Throws if the file cannot be parsed. The whole file is parsed before the
/// track is touched, so a truncated or corrupt file leaves the track as it was.
///
/// Declared here rather than kept file-local so that it can be tested
/// directly; the definition lives in rekordboxfeature.cpp.
void readAnalyze(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath);

/// Run both `readAnalyze` passes for a track's `.DAT` path: beats from the
/// `.DAT`, then cues from the `.EXT`, or from the `.DAT` again when the export
/// has no `.EXT`. Each pass fails on its own, so a corrupt `.EXT` still leaves
/// the grid and a corrupt `.DAT` still leaves the cues.
///
/// Returns the paths that exist but could not be read. A track rekordbox never
/// analysed has no ANLZ file at all, which is not a failure.
QStringList readAnalyzeFiles(TrackPointer track,
        audio::SampleRate sampleRate,
        int timingOffset,
        const QString& anlzPath);

} // namespace rekordbox
} // namespace mixxx
