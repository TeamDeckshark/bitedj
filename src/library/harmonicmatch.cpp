#include "library/harmonicmatch.h"

#include <cstdlib>

#include "control/controlproxy.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_harmonicmatch.cpp"
#include "track/keyutils.h"

using mixxx::track::io::key::ChromaticKey;

// static
const QColor HarmonicMatch::kMatchColor = QColor(QStringLiteral("#6ee128"));

// static
HarmonicMatch* HarmonicMatch::s_pInstance = nullptr;

HarmonicMatch::HarmonicMatch()
        : m_mode(kModeDefault),
          m_referenceKey(mixxx::track::io::key::INVALID),
          m_audibleDeck(-1),
          m_pNumDecks(new ControlProxy(QStringLiteral("[App]"),
                  QStringLiteral("num_decks"),
                  this,
                  ControlFlag::NoAssertIfMissing)) {
    // Decks are added after this is constructed; their controls only exist
    // once num_decks counts them.
    m_pNumDecks->connectValueChanged(this, &HarmonicMatch::slotNumDecksChanged);
    slotNumDecksChanged(m_pNumDecks->get());
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingDeckChanged,
            this,
            &HarmonicMatch::slotCurrentPlayingDeckChanged);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &HarmonicMatch::slotTrackChanged);
    for (int deck = 0; deck < m_decks.size(); ++deck) {
        if (PlayerInfo::instance().getTrackInfo(PlayerManager::groupForDeck(deck))) {
            m_loadedDecks.append(deck);
        }
    }
    m_audibleDeck = PlayerInfo::instance().getCurrentPlayingDeck();
    updateReferenceKey();
}

HarmonicMatch::~HarmonicMatch() = default;

// static
HarmonicMatch& HarmonicMatch::instance() {
    // Lazily created, like PlayedTracks, so the library models can consult it
    // however they are constructed (tests build them without CoreServices).
    if (!s_pInstance) {
        s_pInstance = new HarmonicMatch();
    }
    return *s_pInstance;
}

// static
void HarmonicMatch::destroy() {
    delete s_pInstance;
    s_pInstance = nullptr;
}

// static
bool HarmonicMatch::isCompatible(ChromaticKey reference, ChromaticKey key, int range) {
    if (range < 0 ||
            reference == mixxx::track::io::key::INVALID ||
            key == mixxx::track::io::key::INVALID ||
            !ChromaticKey_IsValid(reference) ||
            !ChromaticKey_IsValid(key)) {
        return false;
    }
    // The OpenKey number is the position on the circle of fifths (1-12); major
    // and minor sit on the outer and inner ring of the same radial.
    const int distance = std::abs(KeyUtils::keyToOpenKeyNumber(reference) -
            KeyUtils::keyToOpenKeyNumber(key));
    const int steps = std::min(distance, 12 - distance);
    if (KeyUtils::keyIsMajor(reference) == KeyUtils::keyIsMajor(key)) {
        return steps <= range;
    }
    // Relative major/minor: the same notes, so one move away at most.
    return steps == 0 && range >= 1;
}

bool HarmonicMatch::matchesKey(ChromaticKey key) const {
    if (!isActive()) {
        return false;
    }
    return isCompatible(m_referenceKey, key, static_cast<int>(m_mode) - 1);
}

bool HarmonicMatch::matchesKeyText(const QString& keyText) const {
    if (!isActive() || keyText.isEmpty()) {
        return false;
    }
    auto it = m_parsedKeyTexts.constFind(keyText);
    if (it == m_parsedKeyTexts.constEnd()) {
        it = m_parsedKeyTexts.insert(keyText, KeyUtils::guessKeyFromText(keyText));
    }
    return matchesKey(it.value());
}

void HarmonicMatch::setMode(HarmonicMatch::Mode mode) {
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    emit matchChanged();
}

void HarmonicMatch::setReferenceKey(ChromaticKey key) {
    if (!ChromaticKey_IsValid(key)) {
        key = mixxx::track::io::key::INVALID;
    }
    if (m_referenceKey == key) {
        return;
    }
    m_referenceKey = key;
    // Only a visible change is worth a repaint of every title.
    if (m_mode != Mode::Off) {
        emit matchChanged();
    }
}

void HarmonicMatch::slotNumDecksChanged(double value) {
    const int numDecks = static_cast<int>(value);
    while (m_decks.size() < numDecks) {
        const QString group = PlayerManager::groupForDeck(m_decks.size());
        // [ChannelN],key is the key the deck is actually sounding in: the file
        // key shifted by pitch (keylock off) or by the key-shift controls. It
        // is rounded to whole keys, so it only changes when a semitone is
        // crossed.
        DeckControls controls{
                new ControlProxy(group,
                        QStringLiteral("play"),
                        this,
                        ControlFlag::NoAssertIfMissing),
                new ControlProxy(group,
                        QStringLiteral("key"),
                        this,
                        ControlFlag::NoAssertIfMissing)};
        controls.pPlay->connectValueChanged(this, &HarmonicMatch::updateReferenceKey);
        controls.pKey->connectValueChanged(this, &HarmonicMatch::updateReferenceKey);
        m_decks.append(controls);
    }
    updateReferenceKey();
}

void HarmonicMatch::slotCurrentPlayingDeckChanged(int deck) {
    m_audibleDeck = deck;
    updateReferenceKey();
}

void HarmonicMatch::slotTrackChanged(const QString& group) {
    int number = 0;
    if (!PlayerManager::isDeckGroup(group, &number) || number < 1) {
        return;
    }
    const int deck = number - 1;
    m_loadedDecks.removeAll(deck);
    if (PlayerInfo::instance().getTrackInfo(group)) {
        m_loadedDecks.append(deck);
    }
    updateReferenceKey();
}

int HarmonicMatch::referenceDeck() const {
    // PlayerInfo only re-polls the audible deck every 2 s, so it can still
    // name a deck that was just ejected.
    if (m_loadedDecks.contains(m_audibleDeck)) {
        return m_audibleDeck;
    }
    for (auto it = m_loadedDecks.crbegin(); it != m_loadedDecks.crend(); ++it) {
        if (*it < m_decks.size() && m_decks.at(*it).pPlay->toBool()) {
            return *it;
        }
    }
    return m_loadedDecks.isEmpty() ? -1 : m_loadedDecks.last();
}

void HarmonicMatch::updateReferenceKey() {
    const int deck = referenceDeck();
    if (deck < 0 || deck >= m_decks.size()) {
        // No track on any deck: nothing to match against.
        setReferenceKey(mixxx::track::io::key::INVALID);
        return;
    }
    setReferenceKey(KeyUtils::keyFromNumericValue(m_decks.at(deck).pKey->get()));
}
