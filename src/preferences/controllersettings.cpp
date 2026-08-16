#include "preferences/controllersettings.h"

#include <QDir>
#include <QFileInfo>
#include <QLatin1String>
#include <QRegularExpression>
#include <QSet>
#include <QtGlobal>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "controllers/controller.h"
#include "controllers/controllermanager.h"
#include "controllers/controllermappinginfoenumerator.h"
#include "controllers/defs_controllers.h"
#include "controllers/legacycontrollermapping.h"
#include "controllers/legacycontrollermappingfilehandler.h"
#include "moc_controllersettings.cpp"
#include "notifications/notifications.h"
#include "preferences/audiodevicesettings.h"

namespace {
const QString kGroup = QStringLiteral("[Controllers]");

// Upper bound on how long we keep the busy state after a Rescan tap. The
// MIDI side completes by re-emitting devicesChanged once the controller
// thread finishes setUpDevices; if the device list hasn't actually changed,
// updateControllerList won't emit and the watchdog clears the busy state
// instead.
constexpr int kRescanWatchdogMs = 5000;

// Upper bound on how long we wait for the controller thread to acknowledge
// one applyMapping (mappingApplied signal). JS engine init for a heavy
// legacy mapping takes tens of milliseconds on a Pi; seconds means the
// device vanished or the thread wedged, and busy (which suppresses touch
// input app-wide) must not stick.
constexpr int kApplyWatchdogMs = 8000;

// One-shot delay before the startup nav-to-Devices check. Long enough for
// the controller thread to finish its initial slotInitialize and for the
// skin parser to construct [Tab],settings / [SettingsTab],midi (both run
// well under a second on a Pi); short enough that an empty-MIDI startup
// doesn't sit on the wrong page for noticeably long.
constexpr int kInitialNavWatchdogMs = 1500;

// Child indices in the skin's two WidgetStacks, used to navigate to
// Settings → Devices on an unconfigured startup.
//   skin.xml top level: overview=0, library=1, samplers=2, levels=3, settings=4
//   settings.xml:       general=0, midi=1, columns=2, system=3, audio=4
// Reordering either stack means moving these with it.
constexpr int kSettingsTabIndex = 4;
constexpr int kDevicesSubtabIndex = 1;

// PortMIDI on ALSA reports devices with a colon-prefixed port name and a
// trailing port-number suffix, e.g. "DDJ-FLX4:DDJ-FLX4 MIDI 1". Strip both
// so the substring heuristic below matches the human-readable mapping titles
// like "Pioneer DDJ-FLX4". The MIDI-suffix strip is word-bounded so a name
// of just "MIDI Express XT" wouldn't be reduced to empty (we fall back to
// the original name in that edge case).
QString shortDeviceName(QString name) {
    const int colon = name.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        name.truncate(colon);
    }
    static const QRegularExpression midiSuffix(
            QStringLiteral("\\s*\\bmidi\\b.*$"),
            QRegularExpression::CaseInsensitiveOption);
    QString stripped = name;
    stripped.remove(midiSuffix);
    stripped = stripped.trimmed();
    if (stripped.isEmpty()) {
        stripped = name.trimmed();
    }
    return stripped.toLower();
}
} // namespace

QAtomicPointer<ControllerSettings> ControllerSettings::s_pInstance = nullptr;

ControllerSettings::ControllerSettings(UserSettingsPointer pConfig,
        std::shared_ptr<ControllerManager> pManager)
        : m_pConfig(pConfig),
          m_pControllerManager(std::move(pManager)),
          m_pendingRescan(false),
          m_initialNavDone(false) {
    m_pCoCount = std::make_unique<ControlObject>(ConfigKey(kGroup, "count"));
    m_pCoCount->setReadOnly();
    m_pCoRescan = std::make_unique<ControlObject>(ConfigKey(kGroup, "rescan"));
    m_pCoApply = std::make_unique<ControlObject>(ConfigKey(kGroup, "apply"));
    m_pCoRevert = std::make_unique<ControlObject>(ConfigKey(kGroup, "revert"));
    m_pCoDirty = std::make_unique<ControlObject>(ConfigKey(kGroup, "dirty"));
    m_pCoDirty->setReadOnly();

    connect(m_pCoRescan.get(),
            &ControlObject::valueChanged,
            this,
            &ControllerSettings::onRescanRequested);
    connect(m_pCoApply.get(),
            &ControlObject::valueChanged,
            this,
            &ControllerSettings::onApplyRequested);
    connect(m_pCoRevert.get(),
            &ControlObject::valueChanged,
            this,
            &ControllerSettings::onRevertRequested);

    connect(m_pControllerManager.get(),
            &ControllerManager::devicesChanged,
            this,
            &ControllerSettings::onDevicesChanged);
    connect(m_pControllerManager.get(),
            &ControllerManager::mappingApplied,
            this,
            &ControllerSettings::onMappingApplied);

    // ControllerManager lives on its own thread (controllermanager.cpp:159);
    // route applyMapping via auto-connection so the GUI thread stays
    // responsive (cross-thread → queued). Completion arrives via the
    // mappingApplied signal above. We trade the stock dlgprefcontroller
    // BlockingQueuedConnection handshake for non-blocking UX with a busy
    // indicator (Notifications::setBusy), since this picker doesn't edit
    // the mapping copy after applying it.
    connect(this,
            &ControllerSettings::applyMapping,
            m_pControllerManager.get(),
            &ControllerManager::slotApplyMapping);

    m_rescanWatchdog.setSingleShot(true);
    m_rescanWatchdog.setInterval(kRescanWatchdogMs);
    connect(&m_rescanWatchdog,
            &QTimer::timeout,
            this,
            &ControllerSettings::onRescanWatchdog);

    m_applyWatchdog.setSingleShot(true);
    m_applyWatchdog.setInterval(kApplyWatchdogMs);
    connect(&m_applyWatchdog,
            &QTimer::timeout,
            this,
            &ControllerSettings::onApplyWatchdog);

    m_initialNavWatchdog.setSingleShot(true);
    m_initialNavWatchdog.setInterval(kInitialNavWatchdogMs);
    connect(&m_initialNavWatchdog,
            &QTimer::timeout,
            this,
            &ControllerSettings::onInitialNavWatchdog);
    m_initialNavWatchdog.start();

    // Pre-create the tab-position COs before the skin parses (we run during
    // CoreServices::initialize, the skin parses after that returns). The
    // skin's controlFromConfigKey() reuses any pre-existing CO, so seeding
    // [Tab],current and [SettingsTab],current here lets the
    // top-level WidgetStack's first showEvent paint Settings → Devices
    // directly, with no flash of Overview while the post-show watchdog
    // catches up.
    //
    // Persist semantics match the skin: skin.xml has no persist attribute
    // on its <WidgetStack currentpage="[Tab],current">, settings.xml has
    // persist="true" on the inner stack. The persistent CO loads its
    // initial value from mixxx.cfg in its ctor, which we then overwrite
    // below if we decide to navigate.
    m_pCoTabCurrent = std::make_unique<ControlObject>(
            ConfigKey(QStringLiteral("[Tab]"), QStringLiteral("current")));
    m_pCoSettingsTabCurrent = std::make_unique<ControlObject>(
            ConfigKey(QStringLiteral("[SettingsTab]"),
                    QStringLiteral("current")),
            /* bIgnoreNops */ true,
            /* bTrack */ false,
            /* bPersist */ true);

    // Synchronous startup nav decision. The audio-output device is the
    // reliable sync proxy for "is this rig usable" — AudioDeviceSettings
    // ran refreshDeviceList() in its ctor (constructed immediately before
    // us in CoreServices::initialize) so selectedIndex() already reflects
    // the persisted-and-currently-attached output. The controller side is
    // async (PortMidi enumeration runs on the controller thread) so we
    // don't gate the sync decision on it — the watchdog above still handles
    // the "audio configured but controller never showed up" case after
    AudioDeviceSettings* pAudio = AudioDeviceSettings::tryInstance();
    if (!pAudio || pAudio->selectedIndex() < 0) {
        m_pCoTabCurrent->set(kSettingsTabIndex);
        m_pCoSettingsTabCurrent->set(kDevicesSubtabIndex);
    }

    s_pInstance.storeRelease(this);
    refreshRows();
}

ControllerSettings::~ControllerSettings() {
    s_pInstance.storeRelease(nullptr);
}

void ControllerSettings::onDevicesChanged() {
    refreshRows();
    if (!m_initialNavDone) {
        m_initialNavDone = true;
        m_initialNavWatchdog.stop();
        navigateToDevicesIfUnconfigured();
    }
    if (m_pendingRescan) {
        m_pendingRescan = false;
        m_rescanWatchdog.stop();

        // A Rescan only re-enumerates the controller list and re-opens the
        // controllers whose [Controller],<name>=1 flag survived from a
        // previous session (slotSetUpDevices, on the controller thread). It
        // deliberately does NOT mass-enable every detected device: enabling
        // is a per-row, explicit action now (toggleRow). It also does not
        // touch audio routing — outputs are owned by the Audio settings tab.
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->setBusy(false);
            pNotifications->publish(tr("Devices refreshed"),
                    Notifications::Severity::Info);
        }
    }
}

void ControllerSettings::onRescanRequested(double value) {
    if (value < 0.5) {
        return;
    }
    // Drop the rescan if another operation is already in flight; the busy
    // indicator already tells the user something's happening.
    Notifications* pNotifications = Notifications::tryInstance();
    if (m_pendingRescan || m_pendingApply.has_value()) {
        m_pCoRescan->forceSet(0.0);
        return;
    }
    m_pendingRescan = true;
    if (pNotifications) {
        pNotifications->setBusy(true);
        pNotifications->publishSticky(tr("Refreshing devices..."),
                Notifications::Severity::Info);
    }
    m_rescanWatchdog.start();
    m_pCoRescan->forceSet(0.0);

    // Defer the synchronous part so the busy state and sticky "Refreshing
    // devices..." paint events flush first: a 0-ms singleShot returns control
    // to the event loop, paint runs, then the lambda fires. This rescan is
    // MIDI-only — the Audio settings tab has its own [AudioDevices],rescan
    // for re-enumerating output hardware, so we no longer close/reopen the
    // audio devices here (that was disruptive and coupled the two tabs).
    QTimer::singleShot(0, this, [this]() {
        m_pControllerManager->setUpDevices();
    });
}

void ControllerSettings::onRescanWatchdog() {
    if (!m_pendingRescan) {
        return;
    }
    m_pendingRescan = false;
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(false);
        pNotifications->clear();
    }
}

void ControllerSettings::onInitialNavWatchdog() {
    // Fired only if onDevicesChanged didn't already trip the nav check.
    // ControllerManager::updateControllerList only emits devicesChanged
    // when the device pointer list changes — an empty-on-startup system
    // never sees that signal, so this timer is the fallback path.
    if (m_initialNavDone) {
        return;
    }
    m_initialNavDone = true;
    navigateToDevicesIfUnconfigured();
}

void ControllerSettings::navigateToDevicesIfUnconfigured() {
    // Treat "no MIDI controller" and "no audio output device configured" as
    // the same precondition: the audio device and MIDI device are usually
    // the same physical USB controller, and either gap is fixed from the
    // same Devices page. AudioDeviceSettings is constructed before us in
    // CoreServices::initialize() and runs refreshDeviceList() in its
    // constructor, so its selected-index reflects the
    // persisted-and-currently-attached output by the time we run here.
    AudioDeviceSettings* pAudio = AudioDeviceSettings::tryInstance();
    const bool audioConfigured = pAudio && pAudio->selectedIndex() >= 0;
    if (!m_controllers.isEmpty() && audioConfigured) {
        return;
    }
    // Force-switch to Settings → Devices. Order matters: the inner
    // WWidgetStack's showEvent (fired when the Settings page first becomes
    // visible) unconditionally resets every sub-tab trigger CO based on the
    // persisted [SettingsTab],current value, so a [SettingsTab],midi=1 write
    // issued AFTER the top-level switch is undone before it can land. Seed
    // [SettingsTab],current first so showEvent reads the Devices index when
    // the stack is reparented through WSingletonContainer, then trip the
    // top-level trigger to actually navigate:
    //
    //  1. [SettingsTab],current — currentpage for the inner WidgetStack
    //     (kDevicesSubtabIndex). Read by the inner showEvent and persisted,
    //     so a subsequent restart-while-empty also lands here.
    //  2. [Tab],current — currentpage for the top-level WidgetStack
    //     (kSettingsTabIndex).
    //  3. [Tab],settings = 1 — the trigger CO that the top-level
    //     WidgetStack's WidgetStackControlListener watches. Same write the
    //     topbar's Settings button performs on tap. Written last so the
    //     show events that fire on Settings becoming visible see the
    //     already-seeded [SettingsTab],current.
    //
    // ControlObject::set is used (rather than ControlProxy) so the write
    // bypasses any per-proxy filtering and goes straight to the same path
    // the skin's WPushButton uses on tap.
    ControlObject::set(ConfigKey(QStringLiteral("[SettingsTab]"),
                              QStringLiteral("current")),
            kDevicesSubtabIndex);
    ControlObject::set(ConfigKey(QStringLiteral("[Tab]"),
                              QStringLiteral("current")),
            kSettingsTabIndex);
    ControlObject::set(ConfigKey(QStringLiteral("[Tab]"),
                              QStringLiteral("settings")),
            1.0);
}

void ControllerSettings::onMappingApplied(bool /*applied*/) {
    // mappingApplied fires for every applyMapping caller (including stock
    // DlgPrefController). Ignore unless we have a pending operation we
    // initiated.
    if (!m_pendingApply.has_value()) {
        return;
    }
    m_applyWatchdog.stop();
    const PendingApply apply = std::move(*m_pendingApply);
    m_pendingApply.reset();

    // Audio routing is intentionally NOT touched here. Enabling or disabling
    // a MIDI controller is independent of the Master/Booth/Headphones output
    // assignment, which the user owns from the Audio settings tab
    // (AudioDeviceSettings). This decoupling is what stops a second plugged-in
    // controller from stealing the audio route from the first.

    // After the queued slotApplyMapping ran on the controller thread, our
    // local state may have been rebuilt by an interleaved devicesChanged.
    // Re-validate before writing back; if rows were rebuilt, the new state
    // already reflects reality and we just skip the local update.
    if (apply.rowIndex >= 0 && apply.rowIndex < m_currentMappingIndex.size()) {
        m_currentMappingIndex[apply.rowIndex] = apply.nextMappingIndex;
        m_pendingMappingIndex[apply.rowIndex] = apply.nextMappingIndex;
    }
    const QString rowLabel = apply.nextMappingIndex >= 0
            ? tr("%1 — %2").arg(apply.controllerName, apply.mappingDisplayName)
            : tr("%1 — Disabled").arg(apply.controllerName);
    if (apply.rowIndex >= 0 && apply.rowIndex < m_rowLabels.size()) {
        m_rowLabels[apply.rowIndex] = rowLabel;
        emit rowsChanged(m_rowLabels, rowActiveStates());
    }

    if (!m_applyQueue.isEmpty()) {
        // More rows staged in this Apply batch: keep busy raised and start
        // the next one. Its beginApply/beginDisable publishes the fresh
        // sticky progress message.
        startNextQueuedApply();
        return;
    }
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(false);
        pNotifications->publish(rowLabel, Notifications::Severity::Info);
    }
    updateDirty();
}

void ControllerSettings::refreshRows() {
    m_controllers.clear();
    m_perControllerMappings.clear();
    m_currentMappingIndex.clear();
    m_rowLabels.clear();
    // Row indices are about to change; any staged-but-unapplied batch rows
    // would point at the wrong controllers. An apply already in flight
    // (m_pendingApply) still completes safely — onMappingApplied bounds-
    // checks its row and finds the queue empty, clearing busy.
    m_applyQueue.clear();

    auto pUserEnum = m_pControllerManager->getMainThreadUserMappingEnumerator();
    auto pSystemEnum = m_pControllerManager->getMainThreadSystemMappingEnumerator();

    // Dedupe defensively. ControllerManager::updateControllerList() replaces
    // m_controllers with fresh Controller* instances on every rescan (the
    // enumerator destroys old objects), but if a future enumerator change ever
    // double-registers the same physical device we don't want the picker to
    // grow each tap of Rescan.
    QSet<QString> seenNames;

    for (Controller* pController : m_pControllerManager->getControllers()) {
        // Filter: MIDI only. HID and Bulk devices have richer mapping models
        // (per-button settings, descriptor parsing) that this minimal picker
        // cannot express
        if (pController->mappingExtension() != QLatin1String(MIDI_MAPPING_EXTENSION)) {
            continue;
        }
        const QString& name = pController->getName();
        if (seenNames.contains(name)) {
            continue;
        }
        seenNames.insert(name);

        QList<MappingInfo> mappings;
        if (pUserEnum) {
            mappings.append(pUserEnum->getMappingsByExtension(
                    QLatin1String(MIDI_MAPPING_EXTENSION)));
        }
        if (pSystemEnum) {
            mappings.append(pSystemEnum->getMappingsByExtension(
                    QLatin1String(MIDI_MAPPING_EXTENSION)));
        }
        // Hide devices that can never be enabled — no candidate mapping
        // exists for them. Catches ALSA loopback ports like "MIDI Through
        // Port-0" (no mapping has that name in any spelling) as well as
        // any genuinely unrecognized hardware the user couldn't usefully
        // enable from this picker anyway.
        const int bestIndex = findBestMapping(pController, mappings);
        if (bestIndex < 0) {
            continue;
        }
        // <hidden>true</hidden> in the mapping's <info> block: skip the
        // picker row entirely and silently apply the mapping if it isn't
        // already loaded. Intended for virtual/internal devices that the
        // user has no useful toggle for.
        if (mappings.at(bestIndex).isHidden()) {
            if (findInitialMappingIndex(pController, mappings) != bestIndex) {
                autoApplyHiddenMapping(pController, mappings.at(bestIndex));
            }
            continue;
        }
        m_controllers.append(pController);
        m_currentMappingIndex.append(findInitialMappingIndex(pController, mappings));
        m_perControllerMappings.append(std::move(mappings));
    }

    // Staged state resets to reality on every rebuild (startup, rescan,
    // hot-plug) — un-applied toggles from before the rebuild are discarded.
    m_pendingMappingIndex = m_currentMappingIndex;

    for (int i = 0; i < m_controllers.size(); ++i) {
        m_rowLabels.append(formatRowLabel(i));
    }
    m_pCoCount->forceSet(static_cast<double>(m_controllers.size()));
    emit rowsChanged(m_rowLabels, rowActiveStates());
    updateDirty();
}

QList<bool> ControllerSettings::rowActiveStates() const {
    // Highlight follows the staged state so a toggled row previews what
    // Apply will do; dirty (Apply/Cancel showing) marks it as un-committed.
    QList<bool> active;
    active.reserve(m_pendingMappingIndex.size());
    for (int idx : m_pendingMappingIndex) {
        active.append(idx >= 0);
    }
    return active;
}

int ControllerSettings::findInitialMappingIndex(Controller* pController,
        const QList<MappingInfo>& mappings) const {
    // Prefer the mapping currently loaded on the controller (set up at
    // startup from the persisted config). Falls back to no-match → Disabled.
    auto pCurrentMapping = pController->getMapping();
    if (!pCurrentMapping) {
        return -1;
    }
    const QString currentPath = pCurrentMapping->filePath();
    for (int i = 0; i < mappings.size(); ++i) {
        if (mappings.at(i).getPath() == currentPath) {
            return i;
        }
    }
    return -1;
}

int ControllerSettings::findBestMapping(Controller* pController,
        const QList<MappingInfo>& mappings) const {
    // Pass 1: structured match via the Controller subclass. For MIDI this
    // typically falls through to a name compare against the mapping's
    // <controller id="..."> attribute, so a mapping that doesn't declare
    // matching info will silently miss here and we fall to pass 2.
    for (int i = 0; i < mappings.size(); ++i) {
        if (pController->matchMapping(mappings.at(i))) {
            return i;
        }
    }
    // Pass 2: case-insensitive substring match between the (stripped) device
    // name and the mapping's display name. Handles the common case where
    // ALSA reports "DDJ-FLX4" and the user expects the "Pioneer DDJ-FLX4"
    // mapping (manufacturer prefix not present in the device name).
    const QString needle = shortDeviceName(pController->getName());
    if (needle.isEmpty()) {
        return -1;
    }
    int bestIndex = -1;
    int bestNameLength = 0;
    for (int i = 0; i < mappings.size(); ++i) {
        const QString candidate = mappings.at(i).getName().toLower();
        if (!candidate.contains(needle)) {
            continue;
        }
        // Among matches, prefer the longest name (least likely to be a
        // family-wide generic — e.g. "Pioneer DDJ-FLX4" wins over "DDJ").
        if (bestIndex < 0 || candidate.length() > bestNameLength) {
            bestIndex = i;
            bestNameLength = candidate.length();
        }
    }
    return bestIndex;
}

QString ControllerSettings::formatRowLabel(int index) const {
    if (index < 0 || index >= m_controllers.size()) {
        return {};
    }
    const QString& name = m_controllers.at(index)->getName();
    // Labels render the staged state; it equals the applied state except
    // between a toggle and the Apply/Cancel that resolves it.
    const int staged = m_pendingMappingIndex.at(index);
    if (staged < 0) {
        return tr("%1 — Disabled").arg(name);
    }
    return tr("%1 — %2").arg(name,
            m_perControllerMappings.at(index).at(staged).getName());
}

void ControllerSettings::toggleRow(int index) {
    if (index < 0 || index >= m_controllers.size()) {
        return;
    }
    // Drop overlapping operations. Widgets should also be greyed out by the
    // busy state, but a stale queued click could still arrive here.
    if (m_pendingApply.has_value() || m_pendingRescan) {
        return;
    }
    // Staged only — nothing touches the device until Apply. A staged-active
    // row stages a disable; a staged-inactive row stages an enable with its
    // best-matching mapping. Neither path touches audio.
    if (m_pendingMappingIndex.at(index) >= 0) {
        m_pendingMappingIndex[index] = -1;
    } else {
        const int bestIndex = findBestMapping(
                m_controllers.at(index), m_perControllerMappings.at(index));
        if (bestIndex < 0) {
            if (auto* pNotifications = Notifications::tryInstance()) {
                pNotifications->publish(
                        tr("No mapping found for %1")
                                .arg(m_controllers.at(index)->getName()),
                        Notifications::Severity::Warning);
            }
            return;
        }
        m_pendingMappingIndex[index] = bestIndex;
    }
    m_rowLabels[index] = formatRowLabel(index);
    emit rowsChanged(m_rowLabels, rowActiveStates());
    updateDirty();
}

void ControllerSettings::updateDirty() {
    const bool dirty = m_pendingMappingIndex != m_currentMappingIndex;
    m_pCoDirty->forceSet(dirty ? 1.0 : 0.0);
}

void ControllerSettings::onApplyRequested(double value) {
    if (value < 0.5) {
        return;
    }
    m_pCoApply->forceSet(0.0);
    if (m_pendingApply.has_value() || m_pendingRescan) {
        return;
    }
    m_applyQueue.clear();
    for (int i = 0; i < m_pendingMappingIndex.size() &&
            i < m_currentMappingIndex.size();
            ++i) {
        if (m_pendingMappingIndex.at(i) != m_currentMappingIndex.at(i)) {
            m_applyQueue.append(i);
        }
    }
    if (m_applyQueue.isEmpty()) {
        updateDirty();
        return;
    }
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(true);
    }
    startNextQueuedApply();
}

void ControllerSettings::onRevertRequested(double value) {
    if (value < 0.5) {
        return;
    }
    m_pCoRevert->forceSet(0.0);
    if (m_pendingApply.has_value() || m_pendingRescan) {
        return;
    }
    m_pendingMappingIndex = m_currentMappingIndex;
    for (int i = 0; i < m_rowLabels.size(); ++i) {
        m_rowLabels[i] = formatRowLabel(i);
    }
    emit rowsChanged(m_rowLabels, rowActiveStates());
    updateDirty();
}

void ControllerSettings::startNextQueuedApply() {
    while (!m_applyQueue.isEmpty()) {
        const int row = m_applyQueue.takeFirst();
        if (row < 0 || row >= m_pendingMappingIndex.size()) {
            continue;
        }
        const bool started = m_pendingMappingIndex.at(row) >= 0
                ? beginApply(row)
                : beginDisable(row);
        if (started) {
            m_applyWatchdog.start();
            return;
        }
        // beginApply published its warning; fall through to the next row.
    }
    // Batch finished (or nothing could start). Rows whose apply failed keep
    // their staged value, so dirty recomputes truthfully and the user can
    // retry.
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(false);
    }
    updateDirty();
}

void ControllerSettings::onApplyWatchdog() {
    if (!m_pendingApply.has_value()) {
        return;
    }
    const QString controllerName = m_pendingApply->controllerName;
    m_pendingApply.reset();
    m_applyQueue.clear();
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(false);
        pNotifications->publish(
                tr("Timed out applying %1").arg(controllerName),
                Notifications::Severity::Warning);
    }
    updateDirty();
}

bool ControllerSettings::beginApply(int index) {
    if (index < 0 || index >= m_controllers.size()) {
        return false;
    }
    Controller* pController = m_controllers.at(index);
    const QString controllerName = pController->getName();

    // Loads and applies the mapping staged by toggleRow (which ran
    // findBestMapping at tap time; refreshRows resets the staged index
    // whenever the mapping lists rebuild, so it can't go stale). Audio
    // routing is not involved — that's the Audio settings tab's job.
    const QList<MappingInfo>& mappings = m_perControllerMappings.at(index);
    const int nextIndex = m_pendingMappingIndex.at(index);
    if (nextIndex < 0 || nextIndex >= mappings.size()) {
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(
                    tr("No mapping found for %1").arg(controllerName),
                    Notifications::Severity::Warning);
        }
        return false;
    }
    const MappingInfo& info = mappings.at(nextIndex);
    const QFileInfo fileInfo(info.getPath());
    QString mappingDisplayName = info.getName();
    std::shared_ptr<LegacyControllerMapping> pMapping =
            LegacyControllerMappingFileHandler::loadMapping(
                    fileInfo, QDir(resourceMappingsPath(m_pConfig)));
    if (!pMapping) {
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(
                    tr("Could not load mapping %1").arg(mappingDisplayName),
                    Notifications::Severity::Warning);
        }
        return false;
    }

    // Stash everything onMappingApplied will need; the controller thread
    // may take tens of milliseconds (JS engine init for legacy mappings)
    // and another rescan-driven refreshRows could rebuild our arrays in
    // the meantime.
    m_pendingApply = PendingApply{
            index,
            nextIndex,
            controllerName,
            mappingDisplayName,
    };

    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(true);
        pNotifications->publishSticky(
                tr("Applying %1...").arg(controllerName),
                Notifications::Severity::Info);
    }

    // Queued (auto-connection cross-thread); slotApplyMapping runs on the
    // controller thread and emits mappingApplied when done, which lands in
    // onMappingApplied (queued back to GUI thread) and finishes the work.
    emit applyMapping(pController, pMapping, true);
    return true;
}

bool ControllerSettings::beginDisable(int index) {
    if (index < 0 || index >= m_controllers.size()) {
        return false;
    }
    Controller* pController = m_controllers.at(index);
    const QString controllerName = pController->getName();

    // nextMappingIndex = -1 marks a disable; onMappingApplied writes the
    // "— Disabled" row label and clears the active state.
    m_pendingApply = PendingApply{
            index,
            -1,
            controllerName,
            QString(),
    };

    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(true);
        pNotifications->publishSticky(
                tr("Disabling %1...").arg(controllerName),
                Notifications::Severity::Info);
    }

    // A null mapping tells ControllerManager::slotApplyMapping to close the
    // device, unload its mapping, and clear the persisted [Controllers],<name>
    // key so it stays disabled across restarts. Completion lands in
    // onMappingApplied (mappingApplied(false)).
    emit applyMapping(pController, nullptr, false);
    return true;
}

void ControllerSettings::autoApplyHiddenMapping(Controller* pController,
        const MappingInfo& mappingInfo) {
    // Silent applyMapping for <hidden> mappings. Unlike toggleRow this path
    // deliberately does NOT populate m_pendingApply (so onMappingApplied's
    // early-return short-circuits the row-label/busy bookkeeping — none of
    // which is appropriate for an invisible device) and does NOT gate on or
    // raise the busy state (a slow controller-thread apply for a virtual port
    // shouldn't grey out the picker the user can actually use).
    const QFileInfo fileInfo(mappingInfo.getPath());
    std::shared_ptr<LegacyControllerMapping> pMapping =
            LegacyControllerMappingFileHandler::loadMapping(
                    fileInfo, QDir(resourceMappingsPath(m_pConfig)));
    if (!pMapping) {
        qWarning() << "ControllerSettings: failed to load hidden mapping"
                   << mappingInfo.getPath() << "for" << pController->getName();
        return;
    }
    emit applyMapping(pController, pMapping, true);
}
