#include "library/relatedtracksearch.h"

#include <gtest/gtest.h>

namespace mixxx {
namespace library {

namespace {

const RelatedTrackSearch kKey{QStringLiteral("KEY"),
        QStringLiteral("8A"),
        QStringLiteral("~key:\"8A\"")};
const RelatedTrackSearch kOtherKey{QStringLiteral("KEY"),
        QStringLiteral("9A"),
        QStringLiteral("~key:\"9A\"")};
const RelatedTrackSearch kBpm{QStringLiteral("BPM"),
        QStringLiteral("124"),
        QStringLiteral("bpm:>=120 bpm:<=128")};
const RelatedTrackSearch kArtist{QStringLiteral("ARTIST"),
        QStringLiteral("Kalo Kalo"),
        QStringLiteral("artist:\"Kalo Kalo\"")};

} // namespace

TEST(RelatedTrackSearchTest, termsSplitOutsideQuotes) {
    EXPECT_EQ(QStringList({QStringLiteral("artist:\"Kalo Kalo\""),
                      QStringLiteral("bpm:>=120"),
                      QStringLiteral("house")}),
            relatedSearchTerms(QStringLiteral("  artist:\"Kalo Kalo\"  bpm:>=120 house ")));
    EXPECT_TRUE(relatedSearchTerms(QString()).isEmpty());
}

TEST(RelatedTrackSearchTest, chipsCombine) {
    QString text = toggleRelatedTrackSearch(QString(), kKey);
    EXPECT_EQ(QStringLiteral("~key:\"8A\""), text);
    text = toggleRelatedTrackSearch(text, kBpm);
    EXPECT_EQ(QStringLiteral("~key:\"8A\" bpm:>=120 bpm:<=128"), text);
    text = toggleRelatedTrackSearch(text, kArtist);
    EXPECT_EQ(QStringLiteral("~key:\"8A\" bpm:>=120 bpm:<=128 artist:\"Kalo Kalo\""), text);
    EXPECT_TRUE(isRelatedTrackSearchActive(text, kKey));
    EXPECT_TRUE(isRelatedTrackSearchActive(text, kBpm));
    EXPECT_TRUE(isRelatedTrackSearchActive(text, kArtist));
}

TEST(RelatedTrackSearchTest, litChipIsRemoved) {
    const QString text = QStringLiteral("~key:\"8A\" bpm:>=120 bpm:<=128 artist:\"Kalo Kalo\"");
    EXPECT_EQ(QStringLiteral("~key:\"8A\" artist:\"Kalo Kalo\""),
            toggleRelatedTrackSearch(text, kBpm));
    EXPECT_EQ(QStringLiteral("bpm:>=120 bpm:<=128 artist:\"Kalo Kalo\""),
            toggleRelatedTrackSearch(text, kKey));
    EXPECT_EQ(QString(), toggleRelatedTrackSearch(kKey.query, kKey));
}

TEST(RelatedTrackSearchTest, typedWordsStay) {
    EXPECT_EQ(QStringLiteral("dub ~key:\"8A\""),
            toggleRelatedTrackSearch(QStringLiteral("dub"), kKey));
    EXPECT_EQ(QStringLiteral("dub"),
            toggleRelatedTrackSearch(QStringLiteral("dub ~key:\"8A\""), kKey));
}

TEST(RelatedTrackSearchTest, sameFieldIsReplaced) {
    // Another track's KEY replaces the old key term rather than ANDing it.
    const QString text = QStringLiteral("~key:\"8A\" bpm:>=120 bpm:<=128");
    EXPECT_FALSE(isRelatedTrackSearchActive(text, kOtherKey));
    EXPECT_EQ(QStringLiteral("bpm:>=120 bpm:<=128 ~key:\"9A\""),
            toggleRelatedTrackSearch(text, kOtherKey));
    // Both bounds of a BPM range are replaced.
    const RelatedTrackSearch otherBpm{QStringLiteral("BPM"),
            QStringLiteral("140"),
            QStringLiteral("bpm:>=135 bpm:<=145")};
    EXPECT_EQ(QStringLiteral("~key:\"8A\" bpm:>=135 bpm:<=145"),
            toggleRelatedTrackSearch(text, otherBpm));
}

TEST(RelatedTrackSearchTest, partialMatchIsNotLit) {
    EXPECT_FALSE(isRelatedTrackSearchActive(QStringLiteral("bpm:>=120"), kBpm));
    EXPECT_FALSE(isRelatedTrackSearchActive(QString(), kKey));
}

TEST(RelatedTrackSearchTest, deckOffersKeyAndBpm) {
    const QList<RelatedTrackSearch> searches =
            relatedTrackSearchesForDeck(QStringLiteral("8A"), 124.4);
    ASSERT_EQ(2, searches.size());
    EXPECT_EQ(kKey, searches.at(0));
    EXPECT_EQ(QStringLiteral("BPM"), searches.at(1).label);
    EXPECT_EQ(QStringLiteral("124"), searches.at(1).value);
    EXPECT_TRUE(searches.at(1).query.startsWith(QStringLiteral("bpm:>=")));
}

TEST(RelatedTrackSearchTest, deckBpmFollowsWholeNumbers) {
    // A nudge of the rate fader keeps the chip's query, so it stays lit.
    EXPECT_EQ(relatedTrackSearchesForDeck(QStringLiteral("8A"), 123.6),
            relatedTrackSearchesForDeck(QStringLiteral("8A"), 124.4));
    EXPECT_NE(relatedTrackSearchesForDeck(QStringLiteral("8A"), 124.4),
            relatedTrackSearchesForDeck(QStringLiteral("8A"), 124.6));
}

TEST(RelatedTrackSearchTest, deckSkipsUnknownValues) {
    EXPECT_TRUE(relatedTrackSearchesForDeck(QString(), 0).isEmpty());
    const QList<RelatedTrackSearch> bpmOnly =
            relatedTrackSearchesForDeck(QString(), 124);
    ASSERT_EQ(1, bpmOnly.size());
    EXPECT_EQ(QStringLiteral("BPM"), bpmOnly.at(0).label);
    const QList<RelatedTrackSearch> keyOnly =
            relatedTrackSearchesForDeck(QStringLiteral("8A"), 0);
    ASSERT_EQ(1, keyOnly.size());
    EXPECT_EQ(kKey, keyOnly.at(0));
}

} // namespace library
} // namespace mixxx
