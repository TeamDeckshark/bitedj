#pragma once

// Track time display settings, shared by the deck preferences that own them and
// by the widgets that render a track time. Kept out of dlgprefdeck.h so that
// including them does not drag in the generated preferences dialog UI.
namespace TrackTime {
enum class DisplayMode {
    ELAPSED,
    REMAINING,
    ELAPSED_AND_REMAINING,
};

enum class DisplayFormat {
    TRADITIONAL,
    TRADITIONAL_COARSE,
    SECONDS,
    SECONDS_LONG,
    KILO_SECONDS,
    HECTO_SECONDS,
};
} // namespace TrackTime
