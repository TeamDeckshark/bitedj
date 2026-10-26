#pragma once

#include <QRegularExpression>
#include <QString>
#include <memory>

#include "library/searchquery.h"
#include "util/class.h"

class TrackCollection;
class QueryNode;
class AndNode;

class SearchQueryParser {
  public:
    explicit SearchQueryParser(TrackCollection* pTrackCollection, QStringList searchColumns);

    void setSearchColumns(QStringList searchColumns);

    /// Bite DJ: the columns the searched table really has. External library
    /// tables (Rekordbox, Serato) have only a subset of Mixxx's, e.g. no
    /// album_artist, composer or key_id, and a filter naming a missing column
    /// would fail the whole query and empty the view. Filters are narrowed to
    /// the columns given; a filter left with none matches no track (or every
    /// track, for the explicit-empty `field:""` search), and key filters match
    /// the key text when there is no key_id. Not calling this keeps the stock
    /// behaviour of assuming Mixxx's own library columns.
    void restrictToTableColumns(const QStringList& tableColumns);

    std::unique_ptr<QueryNode> parseQuery(
            const QString& query,
            const QString& extraFilter) const;

    /// splits the query into a list of terms
    static QStringList splitQueryIntoWords(const QString& query);
    /// checks if the changed search query is less specific then the original term
    static bool queryIsLessSpecific(const QString& original, const QString& changed);

  private:
    void parseTokens(QStringList tokens,
                     AndNode* pQuery) const;

    std::unique_ptr<AndNode> parseAndNode(const QString& query) const;
    std::unique_ptr<OrNode> parseOrNode(const QString& query) const;

    bool isFieldUnavailable(const QString& field) const;
    static std::unique_ptr<QueryNode> unavailableFieldNode(const QString& argument);

    struct TextArgumentResult {
        QString argument;
        StringMatch mode;
    };

    TextArgumentResult getTextArgument(QString argument,
            QStringList* tokens,
            bool removeLeadingEqualsSign = true) const;

    TrackCollection* m_pTrackCollection;
    QStringList m_queryColumns;
    bool m_searchCrates;
    QStringList m_textFilters;
    QStringList m_numericFilters;
    QStringList m_specialFilters;
    QHash<QString, QStringList> m_fieldToSqlColumns;
    bool m_restrictedToTableColumns;
    bool m_keyIdAvailable;

    QRegularExpression m_textFilterMatcher;
    QRegularExpression m_crateFilterMatcher;
    QRegularExpression m_numericFilterMatcher;
    QRegularExpression m_specialFilterMatcher;

    DISALLOW_COPY_AND_ASSIGN(SearchQueryParser);
};
