#include "library/rekordbox/rekordboxanalysis.h"

#include <QMutex>
#include <QtDebug>
#include <exception>
#include <memory>

#include "library/dao/fsanalysiscache.h"
#include "library/rekordbox/rekordboxanlz.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "library/rekordbox/rekordboxkeydisplay.h"
#include "track/keyfactory.h"
#include "track/keys.h"
#include "track/track.h"

namespace {

// Guards the lookup, which RekordboxFeature sets and clears.
QMutex s_mutex;

RekordboxAnalysis::Lookup s_lookup;

// The analysis cache is bound to the thread that creates it, and this one is
// only ever used from the GUI thread (see apply()).
FsAnalysisCache& guiThreadCache(UserSettingsPointer pConfig) {
    static std::unique_ptr<FsAnalysisCache> s_pCache;
    if (!s_pCache) {
        s_pCache = std::make_unique<FsAnalysisCache>(pConfig);
    }
    return *s_pCache;
}

bool isRekordboxKeys(const Keys& keys) {
    return keys.getSubVersion() == mixxx::rekordboxconstants::keysSubversion;
}

/// The key the export gives the track, tagged as rekordbox's, or nullopt if
/// it gives none we can read.
std::optional<Keys> rekordboxKeys(const QString& keyText) {
    if (keyText.isEmpty()) {
        return std::nullopt;
    }
    // From 5.4.3 the key format depends on rekordbox's preferences:
    // Classic: Abm,B,Ebm,F#,Bbm,Db,Fm,Ab,…,F#m,A,Dbm,E
    // Alphanumeric (Camelot): 1A,1B,2A,2B,3A,3B,4A,4B,…,11A,11B,12A,12B
    // Not recognized: 1m, 01A
    // Earlier versions allow any format.
    // Normalized so that no garbage is written to file metadata; it is
    // unlikely to lose extra information.
    Keys keys = KeyFactory::makeBasicKeysNormalized(keyText, mixxx::track::io::key::USER);
    if (keys.getGlobalKey() == mixxx::track::io::key::INVALID) {
        return std::nullopt;
    }
    keys.setSubVersion(mixxx::rekordboxconstants::keysSubversion);
    return keys;
}

/// Whether `keys` came from rekordbox. Before keys were tagged, a rekordbox
/// key was stored in the analysis cache as a plain typed-in key (no
/// sub-version); one of those that is exactly the export's key is taken to be
/// that import, not something Bite DJ detected. Bite DJ's own detection always
/// carries a sub-version, so it is never mistaken for one.
bool isFromRekordbox(const Keys& keys, const std::optional<Keys>& exportKeys) {
    if (isRekordboxKeys(keys)) {
        return true;
    }
    return exportKeys && keys.getSubVersion().isEmpty() &&
            keys.getGlobalKey() == exportKeys->getGlobalKey();
}

/// Rekordbox's grid for the track, or null when the export has none (never
/// analysed in rekordbox), it cannot be read, or the track is not opened yet.
/// One already on the track is reused rather than read again, unless it came
/// from a build that did not import rekordbox's bars (no downbeat anchors).
mixxx::BeatsPointer rekordboxGrid(const TrackPointer& pTrack,
        const QString& location,
        const RekordboxAnalysis::Entry& entry) {
    const mixxx::BeatsPointer pCurrent = pTrack->getBeats();
    if (RekordboxAnalysis::isRekordboxBeats(pCurrent) &&
            !pCurrent->getDownbeatAnchors().empty()) {
        return pCurrent;
    }
    try {
        return mixxx::rekordbox::readBeatGrid(entry.anlzPath,
                pTrack->getSampleRate(),
                mixxx::rekordbox::timingOffsetForFile(location));
    } catch (const std::exception& error) {
        qWarning() << "Could not read the Rekordbox beat grid:"
                   << entry.anlzPath << error.what();
        return nullptr;
    }
}

} // anonymous namespace

// static
void RekordboxAnalysis::setLookup(Lookup lookup) {
    {
        const QMutexLocker locker(&s_mutex);
        s_lookup = std::move(lookup);
    }
    RekordboxKeyDisplay::instance().exportChanged();
}

// static
std::optional<RekordboxAnalysis::Entry> RekordboxAnalysis::lookup(const QString& location) {
    Lookup lookup;
    {
        const QMutexLocker locker(&s_mutex);
        lookup = s_lookup;
    }
    if (!lookup || location.isEmpty()) {
        return std::nullopt;
    }
    return lookup(location);
}

// static
RekordboxAnalysis::Result RekordboxAnalysis::applySource(
        const TrackPointer& pTrack, UserSettingsPointer pConfig) {
    return apply(pTrack, std::nullopt, pConfig);
}

// static
RekordboxAnalysis::Result RekordboxAnalysis::chooseSource(const TrackPointer& pTrack,
        AnalysisSource source,
        UserSettingsPointer pConfig) {
    return apply(pTrack, source, pConfig);
}

// static
RekordboxAnalysis::Result RekordboxAnalysis::apply(const TrackPointer& pTrack,
        std::optional<AnalysisSource> chosen,
        UserSettingsPointer pConfig) {
    Result result;
    if (!pTrack) {
        return result;
    }
    const QString location = pTrack->getLocation();
    const std::optional<Entry> entry = lookup(location);
    if (!entry) {
        return result;
    }

    FsAnalysisCache& cache = guiThreadCache(pConfig);
    const std::optional<Keys> exportKeys = rekordboxKeys(entry->keyText);
    const mixxx::BeatsPointer pRekordboxGrid = rekordboxGrid(pTrack, location, *entry);
    result.rekordboxAvailable = static_cast<bool>(pRekordboxGrid);

    AnalysisSource source = kAnalysisSourceDefault;
    if (chosen) {
        source = *chosen;
        // Picking rekordbox's grid where there is none is no pick at all.
        if (result.rekordboxAvailable || source == AnalysisSource::BiteDJ) {
            cache.saveAnalysisSource(location, static_cast<int>(source));
        }
    } else if (const std::optional<int> stored = cache.getAnalysisSource(location)) {
        if (*stored == static_cast<int>(AnalysisSource::BiteDJ) ||
                *stored == static_cast<int>(AnalysisSource::Rekordbox)) {
            source = static_cast<AnalysisSource>(*stored);
        }
    }
    if (!result.rekordboxAvailable) {
        source = AnalysisSource::BiteDJ;
    }

    if (source == AnalysisSource::Rekordbox) {
        if (pTrack->getBeats() == pRekordboxGrid ||
                pTrack->trySetBeatsDiscardingUndo(pRekordboxGrid)) {
            if (exportKeys && pTrack->getKeys() != *exportKeys) {
                pTrack->setKeys(*exportKeys);
            }
            result.source = AnalysisSource::Rekordbox;
            return result;
        }
        // BPM-locked: the DJ's grid stays, and it is Bite DJ's.
    }

    result.source = AnalysisSource::BiteDJ;
    if (isRekordboxBeats(pTrack->getBeats())) {
        const mixxx::audio::SampleRate sampleRate = pTrack->getSampleRate();
        mixxx::BeatsPointer pOwn;
        if (cache.isEnabled() && sampleRate.isValid()) {
            pOwn = cache.getTrackBeats(location, sampleRate);
        }
        // An older build stored the imported grid where Bite DJ's goes.
        if (isRekordboxBeats(pOwn)) {
            pOwn.reset();
        }
        if (pTrack->trySetBeatsDiscardingUndo(pOwn) && !pOwn) {
            result.needsAnalysis = true;
        }
    }
    if (isFromRekordbox(pTrack->getKeys(), exportKeys)) {
        std::optional<Keys> own;
        if (cache.isEnabled()) {
            own = cache.getTrackKeys(location);
        }
        if (own && !isFromRekordbox(*own, exportKeys) &&
                own->getGlobalKey() != mixxx::track::io::key::INVALID) {
            pTrack->setKeys(*own);
        } else {
            pTrack->setKeys(Keys());
            result.needsAnalysis = true;
        }
    }
    return result;
}

// static
bool RekordboxAnalysis::isRekordboxBeats(const mixxx::BeatsPointer& pBeats) {
    return pBeats && pBeats->getSubVersion() == mixxx::rekordboxconstants::beatsSubversion;
}
