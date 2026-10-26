#pragma once

#include <QList>
#include <QPointer>
#include <QPushButton>
#include <QString>

#include "library/relatedtracksearch.h"
#include "widget/wwidget.h"

class ControlProxy;
class Library;
class QDomNode;
class QHBoxLayout;
class QLabel;
class SkinContext;

// Bite DJ: "search related tracks" for the touchscreen — the library's chip
// row under the search box (`<RelatedSearch>`, shown by the RELATED button).
// One chip per related search of the last selected track: KEY (harmonic), BPM
// (the fuzzy range), ARTIST, GENRE, ALBUM. Tapping one adds its terms to the
// search box and runs the search in the view that is open, so in a drive's All
// Tracks it searches the whole drive; lit chips AND together, and tapping a
// lit one takes its terms out again.
//
// The chips follow the last single row selected rather than the selection
// itself, so they survive the related search narrowing the list. Browse's
// folder listings only match plain words, so a track selected there gets a
// note saying so instead of chips that would find nothing.
//
// D1 and D2 at the row's start switch the chips to what deck 1 or 2 is
// playing instead: KEY and BPM only, the playing key and BPM (pitch and rate
// included), following the deck live. The deck holds until the lit one is
// tapped again or the deck is ejected, then the chips are the selected
// track's again: a search can move the table's selection by itself, so
// selecting is no sign the DJ is done with the deck. A source is greyed out
// while its deck is empty.
//
// The chips are plain QPushButtons for styling only: on the touchscreen they
// never see a mouse event of their own (WWidget::event synthesizes touch ->
// mouse to *this* widget), so the tap is hit-tested here, as in WSamplerDrive.
class WRelatedSearch : public WWidget {
    Q_OBJECT
  public:
    explicit WRelatedSearch(QWidget* parent, Library* pLibrary);
    ~WRelatedSearch() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private slots:
    void onSearchesChanged(
            const QList<mixxx::library::RelatedTrackSearch>& searches,
            bool available);
    void onSearchTextChanged(const QString& text);
    void onChipClicked();
    void onSourceClicked();
    void onDeckChanged();

  private:
    // m_source while the chips follow the selected track.
    static constexpr int kSourceSelection = -1;

    void setSource(int source);
    // Puts the current source's searches on the chips, rebuilding them only
    // if they changed.
    void showSearches();
    void updateSourceChips();
    QList<mixxx::library::RelatedTrackSearch> searchesForSource() const;
    QList<mixxx::library::RelatedTrackSearch> deckSearches(int deck) const;
    bool isDeckLoaded(int deck) const;
    void rebuild();
    void clearChildren();
    void updateSelectedChip();
    void applyElidedLabels();
    void activate(int index);
    int chipIndexAt(const QPoint& globalPos) const;
    int sourceIndexAt(const QPoint& globalPos) const;

    struct DeckControls {
        ControlProxy* pLoaded;
        ControlProxy* pKey;
        ControlProxy* pBpm;
    };

    QPointer<Library> m_pLibrary;
    QHBoxLayout* m_pLayout;
    // D1, D2: built once, ahead of the chips.
    QList<QPushButton*> m_sourceChips;
    // Owned (parented proxies), indexed by deck.
    QList<DeckControls> m_decks;
    QList<QPushButton*> m_chips;
    // What the chips show, from the selected track or a deck.
    QList<mixxx::library::RelatedTrackSearch> m_searches;
    // The last selected track's searches, kept while a deck is the source.
    QList<mixxx::library::RelatedTrackSearch> m_selectionSearches;
    QLabel* m_pPlaceholder;
    // kSourceSelection, or the deck index the chips follow.
    int m_source;
    // Whether the last selected track was in a view that can run the related
    // searches; Browse's folder listings cannot.
    bool m_available;
    // Whether a track has been selected since startup.
    bool m_seeded;
    QString m_searchText;
    // Whether the press that would make a release a tap landed on us.
    bool m_pressed;
};
