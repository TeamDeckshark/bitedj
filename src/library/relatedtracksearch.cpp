#include "library/relatedtracksearch.h"

#include <QCoreApplication>
#include <QLocale>
#include <QModelIndex>

#include "library/basesqltablemodel.h"
#include "library/searchquery.h"
#include "library/trackmodel.h"
#include "track/keyutils.h"

namespace mixxx {
namespace library {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("RelatedTrackSearch", text);
}

QVariant cell(const QModelIndex& index,
        const TrackModel& trackModel,
        TrackModel::SortColumnId column,
        int role = Qt::DisplayRole) {
    const int columnIndex = trackModel.columnIndexFromSortColumnId(column);
    if (columnIndex < 0) {
        return {};
    }
    return index.sibling(index.row(), columnIndex).data(role);
}

// A quoted search argument ends at the next quote, so a value containing one
// is cut there. The text filters match substrings, so the cut value still
// finds the track it came from.
QString quotedArgument(const QString& value) {
    return QChar('"') + value.section(QChar('"'), 0, 0).trimmed() + QChar('"');
}

void appendTextSearch(QList<RelatedTrackSearch>* pSearches,
        const QModelIndex& index,
        const TrackModel& trackModel,
        TrackModel::SortColumnId column,
        const QString& label,
        const QString& field) {
    const QString value = cell(index, trackModel, column).toString().trimmed();
    if (value.section(QChar('"'), 0, 0).trimmed().isEmpty()) {
        return;
    }
    pSearches->append({label, value, field + QChar(':') + quotedArgument(value)});
}

RelatedTrackSearch keySearch(const QString& keyText) {
    return {tr("KEY"), keyText, QStringLiteral("~key:") + quotedArgument(keyText)};
}

RelatedTrackSearch bpmSearch(double bpm) {
    // The fuzzy BPM search's range, the one the search box's ~bpm: uses.
    QString bpmArgument = QString::number(bpm);
    const BpmFilterNode bpmNode(bpmArgument, true /* fuzzy */);
    const auto [lowerBound, upperBound] = bpmNode.getBpmRange();
    return {tr("BPM"),
            QString::number(qRound(bpm)),
            QStringLiteral("bpm:>=") + QString::number(lowerBound) +
                    QStringLiteral(" bpm:<=") + QString::number(upperBound)};
}

// The part of a filter term that names what it filters, e.g. `~key:` or
// `bpm:>=`: a chip term replaces the search's terms with the same one. Empty
// for plain words, which are never replaced.
QString termField(const QString& term) {
    const int colon = term.indexOf(QChar(':'));
    if (colon <= 0 || term.left(colon).contains(QChar('"'))) {
        return {};
    }
    int end = colon + 1;
    while (end < term.size() &&
            (term.at(end) == QChar('<') || term.at(end) == QChar('>') ||
                    term.at(end) == QChar('='))) {
        ++end;
    }
    return term.left(end).toLower();
}

} // namespace

QStringList relatedSearchTerms(const QString& searchText) {
    QStringList terms;
    QString term;
    bool quoted = false;
    for (const QChar c : searchText) {
        if (c == QChar('"')) {
            quoted = !quoted;
        } else if (c.isSpace() && !quoted) {
            if (!term.isEmpty()) {
                terms.append(term);
                term.clear();
            }
            continue;
        }
        term.append(c);
    }
    if (!term.isEmpty()) {
        terms.append(term);
    }
    return terms;
}

bool isRelatedTrackSearchActive(const QString& searchText,
        const RelatedTrackSearch& search) {
    const QStringList searchTerms = relatedSearchTerms(searchText);
    const QStringList chipTerms = relatedSearchTerms(search.query);
    if (chipTerms.isEmpty()) {
        return false;
    }
    for (const QString& term : chipTerms) {
        if (!searchTerms.contains(term)) {
            return false;
        }
    }
    return true;
}

QString toggleRelatedTrackSearch(const QString& searchText,
        const RelatedTrackSearch& search) {
    const QStringList chipTerms = relatedSearchTerms(search.query);
    const bool active = isRelatedTrackSearchActive(searchText, search);
    QStringList fields;
    for (const QString& term : chipTerms) {
        const QString field = termField(term);
        if (!field.isEmpty()) {
            fields.append(field);
        }
    }
    QStringList terms;
    for (const QString& term : relatedSearchTerms(searchText)) {
        if (chipTerms.contains(term) ||
                (!active && fields.contains(termField(term)))) {
            continue;
        }
        terms.append(term);
    }
    if (!active) {
        terms.append(chipTerms);
    }
    return terms.join(QChar(' '));
}

bool supportsRelatedTrackSearch(const QModelIndex& index) {
    // A BaseSqlTableModel hands the search to the track cache's
    // SearchQueryParser; Browse's ProxyTrackModel does not.
    return dynamic_cast<const BaseSqlTableModel*>(index.model()) != nullptr;
}

QList<RelatedTrackSearch> relatedTrackSearchesForRow(const QModelIndex& index) {
    QList<RelatedTrackSearch> searches;
    if (!index.isValid() || !supportsRelatedTrackSearch(index)) {
        return searches;
    }
    const auto* pTrackModel = dynamic_cast<const TrackModel*>(index.model());
    if (!pTrackModel) {
        return searches;
    }

    // Mixing first, as in WSearchRelatedTracksMenu.
    const QString keyText =
            cell(index, *pTrackModel, TrackModel::SortColumnId::Key).toString().trimmed();
    if (KeyUtils::guessKeyFromText(keyText) != mixxx::track::io::key::INVALID) {
        searches.append(keySearch(keyText));
    }

    bool bpmOk = false;
    double bpm = cell(index,
            *pTrackModel,
            TrackModel::SortColumnId::Bpm,
            TrackModel::kDataExportRole)
                         .toDouble(&bpmOk);
    if (!bpmOk) {
        // Models without the export role (Browse) only have the cell's text,
        // which is formatted for the locale.
        bpm = QLocale().toDouble(
                cell(index, *pTrackModel, TrackModel::SortColumnId::Bpm).toString(),
                &bpmOk);
    }
    if (bpmOk && bpm > 0) {
        searches.append(bpmSearch(bpm));
    }

    appendTextSearch(&searches,
            index,
            *pTrackModel,
            TrackModel::SortColumnId::Artist,
            tr("ARTIST"),
            QStringLiteral("artist"));
    appendTextSearch(&searches,
            index,
            *pTrackModel,
            TrackModel::SortColumnId::Genre,
            tr("GENRE"),
            QStringLiteral("genre"));
    appendTextSearch(&searches,
            index,
            *pTrackModel,
            TrackModel::SortColumnId::Album,
            tr("ALBUM"),
            QStringLiteral("album"));
    return searches;
}

QList<RelatedTrackSearch> relatedTrackSearchesForDeck(
        const QString& keyText, double bpm) {
    QList<RelatedTrackSearch> searches;
    const QString key = keyText.trimmed();
    if (KeyUtils::guessKeyFromText(key) != mixxx::track::io::key::INVALID) {
        searches.append(keySearch(key));
    }
    // Rounded: the deck's BPM follows the rate fader, and the chip's query
    // must only move when its number does, or the lit chip would go out at
    // the slightest nudge.
    const double wholeBpm = qRound(bpm);
    if (wholeBpm > 0) {
        searches.append(bpmSearch(wholeBpm));
    }
    return searches;
}

} // namespace library
} // namespace mixxx
