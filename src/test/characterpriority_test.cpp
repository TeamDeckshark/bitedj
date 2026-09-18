#include "skin/characterpriority.h"

#include <gtest/gtest.h>

#include <QFont>
#include <QString>
#include <QStringList>

namespace {

using Priority = CharacterPriority::Priority;
using Region = CharacterPriority::Region;

QString splice(const QString& styleSheet) {
    return CharacterPriority::insertFallbackFamily(styleSheet, QStringLiteral("Noto Sans CJK JP"));
}

TEST(CharacterPriorityTest, ExplicitPrioritiesIgnoreTheLocale) {
    // The whole point of the setting: an English UI with a Japanese collection
    // has to be able to say so, and a Japanese UI must not overrule a DJ who
    // asked for Traditional Chinese glyph forms.
    EXPECT_EQ(Region::Japan, CharacterPriority::resolve(Priority::Japanese, "en_US"));
    EXPECT_EQ(Region::TraditionalChina,
            CharacterPriority::resolve(Priority::TraditionalChinese, "ja"));
    EXPECT_EQ(Region::SimplifiedChina,
            CharacterPriority::resolve(Priority::SimplifiedChinese, "ko_KR"));
    EXPECT_EQ(Region::Korea, CharacterPriority::resolve(Priority::Korean, "zh_CN"));
}

TEST(CharacterPriorityTest, AutoFollowsTheUiLocale) {
    EXPECT_EQ(Region::Japan, CharacterPriority::resolve(Priority::Auto, "ja"));
    EXPECT_EQ(Region::Japan, CharacterPriority::resolve(Priority::Auto, "ja_JP"));
    EXPECT_EQ(Region::SimplifiedChina, CharacterPriority::resolve(Priority::Auto, "zh_CN"));
    EXPECT_EQ(Region::TraditionalChina, CharacterPriority::resolve(Priority::Auto, "zh_TW"));
    EXPECT_EQ(Region::HongKong, CharacterPriority::resolve(Priority::Auto, "zh_HK"));
    EXPECT_EQ(Region::Korea, CharacterPriority::resolve(Priority::Auto, "ko_KR"));
}

TEST(CharacterPriorityTest, AutoHandlesTheOtherSpellingsOfALocale) {
    // Translation file names use '-', QLocale uses '_', and Chinese locales
    // name the script as often as the territory.
    EXPECT_EQ(Region::TraditionalChina, CharacterPriority::resolve(Priority::Auto, "zh-TW"));
    EXPECT_EQ(Region::TraditionalChina,
            CharacterPriority::resolve(Priority::Auto, "zh_Hant_TW"));
    EXPECT_EQ(Region::SimplifiedChina,
            CharacterPriority::resolve(Priority::Auto, "zh_Hans_CN"));
    // Singapore writes simplified; plain `zh` is simplified too.
    EXPECT_EQ(Region::SimplifiedChina, CharacterPriority::resolve(Priority::Auto, "zh_SG"));
    EXPECT_EQ(Region::SimplifiedChina, CharacterPriority::resolve(Priority::Auto, "zh"));
}

TEST(CharacterPriorityTest, AutoFallsBackToJapaneseOutsideCjk) {
    // The appliance ships in en_US, which says nothing about the collection.
    // Japanese is what fontconfig already resolves to there, so Auto changes
    // coverage without also changing glyph forms out from under anyone.
    EXPECT_EQ(Region::Japan, CharacterPriority::resolve(Priority::Auto, "en_US"));
    EXPECT_EQ(Region::Japan, CharacterPriority::resolve(Priority::Auto, "de"));
    EXPECT_EQ(Region::Japan, CharacterPriority::resolve(Priority::Auto, QString()));
}

TEST(CharacterPriorityTest, CandidatesLeadWithTheRegionsNotoCut) {
    EXPECT_EQ(QStringLiteral("Noto Sans CJK JP"),
            CharacterPriority::familyCandidates(Region::Japan).first());
    EXPECT_EQ(QStringLiteral("Noto Sans CJK SC"),
            CharacterPriority::familyCandidates(Region::SimplifiedChina).first());
    EXPECT_EQ(QStringLiteral("Noto Sans CJK TC"),
            CharacterPriority::familyCandidates(Region::TraditionalChina).first());
    EXPECT_EQ(QStringLiteral("Noto Sans CJK HK"),
            CharacterPriority::familyCandidates(Region::HongKong).first());
    EXPECT_EQ(QStringLiteral("Noto Sans CJK KR"),
            CharacterPriority::familyCandidates(Region::Korea).first());

    // The proportional cut outranks the monospaced one: mixed CJK/Latin track
    // titles are what the library rows are full of, and Mono costs width on
    // every Latin letter in them.
    const QStringList japanese = CharacterPriority::familyCandidates(Region::Japan);
    EXPECT_LT(japanese.indexOf(QStringLiteral("Noto Sans CJK JP")),
            japanese.indexOf(QStringLiteral("Noto Sans Mono CJK JP")));
}

TEST(CharacterPriorityTest, PreferenceCoversEveryRegionAfterTheChosenOne) {
    const QStringList preference = CharacterPriority::familyPreference(Region::Korea);
    EXPECT_EQ(QStringLiteral("Noto Sans CJK KR"), preference.first());
    // A unit that only has the Japanese cut installed still shows glyphs
    // rather than tofu.
    EXPECT_TRUE(preference.contains(QStringLiteral("Noto Sans CJK JP")));
    EXPECT_TRUE(preference.contains(QStringLiteral("Noto Sans CJK SC")));
    EXPECT_TRUE(preference.contains(QStringLiteral("Noto Sans CJK TC")));
    // Every Korean candidate is tried before any other region's.
    EXPECT_LT(preference.indexOf(QStringLiteral("Noto Sans Mono CJK KR")),
            preference.indexOf(QStringLiteral("Noto Sans CJK JP")));
}

TEST(CharacterPriorityTest, HongKongFallsBackToTraditionalFirst) {
    const QStringList preference = CharacterPriority::familyPreference(Region::HongKong);
    EXPECT_EQ(QStringLiteral("Noto Sans CJK HK"), preference.first());
    // Hong Kong writes traditional, so Taiwan's cut is the closest thing to it.
    EXPECT_LT(preference.indexOf(QStringLiteral("Noto Sans CJK TC")),
            preference.indexOf(QStringLiteral("Noto Sans CJK JP")));
    EXPECT_LT(preference.indexOf(QStringLiteral("Noto Sans CJK TC")),
            preference.indexOf(QStringLiteral("Noto Sans CJK SC")));
}

TEST(CharacterPriorityTest, SplicesBehindThePrimaryFamily) {
    // Latin keeps coming from the skin's own face; only the glyphs it does not
    // have fall through to the CJK font.
    EXPECT_EQ(QStringLiteral("* { font-family: 'Inter', 'Noto Sans CJK JP', sans-serif; }"),
            splice(QStringLiteral("* { font-family: 'Inter', sans-serif; }")));
}

TEST(CharacterPriorityTest, SplicesIntoASingleFamilyDeclaration) {
    EXPECT_EQ(QStringLiteral("* { font-family: 'Inter', 'Noto Sans CJK JP'; }"),
            splice(QStringLiteral("* { font-family: 'Inter'; }")));
    // Unquoted and unterminated declarations are both legal QSS.
    EXPECT_EQ(QStringLiteral("* { font-family: Inter, 'Noto Sans CJK JP' }"),
            splice(QStringLiteral("* { font-family: Inter }")));
}

TEST(CharacterPriorityTest, SplicesEveryDeclaration) {
    const QString styleSheet = QStringLiteral(
            "* { font-family: 'Inter'; }\n"
            "WLabel { color: #fff; font-family: 'Inter'; font-size: 12px; }\n");
    const QString spliced = splice(styleSheet);
    EXPECT_EQ(2, spliced.count(QStringLiteral("Noto Sans CJK JP")));
    EXPECT_TRUE(spliced.contains(QStringLiteral("font-size: 12px")));
}

TEST(CharacterPriorityTest, IsIdempotent) {
    // A sheet is re-parsed on every skin reboot, and the library sheet is
    // composed out of one that has already been through here.
    const QString once = splice(QStringLiteral("* { font-family: 'Inter', sans-serif; }"));
    EXPECT_EQ(once, splice(once));
}

TEST(CharacterPriorityTest, LeavesTheSheetAloneWithoutAFamily) {
    // A unit with no CJK font installed renders exactly as it did before this
    // setting existed.
    const QString styleSheet = QStringLiteral("* { font-family: 'Inter', sans-serif; }");
    EXPECT_EQ(styleSheet, CharacterPriority::insertFallbackFamily(styleSheet, QString()));
}

TEST(CharacterPriorityTest, IgnoresFontFamilyOutsideADeclaration) {
    // Comments, strings and selectors all mention property names; only a
    // declaration may be rewritten, and a malformed sheet is worse than a
    // missing glyph.
    for (const QString& styleSheet : {
                 QStringLiteral("/* font-family: 'Inter'; */ * { color: #fff; }"),
                 QStringLiteral("* { qproperty-text: \"font-family: 'Inter';\"; }"),
                 QStringLiteral("WLabel[font-family=\"x\"] { color: #fff; }"),
                 QStringLiteral("* { -qt-font-family-hack: 'Inter'; }"),
         }) {
        EXPECT_EQ(styleSheet, splice(styleSheet)) << styleSheet.toStdString();
    }
}

TEST(CharacterPriorityTest, SplicesAfterAComment) {
    EXPECT_EQ(QStringLiteral("* { /* note */ font-family: 'Inter', 'Noto Sans CJK JP'; }"),
            splice(QStringLiteral("* { /* note */ font-family: 'Inter'; }")));
}

TEST(CharacterPriorityTest, RefusesAFamilyThatWouldBreakOutOfItsQuotes) {
    const QString styleSheet = QStringLiteral("* { font-family: 'Inter'; }");
    EXPECT_EQ(styleSheet,
            CharacterPriority::insertFallbackFamily(
                    styleSheet, QStringLiteral("Evil'; color: red; font-family: 'x")));
}

TEST(CharacterPriorityTest, FontFallbackGoesBehindThePrimaryFamily) {
    QFont font;
    font.setFamilies({QStringLiteral("Inter"), QStringLiteral("Monospace")});
    const QStringList families =
            CharacterPriority::withFallbackFamily(font, QStringLiteral("Noto Sans CJK JP"))
                    .families();
    EXPECT_EQ(QStringList({QStringLiteral("Inter"),
                      QStringLiteral("Noto Sans CJK JP"),
                      QStringLiteral("Monospace")}),
            families);
}

TEST(CharacterPriorityTest, FontFallbackIsIdempotent) {
    QFont font;
    font.setFamilies({QStringLiteral("Inter")});
    const QFont once =
            CharacterPriority::withFallbackFamily(font, QStringLiteral("Noto Sans CJK JP"));
    EXPECT_EQ(once.families(),
            CharacterPriority::withFallbackFamily(once, QStringLiteral("Noto Sans CJK JP"))
                    .families());
}

} // namespace
