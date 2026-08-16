#pragma once

#include <QObject>
#include <QSet>
#include <QString>

#include "track/track_decl.h"

/// Bite DJ: session-scoped registry of the tracks that have actually been
/// played since the app started, used to tint their library rows so a DJ can
/// see at a glance what is already spent.
///
/// Deliberately *not* backed by the library's `played` database column:
///
/// - that column only exists for library-backed models.
/// - the column is only cleared on a clean shutdown (TrackDAO::finish()), so a
///   power-yanked appliance boots with every track from the last gig still
///   flagged — the opposite of "played this session".
///
/// A track is registered when it becomes the audible one
/// (PlayerInfo::currentPlayingTrackChanged), which is the same signal stock
/// Mixxx uses to bump the play counter and write the history playlist. Loading
/// a track into a deck is not enough; it has to actually play.
class PlayedTracks : public QObject {
    Q_OBJECT

  public:
    /// Lazily created on first use, which startup forces on the GUI thread so
    /// the registry is listening (and has the right thread affinity) before any
    /// deck can play.
    static PlayedTracks& instance();
    static void destroy();

    /// True while nothing has been played yet. Callers use this to skip the
    /// per-cell location lookup in the common (start of the night) case.
    bool isEmpty() const {
        return m_playedLocations.isEmpty();
    }

    bool isPlayed(const QString& trackLocation) const {
        return !trackLocation.isEmpty() &&
                m_playedLocations.contains(trackLocation);
    }

  public slots:
    /// Register a file as played. Normally driven by PlayerInfo (see above).
    void markPlayed(const QString& trackLocation);

    /// Forget everything played so far, e.g. when the next DJ takes over.
    /// Bound to [Library],reset_played_tracks by Library.
    void clear();

  signals:
    /// The played set gained an entry or was reset; views should repaint.
    void playedTracksChanged();

  private slots:
    void slotCurrentPlayingTrackChanged(TrackPointer pTrack);

  private:
    PlayedTracks();

    QSet<QString> m_playedLocations;

    static PlayedTracks* s_pInstance;
};
