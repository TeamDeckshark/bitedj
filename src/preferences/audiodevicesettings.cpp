#include "preferences/audiodevicesettings.h"

#include <QSet>
#include <QTimer>
#include <QtGlobal>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "mixer/playermanager.h"
#include "moc_audiodevicesettings.cpp"
#include "notifications/notifications.h"
#include "soundio/sounddevice.h"
#include "soundio/soundmanager.h"
#include "soundio/soundmanagerconfig.h"

namespace {
const QString kGroup = QStringLiteral("[AudioDevices]");
const QString kAppGroup = QStringLiteral("[App]");
const QString kSoundManagerGroup = QStringLiteral("[SoundManager]");
constexpr int kFallbackSampleRate = 48000;

// The two latency/quality preset points. Both resolve to a 64-frame base
// period (see AudioDeviceSettings::LatencyMode), so the buffer-size index is
// what actually picks the frame count:
//   Latency: 44100 Hz, index 3 ->  256 frames -> 5.8 ms
//   Quality: 48000 Hz, index 5 -> 1024 frames -> 21.3 ms
constexpr int kLatencySampleRate = 44100;
constexpr int kQualitySampleRate = 48000;
constexpr unsigned int kLatencyBufferSizeIndex = static_cast<unsigned int>(
        SoundManagerConfig::AudioBufferSizeIndex::Size5xms);
constexpr unsigned int kQualityBufferSizeIndex = static_cast<unsigned int>(
        SoundManagerConfig::AudioBufferSizeIndex::Size20xms);
constexpr int kApplyPaintDelayMs = 300;
// How often to re-enumerate while waiting for a lost output device to come
// back. Re-enumeration means Pa_Terminate() + Pa_Initialize() plus a full ALSA
// device walk, which is slow on a Pi, so this is deliberately unhurried: the
// device is physically absent and nothing we do speeds up its return.
constexpr int kRecoveryIntervalMs = 3000;
} // namespace

QAtomicPointer<AudioDeviceSettings> AudioDeviceSettings::s_pInstance = nullptr;

AudioDeviceSettings::AudioDeviceSettings(UserSettingsPointer pConfig,
        std::shared_ptr<SoundManager> pSoundManager)
        : m_pConfig(pConfig),
          m_pSoundManager(std::move(pSoundManager)),
          m_liveSampleRate(kFallbackSampleRate),
          m_bufferSizeIndex(kQualityBufferSizeIndex),
          m_liveBufferSizeIndex(kQualityBufferSizeIndex),
          m_applyPending(false),
          m_pNumDecks(nullptr),
          m_pSoundStatus(nullptr),
          m_recovering(false) {
    for (int b = 0; b < BusCount; ++b) {
        m_busDeviceIndex[b] = -1;
        m_busChannelBase[b] = 0;
        m_liveBusDeviceIndex[b] = -1;
        m_liveBusChannelBase[b] = 0;
    }

    m_pCoCount = std::make_unique<ControlObject>(ConfigKey(kGroup, "count"));
    m_pCoCount->setReadOnly();
    m_pCoSelectedIndex = std::make_unique<ControlObject>(
            ConfigKey(kGroup, "selected_index"));
    m_pCoSelectedIndex->setReadOnly();
    m_pCoConfigured = std::make_unique<ControlObject>(
            ConfigKey(kGroup, "configured"));
    m_pCoConfigured->setReadOnly();

    m_pCoSampleRate = std::make_unique<ControlObject>(
            ConfigKey(kGroup, "sample_rate"));
    const auto currentRate = m_pSoundManager->getConfig().getSampleRate();
    m_pCoSampleRate->forceSet(static_cast<double>(
            currentRate.isValid() ? currentRate.value() : kFallbackSampleRate));

    // Latency-vs-quality preset. Writable (the segmented buttons poke it);
    // refreshDeviceList/updateLatencyMode keep it in sync with the staged
    // rate + buffer-size pair, so it is derived state everywhere else.
    m_bufferSizeIndex = m_pSoundManager->getConfig().getAudioBufferSizeIndex();
    m_liveBufferSizeIndex = m_bufferSizeIndex;
    m_pCoLatencyMode = std::make_unique<ControlObject>(
            ConfigKey(kGroup, "latency_mode"));
    m_pCoLatencyMode->forceSet(static_cast<double>(modeForConfig(
            static_cast<int>(m_pCoSampleRate->get()), m_bufferSizeIndex)));

    m_pCoRescan = std::make_unique<ControlObject>(ConfigKey(kGroup, "rescan"));
    m_pCoApply = std::make_unique<ControlObject>(ConfigKey(kGroup, "apply"));
    m_pCoRevert = std::make_unique<ControlObject>(ConfigKey(kGroup, "revert"));
    m_pCoDirty = std::make_unique<ControlObject>(ConfigKey(kGroup, "dirty"));
    m_pCoDirty->setReadOnly();
    m_pCoReconfigureEnabled = std::make_unique<ControlObject>(
            ConfigKey(kGroup, "reconfigure_enabled"));
    m_pCoReconfigureEnabled->setReadOnly();

    // Watch the decks so Apply and Rescan can grey out while music is playing.
    // Decks are created before this object (CoreServices adds them well before
    // the settings objects), so the proxies bind on the first pass; the
    // num_decks subscription only covers a deck being added later.
    m_pNumDecks = new ControlProxy(kAppGroup, QStringLiteral("num_decks"), this);
    m_pNumDecks->connectValueChanged(this, &AudioDeviceSettings::onDeckCountChanged);

    // Watch the sound status too, because a deck's play CO on its own is not
    // trustworthy: it is EngineBuffer::process() — i.e. the audio callback —
    // that clears play at the end of a track or when playback stops, so if the
    // output device dies mid-track (USB controller re-enumerating, its ALSA
    // card gone) play stays latched at 1 with nothing left to clear it. That
    // used to leave anyDeckPlaying() permanently true, greying out Apply *and*
    // Rescan and rejecting a direct [AudioDevices],rescan poke with "Stop
    // playback before rescanning" — locking the user out of the one in-app
    // path that recovers the audio device. See anyDeckPlaying().
    m_pSoundStatus = new ControlProxy(
            kSoundManagerGroup, QStringLiteral("status"), this);
    m_pSoundStatus->connectValueChanged(
            this, &AudioDeviceSettings::updateReconfigureEnabled);

    rebuildDeckPlayProxies();

    connect(m_pCoApply.get(),
            &ControlObject::valueChanged,
            this,
            &AudioDeviceSettings::onApplyRequested);
    connect(m_pCoRevert.get(),
            &ControlObject::valueChanged,
            this,
            &AudioDeviceSettings::onRevertRequested);

    connect(m_pCoSampleRate.get(),
            &ControlObject::valueChanged,
            this,
            &AudioDeviceSettings::onSampleRateChanged);
    connect(m_pCoLatencyMode.get(),
            &ControlObject::valueChanged,
            this,
            &AudioDeviceSettings::onLatencyModeChanged);
    connect(m_pCoRescan.get(),
            &ControlObject::valueChanged,
            this,
            &AudioDeviceSettings::onRescanRequested);

    m_recoveryTimer.setInterval(kRecoveryIntervalMs);
    m_recoveryTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_recoveryTimer,
            &QTimer::timeout,
            this,
            &AudioDeviceSettings::attemptRecovery);
    connect(m_pSoundManager.get(),
            &SoundManager::outputDeviceLost,
            this,
            &AudioDeviceSettings::onOutputDeviceLost);

    connect(m_pSoundManager.get(),
            &SoundManager::devicesUpdated,
            this,
            &AudioDeviceSettings::onSoundManagerDevicesUpdated);
    connect(m_pSoundManager.get(),
            &SoundManager::devicesSetup,
            this,
            &AudioDeviceSettings::onSoundManagerDevicesUpdated);

    s_pInstance.storeRelease(this);
    refreshDeviceList();
}

AudioDeviceSettings::~AudioDeviceSettings() {
    s_pInstance.storeRelease(nullptr);
}

// static
AudioPathType AudioDeviceSettings::typeForBus(int busIndex) {
    switch (busIndex) {
    case BusBooth:
        return AudioPathType::Booth;
    case BusHeadphones:
        return AudioPathType::Headphones;
    case BusMaster:
    default:
        return AudioPathType::Main;
    }
}

// static
QString AudioDeviceSettings::nameForBus(int busIndex) {
    switch (busIndex) {
    case BusBooth:
        return tr("Booth");
    case BusHeadphones:
        return tr("Headphones");
    case BusMaster:
    default:
        return tr("Master");
    }
}

QList<bool> AudioDeviceSettings::busAssigned() const {
    QList<bool> assigned;
    assigned.reserve(BusCount);
    for (int b = 0; b < BusCount; ++b) {
        assigned.append(m_busDeviceIndex[b] >= 0);
    }
    return assigned;
}

QList<AudioDeviceSettings::Option> AudioDeviceSettings::buildOptions(
        int busIndex) const {
    QList<Option> options;
    options.append(Option{-1, 0}); // None is always first
    for (int d = 0; d < m_deviceIds.size(); ++d) {
        const int channels = m_deviceOutputChannels.at(d);
        for (int base = 0; base + 2 <= channels; base += 2) {
            // Skip pairs another bus already holds (pending state counts:
            // m_busDeviceIndex reflects un-applied cycling too) so the user
            // can't route two buses onto the same physical output.
            bool takenByOtherBus = false;
            for (int b = 0; b < BusCount; ++b) {
                if (b != busIndex && m_busDeviceIndex[b] == d &&
                        m_busChannelBase[b] == base) {
                    takenByOtherBus = true;
                    break;
                }
            }
            if (!takenByOtherBus) {
                options.append(Option{d, base});
            }
        }
    }
    return options;
}

int AudioDeviceSettings::optionIndexForAssignment(
        int busIndex, int deviceIndex, int channelBase) const {
    if (deviceIndex < 0) {
        return 0; // None
    }
    const QList<Option> options = buildOptions(busIndex);
    for (int i = 0; i < options.size(); ++i) {
        if (options.at(i).deviceIndex == deviceIndex &&
                options.at(i).channelBase == channelBase) {
            return i;
        }
    }
    return 0;
}

void AudioDeviceSettings::cycleBus(int busIndex) {
    if (busIndex < 0 || busIndex >= BusCount) {
        return;
    }
    const QList<Option> options = buildOptions(busIndex);
    if (options.isEmpty()) {
        return;
    }
    const int current = optionIndexForAssignment(busIndex,
            m_busDeviceIndex[busIndex], m_busChannelBase[busIndex]);
    const Option& next = options.at((current + 1) % options.size());
    m_busDeviceIndex[busIndex] = next.deviceIndex;
    m_busChannelBase[busIndex] = next.channelBase;

    // Staged only: the row label updates immediately, [AudioDevices],dirty
    // swaps the skin's Rescan button for Apply/Cancel, and nothing touches
    // the hardware until Apply. SoundManager::setConfig() closes and re-opens
    // every device and blocks the GUI thread for several seconds (closeDevices
    // sleeps kSleepSecondsAfterClosingDevice on Linux) — never pay that per
    // tap.
    rebuildBusLabels();
    emit busesChanged(m_busLabels, busAssigned());
    updateDirty();
}

void AudioDeviceSettings::onDeckCountChanged(double /*numDecks*/) {
    rebuildDeckPlayProxies();
}

void AudioDeviceSettings::rebuildDeckPlayProxies() {
    qDeleteAll(m_deckPlayProxies);
    m_deckPlayProxies.clear();

    const int numDecks = static_cast<int>(m_pNumDecks->get());
    for (int i = 0; i < numDecks; ++i) {
        auto* pPlay = new ControlProxy(PlayerManager::groupForDeck(i),
                QStringLiteral("play"),
                this);
        pPlay->connectValueChanged(this, &AudioDeviceSettings::updateReconfigureEnabled);
        m_deckPlayProxies.append(pPlay);
    }
    updateReconfigureEnabled();
}

bool AudioDeviceSettings::anyDeckPlaying() const {
    // No connected output device means nothing can be audible, whatever the
    // play COs say — and in exactly that state they cannot be trusted, since
    // the engine that would clear them is not running (see the m_pSoundStatus
    // subscription in the ctor). Answering false here is both honest and what
    // keeps Rescan reachable after the sound device drops out: SoundManager
    // sets [SoundManager],status to SOUNDMANAGER_DISCONNECTED from
    // closeDevices(), which every failed re-open path also runs.
    if (static_cast<int>(m_pSoundStatus->get()) != SOUNDMANAGER_CONNECTED) {
        return false;
    }
    for (const ControlProxy* pPlay : std::as_const(m_deckPlayProxies)) {
        if (pPlay->toBool()) {
            return true;
        }
    }
    return false;
}

void AudioDeviceSettings::updateReconfigureEnabled() {
    m_pCoReconfigureEnabled->forceSet(anyDeckPlaying() ? 0.0 : 1.0);
}

void AudioDeviceSettings::onApplyRequested(double value) {
    // Push-button semantics: act on the rising edge only (the skin button
    // writes 1 on press, 0 on release).
    if (value < 0.5) {
        return;
    }
    m_pCoApply->forceSet(0.0);
    if (m_applyPending || !m_pCoDirty->toBool()) {
        return;
    }
    // Applying closes and re-opens every device, which silences a running
    // deck for seconds. The skin's Apply button is already disabled while a
    // deck plays ([AudioDevices],reconfigure_enabled), so this only catches
    // requests that bypass it (controller mapping, or a deck started between
    // the tap and this slot). The staged assignment is kept — the user can
    // stop the deck and tap Apply again.
    if (anyDeckPlaying()) {
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(
                    tr("Stop playback before applying audio settings"),
                    Notifications::Severity::Warning);
        }
        return;
    }
    // Raise the busy state (grey rows, suppress input) and the sticky
    // message first, then give the strip a few frames to paint before
    // setConfig() blocks the GUI thread.
    m_applyPending = true;
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->publishSticky(tr("Applying audio settings..."),
                Notifications::Severity::Info);
        pNotifications->setBusy(true);
    }
    QTimer::singleShot(kApplyPaintDelayMs, this, [this]() {
        applyBusConfig();
        m_applyPending = false;
    });
}

void AudioDeviceSettings::onRevertRequested(double value) {
    if (value < 0.5) {
        return;
    }
    m_pCoRevert->forceSet(0.0);
    if (m_applyPending) {
        return;
    }
    // Discard the staged assignments by re-reading the live config; this
    // also resets the sample-rate CO and clears the dirty flag.
    refreshDeviceList();
}

void AudioDeviceSettings::onSampleRateChanged(double /*rate*/) {
    // The rate can also be poked directly (controller mapping, script), which
    // may land on or off a preset point, so re-derive the mode either way.
    updateLatencyMode();
    updateDirty();
}

// static
int AudioDeviceSettings::sampleRateForMode(int mode) {
    return mode == ModeLatency ? kLatencySampleRate : kQualitySampleRate;
}

// static
unsigned int AudioDeviceSettings::bufferSizeIndexForMode(int mode) {
    return mode == ModeLatency ? kLatencyBufferSizeIndex : kQualityBufferSizeIndex;
}

// static
int AudioDeviceSettings::modeForConfig(
        int sampleRate, unsigned int bufferSizeIndex) {
    if (sampleRate == kLatencySampleRate &&
            bufferSizeIndex == kLatencyBufferSizeIndex) {
        return ModeLatency;
    }
    if (sampleRate == kQualitySampleRate &&
            bufferSizeIndex == kQualityBufferSizeIndex) {
        return ModeQuality;
    }
    return ModeCustom;
}

void AudioDeviceSettings::updateLatencyMode() {
    const int mode = modeForConfig(
            static_cast<int>(m_pCoSampleRate->get()), m_bufferSizeIndex);
    if (static_cast<int>(m_pCoLatencyMode->get()) != mode) {
        m_pCoLatencyMode->forceSet(static_cast<double>(mode));
    }
}

void AudioDeviceSettings::onLatencyModeChanged(double mode) {
    const int requested = static_cast<int>(mode);
    // ModeCustom is a readout, not a target: it exists so a soundconfig.xml
    // holding some other rate/index pair leaves both segments unlit. Tapping
    // it is impossible from the skin; ignore it if poked.
    if (requested != ModeQuality && requested != ModeLatency) {
        return;
    }
    const int rate = sampleRateForMode(requested);
    const unsigned int bufferSizeIndex = bufferSizeIndexForMode(requested);
    if (static_cast<int>(m_pCoSampleRate->get()) == rate &&
            m_bufferSizeIndex == bufferSizeIndex) {
        // Already there — this is the echo from updateLatencyMode's forceSet,
        // or a re-tap of the lit segment. Bail before recursing back through
        // onSampleRateChanged.
        return;
    }
    // Staged only, like bus cycling: the segment lights and dirty flips on,
    // but nothing reaches the hardware until Apply. Changing either half
    // requires closing and re-opening every device, which would cut a running
    // deck — the same reason Apply is gated on reconfigure_enabled.
    m_bufferSizeIndex = bufferSizeIndex;
    m_pCoSampleRate->forceSet(static_cast<double>(rate));
    updateDirty();
}

void AudioDeviceSettings::updateDirty() {
    bool dirty = false;
    for (int b = 0; b < BusCount; ++b) {
        if (m_busDeviceIndex[b] != m_liveBusDeviceIndex[b] ||
                m_busChannelBase[b] != m_liveBusChannelBase[b]) {
            dirty = true;
            break;
        }
    }
    if (static_cast<int>(m_pCoSampleRate->get()) != m_liveSampleRate ||
            m_bufferSizeIndex != m_liveBufferSizeIndex) {
        dirty = true;
    }
    m_pCoDirty->forceSet(dirty ? 1.0 : 0.0);
}

void AudioDeviceSettings::onRescanRequested(double value) {
    // Push-button semantics: only act on the rising edge, then reset to 0
    // so the next press fires another valueChanged.
    if (value < 0.5) {
        return;
    }
    m_pCoRescan->forceSet(0.0);
    if (m_applyPending) {
        return;
    }
    // Same playback gate as Apply, for the same reason: rescan() runs
    // clearAndQueryDevices() + a re-open, so a running deck goes silent for
    // seconds. The Rescan button binds the same
    // [AudioDevices],reconfigure_enabled, so this only catches requests that
    // bypass it.
    if (anyDeckPlaying()) {
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(
                    tr("Stop playback before rescanning audio devices"),
                    Notifications::Severity::Warning);
        }
        return;
    }
    // A rescan tears the devices down and re-opens them, so it blocks the GUI
    // thread just like an Apply does — same busy/sticky/paint-delay treatment
    // (see onApplyRequested), and the same guard against a second tap landing
    // mid-flight.
    m_applyPending = true;
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->publishSticky(tr("Rescanning audio devices..."),
                Notifications::Severity::Info);
        pNotifications->setBusy(true);
    }
    QTimer::singleShot(kApplyPaintDelayMs, this, [this]() {
        rescan();
        m_applyPending = false;
    });
}

void AudioDeviceSettings::rescan() {
    // reopenDevices() calls clearAndQueryDevices(), which *closes every open
    // device*, destroys the SoundDevice objects and calls Pa_Terminate()
    // before re-enumerating. On its own it never re-opens anything: the audio
    // callback stops, no clock reference device is left, so
    // EngineMixer::process() is never called again — playing decks freeze and
    // Play does nothing. Nothing else picks up the pieces either (the skin
    // only offers Apply while dirty == 1, and the refreshDeviceList below
    // clears dirty), so the rescan must re-open the devices itself.
    //
    // Re-open against the live config. Routing is not being edited here, so
    // this is a pure re-open; setConfig's own closeDevices() is a no-op
    // (everything is already shut) and therefore skips the
    // kSleepSecondsAfterClosingDevice cooldown, but it still runs checkConfig()
    // so a device or API that vanished while we were away falls back to
    // defaults instead of leaving us silent.
    const SoundDeviceStatus status = reopenDevices();
    if (status == SoundDeviceStatus::Ok) {
        // A manual rescan that worked supersedes any retry loop in flight; it
        // publishes its own message below, so endRecovery() stays quiet.
        endRecovery(false);
    }
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(false);
        if (status == SoundDeviceStatus::Ok) {
            pNotifications->publish(tr("Audio devices rescanned"),
                    Notifications::Severity::Info);
            logAudioPathState("after manual rescan");
        } else {
            pNotifications->publish(
                    m_pSoundManager->getLastErrorMessage(status),
                    Notifications::Severity::Warning);
        }
    }

    // Rebuilds per-bus assignments from the live config, discarding any
    // staged-but-unapplied cycling. setConfig only emits devicesSetup on
    // success, so refresh explicitly to stay honest on the failure path too.
    refreshDeviceList();
}

SoundDeviceStatus AudioDeviceSettings::reopenDevices() {
    // Re-enumerate first. PortAudio caches the device list at Pa_Initialize(),
    // so hardware that appeared (or re-appeared on a new card number) since
    // then is invisible until we tear the library down and bring it back up —
    // which is exactly what clearAndQueryDevices() does.
    m_pSoundManager->clearAndQueryDevices();

    // Then re-point the live config at whatever the configured devices came
    // back as. SoundManager::setupDevices() looks up outputs by the full
    // SoundDeviceId, which carries the PortAudio index and the ALSA hw:X,Y —
    // neither of which survives a device re-enumerating. Without this the
    // controller can be plugged in, powered up and listed, and still be
    // reported "not found" because the stale index no longer matches.
    SoundManagerConfig config = m_pSoundManager->getConfig();
    config.relinkDeviceIds();
    return m_pSoundManager->setConfig(config);
}

bool AudioDeviceSettings::configuredOutputDevicesPresent() const {
    const SoundManagerConfig config = m_pSoundManager->getConfig();
    const auto outputs = config.getOutputs();
    if (outputs.isEmpty()) {
        return false;
    }
    const QList<SoundDevicePointer> devices =
            m_pSoundManager->getDeviceList(config.getAPI(), true, false);
    QSet<QString> presentNames;
    for (const auto& pDevice : devices) {
        presentNames.insert(pDevice->getDeviceId().name);
    }
    for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it) {
        if (it.key().name == kNetworkDeviceInternalName) {
            // Always "present"; SoundManager wires it internally.
            continue;
        }
        // Name only, deliberately: the index and ALSA card number are what
        // change when the device re-enumerates, and relinkDeviceIds() is what
        // reconciles them once we commit to re-opening.
        if (!presentNames.contains(it.key().name)) {
            return false;
        }
    }
    return true;
}

void AudioDeviceSettings::onOutputDeviceLost() {
    if (m_recovering) {
        return;
    }
    m_recovering = true;
    qWarning() << "Audio output device lost; starting automatic recovery";
    if (auto* pNotifications = Notifications::tryInstance()) {
        // Sticky: we have no idea how long the controller stays off. Note that
        // no busy state is raised — the user must stay able to drive the app
        // (and reach Rescan) while this runs in the background.
        pNotifications->publishSticky(
                tr("Audio device disconnected — reconnecting..."),
                Notifications::Severity::Warning);
    }
    m_recoveryTimer.start();
}

void AudioDeviceSettings::attemptRecovery() {
    if (!m_recovering) {
        m_recoveryTimer.stop();
        return;
    }
    if (m_applyPending) {
        // A user-driven Apply or Rescan is mid-flight and owns the devices.
        // Whichever way it lands, it either fixes this or leaves us to retry
        // on the next tick.
        return;
    }

    m_pSoundManager->clearAndQueryDevices();
    if (!configuredOutputDevicesPresent()) {
        // Still gone. Deliberately do *not* go through setConfig() here: it
        // runs checkConfig(), which falls back to default routing when the
        // configured API or device is missing, and that would quietly throw
        // away the user's bus assignment while the controller is merely
        // switched off.
        return;
    }

    const SoundDeviceStatus status = reopenDevices();
    if (status != SoundDeviceStatus::Ok) {
        qWarning() << "Audio device came back but re-opening it failed:"
                   << m_pSoundManager->getLastErrorMessage(status)
                   << "- will retry";
        return;
    }
    endRecovery(true);
}

void AudioDeviceSettings::logAudioPathState(const char* context) const {
    // One line, GUI thread, only on a (re)connect — never in the callback.
    //
    // A deck whose transport advances while nothing is audible has several
    // possible causes that all look identical from the outside, and the state
    // that separates them is not visible anywhere in the skin:
    //   [Master],enabled == 0    -> EngineMixer::process() clears the main
    //                               buffer outright (see the mainEnabled
    //                               branch at the end of process()); every
    //                               deck still runs. Only
    //                               EngineMixer::onOutputConnected(Main) ever
    //                               sets this back to 1, so it stays 0 if the
    //                               Main path failed to match a device.
    //   volume/gain at 0         -> routed and running, just turned down.
    //   track_loaded == 0        -> the deck lost its track.
    //   playposition not moving  -> the engine is not running at all.
    // Capturing them at the moment audio comes back is what makes the next
    // report of this actionable.
    QStringList decks;
    const int numDecks = static_cast<int>(m_pNumDecks->get());
    for (int i = 0; i < numDecks; ++i) {
        const QString group = PlayerManager::groupForDeck(i);
        decks << QStringLiteral("%1 play=%2 loaded=%3 pos=%4 vol=%5")
                         .arg(group)
                         .arg(ControlObject::get(ConfigKey(group, "play")))
                         .arg(ControlObject::get(ConfigKey(group, "track_loaded")))
                         .arg(ControlObject::get(ConfigKey(group, "playposition")), 0, 'f', 4)
                         .arg(ControlObject::get(ConfigKey(group, "volume")));
    }
    qInfo() << "Audio path state" << context
            << "| sound_status="
            << ControlObject::get(ConfigKey(kSoundManagerGroup, "status"))
            << "main_enabled="
            << ControlObject::get(ConfigKey(QStringLiteral("[Master]"), "enabled"))
            << "main_gain="
            << ControlObject::get(ConfigKey(QStringLiteral("[Master]"), "gain"))
            << "|" << decks.join(QStringLiteral(" | "));
}

void AudioDeviceSettings::endRecovery(bool succeeded) {
    if (!m_recovering) {
        return;
    }
    m_recoveryTimer.stop();
    m_recovering = false;
    if (succeeded) {
        // A non-sticky publish supersedes the sticky "reconnecting" message.
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(tr("Audio device reconnected"),
                    Notifications::Severity::Info);
        }
        logAudioPathState("after automatic recovery");
        refreshDeviceList();
    }
    // On the !succeeded path the caller is a manual Rescan/Apply that
    // publishes its own outcome and refreshes the rows itself; saying anything
    // here would just stomp on it.
}

void AudioDeviceSettings::onSoundManagerDevicesUpdated() {
    refreshDeviceList();
}

void AudioDeviceSettings::refreshDeviceList() {
    const QString api = m_pSoundManager->getConfig().getAPI();
    const QList<SoundDevicePointer> devices = m_pSoundManager->getDeviceList(
            api, true /* outputs */, false /* inputs */);

    m_deviceNames.clear();
    m_deviceIds.clear();
    m_deviceOutputChannels.clear();
    for (const auto& pDevice : devices) {
        // Never offer the internal "Network stream" device as a bus target.
        // SoundManager::setupDevices always statically wires a RecordBroadcast
        // output to it on channel 0 (the broadcast/record sidechain), so
        // routing any of our buses there collides on ch 0 and setConfig fails
        // with ErrorDuplicateOutputChannel ("Two outputs cannot share channels
        // on Network stream"). It is not a physical output the user can hear.
        if (pDevice->getDeviceId().name == kNetworkDeviceInternalName) {
            continue;
        }
        const int channels = static_cast<int>(pDevice->getNumOutputChannels());
        if (channels < 2) {
            continue; // need at least stereo for a bus
        }
        m_deviceNames.append(pDevice->getDisplayName());
        m_deviceIds.append(pDevice->getDeviceId());
        m_deviceOutputChannels.append(channels);
    }

    // Reconstruct per-bus assignments from the live config. This is the single
    // source of truth: it survives restarts (SoundManager persists outputs to
    // sounddevices.xml) and reflects whatever setConfig actually opened.
    for (int b = 0; b < BusCount; ++b) {
        m_busDeviceIndex[b] = -1;
        m_busChannelBase[b] = 0;
    }
    const auto outputs = m_pSoundManager->getConfig().getOutputs();
    for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it) {
        int bus = -1;
        switch (it.value().getType()) {
        case AudioPathType::Main:
            bus = BusMaster;
            break;
        case AudioPathType::Booth:
            bus = BusBooth;
            break;
        case AudioPathType::Headphones:
            bus = BusHeadphones;
            break;
        default:
            continue;
        }
        m_busDeviceIndex[bus] = findIndexForDeviceId(it.key());
        m_busChannelBase[bus] =
                it.value().getChannelGroup().getChannelBase();
    }

    // Snapshot the live state and reset the staged state to it — any
    // cycling that wasn't applied is discarded here (device hot-plug,
    // post-apply reconcile, or an explicit Cancel/revert).
    const auto liveRate = m_pSoundManager->getConfig().getSampleRate();
    m_liveSampleRate = liveRate.isValid()
            ? static_cast<int>(liveRate.value())
            : kFallbackSampleRate;
    m_liveBufferSizeIndex =
            m_pSoundManager->getConfig().getAudioBufferSizeIndex();
    m_bufferSizeIndex = m_liveBufferSizeIndex;
    for (int b = 0; b < BusCount; ++b) {
        m_liveBusDeviceIndex[b] = m_busDeviceIndex[b];
        m_liveBusChannelBase[b] = m_busChannelBase[b];
    }
    m_pCoSampleRate->forceSet(static_cast<double>(m_liveSampleRate));
    updateLatencyMode();

    m_pCoCount->forceSet(static_cast<double>(m_deviceIds.size()));
    m_pCoSelectedIndex->forceSet(
            static_cast<double>(m_busDeviceIndex[BusMaster]));
    m_pCoConfigured->forceSet(m_busDeviceIndex[BusMaster] >= 0 ? 1.0 : 0.0);
    rebuildBusLabels();
    emit busesChanged(m_busLabels, busAssigned());
    updateDirty();
}

void AudioDeviceSettings::applyBusConfig() {
    // Snapshot the last-good config so a failed apply (e.g. the device was
    // unplugged during the cooldown and opening it fails) can be rolled
    // back. SoundManager assigns m_config = config before it tries to open the
    // devices, so on failure getConfig() would otherwise report the bad routing
    // and refreshDeviceList would mislabel the rows.
    const SoundManagerConfig previousConfig = m_pSoundManager->getConfig();
    SoundManagerConfig config = previousConfig;
    config.clearOutputs();
    for (int b = 0; b < BusCount; ++b) {
        const int devIdx = m_busDeviceIndex[b];
        if (devIdx < 0 || devIdx >= m_deviceIds.size()) {
            continue;
        }
        config.addOutput(m_deviceIds.at(devIdx),
                AudioOutput(typeForBus(b),
                        static_cast<unsigned char>(m_busChannelBase[b]),
                        mixxx::audio::ChannelCount::stereo(),
                        0));
    }
    auto rate = mixxx::audio::SampleRate(
            static_cast<int>(m_pCoSampleRate->get()));
    if (!rate.isValid()) {
        rate = mixxx::audio::SampleRate(kFallbackSampleRate);
    }
    config.setSampleRate(rate);
    config.setAudioBufferSizeIndex(m_bufferSizeIndex);

    const SoundDeviceStatus status = m_pSoundManager->setConfig(config);
    // Clear the busy state raised in onApplyRequested and replace the sticky
    // "Applying..." message with the outcome (both branches publish a
    // non-sticky message, which supersedes the sticky). Input stays
    // suppressed for a grace window after this so taps queued during the
    // freeze are discarded (see Notifications::setBusy).
    if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->setBusy(false);
    }
    if (status != SoundDeviceStatus::Ok) {
        // Roll back to the last-good routing so audio keeps flowing and the
        // rows reflect what's actually open. Best-effort: if this also fails
        // (e.g. the device vanished) refreshDeviceList still reports reality.
        m_pSoundManager->setConfig(previousConfig);
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(
                    m_pSoundManager->getLastErrorMessage(status),
                    Notifications::Severity::Warning);
        }
    } else if (auto* pNotifications = Notifications::tryInstance()) {
        pNotifications->publish(tr("Audio outputs updated"),
                Notifications::Severity::Info);
    }
    // setConfig only emits devicesSetup on success, so refresh explicitly to
    // reconcile per-bus state (and re-emit busesChanged) on both paths.
    refreshDeviceList();
}

void AudioDeviceSettings::rebuildBusLabels() {
    m_busLabels.clear();
    for (int b = 0; b < BusCount; ++b) {
        m_busLabels.append(formatBusLabel(b));
    }
}

QString AudioDeviceSettings::formatBusLabel(int busIndex) const {
    const QString name = nameForBus(busIndex);
    const int devIdx = m_busDeviceIndex[busIndex];
    if (devIdx < 0 || devIdx >= m_deviceNames.size()) {
        return tr("%1 — None").arg(name);
    }
    const int base = m_busChannelBase[busIndex];
    return tr("%1 — %2 ch %3-%4")
            .arg(name, m_deviceNames.at(devIdx))
            .arg(base + 1)
            .arg(base + 2);
}

int AudioDeviceSettings::findIndexForDeviceId(const SoundDeviceId& id) const {
    for (int i = 0; i < m_deviceIds.size(); ++i) {
        if (m_deviceIds.at(i) == id) {
            return i;
        }
    }
    return -1;
}
