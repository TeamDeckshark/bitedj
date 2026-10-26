#pragma once

#include <QHash>
#include <QList>
#include <memory>
#include <vector>

#include "analyzer/analyzer.h"
#include "analyzer/plugins/analyzerplugin.h"
#include "audio/frame.h"
#include "preferences/beatdetectionsettings.h"
#include "preferences/usersettings.h"
#include "track/beats.h"

class AnalyzerBeats : public Analyzer {
  public:
    /// `provisionalBeats`: give the track a grid from the audio analyzed so
    /// far while the analysis runs (see publishProvisionalBeats()). Meant for
    /// a track loaded on a deck, not for batch analysis, where nobody watches
    /// the grid grow and the extra beat tracking would be wasted.
    explicit AnalyzerBeats(
            UserSettingsPointer pConfig,
            bool enforceBpmDetection = false,
            bool provisionalBeats = false);
    ~AnalyzerBeats() override = default;

    static QList<mixxx::AnalyzerPluginInfo> availablePlugins();
    static mixxx::AnalyzerPluginInfo defaultPlugin();

    bool initialize(const AnalyzerTrack& track,
            mixxx::audio::SampleRate sampleRate,
            SINT frameLength) override;
    bool processSamples(const CSAMPLE* pIn, SINT count) override;
    void storeResults(TrackPointer tio) override;
    void cleanup() override;

  private:
    bool shouldAnalyze(TrackPointer pTrack) const;

    /// The grid for `beats` tracked by the plugin, reconciled with the
    /// track's metadata BPM.
    mixxx::BeatsPointer makeBeats(const QVector<mixxx::audio::FramePos>& beats,
            const TrackPointer& pTrack,
            bool provisional) const;
    mixxx::BeatsPointer reconciledWithMetadataBpm(
            mixxx::BeatsPointer pBeats, const TrackPointer& pTrack) const;

    /// Puts a grid of the beats tracked so far on the track, tagged
    /// provisional so it is never mistaken for a finished analysis.
    void publishProvisionalBeats();

    /// True for a grid publishProvisionalBeats() made (or one the DJ edited
    /// from it).
    static bool isProvisional(const mixxx::BeatsPointer& pBeats);
    static QHash<QString, QString> getExtraVersionInfo(
            const QString& pluginId, bool bPreferencesFastAnalysis);

    // Process one frame of stereo samples through the bass-band detector and
    // accumulate squared low-band energy into m_bassEnergyBins.
    void accumulateBassEnergy(CSAMPLE left, CSAMPLE right);

    // After beat detection, walk beats and find frame positions where the
    // bass energy makes a large single-beat jump.
    std::vector<mixxx::audio::FramePos> detectDownbeatAnchors(
            const mixxx::BeatsPointer& pBeats,
            SINT trackFrames) const;

    BeatDetectionSettings m_bpmSettings;
    std::unique_ptr<mixxx::AnalyzerBeatsPlugin> m_pPlugin;
    const bool m_enforceBpmDetection;
    const bool m_provisionalBeats;
    QString m_pluginId;
    bool m_bPreferencesReanalyzeOldBpm;
    bool m_bPreferencesReanalyzeImported;
    bool m_bPreferencesFixedTempo;
    bool m_bPreferencesFastAnalysis;

    mixxx::audio::SampleRate m_sampleRate;
    SINT m_maxFramesToProcess;
    SINT m_currentFrame;
    // The track's BPM before this analysis set any grid (see makeBeats()).
    mixxx::Bpm m_metadataBpm;

    // Only held while provisional grids are published: the track to put them
    // on, the frame count at which the next one is due, and the last one put
    // there (to tell whether the DJ has edited it since).
    TrackPointer m_pProvisionalTrack;
    SINT m_nextProvisionalFrame;
    mixxx::BeatsPointer m_pLastProvisionalBeats;

    // One-pole IIR lowpass state for the bass-band envelope (one per channel).
    float m_bassFilterStateL;
    float m_bassFilterStateR;
    float m_bassFilterCoefficient;
    // Per-bin sum of squared low-band samples (mono mixdown of L+R).
    // Bin size is kBassBinFrames (defined in the .cpp).
    std::vector<float> m_bassEnergyBins;
    SINT m_currentBassBinFrame;
    double m_currentBassBinAccumulator;
};
