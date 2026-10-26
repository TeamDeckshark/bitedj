#pragma once

#include <QString>

namespace mixxx {
namespace rekordboxconstants {
const QString beatsSubversion = QStringLiteral("Rekordbox USB drive");
// Bite DJ: the same tag on a key taken from a rekordbox export, so that one can
// be told apart from Bite DJ's own key detection (and a key the DJ typed in,
// which has no sub-version at all) wherever keys are stored or re-analysed.
const QString keysSubversion = QStringLiteral("Rekordbox USB drive");
}
} // namespace mixxx
