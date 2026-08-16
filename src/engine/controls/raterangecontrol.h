#pragma once

#include <QList>
#include <QObject>
#include <memory>

#include "preferences/usersettings.h"

class ControlObject;
class ControlProxy;

/// Owns the global [Controls],rate_range_percent ControlObject and fans
/// changes out to every [ChannelN]/[SamplerN] rateRange CO. Persists the
/// percent through the legacy [Controls],RateRangePercent config key so the
/// stock DlgPrefDeck and this control stay in agreement across restarts.
class RateRangeControl : public QObject {
    Q_OBJECT
  public:
    explicit RateRangeControl(UserSettingsPointer pConfig);
    ~RateRangeControl() override;

  private slots:
    void slotPercentChanged(double percent);
    void slotNumDecksChanged(double newCount);
    void slotNumSamplersChanged(double newCount);

  private:
    void appendDeckProxies(int previousCount, int newCount);
    void appendSamplerProxies(int previousCount, int newCount);
    void applyPercent(int percent);

    UserSettingsPointer m_pConfig;
    std::unique_ptr<ControlObject> m_pCOPercent;
    std::unique_ptr<ControlProxy> m_pNumDecks;
    std::unique_ptr<ControlProxy> m_pNumSamplers;
    QList<ControlProxy*> m_rateRangeProxies;
    int m_numConfiguredDecks;
    int m_numConfiguredSamplers;
};
