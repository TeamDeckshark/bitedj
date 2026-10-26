#include "analyzer/analyzerbeats.h"

#include <QHash>
#include <QString>
#include <QVector>
#include <QtDebug>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "analyzer/analyzertrack.h"
#include "analyzer/constants.h"
#include "analyzer/plugins/analyzerqueenmarybeats.h"
#include "analyzer/plugins/analyzersoundtouchbeats.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "track/beatfactory.h"
#include "track/track.h"

namespace {

// Coarse bin size for the bass-energy envelope. 4096 frames ≈ 93ms at 44.1k —
// finer than any beat we care about, much cheaper than per-sample storage.
constexpr SINT kBassBinFrames = mixxx::kAnalysisFramesPerChunk;

// One-pole IIR lowpass corner frequency. ~120 Hz captures kick fundamentals
// (~50–80 Hz) and bass synths without picking up snare/clap mid-band energy.
constexpr double kBassCutoffHz = 100.0;

// Minimum E[i+1] / max(E[i], eps) for a beat boundary to qualify as a drop.
// Drops typically go from "near-silent low end" to "huge low end", so 2x is
// conservative; steady-bass tracks won't qualify and stay on the analyzer's
// natural anchor.
constexpr double kDropRatioThreshold = 2.0;

// Minimum absolute energy on the post-drop side. Prevents quiet noise floor
// jumps from registering as drops. Tuned against silenced regions of audio.
constexpr float kDropAbsoluteFloor = 1e-4f;

// Skip the first 15% of the track and the last bar — drops in the very
// first second (rare) or that don't have room to play out aren't useful.
constexpr double kDropMinPositionFraction = 0.15;

// Two detected drops within this many beats of each other are coalesced —
// the louder one wins. Prevents adjacent bins picking up the same drop.
constexpr int kDropMinSeparationBeats = 4;

// Bite DJ: a deck that loads an unanalyzed track gets a provisional grid once
// this much audio has been analyzed, and a fresh one every time the analyzed
// audio doubles after that. The tempo tracker is offline, so every provisional
// grid re-tracks everything so far; doubling keeps the whole series to about
// the cost of one more full tracking pass. 30 s is enough material for the
// tracker's ~6 s tempo windows to settle on a tempo.
constexpr double kProvisionalBeatsFirstSeconds = 30.0;

// Sub-version fragment marking a provisional grid. A grid carrying it may be
// saved (the track can be unloaded mid-analysis) but always gets re-analyzed.
const QString kProvisionalKey = QStringLiteral("provisional");
const QString kProvisionalFragment = QStringLiteral("provisional=1");

// A detected BPM is trusted as-is when it lands within this fraction of the BPM
// stored in the file/library metadata. 5% comfortably absorbs tag rounding and
// genre-typical tempo drift while still catching half/double-time octave errors
// (which are off by 50%/100%) and triplet confusions.
constexpr double kMetadataBpmTolerance = 0.05;

// Harmonic/octave factors a beat tracker is likely to lock onto instead of the
// true tempo: half/double time, the 3-against-2 triplet confusions, and the
// quarter/quadruple-time extremes. Applied to the analyzed BPM when it falls
// outside tolerance of the metadata BPM.
constexpr std::array<double, 8> kBpmOctaveFactors = {
        2.0, 0.5, 1.5, 2.0 / 3.0, 3.0, 1.0 / 3.0, 4.0, 0.25};

// Reconcile an analyzed BPM with the value found in file/library metadata.
//
// Beat trackers — SoundTouch's BPMDetect in particular — frequently lock onto a
// half- or double-time octave of the real tempo. When the file already carries
// a metadata BPM we treat it as ground truth: if the analyzed value is within
// tolerance we keep the (more precise) analyzed value, otherwise we try the
// common octave/harmonic factors and snap to whichever lands inside the
// tolerance band. If nothing fits we fall back to the metadata value outright,
// so the stored BPM is *always* within tolerance of what the file claims.
mixxx::Bpm reconcileBpmWithMetadata(mixxx::Bpm analyzedBpm, mixxx::Bpm metadataBpm) {
    if (!metadataBpm.isValid()) {
        return analyzedBpm;
    }
    if (!analyzedBpm.isValid()) {
        return metadataBpm;
    }
    const double meta = metadataBpm.value();
    const double tolerance = meta * kMetadataBpmTolerance;
    if (std::abs(analyzedBpm.value() - meta) <= tolerance) {
        return analyzedBpm;
    }
    double bestCandidate = 0.0;
    double bestError = std::numeric_limits<double>::max();
    for (double factor : kBpmOctaveFactors) {
        const double candidate = analyzedBpm.value() * factor;
        const double error = std::abs(candidate - meta);
        if (error <= tolerance && error < bestError) {
            bestError = error;
            bestCandidate = candidate;
        }
    }
    if (bestCandidate > 0.0) {
        return mixxx::Bpm(bestCandidate);
    }
    // No octave correction lands within tolerance: trust the metadata outright.
    return metadataBpm;
}

} // namespace

// static
QList<mixxx::AnalyzerPluginInfo> AnalyzerBeats::availablePlugins() {
    QList<mixxx::AnalyzerPluginInfo> plugins;
    // First one below is the default
    plugins.append(mixxx::AnalyzerQueenMaryBeats::pluginInfo());
    plugins.append(mixxx::AnalyzerSoundTouchBeats::pluginInfo());
    return plugins;
}

// static
mixxx::AnalyzerPluginInfo AnalyzerBeats::defaultPlugin() {
    const auto plugins = availablePlugins();
    DEBUG_ASSERT(!plugins.isEmpty());
    return plugins.at(0);
}

AnalyzerBeats::AnalyzerBeats(UserSettingsPointer pConfig,
        bool enforceBpmDetection,
        bool provisionalBeats)
        : m_bpmSettings(pConfig),
          m_enforceBpmDetection(enforceBpmDetection),
          m_provisionalBeats(provisionalBeats),
          m_bPreferencesReanalyzeOldBpm(false),
          m_bPreferencesReanalyzeImported(false),
          m_bPreferencesFixedTempo(true),
          m_bPreferencesFastAnalysis(false),
          m_maxFramesToProcess(0),
          m_currentFrame(0),
          m_nextProvisionalFrame(0),
          m_bassFilterStateL(0.0f),
          m_bassFilterStateR(0.0f),
          m_bassFilterCoefficient(0.0f),
          m_currentBassBinFrame(0),
          m_currentBassBinAccumulator(0.0) {
}

bool AnalyzerBeats::initialize(const AnalyzerTrack& track,
        mixxx::audio::SampleRate sampleRate,
        SINT frameLength) {
    if (frameLength <= 0) {
        return false;
    }

    bool bPreferencesBeatDetectionEnabled =
            m_enforceBpmDetection || m_bpmSettings.getBpmDetectionEnabled();
    if (!bPreferencesBeatDetectionEnabled) {
        qDebug() << "Beat calculation is deactivated";
        return false;
    }

    bool bpmLock = track.getTrack()->isBpmLocked();
    if (bpmLock) {
        qDebug() << "Track is BpmLocked: Beat calculation will not start";
        return false;
    }

    m_bPreferencesFixedTempo = track.getOptions().useFixedTempo.value_or(
            m_bpmSettings.getFixedTempoAssumption());
    m_bPreferencesReanalyzeOldBpm = m_bpmSettings.getReanalyzeWhenSettingsChange();
    m_bPreferencesReanalyzeImported = m_bpmSettings.getReanalyzeImported();
    m_bPreferencesFastAnalysis = m_bpmSettings.getFastAnalysis();

    const auto plugins = availablePlugins();
    if (!plugins.isEmpty()) {
        m_pluginId = defaultPlugin().id();
        QString pluginId = m_bpmSettings.getBeatPluginId();
        for (const auto& info : plugins) {
            if (info.id() == pluginId) {
                m_pluginId = pluginId; // configured Plug-In available
                break;
            }
        }
    }

    qDebug() << "AnalyzerBeats preference settings:"
             << "\nPlugin:" << m_pluginId
             << "\nFixed tempo assumption:" << m_bPreferencesFixedTempo
             << "\nRe-analyze when settings change:" << m_bPreferencesReanalyzeOldBpm
             << "\nRe-analyze imported from other software:" << m_bPreferencesReanalyzeImported
             << "\nFast analysis:" << m_bPreferencesFastAnalysis;

    m_sampleRate = sampleRate;
    // In fast analysis mode, skip processing after
    // kFastAnalysisSecondsToAnalyze seconds are analyzed.
    if (m_bPreferencesFastAnalysis) {
        m_maxFramesToProcess =
                mixxx::kFastAnalysisSecondsToAnalyze * m_sampleRate;
    } else {
        m_maxFramesToProcess = frameLength;
    }
    m_currentFrame = 0;
    m_metadataBpm = track.getTrack()->getMetadata().getTrackInfo().getBpm();

    m_bassFilterStateL = 0.0f;
    m_bassFilterStateR = 0.0f;
    m_bassFilterCoefficient = static_cast<float>(
            std::exp(-2.0 * M_PI * kBassCutoffHz / m_sampleRate));
    m_currentBassBinFrame = 0;
    m_currentBassBinAccumulator = 0.0;
    m_bassEnergyBins.clear();
    m_bassEnergyBins.reserve(static_cast<size_t>(m_maxFramesToProcess / kBassBinFrames + 1));

    // if we can load a stored track don't reanalyze it
    bool bShouldAnalyze = shouldAnalyze(track.getTrack());
    qDebug() << "Should analyze? " << bShouldAnalyze;

    DEBUG_ASSERT(!m_pPlugin);
    if (bShouldAnalyze) {
        if (m_pluginId == mixxx::AnalyzerQueenMaryBeats::pluginInfo().id()) {
            m_pPlugin = std::make_unique<mixxx::AnalyzerQueenMaryBeats>();
        } else if (m_pluginId == mixxx::AnalyzerSoundTouchBeats::pluginInfo().id()) {
            m_pPlugin = std::make_unique<mixxx::AnalyzerSoundTouchBeats>();
        } else {
            // This must not happen, because we have already verified above
            // that the PlugInId is valid
            DEBUG_ASSERT(false);
        }

        if (m_pPlugin) {
            if (m_pPlugin->initialize(m_sampleRate)) {
                qDebug() << "Beat calculation started with plugin" << m_pluginId;
                if (m_provisionalBeats && m_pPlugin->supportsBeatTracking()) {
                    m_pProvisionalTrack = track.getTrack();
                    m_nextProvisionalFrame = static_cast<SINT>(
                            kProvisionalBeatsFirstSeconds * m_sampleRate);
                }
            } else {
                qDebug() << "Beat calculation will not start.";
                m_pPlugin.reset();
                bShouldAnalyze = false;
            }
        } else {
            bShouldAnalyze = false;
        }
    }
    return bShouldAnalyze;
}

bool AnalyzerBeats::shouldAnalyze(TrackPointer pTrack) const {
    bool bpmLock = pTrack->isBpmLocked();
    if (bpmLock) {
        qDebug() << "Track is BpmLocked: Beat calculation will not start";
        return false;
    }

    QString pluginID = m_bpmSettings.getBeatPluginId();
    if (pluginID.isEmpty()) {
        pluginID = defaultPlugin().id();
    }

    // If the track already has a Beats object then we need to decide whether to
    // analyze this track or not.
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    if (!pBeats) {
        return true;
    }
    if (!pBeats->getBpmInRange(mixxx::audio::kStartFramePos,
                       mixxx::audio::FramePos{
                               pTrack->getDuration() * pBeats->getSampleRate()})
                    .isValid()) {
        // Tracks with an invalid bpm <= 0 should be re-analyzed,
        // independent of the preference settings. We expect that
        // all tracks have a bpm > 0 when analyzed. Users that want
        // to keep their zero bpm tracks could lock them to prevent
        // this re-analysis (see the check above).
        qDebug() << "Re-analyzing track with invalid BPM despite preference settings.";
        return true;
    }

    QString subVersion = pBeats->getSubVersion();
    if (subVersion == mixxx::rekordboxconstants::beatsSubversion) {
        return m_bPreferencesReanalyzeImported;
    }

    if (isProvisional(pBeats)) {
        qDebug() << "Re-analyzing track whose analysis was interrupted.";
        return true;
    }

    if (subVersion.isEmpty() && pBeats->firstBeat() <= mixxx::audio::kStartFramePos &&
            m_pluginId != mixxx::AnalyzerSoundTouchBeats::pluginInfo().id()) {
        // This happens if the beat grid was created from the metadata BPM value.
        qDebug() << "First beat is 0 for grid so analyzing track to find first beat.";
        return true;
    }

    QString version = pBeats->getVersion();
    QHash<QString, QString> extraVersionInfo = getExtraVersionInfo(
            pluginID,
            m_bPreferencesFastAnalysis);
    QString newVersion = BeatFactory::getPreferredVersion(
            m_bPreferencesFixedTempo);
    QString newSubVersion = BeatFactory::getPreferredSubVersion(
            extraVersionInfo);

    if (version == newVersion && subVersion == newSubVersion) {
        // If the version and settings have not changed then if the world is
        // sane, re-analyzing will do nothing.
        return false;
    }
    // Beat grid exists but version and settings differ
    if (!m_bPreferencesReanalyzeOldBpm) {
        qDebug() << "Beat calculation skips analyzing because the track has"
                << "a BPM computed by a previous Mixxx version and user"
                << "preferences indicate we should not change it.";
        return false;
    }

    return true;
}

bool AnalyzerBeats::processSamples(const CSAMPLE* pIn, SINT count) {
    VERIFY_OR_DEBUG_ASSERT(m_pPlugin) {
        return false;
    }

    m_currentFrame += count / mixxx::kAnalysisChannels;
    if (m_currentFrame > m_maxFramesToProcess) {
        return true; // silently ignore all remaining samples
    }

    // Accumulate bass-band envelope alongside beat detection. Run on the
    // same sample stream so we don't pay for a second decode pass.
    const SINT frames = count / mixxx::kAnalysisChannels;
    for (SINT i = 0; i < frames; i++) {
        accumulateBassEnergy(pIn[i * 2], pIn[i * 2 + 1]);
    }

    if (!m_pPlugin->processSamples(pIn, count)) {
        return false;
    }
    if (m_pProvisionalTrack && m_currentFrame >= m_nextProvisionalFrame) {
        publishProvisionalBeats();
        m_nextProvisionalFrame *= 2;
    }
    return true;
}

void AnalyzerBeats::publishProvisionalBeats() {
    const mixxx::BeatsPointer pCurrentBeats = m_pProvisionalTrack->getBeats();
    if (pCurrentBeats && pCurrentBeats != m_pLastProvisionalBeats &&
            (isProvisional(pCurrentBeats) ||
                    pCurrentBeats->getSubVersion() ==
                            mixxx::rekordboxconstants::beatsSubversion)) {
        // The DJ has corrected the last provisional grid, or switched the
        // track to rekordbox's grid. Either way it's theirs now: stop
        // replacing it until the final result (see storeResults()).
        m_pProvisionalTrack.reset();
        return;
    }
    const QVector<mixxx::audio::FramePos> beats = m_pPlugin->getProvisionalBeats();
    if (beats.size() < 2) {
        return;
    }
    const mixxx::BeatsPointer pBeats = makeBeats(beats, m_pProvisionalTrack, true);
    if (!pBeats) {
        return;
    }
    // Off the undo stack: Undo must never step back into a half-analyzed grid.
    if (m_pProvisionalTrack->trySetBeatsDiscardingUndo(pBeats)) {
        m_pLastProvisionalBeats = pBeats;
        qDebug() << "AnalyzerBeats published a provisional grid after"
                 << m_currentFrame / m_sampleRate.value() << "s";
    }
}

// static
bool AnalyzerBeats::isProvisional(const mixxx::BeatsPointer& pBeats) {
    return pBeats &&
            pBeats->getSubVersion().split(QChar('|')).contains(kProvisionalFragment);
}

void AnalyzerBeats::accumulateBassEnergy(CSAMPLE left, CSAMPLE right) {
    // One-pole IIR lowpass: y[n] = a*y[n-1] + (1-a)*x[n]
    // a = exp(-2*pi*fc/fs), set in initialize().
    const float a = m_bassFilterCoefficient;

    m_bassFilterStateL = a * m_bassFilterStateL + (1.0f - a) * left;
    m_bassFilterStateR = a * m_bassFilterStateR + (1.0f - a) * right;

    const float monoLowSquared = m_bassFilterStateL * m_bassFilterStateL +
            m_bassFilterStateR * m_bassFilterStateR;
    m_currentBassBinAccumulator += monoLowSquared;

    m_currentBassBinFrame++;
    if (m_currentBassBinFrame >= kBassBinFrames) {
        m_bassEnergyBins.push_back(
                static_cast<float>(m_currentBassBinAccumulator / kBassBinFrames));
        m_currentBassBinFrame = 0;
        m_currentBassBinAccumulator = 0.0;
    }
}

void AnalyzerBeats::cleanup() {
    m_pPlugin.reset();
    m_pProvisionalTrack.reset();
    m_pLastProvisionalBeats.reset();
}

mixxx::BeatsPointer AnalyzerBeats::makeBeats(
        const QVector<mixxx::audio::FramePos>& beats,
        const TrackPointer& pTrack,
        bool provisional) const {
    QHash<QString, QString> extraVersionInfo = getExtraVersionInfo(
            m_pluginId, m_bPreferencesFastAnalysis);
    if (provisional) {
        extraVersionInfo[kProvisionalKey] = QStringLiteral("1");
    }
    return reconciledWithMetadataBpm(
            BeatFactory::makePreferredBeats(
                    beats,
                    extraVersionInfo,
                    m_bPreferencesFixedTempo,
                    m_sampleRate),
            pTrack);
}

mixxx::BeatsPointer AnalyzerBeats::reconciledWithMetadataBpm(
        mixxx::BeatsPointer pBeats, const TrackPointer& pTrack) const {
    // Reconcile the detected tempo against the file/library metadata BPM so the
    // stored grid is always within tolerance of the tempo the file claims. This
    // matters most for the SoundTouch plugin, whose BPMDetect routinely reports
    // a half- or double-time octave of the true tempo. The metadata BPM must be
    // read before the first trySetBeats() of this analysis overwrites it with
    // the beats-derived value — a provisional grid counts — so initialize()
    // takes it while the record still holds it. Only constant-tempo grids
    // are reconciled; variable-tempo grids (live sets, old disco) keep the
    // analyzer's per-beat positions.
    if (pBeats && pBeats->hasConstantTempo()) {
        const mixxx::Bpm metadataBpm = m_metadataBpm;
        const mixxx::Bpm analyzedBpm = pBeats->getBpmInRange(
                mixxx::audio::kStartFramePos,
                mixxx::audio::FramePos{
                        pTrack->getDuration() * pBeats->getSampleRate()});
        const mixxx::Bpm reconciledBpm =
                reconcileBpmWithMetadata(analyzedBpm, metadataBpm);
        if (!reconciledBpm.compareEq(analyzedBpm)) {
            qDebug() << "AnalyzerBeats reconciled BPM from" << analyzedBpm
                     << "to" << reconciledBpm
                     << "to match metadata BPM" << metadataBpm;
            pBeats = mixxx::Beats::fromConstTempo(
                    pBeats->getSampleRate(),
                    pBeats->firstBeat(),
                    reconciledBpm,
                    pBeats->getSubVersion());
        }
    }
    return pBeats;
}

void AnalyzerBeats::storeResults(TrackPointer pTrack) {
    VERIFY_OR_DEBUG_ASSERT(m_pPlugin) {
        return;
    }

    if (!m_pPlugin->finalize()) {
        qWarning() << "Beat/BPM analysis failed";
        return;
    }

    mixxx::BeatsPointer pBeats;
    if (m_pPlugin->supportsBeatTracking()) {
        QVector<mixxx::audio::FramePos> beats = m_pPlugin->getBeats();
        pBeats = makeBeats(beats, pTrack, false);
        qDebug() << "AnalyzerBeats plugin detected" << beats.size()
                 << "beats. Predominant BPM:"
                 << (pBeats ? pBeats->getBpmInRange(
                                      mixxx::audio::kStartFramePos,
                                      mixxx::audio::FramePos{
                                              pTrack->getDuration() *
                                              pBeats->getSampleRate()})
                            : mixxx::Bpm());
    } else {
        mixxx::Bpm bpm = m_pPlugin->getBpm();
        qDebug() << "AnalyzerBeats plugin detected constant BPM: " << bpm;
        pBeats = reconciledWithMetadataBpm(
                mixxx::Beats::fromConstTempo(
                        m_sampleRate, mixxx::audio::kStartFramePos, bpm),
                pTrack);
    }

    // Flush any partial bin so the energy envelope covers the full analyzed
    // range before drop detection runs.
    if (m_currentBassBinFrame > 0) {
        m_bassEnergyBins.push_back(static_cast<float>(
                m_currentBassBinAccumulator / m_currentBassBinFrame));
        m_currentBassBinFrame = 0;
        m_currentBassBinAccumulator = 0.0;
    }

    // Detect drops and rebuild the beats with anchors. Only const-tempo
    // tracks get drop anchoring — variable-tempo (live recordings, old
    // disco) fall through to the analyzer's natural beat-1 anchor.
    if (pBeats && pBeats->hasConstantTempo() && !m_bassEnergyBins.empty()) {
        std::vector<mixxx::audio::FramePos> anchors =
                detectDownbeatAnchors(pBeats, m_currentFrame);
        if (!anchors.empty()) {
            qDebug() << "AnalyzerBeats detected" << anchors.size()
                     << "downbeat anchor(s); first at frame"
                     << anchors.front().value();
            pBeats = mixxx::Beats::fromConstTempo(
                    pBeats->getSampleRate(),
                    pBeats->getLastMarkerPosition(),
                    pBeats->getLastMarkerBpm(),
                    pBeats->getSubVersion(),
                    std::move(anchors));
        }
    }

    // Bite DJ: a player that asked for this analysis may have switched back to
    // rekordbox's grid while it ran ([ChannelN],analysis_source). That grid is
    // the one it wants now, and shouldAnalyze() would not have replaced it.
    const mixxx::BeatsPointer pCurrentBeats = pTrack->getBeats();
    if (!m_bPreferencesReanalyzeImported && pCurrentBeats &&
            pCurrentBeats->getSubVersion() == mixxx::rekordboxconstants::beatsSubversion) {
        qDebug() << "Beat analysis finished after a Rekordbox grid was put back. Discarded.";
        return;
    }
    if (pBeats && isProvisional(pCurrentBeats)) {
        // Replacing a provisional grid: keep it off the undo stack, Undo must
        // never step back into a half-analyzed grid.
        pTrack->trySetBeatsDiscardingUndo(pBeats);
        if (pCurrentBeats != m_pLastProvisionalBeats) {
            // ...but the DJ corrected it while the analysis ran, so their
            // grid stays, now as a finished one, and Undo restores the
            // analyzed grid just set.
            qDebug() << "Beat analysis finished after the provisional grid was edited."
                     << "Keeping the edit.";
            pTrack->trySetBeats(pCurrentBeats->withSubVersion(pBeats->getSubVersion()));
        }
        return;
    }
    pTrack->trySetBeats(pBeats);
}

std::vector<mixxx::audio::FramePos> AnalyzerBeats::detectDownbeatAnchors(
        const mixxx::BeatsPointer& pBeats, SINT trackFrames) const {
    std::vector<mixxx::audio::FramePos> anchors;
    if (!pBeats || m_bassEnergyBins.empty() || trackFrames <= 0) {
        return anchors;
    }

    // Build a list of (beat position, energy[beat..next beat]) for all beats
    // within the analyzed range. Energy = average of bass-energy bins
    // overlapping the beat interval.
    struct BeatEnergy {
        mixxx::audio::FramePos position;
        float energy;
    };
    std::vector<BeatEnergy> beatEnergies;

    const auto trackEnd = mixxx::audio::FramePos(static_cast<double>(trackFrames));
    auto it = pBeats->iteratorFrom(mixxx::audio::kStartFramePos);
    auto end = pBeats->iteratorFrom(trackEnd);

    const SINT totalBins = static_cast<SINT>(m_bassEnergyBins.size());
    for (; it != end; ++it) {
        const auto nextIt = std::next(it);
        if (nextIt == end) {
            break;
        }
        const double startFrame = (*it).value();
        const double endFrame = (*nextIt).value();
        if (startFrame < 0 || endFrame <= startFrame) {
            continue;
        }
        const SINT startBin = std::max<SINT>(
                0, static_cast<SINT>(startFrame / kBassBinFrames));
        const SINT endBin = std::min<SINT>(totalBins,
                static_cast<SINT>(endFrame / kBassBinFrames) + 1);
        if (endBin <= startBin) {
            continue;
        }
        double sum = 0.0;
        for (SINT b = startBin; b < endBin; b++) {
            sum += m_bassEnergyBins[b];
        }
        const float avg = static_cast<float>(sum / (endBin - startBin));
        beatEnergies.push_back({*it, avg});
    }

    if (beatEnergies.size() < 8) {
        return anchors; // not enough material to make a meaningful judgment
    }

    // Scan for qualifying drops: E[i+1] > floor, E[i+1] / max(E[i], eps) >= ratio.
    // Collect all qualifying boundaries, then coalesce nearby ones.
    struct DropCandidate {
        size_t beatIndex; // index into beatEnergies
        double jump;      // E[i+1] - E[i] (for ranking when coalescing)
    };
    std::vector<DropCandidate> candidates;

    const size_t minBeatIdx = static_cast<size_t>(
            beatEnergies.size() * kDropMinPositionFraction);
    const size_t maxBeatIdx = beatEnergies.size() > 4 ? beatEnergies.size() - 4 : 0;
    constexpr float kEps = 1e-12f;

    for (size_t i = minBeatIdx; i + 1 < maxBeatIdx; i++) {
        const float pre = beatEnergies[i].energy;
        const float post = beatEnergies[i + 1].energy;
        if (post < kDropAbsoluteFloor) {
            continue;
        }
        const float ratio = post / std::max(pre, kEps);
        if (ratio < kDropRatioThreshold) {
            continue;
        }
        candidates.push_back({i + 1, post - pre});
    }

    if (candidates.empty()) {
        return anchors;
    }

    // Coalesce: walk candidates left-to-right; if within kDropMinSeparationBeats
    // of the last accepted, replace if jump is larger, else skip.
    std::vector<DropCandidate> accepted;
    for (const auto& c : candidates) {
        if (!accepted.empty() &&
                static_cast<int>(c.beatIndex - accepted.back().beatIndex) <
                        kDropMinSeparationBeats) {
            if (c.jump > accepted.back().jump) {
                accepted.back() = c;
            }
        } else {
            accepted.push_back(c);
        }
    }

    // Only the single loudest jump becomes the anchor. Downbeats must land on
    // every 4th beat consistently across the *entire* track, and beat spacing
    // is uniform here (const-tempo only): if a track has multiple candidate
    // "drops" that aren't themselves exactly a multiple of 4 beats apart,
    // keeping more than one anchor would re-anchor the bar count mid-track and
    // visibly shift the downbeat by 1-3 beats the moment playback crosses into
    // the next anchor's region. A single global anchor makes that impossible.
    const auto strongest = std::max_element(accepted.cbegin(),
            accepted.cend(),
            [](const DropCandidate& lhs, const DropCandidate& rhs) {
                return lhs.jump < rhs.jump;
            });
    anchors.push_back(beatEnergies[strongest->beatIndex].position.toLowerFrameBoundary());
    return anchors;
}

// static
QHash<QString, QString> AnalyzerBeats::getExtraVersionInfo(
        const QString& pluginId, bool bPreferencesFastAnalysis) {
    QHash<QString, QString> extraVersionInfo;
    extraVersionInfo["vamp_plugin_id"] = pluginId;
    if (bPreferencesFastAnalysis) {
        extraVersionInfo["fast_analysis"] = "1";
    }
    return extraVersionInfo;
}
