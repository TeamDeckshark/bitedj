#pragma once

#include <QString>
#include <algorithm>
#include <cmath>

#include "util/math.h"

/// How an EQ knob responds between its centre detent and the bottom stop.
/// One global choice, shared by the per-deck 3-band EQs (driven by the MIDI
/// controller) and by the master Graphic EQ on the Levels page, so a knob on
/// the controller and a knob on the screen cut the same way.
enum class EqMode {
    /// Shelving/bell EQ. Bottoms out at a finite cut and flattens on the way
    /// there — at the stop the band is heavily attenuated but still audible.
    Eq = 0,
    /// Isolator. Reaches silence exactly at the stop, which is what forces the
    /// curve to steepen over the second half of the travel.
    Isolator = 1,
};

namespace EqCurve {

/// [BiteDJ],eq_mode — 0 = EQ, 1 = Isolator. Owned by EffectsManager rather
/// than SystemSettings: the effect processors take a PollingControlProxy on it
/// while they are being instantiated, which happens inside
/// EffectsManager::setup(), long before SystemSettings exists.
const QString kModeGroup = QStringLiteral("[BiteDJ]");
const QString kModeKey = QStringLiteral("eq_mode");

/// Isolator preserves the full kill at the stop that this appliance has always
/// had, so it is what an existing unit keeps on the first launch after the
/// setting appears.
constexpr double kModeDefault = static_cast<double>(EqMode::Isolator);

inline EqMode modeFromControlValue(double value) {
    return value != 0.0 ? EqMode::Isolator : EqMode::Eq;
}

/// Deepest cut the deck EQs reach in EQ mode, and the boost they reach the
/// other way. -26 dB is the Xone-ish figure the stock Biquad Equalizer uses.
constexpr double kDeckMaxCutDb = -26.0;
constexpr double kDeckBoostDb = 12.0;

/// A biquad cannot be asked for -inf dB, so the isolator's last sliver of
/// travel is floored here on the paths that have no true kill (the master
/// Graphic EQ). Far below anything the mixer can put out.
constexpr double kMinCutDb = -60.0;

/// Exponent of the isolator's amplitude curve, `gain = (1 - cut)^p`.
///
/// Two requirements fix it. The curve has to reach zero at the stop, which any
/// p > 0 gives; and it has to cross the EQ curve at half travel, so the first
/// half of the sweep belongs to EQ mode and the second half to the isolator.
/// At cut = 0.5 the EQ curve sits at maxCutDb/2, so
///     0.5^p = 10^(maxCutDb / 40)   ->   p = -maxCutDb / (40 * log10 2).
/// A deeper EQ-mode floor therefore buys the isolator a steeper late dive,
/// which is the whole character difference: EQ moves more per degree early,
/// the isolator more per degree late.
inline double isolatorExponent(double maxCutDb) {
    return -maxCutDb / (40.0 * std::log10(2.0));
}

/// Linear gain for a band from how far its knob is turned down: `cut` is 0.0
/// at the centre detent and 1.0 at the bottom stop. `maxCutDb` (negative) is
/// where the EQ-mode curve bottoms out.
inline double cutGain(EqMode mode, double cut, double maxCutDb) {
    cut = std::clamp(cut, 0.0, 1.0);
    if (mode == EqMode::Isolator) {
        return std::pow(1.0 - cut, isolatorExponent(maxCutDb));
    }
    return db2ratio(maxCutDb * cut);
}

/// The same curve in dB, floored at kMinCutDb for filters that cannot express
/// silence. In EQ mode this is exactly `maxCutDb * cut` and so is a no-op
/// wrapper; only the isolator ever hits the floor.
inline double cutGainDb(EqMode mode, double cut, double maxCutDb) {
    const double gain = cutGain(mode, cut, maxCutDb);
    if (gain <= db2ratio(kMinCutDb)) {
        return kMinCutDb;
    }
    return ratio2db(gain);
}

/// Linear gain a deck EQ band is asking for, from its raw parameter value:
/// 0.0 at the bottom stop, 1.0 at the centre detent, 2.0 at full boost. Boost
/// is the same in both modes — only the cut side has two characters.
///
/// The effect itself does not call this (it has to split the request between
/// its biquads and its crossover); it is here so the waveform tint and any
/// other consumer of parameter1..3 read the knob the same way the engine does.
inline double deckBandGain(EqMode mode, double value) {
    if (value >= 1.0) {
        return db2ratio(std::min(value - 1.0, 1.0) * kDeckBoostDb);
    }
    return cutGain(mode, 1.0 - value, kDeckMaxCutDb);
}

} // namespace EqCurve
