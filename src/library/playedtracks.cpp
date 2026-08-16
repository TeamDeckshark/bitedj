#include "library/playedtracks.h"

#include "mixer/playerinfo.h"
#include "moc_playedtracks.cpp"
#include "track/track.h"

// static
PlayedTracks* PlayedTracks::s_pInstance = nullptr;

PlayedTracks::PlayedTracks() {
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &PlayedTracks::slotCurrentPlayingTrackChanged);
}

// static
PlayedTracks& PlayedTracks::instance() {
    // Lazily created so the library models can consult the registry no matter
    // how they are constructed (tests build them without CoreServices).
    if (!s_pInstance) {
        s_pInstance = new PlayedTracks();
    }
    return *s_pInstance;
}

// static
void PlayedTracks::destroy() {
    delete s_pInstance;
    s_pInstance = nullptr;
}

void PlayedTracks::slotCurrentPlayingTrackChanged(TrackPointer pTrack) {
    if (!pTrack) {
        // No deck is audible right now.
        return;
    }
    markPlayed(pTrack->getLocation());
}

void PlayedTracks::markPlayed(const QString& trackLocation) {
    if (trackLocation.isEmpty()) {
        return;
    }
    const int sizeBefore = m_playedLocations.size();
    m_playedLocations.insert(trackLocation);
    if (m_playedLocations.size() != sizeBefore) {
        emit playedTracksChanged();
    }
}

void PlayedTracks::clear() {
    if (m_playedLocations.isEmpty()) {
        return;
    }
    m_playedLocations.clear();
    emit playedTracksChanged();
}
