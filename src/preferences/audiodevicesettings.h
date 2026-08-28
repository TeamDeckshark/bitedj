#pragma once

#include <QAtomicPointer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>

#include "audio/types.h"
#include "preferences/usersettings.h"
#include "soundio/sounddevicestatus.h"
#include "soundio/soundmanagerutil.h"

class ControlObject;
class ControlProxy;
class SoundManager;

// Bite DJ: in-skin replacement for the output-routing half of DlgPrefSound.
// Owns the [AudioDevices],* COs and a Qt signal carrying the per-bus display
// strings (CO values are doubles, so list strings ride a signal alongside —
// same transport as Notifications::messagePosted and the ControllerSettings
// picker).
//
// The Audio settings sub-page assigns each output bus (Master, Booth,
// Headphones) independently to any detected device + stereo channel pair, or
// leaves it unrouted ("None"), and picks a latency-vs-quality preset that sets
// the sample rate and audio buffer size together (see LatencyMode). Inputs,
// per-deck routing, and API selection are still not exposed.
//
// Output routing lives entirely here: it is the sole owner of the audio
// output config. Enabling/disabling a MIDI controller (ControllerSettings)
// never touches routing, so plugging in a second controller can't steal the
// Master/Booth/Headphones assignment. Routing persists via SoundManager's
// sounddevices.xml and is reconstructed from the live config on every refresh.
class AudioDeviceSettings : public QObject {
    Q_OBJECT
  public:
    // Stable bus order — indexes m_busDeviceIndex / m_busChannelBase and the
    // busLabels() list the Audio sub-page renders.
    enum Bus {
        BusMaster = 0,
        BusBooth = 1,
        BusHeadphones = 2,
        BusCount = 3,
    };

    // Latency-vs-quality preset. The Audio sub-page exposes exactly these two
    // points instead of the sample-rate and buffer-size combo boxes upstream
    // DlgPrefSound shows: on a fixed appliance the only question a DJ actually
    // has is "tight jog/scratch response" vs "headroom against underruns".
    //
    // Both presets land on the same 64-frame base period, because
    // SoundManagerConfig::getFramesPerBuffer() computes
    // bit_ceil(sampleRate / 1000) << (index - 1) and bit_ceil is 64 for both
    // 44 and 48 kHz -- so the index alone picks the frame count:
    //   Latency: 44100 Hz, index 3 ->  256 frames -> 5.8 ms
    //   Quality: 48000 Hz, index 5 -> 1024 frames -> 21.3 ms
    // Custom is never selectable from the UI; it is what a soundconfig.xml
    // holding any other rate/index pair (hand-edited, or carried over from
    // desktop Mixxx) reports, so neither segment lights and the hint says so
    // rather than silently mislabelling the running config.
    enum LatencyMode {
        ModeQuality = 0,
        ModeLatency = 1,
        ModeCustom = 2,
    };

    AudioDeviceSettings(UserSettingsPointer pConfig,
            std::shared_ptr<SoundManager> pSoundManager);
    ~AudioDeviceSettings() override;

    static AudioDeviceSettings* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    // Snapshot so a freshly-created WAudioDeviceList can render its initial
    // state without waiting for the next busesChanged emit.
    QStringList busLabels() const {
        return m_busLabels;
    }
    // Per-bus flag: true when the bus is routed to a device, false for "None".
    // Drives the [assigned] Q_PROPERTY so unrouted rows paint dimmed.
    QList<bool> busAssigned() const;

    // Index of the device currently routed to Master (-1 if none). Kept as
    // the startup "is this rig usable" proxy read by ControllerSettings.
    int selectedIndex() const {
        return m_busDeviceIndex[BusMaster];
    }

    // Advances the given bus to its next (device, channel-pair) option,
    // wrapping through "None". The change is staged only: labels update
    // immediately and [AudioDevices],dirty flips on, but nothing touches the
    // sound hardware until the user hits Apply ([AudioDevices],apply). Safe
    // to call from the GUI thread; out-of-range is a no-op.
    void cycleBus(int busIndex);

    // Re-queries the host for currently-attached devices, re-opens the audio
    // devices against the live config, and re-emits busesChanged so the UI
    // reflects the new state. Exposed for the Rescan button on the Audio
    // settings sub-page and the Devices Rescan flow.
    //
    // Blocking: the re-open goes through SoundManager::setConfig. Callers
    // should raise the busy state first (onRescanRequested does); rescan()
    // clears it when it finishes.
    void rescan();

  signals:
    // Pre-rendered per-bus labels, one per Bus, e.g. "Master — DDJ-400 ch 1-2"
    // or "Booth — None", paired with per-bus routed flags (false → "None",
    // drives the [assigned="false"] QSS rule in WAudioDeviceList).
    void busesChanged(const QStringList& busLabels, const QList<bool>& assigned);

  private slots:
    void onSoundManagerDevicesUpdated();
    void onSampleRateChanged(double rate);
    void onLatencyModeChanged(double mode);
    void onRescanRequested(double value);
    void onApplyRequested(double value);
    void onRevertRequested(double value);
    void onDeckCountChanged(double numDecks);
    // The output device we had open died under us (SoundManager's watchdog
    // already closed everything). Starts the automatic recovery retry.
    void onOutputDeviceLost();
    // One recovery attempt: re-enumerate and, if the configured device is back,
    // re-open against it. Rescheduled by m_recoveryTimer until it succeeds.
    void attemptRecovery();

  private:
    // A selectable target for a bus: a device + the base channel of a stereo
    // pair, or "None" (deviceIndex < 0).
    struct Option {
        int deviceIndex;
        int channelBase;
    };

    void refreshDeviceList();
    // Recomputes [AudioDevices],dirty: 1 while any staged bus assignment, the
    // sample-rate CO, or the staged buffer-size index differs from the live
    // config snapshot taken by the last refreshDeviceList. The skin keys the
    // Apply/Cancel vs Rescan button swap off this CO.
    void updateDirty();
    // Rate + buffer-size index that a preset stands for. ModeCustom maps to
    // the currently staged pair, so asking for it is a no-op.
    static int sampleRateForMode(int mode);
    static unsigned int bufferSizeIndexForMode(int mode);
    // Reverse lookup: the preset a (rate, index) pair corresponds to, or
    // ModeCustom when it matches neither.
    static int modeForConfig(int sampleRate, unsigned int bufferSizeIndex);
    // Recomputes [AudioDevices],latency_mode from the staged rate/index pair
    // without re-entering onLatencyModeChanged (the CO is written with
    // forceSet, which still emits, so the slot guards on an unchanged pair).
    void updateLatencyMode();
    // Rebuilds SoundManagerConfig from the three m_busDeviceIndex/
    // m_busChannelBase assignments and applies it (blocking). Reconciles state
    // from the resulting config afterwards (setConfig only emits devicesSetup
    // on success, so we can't rely on the signal to refresh on failure), and
    // clears the busy state raised by onApplyRequested.
    void applyBusConfig();
    // True while any deck's play CO is on *and* an output device is actually
    // connected. Applying and rescanning both close and re-open every audio
    // device, so running either while a deck plays drops the music mid-track —
    // see updateReconfigureEnabled. The [SoundManager],status half matters
    // because play is cleared by the audio callback: with the device gone it
    // stays latched and would otherwise gate off the only recovery path.
    bool anyDeckPlaying() const;
    // (Re)subscribes to every deck's play CO. Called from the ctor and whenever
    // [App],num_decks changes.
    void rebuildDeckPlayProxies();
    // Recomputes [AudioDevices],reconfigure_enabled: 0 while a deck is playing,
    // 1 otherwise. The skin binds the Apply *and* Rescan buttons' `enabled`
    // property to it, so both paint greyed and swallow taps instead of cutting
    // the audio out from under a live set. onApplyRequested/onRescanRequested
    // enforce the same rule for callers that poke the COs directly (e.g. a
    // controller mapping).
    void updateReconfigureEnabled();
    // The cyclable option list for the given bus: index 0 is always "None",
    // followed by each device's stereo pairs (ch 1-2, 3-4, ...) — except
    // pairs currently assigned to one of the *other* buses, which are
    // skipped so two buses can't be cycled onto the same physical output.
    // The bus's own current assignment is always included (it anchors the
    // cycle position).
    QList<Option> buildOptions(int busIndex) const;
    int optionIndexForAssignment(
            int busIndex, int deviceIndex, int channelBase) const;
    void rebuildBusLabels();
    QString formatBusLabel(int busIndex) const;
    static AudioPathType typeForBus(int busIndex);
    static QString nameForBus(int busIndex);
    int findIndexForDeviceId(const SoundDeviceId& id) const;
    // Shared by rescan() and attemptRecovery(): re-enumerate the host's
    // devices, re-point the live config at whatever the configured devices
    // came back as, and re-open. Returns the setConfig status.
    SoundDeviceStatus reopenDevices();
    // True when every device the live config routes a bus to is present in the
    // freshly enumerated device list. Recovery only pays for a re-open once
    // the hardware is actually back.
    bool configuredOutputDevicesPresent() const;
    // Stops the retry loop and clears the "reconnecting" notification.
    void endRecovery(bool succeeded);
    // Writes one line describing the state that decides whether audio is
    // actually audible (master enable/gain, and each deck's play, loaded,
    // position and volume) to the log. Called on every successful (re)connect.
    void logAudioPathState(const char* context) const;

    static QAtomicPointer<AudioDeviceSettings> s_pInstance;

    UserSettingsPointer m_pConfig;
    std::shared_ptr<SoundManager> m_pSoundManager;

    QStringList m_deviceNames;
    QList<SoundDeviceId> m_deviceIds;
    QList<int> m_deviceOutputChannels;

    // Per-bus staged assignment. deviceIndex < 0 => None (unrouted). Edited
    // by cycleBus, committed to hardware only by applyBusConfig.
    int m_busDeviceIndex[BusCount];
    int m_busChannelBase[BusCount];

    // Snapshot of the live (applied) config, taken by refreshDeviceList.
    // updateDirty compares the staged state against these.
    int m_liveBusDeviceIndex[BusCount];
    int m_liveBusChannelBase[BusCount];
    int m_liveSampleRate;

    // Staged audio buffer size index (SoundManagerConfig::AudioBufferSizeIndex),
    // moved as a pair with the sample-rate CO by the latency-mode preset and
    // committed only by applyBusConfig, plus the live snapshot updateDirty
    // compares it against.
    unsigned int m_bufferSizeIndex;
    unsigned int m_liveBufferSizeIndex;

    QStringList m_busLabels;

    // True between the Apply/Rescan tap and applyBusConfig/rescan finishing —
    // drops repeat requests (e.g. poked via a controller mapping) while the
    // deferred blocking work is pending or running. Shared by both because
    // either one closes and re-opens the devices.
    bool m_applyPending;

    std::unique_ptr<ControlObject> m_pCoCount;
    std::unique_ptr<ControlObject> m_pCoSelectedIndex;
    std::unique_ptr<ControlObject> m_pCoSampleRate;
    std::unique_ptr<ControlObject> m_pCoLatencyMode;
    std::unique_ptr<ControlObject> m_pCoRescan;
    std::unique_ptr<ControlObject> m_pCoConfigured;
    std::unique_ptr<ControlObject> m_pCoApply;
    std::unique_ptr<ControlObject> m_pCoRevert;
    std::unique_ptr<ControlObject> m_pCoDirty;
    std::unique_ptr<ControlObject> m_pCoReconfigureEnabled;

    // One proxy per deck's play CO, parented to this (deleted with it), plus
    // the deck-count proxy that rebuilds them. Deck play changes arrive from
    // the engine thread, so these are queued (AutoConnection) onto the GUI
    // thread like every other CO subscription here.
    ControlProxy* m_pNumDecks;
    QList<ControlProxy*> m_deckPlayProxies;
    // [SoundManager],status — the other half of anyDeckPlaying(). Subscribed
    // so reconfigure_enabled is recomputed the moment the devices go down,
    // which is what un-greys Rescan instead of waiting on a play CO that the
    // dead engine will never clear.
    ControlProxy* m_pSoundStatus;

    // Automatic recovery from an output device that vanished (USB controller
    // power-cycled, hub reset). SoundManager's watchdog notices and closes the
    // devices; this timer then re-enumerates every kRecoveryIntervalMs until
    // the device is back and re-opens against it, so the user does not have to
    // find Rescan in the settings to get audio again.
    QTimer m_recoveryTimer;
    // True from the loss until a re-open succeeds (or the user reconfigures
    // the audio devices themselves, which supersedes the retry).
    bool m_recovering;
};
