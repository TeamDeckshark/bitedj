#include "engine/controls/raterangecontrol.h"

#include <QString>
#include <QtGlobal>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "mixer/playermanager.h"
#include "moc_raterangecontrol.cpp"

namespace {
const QString kAppGroup = QStringLiteral("[App]");
const QString kControlsGroup = QStringLiteral("[Controls]");
const QString kRateRangePercentLegacyKey = QStringLiteral("RateRangePercent");
const QString kRateRangePercentControlKey = QStringLiteral("rate_range_percent");
const QString kRateRangeKey = QStringLiteral("rateRange");
constexpr int kDefaultPercent = 8;
constexpr int kMinPercent = 1;
constexpr int kMaxPercent = 400;
} // namespace

RateRangeControl::RateRangeControl(UserSettingsPointer pConfig)
        : m_pConfig(pConfig),
          m_numConfiguredDecks(0),
          m_numConfiguredSamplers(0) {
    int percent = m_pConfig->getValue(
            ConfigKey(kControlsGroup, kRateRangePercentLegacyKey),
            kDefaultPercent);
    if (percent < kMinPercent || percent > kMaxPercent) {
        percent = kDefaultPercent;
    }

    m_pCOPercent = std::make_unique<ControlObject>(
            ConfigKey(kControlsGroup, kRateRangePercentControlKey));
    m_pCOPercent->set(percent);

    m_pNumDecks = std::make_unique<ControlProxy>(
            kAppGroup, QStringLiteral("num_decks"));
    m_pNumSamplers = std::make_unique<ControlProxy>(
            kAppGroup, QStringLiteral("num_samplers"));

    appendDeckProxies(0, static_cast<int>(m_pNumDecks->get()));
    appendSamplerProxies(0, static_cast<int>(m_pNumSamplers->get()));

    // Sync any decks/samplers that were created before this control existed
    // to the persisted percent.
    applyPercent(percent);

    connect(m_pCOPercent.get(),
            &ControlObject::valueChanged,
            this,
            &RateRangeControl::slotPercentChanged);
    m_pNumDecks->connectValueChanged(
            this, &RateRangeControl::slotNumDecksChanged);
    m_pNumSamplers->connectValueChanged(
            this, &RateRangeControl::slotNumSamplersChanged);
}

RateRangeControl::~RateRangeControl() = default;

void RateRangeControl::slotPercentChanged(double percent) {
    int clamped = qBound(kMinPercent, static_cast<int>(percent), kMaxPercent);
    applyPercent(clamped);
    m_pConfig->set(ConfigKey(kControlsGroup, kRateRangePercentLegacyKey),
            ConfigValue{clamped});
}

void RateRangeControl::slotNumDecksChanged(double newCount) {
    int n = static_cast<int>(newCount);
    if (n <= m_numConfiguredDecks) {
        return;
    }
    appendDeckProxies(m_numConfiguredDecks, n);
    applyPercent(static_cast<int>(m_pCOPercent->get()));
}

void RateRangeControl::slotNumSamplersChanged(double newCount) {
    int n = static_cast<int>(newCount);
    if (n <= m_numConfiguredSamplers) {
        return;
    }
    appendSamplerProxies(m_numConfiguredSamplers, n);
    applyPercent(static_cast<int>(m_pCOPercent->get()));
}

void RateRangeControl::appendDeckProxies(int previousCount, int newCount) {
    for (int i = previousCount; i < newCount; ++i) {
        m_rateRangeProxies.push_back(new ControlProxy(
                PlayerManager::groupForDeck(i), kRateRangeKey, this));
    }
    m_numConfiguredDecks = newCount;
}

void RateRangeControl::appendSamplerProxies(int previousCount, int newCount) {
    for (int i = previousCount; i < newCount; ++i) {
        m_rateRangeProxies.push_back(new ControlProxy(
                PlayerManager::groupForSampler(i), kRateRangeKey, this));
    }
    m_numConfiguredSamplers = newCount;
}

void RateRangeControl::applyPercent(int percent) {
    const double rateRange = percent / 100.0;
    for (ControlProxy* pProxy : std::as_const(m_rateRangeProxies)) {
        pProxy->set(rateRange);
    }
}
