#pragma once

#include <QObject>
#include <QTimer>

#ifdef __BROADCAST__
#include "preferences/broadcastsettings.h"
#endif
#include "preferences/usersettings.h"

class SettingsManager : public QObject {
    Q_OBJECT
  public:
    explicit SettingsManager(const QString& settingsPath);
    ~SettingsManager() override;

    UserSettingsPointer settings() const {
        return m_pSettings;
    }

#ifdef __BROADCAST__
    BroadcastSettingsPointer broadcastSettings() const {
        return m_pBroadcastSettings;
    }
#endif

    void save() {
        m_pSettings->save();
    }

    bool shouldRescanLibrary() {
        return m_bShouldRescanLibrary;
    }

  private slots:
    void slotSettingsDirty();

  private:
    UserSettingsPointer m_pSettings;
    // Bite DJ: flush dirty settings to disk shortly after every change.
    // The appliance is hard-powered-off, so the stock save-on-clean-exit
    // never runs; single-shot (not restarted while pending) so a burst of
    // changes coalesces into one write with bounded latency.
    QTimer m_autoSaveTimer;
    bool m_bShouldRescanLibrary;
#ifdef __BROADCAST__
    BroadcastSettingsPointer m_pBroadcastSettings;
#endif
};
