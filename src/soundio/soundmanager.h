#pragma once

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QTimer>

#include "audio/types.h"
#include "control/pollingcontrolproxy.h"
#include "engine/sidechain/enginenetworkstream.h"
#include "preferences/usersettings.h"
#include "soundio/sounddevice.h"
#include "soundio/soundmanagerconfig.h"
#include "util/cmdlineargs.h"
#include "util/types.h"

class EngineMixer;
class ControlObject;

#define MIXXX_PORTAUDIO_JACK_STRING "JACK Audio Connection Kit"
#define MIXXX_PORTAUDIO_ALSA_STRING "ALSA"
#define MIXXX_PORTAUDIO_OSS_STRING "OSS"
#define MIXXX_PORTAUDIO_ASIO_STRING "ASIO"
#define MIXXX_PORTAUDIO_DIRECTSOUND_STRING "Windows DirectSound"
// NOTE: This is what our patched version of PortAudio uses for the Core Audio
// backend on iOS. If/when upstream supports iOS officially
// (https://github.com/PortAudio/portaudio/pull/881), we may have to update this
#define MIXXX_PORTAUDIO_IOSAUDIO_STRING "iOS Audio"
#define MIXXX_PORTAUDIO_COREAUDIO_STRING "Core Audio"

#define SOUNDMANAGER_DISCONNECTED 0
#define SOUNDMANAGER_CONNECTING 1
#define SOUNDMANAGER_CONNECTED 2


class SoundManager : public QObject {
    Q_OBJECT
  public:
    SoundManager(UserSettingsPointer pConfig, EngineMixer* pEngineMixer);
    ~SoundManager() override;

    // Returns a list of all devices we've enumerated that match the provided
    // filterApi, and have at least one output or input channel if the
    // bOutputDevices or bInputDevices are set, respectively.
    QList<SoundDevicePointer> getDeviceList(
            const QString& filterAPI, bool bOutputDevices, bool bInputDevices) const;

    // Creates a list of sound devices
    void clearAndQueryDevices();
    void queryDevices();
    void queryDevicesPortaudio();
    void queryDevicesMixxx();

    // Opens all the devices chosen by the user in the preferences dialog, and
    // establishes the proper connections between them and the mixing engine.
    SoundDeviceStatus setupDevices();

    // Playermanager will notify us when the number of decks changes.
    void setConfiguredDeckCount(int count);
    int getConfiguredDeckCount() const;

    SoundDevicePointer getErrorDevice() const;
    QString getErrorDeviceName() const;
    QString getLastErrorMessage(SoundDeviceStatus status) const;

    // Returns a list of samplerates we will attempt to support for a given API.
    QList<mixxx::audio::SampleRate> getSampleRates(const QString& api) const;

    // Convenience overload for SoundManager::getSampleRates(QString)
    QList<mixxx::audio::SampleRate> getSampleRates() const;

    // Get a list of host APIs supported by PortAudio.
    QList<QString> getHostAPIList() const;
    SoundManagerConfig getConfig() const;
    SoundDeviceStatus setConfig(const SoundManagerConfig& config);
    void checkConfig();

    void onDeviceOutputCallback(const SINT iFramesPerBuffer);

    // Used by SoundDevices to "push" any audio from their inputs that they have
    // into the mixing engine.
    void pushInputBuffers(const QList<AudioInputBuffer>& inputs,
                          const SINT iFramesPerBuffer);

    void writeProcess(SINT framesPerBuffer) const;
    void readProcess(SINT framesPerBuffer) const;

    void registerOutput(const AudioOutput& output, AudioSource* src);
    void registerInput(const AudioInput& input, AudioDestination* dest);
    QList<AudioOutput> registeredOutputs() const;
    QList<AudioInput> registeredInputs() const;

    QSharedPointer<EngineNetworkStream> getNetworkStream() const {
        return m_pNetworkStream;
    }

    void underflowHappened(int code) {
        // Count every underrun so that logUnderflows() can report them in
        // aggregate. Only lock-free atomics here, this runs in the audio
        // callback.
        //
        // The network stream is counted separately and deliberately does NOT
        // set m_underflowHappened: its FIFO feeds the broadcast workers, not a
        // sound card, so its over/underflows are inaudible. Counting the two
        // together made a stalled broadcast worker read as audio dropouts in
        // the log and lit the skin's latency-overload indicator for it.
        if (code >= kFirstNetworkUnderflowCode) {
            m_networkUnderflowCount.fetchAndAddRelaxed(1);
            if (code < 32) {
                m_networkUnderflowCodes.fetchAndOrRelaxed(1U << code);
            }
            return;
        }

        m_underflowHappened = 1;
        m_underflowCount.fetchAndAddRelaxed(1);
        if (code >= 0 && code < 32) {
            m_underflowCodes.fetchAndOrRelaxed(1U << code);
        }
        // Disable the engine warnings by default, because printing a warning is a
        // locking function that will make the problem worse
        if (CmdlineArgs::Instance().getDeveloper()) {
            qWarning() << "underflowHappened code:" << code;
        }
    }

    void processUnderflowHappened(SINT framesPerBuffer);

  signals:
    void devicesUpdated(); // emitted when pointers to SoundDevices go stale
    void devicesSetup(); // emitted when the sound devices have been set up
    // Bite DJ: emitted from the GUI thread when the watchdog finds that an
    // output device we had open has stopped driving the engine (its host API
    // stream died, or it stopped calling us). The devices have already been
    // closed by the time this fires, so [SoundManager],status reads
    // SOUNDMANAGER_DISCONNECTED and it is safe to re-open from the slot.
    // AudioDeviceSettings owns the recovery policy.
    void outputDeviceLost();
    void outputRegistered(const AudioOutput& output, AudioSource* src);
    void inputRegistered(const AudioInput& input, AudioDestination* dest);

  private slots:
    // Writes the underruns counted since the last call to the log. Called
    // periodically from the main thread, never from the audio callback.
    void logUnderflows();

    // Bite DJ: polls the open devices for liveness and tears them down if one
    // has died under us. See the comment on m_deviceWatchdogTimer.
    void checkDeviceHealth();

  private:
    // Closes all the devices and empties the list of devices we have.
    void clearDeviceList(bool sleepAfterClosing);

    // Closes all the open sound devices. Because multiple soundcards might be
    // open, this method simply runs through the list of all known soundcards
    // (from PortAudio) and attempts to close them all. Closing a soundcard that
    // isn't open is safe.
    void closeDevices(bool sleepAfterClosing);

    // Bite DJ: arm/disarm the watchdog. Armed only while at least one real
    // (non-network) output device is open.
    void startDeviceWatchdog();
    void stopDeviceWatchdog();

    void setJACKName() const;
    bool jackApiUsed() const {
        return m_config.getAPI() == MIXXX_PORTAUDIO_JACK_STRING;
    }

    EngineMixer* m_pEngineMixer;
    UserSettingsPointer m_pConfig;
    bool m_paInitialized;
    mixxx::audio::SampleRate m_jackSampleRate;
    QList<SoundDevicePointer> m_devices;
    QList<mixxx::audio::SampleRate> m_samplerates;
    QList<CSAMPLE*> m_inputBuffers;

    SoundManagerConfig m_config;
    SoundDevicePointer m_pErrorDevice;
    QHash<AudioOutput, AudioSource*> m_registeredSources;
    QMultiHash<AudioInput, AudioDestination*> m_registeredDestinations;
    ControlObject* m_pControlObjectSoundStatusCO;
    ControlObject* m_pControlObjectVinylControlGainCO;

    QSharedPointer<EngineNetworkStream> m_pNetworkStream;

    // The underrun codes passed to underflowHappened() are literals at the call
    // sites: SoundDevicePortAudio raises 1-20, SoundDeviceNetwork 21 and up.
    // Keep this in step with those two files if either gains a code.
    static constexpr int kFirstNetworkUnderflowCode = 21;

    QAtomicInt m_underflowHappened;
    // Number of underruns since the last logUnderflows() call and the bit set
    // of the codes they were reported with. Written from the audio callback,
    // consumed by logUnderflows().
    QAtomicInt m_underflowCount;
    QAtomicInteger<quint32> m_underflowCodes;
    // Underruns since application start. Only touched by logUnderflows().
    quint64 m_underflowTotalCount;
    // The same three, for the network stream (codes >= kFirstNetworkUnderflowCode).
    QAtomicInt m_networkUnderflowCount;
    QAtomicInteger<quint32> m_networkUnderflowCodes;
    quint64 m_networkUnderflowTotalCount;
    QTimer m_underflowLogTimer;
    int m_underflowUpdateCount;
    PollingControlProxy m_audioLatencyOverloadCount;
    PollingControlProxy m_audioLatencyOverload;

    // Bite DJ: device watchdog.
    //
    // PortAudio does not report device removal. When the USB controller loses
    // power its ALSA card disappears, and from Mixxx's side the stream handle
    // stays valid, SoundDevice::isOpen() keeps returning true, and
    // [SoundManager],status keeps reading SOUNDMANAGER_CONNECTED forever — the
    // engine callback simply stops. Everything downstream is built on the
    // assumption that a connected device means a running engine, so without
    // this poll nothing ever notices and the only way back is the user finding
    // Rescan in the settings.
    //
    // The timer runs on the GUI thread, the same thread that opens and closes
    // devices, so it can never race setupDevices()/closeDevices().
    QTimer m_deviceWatchdogTimer;
    // Sum of every open device's callbackTick(), and how long it has been
    // unchanged. Only a change matters, so a device that cannot report ticks
    // contributes a constant and is effectively ignored.
    quint64 m_lastCallbackTickSum;
    QElapsedTimer m_callbackTickTimer;
};
