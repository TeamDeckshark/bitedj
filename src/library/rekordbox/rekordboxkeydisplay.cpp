#include "library/rekordbox/rekordboxkeydisplay.h"

#include "library/rekordbox/rekordboxanalysis.h"
#include "moc_rekordboxkeydisplay.cpp"
#include "track/keyutils.h"

using mixxx::track::io::key::ChromaticKey;

// static
RekordboxKeyDisplay& RekordboxKeyDisplay::instance() {
    // Lazily created, like HarmonicMatch, so the models can consult it however
    // they are constructed (tests build them without a Library).
    static RekordboxKeyDisplay* s_pInstance = new RekordboxKeyDisplay();
    return *s_pInstance;
}

void RekordboxKeyDisplay::setEnabled(bool enabled) {
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    emit changed();
}

ChromaticKey RekordboxKeyDisplay::keyFor(const QString& location) const {
    if (!m_enabled || location.isEmpty()) {
        return mixxx::track::io::key::INVALID;
    }
    const auto it = m_keys.constFind(location);
    if (it != m_keys.constEnd()) {
        return *it;
    }
    ChromaticKey key = mixxx::track::io::key::INVALID;
    if (const auto entry = RekordboxAnalysis::lookup(location)) {
        key = KeyUtils::guessKeyFromText(entry->keyText);
    }
    // Misses are cached too: most of what Browse lists is on no export.
    m_keys.insert(location, key);
    return key;
}

void RekordboxKeyDisplay::exportChanged() {
    QMetaObject::invokeMethod(this, [this] {
        m_keys.clear();
        if (m_enabled) {
            emit changed();
        }
    });
}
