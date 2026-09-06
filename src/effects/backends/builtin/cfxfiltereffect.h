#pragma once

#include "effects/backends/effectprocessor.h"
#include "util/class.h"
#include "util/samplebuffer.h"
#include "util/types.h"

class EngineFilterBiquad1Low;
class EngineFilterBiquad1High;

struct CFXFilterGroupState : public EffectState {
    explicit CFXFilterGroupState(const mixxx::EngineParameters& engineParameters);
    ~CFXFilterGroupState() noexcept override;

    mixxx::SampleBuffer m_buffer;
    EngineFilterBiquad1Low* m_pLowFilter;
    EngineFilterBiquad1High* m_pHighFilter;

    double m_loFreq;
    double m_q;
    double m_hiFreq;
};

class CFXFilterEffect : public EffectProcessorImpl<CFXFilterGroupState> {
  public:
    CFXFilterEffect() = default;
    ~CFXFilterEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            CFXFilterGroupState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pLPF;
    EngineEffectParameterPointer m_pQ;
    EngineEffectParameterPointer m_pHPF;

    DISALLOW_COPY_AND_ASSIGN(CFXFilterEffect);
};
