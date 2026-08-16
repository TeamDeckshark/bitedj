#pragma once

#include <QAtomicPointer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <optional>

#include "controllers/controllermappinginfo.h"
#include "preferences/usersettings.h"

class Controller;
class ControllerManager;
class ControlObject;
class LegacyControllerMapping;

// Bite DJ: in-skin replacement for the per-device mapping picker in
// DlgPrefController. Owns the [Controllers],* COs that the MIDI settings
// sub-page binds to and a Qt signal carrying the per-controller display
// strings (CO transport carries doubles only, so list strings ride a signal
// alongside — same pattern as Notifications::messagePosted and
// AudioDeviceSettings::devicesChanged).
//
// Scope is deliberately narrow:
//  - Only MIDI controllers appear
//  - Each row collapses to a tap-to-toggle action: tap an enabled row to
//    disable it, tap a disabled row to enable it with the best-matching
//    mapping for that device (Controller::matchMapping first, then a
//    case-insensitive name-substring heuristic — see findBestMapping).
//    Cycling through every available preset, script editing, per-mapping
//    settings, and learning wizards are not exposed.
class ControllerSettings : public QObject {
    Q_OBJECT
  public:
    ControllerSettings(UserSettingsPointer pConfig,
            std::shared_ptr<ControllerManager> pManager);
    ~ControllerSettings() override;

    static ControllerSettings* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    QStringList rowLabels() const {
        return m_rowLabels;
    }

    // Per-row active flag — true when the controller currently has a
    // mapping applied (slotSetUpDevices opened it from config, or a tap
    // explicitly re-applied). Drives the WControllerList "active" Q_PROPERTY
    // so the active row paints with the highlight style.
    QList<bool> rowActiveStates() const;

    // Toggles the *staged* enabled state of controller[index]: an active row
    // stages a disable and an inactive row stages an enable with its
    // best-matching mapping. Nothing is applied to the device until the user
    // hits Apply ([Controllers],apply) — the row label and [Controllers],dirty
    // update immediately. Audio routing is deliberately NOT touched — outputs
    // are owned exclusively by the Audio settings tab (AudioDeviceSettings),
    // so enabling/disabling a controller never moves the Master/Booth/
    // Headphones assignment. Idempotent on out-of-range; safe to call from
    // the GUI thread.
    void toggleRow(int index);

  signals:
    // Per-controller pre-rendered labels of the form
    // "Device Name — Mapping Name" or "Device Name — Disabled", paired
    // with per-row active flags (true → row paints highlighted, drives
    // the [active="true"] QSS rule in WControllerList).
    void rowsChanged(const QStringList& rowLabels, const QList<bool>& rowActive);

    // Routed to ControllerManager::slotApplyMapping via QueuedConnection so
    // the GUI thread stays responsive while the controller thread runs
    // close + setMapping + open (which on a heavy mapping like the DDJ-FLX4
    // includes JS engine init and takes tens of milliseconds). Completion
    // is signalled back to onMappingApplied via ControllerManager's
    // mappingApplied signal, where we finish the apply (audio routing, row
    // label, busy-state clear).
    void applyMapping(Controller* pController,
            std::shared_ptr<LegacyControllerMapping> pMapping,
            bool bEnabled);

  private slots:
    void onDevicesChanged();
    void onRescanRequested(double value);
    void onApplyRequested(double value);
    void onRevertRequested(double value);
    void onMappingApplied(bool applied);
    void onRescanWatchdog();
    void onApplyWatchdog();
    void onInitialNavWatchdog();

  private:
    void refreshRows();
    // Recomputes [Controllers],dirty: 1 while any row's staged mapping index
    // differs from its applied one. The skin keys the Apply/Cancel vs Rescan
    // button swap off this CO.
    void updateDirty();
    // Pops rows off m_applyQueue until one successfully starts an
    // applyMapping (arming the apply watchdog) or the queue drains, in which
    // case the batch finishes: busy clears and dirty is recomputed.
    void startNextQueuedApply();
    QString formatRowLabel(int index) const;
    int findInitialMappingIndex(Controller* pController,
            const QList<MappingInfo>& mappings) const;
    // Silently load and apply a mapping flagged <hidden>true</hidden> in its
    // <info> block — bypasses the picker row, busy indicator, and
    // notification strip so virtual/internal devices self-configure without
    // surfacing in the UI. No-op if the controller already has the target
    // mapping loaded.
    void autoApplyHiddenMapping(Controller* pController,
            const MappingInfo& mappingInfo);
    // Switches the top-level Tab to Settings and the Settings sub-tab to
    // Devices on startup when EITHER no MIDI controller is detected OR no
    // audio output device is configured (no persisted output, or the
    // persisted device isn't currently attached).
    void navigateToDevicesIfUnconfigured();
    // Returns the index of the mapping that best matches `pController`, or
    // -1 if no candidate is found. Tries Controller::matchMapping first
    // (structured vendor/product-ID match per controller subclass) and then
    // falls back to a name-substring heuristic so e.g. an ALSA-reported
    // "DDJ-FLX4" device picks the "Pioneer DDJ-FLX4" mapping.
    int findBestMapping(Controller* pController,
            const QList<MappingInfo>& mappings) const;

    // Enable path of a queued apply: loads and applies the staged mapping
    // for controller[index], stashing the PendingApply and raising the busy
    // state. Returns true when an applyMapping was emitted (completion lands
    // in onMappingApplied), false when no usable mapping could be loaded (a
    // warning is published and the caller should move on). Does NOT gate on
    // in-flight operations — callers own that check.
    bool beginApply(int index);
    // Disable path of a queued apply: applies a null mapping for
    // controller[index] (ControllerManager closes the device and clears its
    // persisted enable flag), stashing a PendingApply with
    // nextMappingIndex = -1 and raising the busy state. Always succeeds
    // (returns true).
    bool beginDisable(int index);

    // Captured state for an in-flight applyMapping. The completion handler
    // (onMappingApplied) reads these to finish the row-label update without
    // indexing back into m_controllers / m_perControllerMappings, which could
    // have been rebuilt by an interleaved devicesChanged signal while we
    // waited for the controller thread.
    struct PendingApply {
        int rowIndex;
        int nextMappingIndex; // -1 = disabling
        QString controllerName;
        QString mappingDisplayName;
    };

    static QAtomicPointer<ControllerSettings> s_pInstance;

    UserSettingsPointer m_pConfig;
    std::shared_ptr<ControllerManager> m_pControllerManager;

    QList<Controller*> m_controllers;
    QList<QList<MappingInfo>> m_perControllerMappings;
    QList<int> m_currentMappingIndex; // -1 = Disabled
    // Staged per-row mapping index, edited by toggleRow and committed by
    // Apply. Reset to m_currentMappingIndex by refreshRows and revert.
    QList<int> m_pendingMappingIndex;
    // Rows still waiting for their turn inside an Apply batch. Applies run
    // one at a time (the single m_pendingApply slot is the completion
    // context); onMappingApplied starts the next.
    QList<int> m_applyQueue;

    QStringList m_rowLabels;

    std::unique_ptr<ControlObject> m_pCoCount;
    std::unique_ptr<ControlObject> m_pCoRescan;
    std::unique_ptr<ControlObject> m_pCoApply;
    std::unique_ptr<ControlObject> m_pCoRevert;
    std::unique_ptr<ControlObject> m_pCoDirty;

    // Pre-created so the skin parser's controlFromConfigKey() reuses these
    // instead of creating them itself. That lets us seed an initial value
    // before the skin's WidgetStack runs its first showEvent, so a startup
    // with no audio device configured lands directly on Settings → Devices
    // (no flash of Overview while the watchdog catches up later). Match
    // the skin's persist semantics: [Tab],current is non-persistent
    // (skin.xml), [SettingsTab],current is persistent (settings.xml,
    // persist="true").
    std::unique_ptr<ControlObject> m_pCoTabCurrent;
    std::unique_ptr<ControlObject> m_pCoSettingsTabCurrent;

    std::optional<PendingApply> m_pendingApply;
    bool m_pendingRescan;
    // Watchdog: ControllerManager::devicesChanged is the rescan completion
    // signal, but updateControllerList only emits it when the device-pointer
    // list actually changes. If nothing changed (e.g. PortMidi returns an
    // identical list), the signal would never arrive and the busy state
    // would stick. The watchdog forcibly clears the pending rescan after a
    // safe upper bound.
    QTimer m_rescanWatchdog;
    // Watchdog for an in-flight applyMapping: if the controller thread never
    // emits mappingApplied (device yanked mid-apply, thread wedged), the busy
    // state — which now also suppresses touch input app-wide — must not
    // stick. Abandons the batch and clears busy after a safe upper bound.
    QTimer m_applyWatchdog;
    // Fallback startup check for the case the sync ctor seed didn't catch:
    // audio output configured (built-in HDMI or saved device still attached)
    // but no MIDI controller plugged in. The sync seed only checks audio
    // because the controller list is populated asynchronously on the
    // controller thread, so we trust audio as a proxy at ctor time and let
    // this timer fire 1.5s later — long enough for slotInitialize to finish
    // (well under a second on a Pi) and short enough to feel responsive.
    // We use the same trigger if onDevicesChanged arrives first. If no MIDI
    // controllers were detected by then, snap the UI to Settings → Devices
    // so the user sees the rescan/diagnostic affordance rather than landing
    // on the (silent) play view.
    QTimer m_initialNavWatchdog;
    bool m_initialNavDone;
};
