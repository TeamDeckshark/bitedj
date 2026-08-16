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
    explicit AnalyzerBeats(
            UserSettingsPointer pConfig,
            bool enforceBpmDetection = false);
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
    QString m_pluginId;
    bool m_bPreferencesReanalyzeOldBpm;
    bool m_bPreferencesReanalyzeImported;
    bool m_bPreferencesFixedTempo;
    bool m_bPreferencesFastAnalysis;

    mixxx::audio::SampleRate m_sampleRate;
    SINT m_maxFramesToProcess;
    SINT m_currentFrame;

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
