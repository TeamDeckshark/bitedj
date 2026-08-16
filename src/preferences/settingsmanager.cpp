#include "preferences/settingsmanager.h"

#include <QDir>

#include "control/control.h"
#include "moc_settingsmanager.cpp"
#include "preferences/upgrade.h"
#include "util/assert.h"

namespace {
constexpr int kAutoSaveDelayMs = 1000;
} // namespace

SettingsManager::SettingsManager(const QString& settingsPath)
        : m_bShouldRescanLibrary(false) {
    // First make sure the settings path exists. If we don't then other parts of
    // Mixxx (such as the library) will produce confusing errors.
    if (!QDir(settingsPath).exists()) {
        QDir().mkpath(settingsPath);
    }

    // Check to see if this is the first time this version of Mixxx is run
    // after an upgrade and make any needed changes.
    Upgrade upgrader;
    m_pSettings = upgrader.versionUpgrade(settingsPath);
    VERIFY_OR_DEBUG_ASSERT(!m_pSettings.isNull()) {
        m_pSettings = UserSettingsPointer(new UserSettings(""));
    }
    m_bShouldRescanLibrary = upgrader.rescanLibrary();

    ControlDoublePrivate::setUserConfig(m_pSettings);

    m_autoSaveTimer.setSingleShot(true);
    m_autoSaveTimer.setInterval(kAutoSaveDelayMs);
    connect(&m_autoSaveTimer,
            &QTimer::timeout,
            this,
            [this] {
                save();
            });
    // The dirty callback fires from whichever thread changed the config
    // (GUI, controller, ...); hop to this object's thread before touching
    // the timer.
    m_pSettings->setDirtyCallback([this] {
        QMetaObject::invokeMethod(
                this,
                [this] {
                    slotSettingsDirty();
                },
                Qt::QueuedConnection);
    });

#ifdef __BROADCAST__
    m_pBroadcastSettings = BroadcastSettingsPointer(
                               new BroadcastSettings(m_pSettings));
#endif
}

SettingsManager::~SettingsManager() {
    // The callback captures this; drop it before we go away. Takes the
    // config's write lock, so no thread is mid-invocation afterwards.
    m_pSettings->setDirtyCallback(nullptr);
    ControlDoublePrivate::setUserConfig(UserSettingsPointer());
}

void SettingsManager::slotSettingsDirty() {
    if (!m_autoSaveTimer.isActive()) {
        m_autoSaveTimer.start();
    }
}
