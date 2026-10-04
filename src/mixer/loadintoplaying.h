#pragma once

#include "preferences/configobject.h"

// Bite DJ: what happens when the DJ loads a track into a deck that is playing
// (General settings tab, [BiteDJ],load_into_playing). Stock Mixxx just does it,
// which unloads the playing track and cuts it off mid-air. Only decks are
// covered: a sampler or preview deck is expected to be swapped while it sounds.
enum class LoadIntoPlaying {
    // Load straight away, stopping the deck (stock Mixxx).
    Allow = 0,
    // Hold the load and ask on the notification strip; it goes ahead only if
    // the DJ taps LOAD before the strip times out.
    Warn = 1,
    // Refuse the load and say so on the notification strip.
    Prevent = 2,
};

inline const ConfigKey kLoadIntoPlayingConfigKey =
        ConfigKey(QStringLiteral("[BiteDJ]"), QStringLiteral("load_into_playing"));
constexpr LoadIntoPlaying kLoadIntoPlayingDefault = LoadIntoPlaying::Allow;
