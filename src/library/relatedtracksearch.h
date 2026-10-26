#pragma once

#include <QList>
#include <QMetaType>
#include <QStringList>
#include <QString>

class QModelIndex;

namespace mixxx {
namespace library {

/// Bite DJ: one "find tracks like this one" search, as the related-search chip
/// row in the library offers it (WRelatedSearch). The touch UI has no context
/// menu to reach WSearchRelatedTracksMenu from, so this is its in-skin
/// counterpart, limited to the searches that matter mid-set.
struct RelatedTrackSearch {
    /// Short field name for the chip, e.g. "KEY".
    QString label;
    /// The selected track's value of that field, e.g. "8A".
    QString value;
    /// What goes into the search box, e.g. `~key:"8A"`.
    QString query;

    bool operator==(const RelatedTrackSearch& other) const {
        return label == other.label && value == other.value && query == other.query;
    }
};

/// Whether the view `index` is in understands the search language the related
/// searches are written in. The library-table views do; Browse's folder
/// listing only matches words in the cells.
bool supportsRelatedTrackSearch(const QModelIndex& index);

/// The related searches for the track in `index`'s row: harmonic key, BPM
/// range, artist, genre and album, skipping fields the row has no value for.
///
/// Read from the row's cells, never from TrackModel::getTrack(): for the
/// external libraries that call imports the track, and this runs for every
/// row the DJ merely selects.
QList<RelatedTrackSearch> relatedTrackSearchesForRow(const QModelIndex& index);

/// The related searches for what a deck is playing: KEY and BPM only, the
/// two that matter for the next track. `keyText` is the deck's playing key
/// (pitch shift included) and `bpm` its playing BPM (rate included), so the
/// next track is matched against what is heard, not the file. The BPM is
/// rounded to a whole number first. Either is skipped if it is unknown.
QList<RelatedTrackSearch> relatedTrackSearchesForDeck(
        const QString& keyText, double bpm);

/// The search box's text split into the terms the parser ANDs together, on
/// spaces outside quotes, so `artist:"Kalo Kalo" bpm:>=120` is two terms.
QStringList relatedSearchTerms(const QString& searchText);

/// Whether every term of `search` is in `searchText`, i.e. its chip is lit.
bool isRelatedTrackSearchActive(const QString& searchText,
        const RelatedTrackSearch& search);

/// The search box's text after tapping `search`'s chip. The chips combine:
/// a lit chip's terms are taken out of the search, any other chip's are added
/// to it, replacing terms on the same field (a KEY tapped for another track
/// replaces the old `~key:` term instead of ANDing two keys). Whatever else
/// the search holds, typed or from other chips, stays.
QString toggleRelatedTrackSearch(const QString& searchText,
        const RelatedTrackSearch& search);

} // namespace library
} // namespace mixxx

Q_DECLARE_METATYPE(QList<mixxx::library::RelatedTrackSearch>)
