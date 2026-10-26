#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include "proto/keys.pb.h"

/// Bite DJ: the key the library views show for a track a scanned rekordbox
/// export knows, whatever view lists it (Browse, the library, history...).
///
/// With Settings -> Library -> Key -> RB on (`[Library],rekordbox_key`), such a
/// track shows the key rekordbox exported for it, the same key a deck loads it
/// with on RB (RekordboxAnalysis). Off, every view shows what it always has:
/// file tags in Browse, the library's own column elsewhere. The Rekordbox view
/// itself lists the export's key either way.
///
/// Only the painted key changes: sorting and `~key:` searches still run on
/// each view's own column.
///
/// Asked per painted cell, so the export is looked up once per location and
/// cached until a drive is scanned or ejected (exportChanged()). GUI thread
/// only, like the views and the lookup itself.
class RekordboxKeyDisplay : public QObject {
    Q_OBJECT

  public:
    static RekordboxKeyDisplay& instance();

    bool isEnabled() const {
        return m_enabled;
    }
    void setEnabled(bool enabled);

    /// Rekordbox's key for the audio file at `location`, or INVALID when the
    /// option is off, no scanned export knows the file, or the export gives no
    /// key it is possible to read.
    mixxx::track::io::key::ChromaticKey keyFor(const QString& location) const;

    /// A scanned export was added, replaced or dropped. Safe from any thread;
    /// the cache is cleared on the GUI thread.
    void exportChanged();

  signals:
    /// The keys shown may have changed; views should repaint.
    void changed();

  private:
    RekordboxKeyDisplay() = default;

    bool m_enabled = false;
    mutable QHash<QString, mixxx::track::io::key::ChromaticKey> m_keys;
};
