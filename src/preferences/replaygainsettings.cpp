#include "preferences/replaygainsettings.h"

#include "track/track.h"

namespace {
const char* kConfigKey = "[ReplayGain]";

const char* kInitialReplayGainBoost = "InitialReplayGainBoost";
const char* kInitialDefaultBoost = "InitialDefaultBoost";
// WARNING: Do not fix the "analyser" spelling here since user config files
// contain these strings.
const char* kReplayGainAnalyzerEnabled = "ReplayGainAnalyserEnabled";
const char* kReplayGainAnalyzerVersion = "ReplayGainAnalyserVersion";
const char* kReplayGainReanalyze = "ReplayGainReanalyze";

const char* kReplayGainEnabled = "ReplayGainEnabled";

constexpr int kInitialDefaultBoostDefault = -6;
} // anonymous namespace

ReplayGainSettings::ReplayGainSettings(UserSettingsPointer pConfig)
    : m_pConfig(pConfig) {
}

int ReplayGainSettings::getInitialReplayGainBoost() const {
    return m_pConfig->getValue(
            ConfigKey(kConfigKey, kInitialReplayGainBoost), 0);
}

void ReplayGainSettings::setInitialReplayGainBoost(int value) {
    m_pConfig->set(ConfigKey(kConfigKey, kInitialReplayGainBoost),
            ConfigValue(value));
}

int ReplayGainSettings::getInitialDefaultBoost() const {
    return m_pConfig->getValue(ConfigKey(kConfigKey, kInitialDefaultBoost),
            kInitialDefaultBoostDefault);
}

void ReplayGainSettings::setInitialDefaultBoost(int value) {
    m_pConfig->set(ConfigKey(kConfigKey, kInitialDefaultBoost),
                ConfigValue(value));
}

// ReplayGain is off on BiteDJ, whatever mixxx.cfg says: a unit keeps its own
// mixxx.cfg across firmware updates, so the shipped file alone would never
// reach the ones already in the field. Measuring a track means reading the
// whole file off the USB stick as soon as it is loaded, at full speed, and on
// the Pi 3 that stick shares the one USB host with the controller's audio and
// MIDI. With a deck playing and the jog in use, those reads were measured
// starving the audio output into xruns (October 2026). Nothing on the device
// exposes the setting. Off means no analysis and no gain applied: every track
// plays at the level it was mastered at, adjusted by trim alone.
bool ReplayGainSettings::getReplayGainEnabled() const {
    return false;
}

void ReplayGainSettings::setReplayGainEnabled(bool value) {
    if (value) {
        m_pConfig->set(ConfigKey(kConfigKey, kReplayGainEnabled), ConfigValue(1));
    } else {
        m_pConfig->set(ConfigKey(kConfigKey, kReplayGainEnabled), ConfigValue(0));
    }
}

// Off for the same reason as getReplayGainEnabled() above.
bool ReplayGainSettings::getReplayGainAnalyzerEnabled() const {
    return false;
}

void ReplayGainSettings::setReplayGainAnalyzerEnabled(bool value) {
    m_pConfig->set(ConfigKey(kConfigKey, kReplayGainAnalyzerEnabled),
                ConfigValue(value));
}

int ReplayGainSettings::getReplayGainAnalyzerVersion() const {
    return m_pConfig->getValue(
            ConfigKey(kConfigKey, kReplayGainAnalyzerVersion), 2);
}

void ReplayGainSettings::setReplayGainAnalyzerVersion(int value) {
    m_pConfig->set(ConfigKey(kConfigKey, kReplayGainAnalyzerVersion),
            ConfigValue(value));
}

bool ReplayGainSettings::getReplayGainReanalyze() const {
    return m_pConfig->getValueString(
        ConfigKey(kConfigKey, kReplayGainReanalyze)).toInt() > 0;
}

void ReplayGainSettings::setReplayGainReanalyze(bool value) {
    m_pConfig->set(ConfigKey(kConfigKey, kReplayGainReanalyze),
                ConfigValue(value));
}

bool ReplayGainSettings::isAnalyzerEnabled(int version) const {
    return getReplayGainAnalyzerEnabled()
            && (version == getReplayGainAnalyzerVersion());
}

bool ReplayGainSettings::isAnalyzerDisabled(int version, TrackPointer tio) const {
    if (isAnalyzerEnabled(version)) {
        if (getReplayGainReanalyze()) {
            // ignore stored replay gain
            return false;
        }
        return tio->getReplayGain().hasRatio();
    }
    // not enabled, pretend we have already a stored value.
    return true;
}
