#pragma once

// required for Qt-Macros
#include <qobjectdefs.h>

namespace mixxx {

namespace preferences {

inline namespace constants {
Q_NAMESPACE

// In order for this Q_NAMESPACE to work, all members of the namespace must
// be declared here. see QTBUG-68611

// Don't change these constants since they are stored in user configuration
// files.
enum class Tooltips {
    Off = 0,
    On = 1,
    OnlyInLibrary = 2,
};
Q_ENUM_NS(Tooltips);

// Settings to enable or disable the prevention to run the screensaver.
enum class ScreenSaver {
    Off = 0,
    On = 1,
    OnPlay = 2
};
Q_ENUM_NS(ScreenSaver);

} // namespace constants
} // namespace preferences
} // namespace mixxx
