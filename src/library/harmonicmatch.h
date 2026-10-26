#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>

#include "proto/keys.pb.h"

class ControlProxy;

/// Bite DJ: decides which library rows are harmonically compatible with the
/// track on the reference deck, so their rows can be tinted green while
/// the DJ browses for the next one.
///
/// The reference is the *playing* key of a deck (`[ChannelN],key`), not the
/// file key: with keylock off a pitched-up track really is in a different key,
/// and that is what the next track has to sit against. The deck is, in order:
/// - the audible one, PlayerInfo's choice (the signal the played tint and the
///   history use), so with the mix running the deck out front wins over one
///   cued in the headphones;
/// - else a playing one: decks here start with their fader down and the skin
///   has none, so without a controller nothing is ever audible;
/// - else the one a track was loaded into last.
/// Among several playing decks the one loaded last wins.
///
/// Compatibility is measured on the Camelot wheel (circle of fifths):
/// - same mode (both major or both minor) and at most `range` steps apart, or
/// - the relative major/minor (same Camelot number, other mode), once the range
///   is at least one step: it shares every note with the reference.
class HarmonicMatch : public QObject {
    Q_OBJECT

  public:
    /// What the Settings button cycles through, in order. The value is also
    /// the `[Library],key_match` control value and the persisted config value.
    enum class Mode {
        Off = 0,
        SameKey = 1,
        OneStep = 2,
        TwoSteps = 3,
    };
    static constexpr int kModeCount = 4;
    static constexpr Mode kModeDefault = Mode::Off;

    /// Text colour of a compatible track's row. The skin's existing accent
    /// green (loop pads, loop buttons), readable on the track table's #111/#222.
    static const QColor kMatchColor;

    static HarmonicMatch& instance();
    static void destroy();

    /// Pure compatibility rule, see the class comment. `range` is the number of
    /// circle-of-fifths steps allowed; 0 means the identical key only.
    static bool isCompatible(
            mixxx::track::io::key::ChromaticKey reference,
            mixxx::track::io::key::ChromaticKey key,
            int range);

    Mode mode() const {
        return m_mode;
    }
    mixxx::track::io::key::ChromaticKey referenceKey() const {
        return m_referenceKey;
    }

    /// True while highlighting is switched on *and* something is audible with
    /// a known key. Callers check this first to skip the per-cell key lookup.
    bool isActive() const {
        return m_mode != Mode::Off &&
                m_referenceKey != mixxx::track::io::key::INVALID;
    }

    bool matchesKey(mixxx::track::io::key::ChromaticKey key) const;
    /// Same, for libraries that only carry the key as text (Rekordbox, Serato,
    /// file tags in Browse). Parsed results are cached: a library holds a few
    /// dozen distinct key spellings, and this runs for every painted row.
    bool matchesKeyText(const QString& keyText) const;

  public slots:
    void setMode(HarmonicMatch::Mode mode);
    /// Override the reference key directly. Normally driven by the reference
    /// deck's `[ChannelN],key`; exposed for tests.
    void setReferenceKey(mixxx::track::io::key::ChromaticKey key);

  signals:
    /// The mode or the reference key changed; views should repaint rows.
    void matchChanged();

  private slots:
    void slotCurrentPlayingDeckChanged(int deck);
    void slotTrackChanged(const QString& group);
    void slotNumDecksChanged(double value);
    void updateReferenceKey();

  private:
    HarmonicMatch();
    ~HarmonicMatch() override;

    /// The deck the reference key is read from, see the class comment; -1 if
    /// none.
    int referenceDeck() const;

    struct DeckControls {
        ControlProxy* pPlay;
        ControlProxy* pKey;
    };

    Mode m_mode;
    mixxx::track::io::key::ChromaticKey m_referenceKey;
    /// PlayerInfo's audible deck, -1 if none.
    int m_audibleDeck;
    /// Decks holding a track, the one loaded last at the end.
    QList<int> m_loadedDecks;
    /// Owned (parented proxies), indexed by deck; grows with [App],num_decks.
    QList<DeckControls> m_decks;
    ControlProxy* m_pNumDecks;
    mutable QHash<QString, mixxx::track::io::key::ChromaticKey> m_parsedKeyTexts;

    static HarmonicMatch* s_pInstance;
};
