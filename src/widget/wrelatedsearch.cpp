#include "widget/wrelatedsearch.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QStyle>
#include <algorithm>

#include "control/controlproxy.h"
#include "library/library.h"
#include "mixer/playermanager.h"
#include "moc_wrelatedsearch.cpp"
#include "skin/legacy/skincontext.h"
#include "track/keyutils.h"

namespace {
const char* kChipObjectName = "RelatedSearchChip";
const char* kSourceObjectName = "RelatedSearchSource";
const char* kPlaceholderObjectName = "RelatedSearchPlaceholder";
const char* kSelectedProperty = "selected";
// The decks the chips can follow instead of the selected track.
constexpr int kNumSourceDecks = 2;


// Repolishes a widget so a property change picked up by the stylesheet
// ([selected]) is actually repainted.
void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}

QString chipText(const mixxx::library::RelatedTrackSearch& search) {
    return search.label + QChar(' ') + search.value;
}
} // namespace

WRelatedSearch::WRelatedSearch(QWidget* parent, Library* pLibrary)
        : WWidget(parent),
          m_pLibrary(pLibrary),
          m_pLayout(new QHBoxLayout(this)),
          m_pPlaceholder(nullptr),
          m_source(kSourceSelection),
          m_available(true),
          m_seeded(false),
          m_pressed(false) {
    setAttribute(Qt::WA_StyledBackground, true);
    m_pLayout->setContentsMargins(0, 0, 0, 0);
    m_pLayout->setSpacing(6);

    for (int deck = 0; deck < kNumSourceDecks; ++deck) {
        const QString group = PlayerManager::groupForDeck(deck);
        // key and bpm are what the deck sounds like: the file's shifted by
        // pitch and rate, as the harmonic match uses.
        DeckControls controls{
                new ControlProxy(group,
                        QStringLiteral("track_loaded"),
                        this,
                        ControlFlag::NoAssertIfMissing),
                new ControlProxy(group,
                        QStringLiteral("key"),
                        this,
                        ControlFlag::NoAssertIfMissing),
                new ControlProxy(group,
                        QStringLiteral("bpm"),
                        this,
                        ControlFlag::NoAssertIfMissing)};
        controls.pLoaded->connectValueChanged(this, &WRelatedSearch::onDeckChanged);
        controls.pKey->connectValueChanged(this, &WRelatedSearch::onDeckChanged);
        controls.pBpm->connectValueChanged(this, &WRelatedSearch::onDeckChanged);
        m_decks.append(controls);

        auto* pSource = new QPushButton(QStringLiteral("D") + QString::number(deck + 1), this);
        pSource->setObjectName(kSourceObjectName);
        // For the per-deck accent in the stylesheet.
        pSource->setProperty("deck", deck + 1);
        pSource->setFocusPolicy(Qt::NoFocus);
        pSource->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        connect(pSource, &QPushButton::clicked, this, &WRelatedSearch::onSourceClicked);
        m_pLayout->addWidget(pSource);
        m_sourceChips.append(pSource);
    }
    updateSourceChips();

    if (pLibrary) {
        connect(pLibrary,
                &Library::relatedTrackSearchesChanged,
                this,
                &WRelatedSearch::onSearchesChanged);
        // Whatever the search box shows, typed or restored with a view, so
        // the chip of the search that is running stays lit.
        connect(pLibrary,
                &Library::search,
                this,
                &WRelatedSearch::onSearchTextChanged);
        connect(pLibrary,
                &Library::restoreSearch,
                this,
                &WRelatedSearch::onSearchTextChanged);
    }
    rebuild();
}

WRelatedSearch::~WRelatedSearch() = default;

void WRelatedSearch::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by the library.
}

void WRelatedSearch::onSearchesChanged(
        const QList<mixxx::library::RelatedTrackSearch>& searches,
        bool available) {
    if (m_seeded && searches == m_selectionSearches && available == m_available) {
        return;
    }
    m_seeded = true;
    m_selectionSearches = searches;
    m_available = available;
    // A deck source holds until it is tapped off: a search that filters the
    // selected row out moves the table's selection on its own, which must not
    // take the chips away from the deck the DJ picked.
    if (m_source == kSourceSelection) {
        m_searches = searches;
        rebuild();
    }
}

void WRelatedSearch::onDeckChanged() {
    if (m_source != kSourceSelection && !isDeckLoaded(m_source)) {
        // Ejected: nothing left to follow there.
        setSource(kSourceSelection);
        return;
    }
    updateSourceChips();
    showSearches();
}

bool WRelatedSearch::isDeckLoaded(int deck) const {
    return deck >= 0 && deck < m_decks.size() && m_decks.at(deck).pLoaded->toBool();
}

QList<mixxx::library::RelatedTrackSearch> WRelatedSearch::deckSearches(int deck) const {
    if (!isDeckLoaded(deck)) {
        return {};
    }
    const DeckControls& controls = m_decks.at(deck);
    return mixxx::library::relatedTrackSearchesForDeck(
            KeyUtils::keyToString(KeyUtils::keyFromNumericValue(controls.pKey->get())),
            controls.pBpm->get());
}

void WRelatedSearch::setSource(int source) {
    if (source != kSourceSelection && !isDeckLoaded(source)) {
        return;
    }
    const bool changed = source != m_source;
    m_source = source;
    updateSourceChips();
    if (changed) {
        // The placeholder's text depends on the source too.
        m_searches = searchesForSource();
        rebuild();
    }
}

QList<mixxx::library::RelatedTrackSearch> WRelatedSearch::searchesForSource() const {
    return m_source == kSourceSelection ? m_selectionSearches : deckSearches(m_source);
}

void WRelatedSearch::showSearches() {
    // A deck's BPM follows its rate fader: only a change the chips show is
    // worth rebuilding them for.
    const QList<mixxx::library::RelatedTrackSearch> searches = searchesForSource();
    if (searches == m_searches) {
        return;
    }
    m_searches = searches;
    rebuild();
}

void WRelatedSearch::updateSourceChips() {
    for (int deck = 0; deck < m_sourceChips.size(); ++deck) {
        QPushButton* pSource = m_sourceChips.at(deck);
        const bool enabled = isDeckLoaded(deck);
        const bool selected = m_source == deck;
        const QVariant current = pSource->property(kSelectedProperty);
        if (current.isValid() && current.toBool() == selected &&
                pSource->isEnabled() == enabled) {
            continue;
        }
        pSource->setEnabled(enabled);
        pSource->setProperty(kSelectedProperty, selected);
        restyle(style(), pSource);
    }
}

void WRelatedSearch::onSourceClicked() {
    // Desktop/mouse path, as onChipClicked.
    const int deck = m_sourceChips.indexOf(qobject_cast<QPushButton*>(sender()));
    if (deck >= 0) {
        setSource(m_source == deck ? kSourceSelection : deck);
    }
}

void WRelatedSearch::onSearchTextChanged(const QString& text) {
    m_searchText = text.trimmed();
    updateSelectedChip();
}

void WRelatedSearch::clearChildren() {
    // Detach immediately (so they stop painting) but deleteLater, so Qt can
    // finish dispatching the event that triggered the rebuild.
    for (QPushButton* pChip : std::as_const(m_chips)) {
        pChip->setParent(nullptr);
        pChip->deleteLater();
    }
    m_chips.clear();
    if (m_pPlaceholder) {
        m_pPlaceholder->setParent(nullptr);
        m_pPlaceholder->deleteLater();
        m_pPlaceholder = nullptr;
    }
}

void WRelatedSearch::rebuild() {
    clearChildren();
    // The trailing stretch of the last build, if any.
    for (int i = m_pLayout->count() - 1; i >= 0; --i) {
        if (m_pLayout->itemAt(i)->spacerItem()) {
            delete m_pLayout->takeAt(i);
        }
    }

    if (m_searches.isEmpty()) {
        QString text;
        if (m_source != kSourceSelection) {
            text = tr("No key or BPM on this deck yet");
        } else if (!m_seeded) {
            text = tr("Select a track or deck to find related ones");
        } else if (!m_available) {
            text = tr("Related search needs a Rekordbox or Serato library");
        } else {
            text = tr("Nothing to search by for this track");
        }
        m_pPlaceholder = new QLabel(text, this);
        m_pPlaceholder->setObjectName(kPlaceholderObjectName);
        m_pPlaceholder->setAlignment(Qt::AlignCenter);
        m_pLayout->addWidget(m_pPlaceholder);
        restyle(style(), m_pPlaceholder);
        return;
    }
    for (int i = 0; i < m_searches.size(); ++i) {
        auto* pChip = new QPushButton(chipText(m_searches.at(i)), this);
        pChip->setObjectName(kChipObjectName);
        pChip->setFocusPolicy(Qt::NoFocus);
        connect(pChip, &QPushButton::clicked, this, &WRelatedSearch::onChipClicked);
        // The mixing chips (KEY, BPM) are a few characters and the ones that
        // matter mid-set: they keep the width their text needs. The name
        // chips are Ignored and share what is left, so a long artist is
        // elided to its share instead of squeezing the key off the row.
        const QString& query = m_searches.at(i).query;
        const bool isMixing = query.startsWith(QStringLiteral("~key:")) ||
                query.startsWith(QStringLiteral("bpm:"));
        restyle(style(), pChip);
        pChip->setSizePolicy(isMixing ? QSizePolicy::Fixed : QSizePolicy::Ignored,
                QSizePolicy::Preferred);
        // The chip's width is only known once the layout has placed it, which
        // does not resize this widget: elide on the chip's own resize.
        pChip->installEventFilter(this);
        m_pLayout->addWidget(pChip, isMixing ? 0 : 1);
        m_chips.append(pChip);
    }
    // Without a name chip to take up the row (a deck offers KEY and BPM
    // only), the fixed chips would be spread across it.
    if (std::none_of(m_chips.cbegin(), m_chips.cend(), [](const QPushButton* pChip) {
            return pChip->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored;
        })) {
        m_pLayout->addStretch(1);
    }
    updateSelectedChip();
    applyElidedLabels();
}

void WRelatedSearch::updateSelectedChip() {
    for (int i = 0; i < m_chips.size(); ++i) {
        QPushButton* pChip = m_chips.at(i);
        const bool selected = mixxx::library::isRelatedTrackSearchActive(
                m_searchText, m_searches.at(i));
        const QVariant current = pChip->property(kSelectedProperty);
        if (current.isValid() && current.toBool() == selected) {
            continue;
        }
        pChip->setProperty(kSelectedProperty, selected);
        restyle(style(), pChip);
    }
}

bool WRelatedSearch::eventFilter(QObject* pObject, QEvent* pEvent) {
    if (pEvent->type() == QEvent::Resize) {
        applyElidedLabels();
    }
    return WWidget::eventFilter(pObject, pEvent);
}

void WRelatedSearch::resizeEvent(QResizeEvent* e) {
    WWidget::resizeEvent(e);
    applyElidedLabels();
}

void WRelatedSearch::applyElidedLabels() {
    for (int i = 0; i < m_chips.size(); ++i) {
        QPushButton* pChip = m_chips.at(i);
        // Leave room for the chip's own padding.
        constexpr int kTextInsetPx = 16;
        const int available = pChip->width() - kTextInsetPx;
        if (available <= 0) {
            continue;
        }
        const QString text = pChip->fontMetrics().elidedText(
                chipText(m_searches.at(i)), Qt::ElideRight, available);
        // Fixed chips are sized from their text; eliding it would shrink them.
        if (pChip->sizePolicy().horizontalPolicy() != QSizePolicy::Fixed &&
                text != pChip->text()) {
            pChip->setText(text);
        }
    }
}

void WRelatedSearch::activate(int index) {
    if (!m_pLibrary || index < 0 || index >= m_searches.size()) {
        return;
    }
    // The chips AND together: a tap adds the chip's terms to the search, and
    // tapping a lit chip takes them out again.
    m_pLibrary->searchInCurrentView(mixxx::library::toggleRelatedTrackSearch(
            m_searchText, m_searches.at(index)));
}

void WRelatedSearch::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }
    m_pressed = true;
    e->accept();
}

void WRelatedSearch::mouseReleaseEvent(QMouseEvent* e) {
    const bool pressed = m_pressed;
    m_pressed = false;
    if (pressed) {
        const int index = chipIndexAt(e->globalPosition().toPoint());
        if (index >= 0) {
            activate(index);
            e->accept();
            return;
        }
        const int deck = sourceIndexAt(e->globalPosition().toPoint());
        if (deck >= 0) {
            setSource(m_source == deck ? kSourceSelection : deck);
            e->accept();
            return;
        }
    }
    WWidget::mouseReleaseEvent(e);
}

void WRelatedSearch::onChipClicked() {
    // Desktop/mouse path: a real QMouseEvent reaches the child button directly
    // and emits clicked(). (On the touchscreen this never fires; the release
    // handler above does the dispatching instead.)
    activate(m_chips.indexOf(qobject_cast<QPushButton*>(sender())));
}

int WRelatedSearch::chipIndexAt(const QPoint& globalPos) const {
    for (int i = 0; i < m_chips.size(); ++i) {
        const QPushButton* pChip = m_chips.at(i);
        if (pChip->rect().contains(pChip->mapFromGlobal(globalPos))) {
            return i;
        }
    }
    return -1;
}

int WRelatedSearch::sourceIndexAt(const QPoint& globalPos) const {
    for (int deck = 0; deck < m_sourceChips.size(); ++deck) {
        const QPushButton* pSource = m_sourceChips.at(deck);
        if (pSource->isEnabled() &&
                pSource->rect().contains(pSource->mapFromGlobal(globalPos))) {
            return deck;
        }
    }
    return -1;
}
