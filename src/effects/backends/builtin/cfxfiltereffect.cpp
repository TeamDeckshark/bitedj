#include "effects/backends/builtin/cfxfiltereffect.h"

#include <cmath>

#include "effects/backends/effectmanifest.h"
#include "engine/effects/engineeffectparameter.h"
#include "engine/filters/enginefilterbiquad1.h"
#include "util/math.h"

namespace {
constexpr double kBypassLowCorner = 22050.0;
constexpr double kBypassHighCorner = 13.0;
constexpr double kDefaultQ = 2.0;
constexpr double kLowPassMinimum = 100.0;
constexpr double kLowPassMaximum = 12000.0;
constexpr double kLowPassCurveExponent = 2.75;
constexpr double kLowPassBypassPosition = 59.0 / 64.0;
constexpr double kHighPassMaximum = 7000.0;
constexpr double kHighPassCurveExponent = 3.0;

// A single Q=2 biquad with these curves fits two measured CFX Filter
// passes with median response errors of 1.28 dB (LPF) and 1.18 dB (HPF).
double lowPassCutoff(double position) {
    position = math_clamp(position, 0.0, 1.0);
    if (position >= kLowPassBypassPosition) {
        return kBypassLowCorner;
    }
    const double shapedPosition = std::pow(position, kLowPassCurveExponent);
    return math_min(kLowPassMinimum +
                    (kBypassLowCorner - kLowPassMinimum) * shapedPosition,
            kLowPassMaximum);
}

double highPassCutoff(double position) {
    position = math_clamp(position, 0.0, 1.0);
    const double shapedPosition = std::pow(position, kHighPassCurveExponent);
    return kBypassHighCorner +
            (kHighPassMaximum - kBypassHighCorner) * shapedPosition;
}
} // namespace

QString CFXFilterEffect::getId() {
    return QStringLiteral("us.bitedj.effects.cfxfilter");
}

EffectManifestPointer CFXFilterEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("CFX Filter"));
    pManifest->setShortName(QObject::tr("CFX Filter"));
    pManifest->setAuthor("The BiteDJ Team");
    pManifest->setVersion("1.0");
    pManifest->setDescription(
            QObject::tr("A resonant high-pass/low-pass filter modeled from rekordbox "
                        "CFX measurements."));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(0.5);

    EffectManifestParameterPointer lpf = pManifest->addParameter();
    lpf->setId("lpf");
    lpf->setName(QObject::tr("Low Pass Filter Position"));
    lpf->setShortName(QObject::tr("LPF"));
    lpf->setDescription(QObject::tr("Measured low-pass filter knob position"));
    lpf->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    lpf->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    lpf->setDefaultLinkType(EffectManifestParameter::LinkType::LinkedLeft);
    lpf->setNeutralPointOnScale(1.0);
    lpf->setRange(0.0, 1.0, 1.0);

    EffectManifestParameterPointer q = pManifest->addParameter();
    q->setId("q");
    q->setName(QObject::tr("Resonance"));
    q->setShortName(QObject::tr("Q"));
    q->setDescription(QObject::tr("Resonance at the filter cutoff"));
    q->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    q->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    q->setRange(0.707106781, kDefaultQ, 3.0);

    EffectManifestParameterPointer hpf = pManifest->addParameter();
    hpf->setId("hpf");
    hpf->setName(QObject::tr("High Pass Filter Position"));
    hpf->setShortName(QObject::tr("HPF"));
    hpf->setDescription(QObject::tr("Measured high-pass filter knob position"));
    hpf->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    hpf->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    hpf->setDefaultLinkType(EffectManifestParameter::LinkType::LinkedRight);
    hpf->setNeutralPointOnScale(0.0);
    hpf->setRange(0.0, 0.0, 1.0);

    return pManifest;
}

CFXFilterGroupState::CFXFilterGroupState(
        const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          m_buffer(engineParameters.samplesPerBuffer()),
          m_pLowFilter(new EngineFilterBiquad1Low(
                  engineParameters.sampleRate(), kBypassLowCorner, kDefaultQ, true)),
          m_pHighFilter(new EngineFilterBiquad1High(
                  engineParameters.sampleRate(), kBypassHighCorner, kDefaultQ, true)),
          m_loFreq(kBypassLowCorner),
          m_q(kDefaultQ),
          m_hiFreq(kBypassHighCorner) {
}

CFXFilterGroupState::~CFXFilterGroupState() noexcept {
    delete m_pLowFilter;
    delete m_pHighFilter;
}

void CFXFilterEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pLPF = parameters.value("lpf");
    m_pQ = parameters.value("q");
    m_pHPF = parameters.value("hpf");
}

void CFXFilterEffect::processChannel(
        CFXFilterGroupState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    Q_UNUSED(groupFeatures);

    const double lpfPosition =
            enableState == EffectEnableState::Disabling ? 1.0 : m_pLPF->value();
    const double hpfPosition =
            enableState == EffectEnableState::Disabling ? 0.0 : m_pHPF->value();
    const double q = m_pQ->value();
    const double lpf = lowPassCutoff(lpfPosition);
    const double hpf = highPassCutoff(hpfPosition);

    if (pState->m_loFreq != lpf || pState->m_q != q) {
        pState->m_pLowFilter->setFrequencyCorners(engineParameters.sampleRate(),
                lpf,
                q);
    }
    if (pState->m_hiFreq != hpf || pState->m_q != q) {
        pState->m_pHighFilter->setFrequencyCorners(engineParameters.sampleRate(),
                hpf,
                q);
    }

    const bool lpfEnabled = lpfPosition < kLowPassBypassPosition;
    const bool hpfEnabled = hpfPosition > 0.0;
    const CSAMPLE* pLpfInput = pState->m_buffer.data();
    CSAMPLE* pHpfOutput = pState->m_buffer.data();
    if (!lpfEnabled && pState->m_loFreq >= kBypassLowCorner) {
        pHpfOutput = pOutput;
        pLpfInput = pHpfOutput;
    }

    if (hpfEnabled) {
        pState->m_pHighFilter->process(
                pInput, pHpfOutput, engineParameters.samplesPerBuffer());
    } else if (pState->m_hiFreq > kBypassHighCorner) {
        pState->m_pHighFilter->processAndPauseFilter(
                pInput, pHpfOutput, engineParameters.samplesPerBuffer());
    } else {
        pLpfInput = pInput;
    }

    if (lpfEnabled) {
        pState->m_pLowFilter->process(
                pLpfInput, pOutput, engineParameters.samplesPerBuffer());
    } else if (pState->m_loFreq < kBypassLowCorner) {
        pState->m_pLowFilter->processAndPauseFilter(
                pLpfInput, pOutput, engineParameters.samplesPerBuffer());
    } else if (pLpfInput == pInput && pOutput != pInput) {
        SampleUtil::copy(pOutput, pInput, engineParameters.samplesPerBuffer());
    }

    pState->m_loFreq = lpf;
    pState->m_q = q;
    pState->m_hiFreq = hpf;
}
