#include "skin/highcontrast.h"

#include <gtest/gtest.h>

#include <QColor>
#include <QString>

namespace {

class HighContrastTest : public testing::Test {
  protected:
    static QString invert(const QString& styleSheet) {
        // No icon cache dir: url()s are left alone, which keeps the test
        // independent of the filesystem.
        return HighContrast::invertStyleSheet(styleSheet);
    }
};

TEST_F(HighContrastTest, InvertColorFlipsLightness) {
    EXPECT_EQ(QColor(Qt::white), HighContrast::invertColor(QColor(Qt::black)));
    EXPECT_EQ(QColor(Qt::black), HighContrast::invertColor(QColor(Qt::white)));

    // Mid grey is its own inverse (up to the 8-bit rounding of L = 0.502).
    const QColor midGrey(128, 128, 128);
    EXPECT_NEAR(midGrey.lightness(), HighContrast::invertColor(midGrey).lightness(), 1);
}

TEST_F(HighContrastTest, InvertColorKeepsHue) {
    // The skin's state colors mirror the hardware LEDs, so a danger red has to
    // stay red and the playing-deck orange has to stay orange.
    for (const QColor& color : {QColor("#d73535"), QColor("#f15921"), QColor("#488ab3")}) {
        const QColor inverted = HighContrast::invertColor(color);
        EXPECT_NEAR(color.hueF(), inverted.hueF(), 0.01) << color.name().toStdString();
        EXPECT_NEAR(color.hslSaturationF(), inverted.hslSaturationF(), 0.01)
                << color.name().toStdString();
        EXPECT_NEAR(1.0, color.lightnessF() + inverted.lightnessF(), 0.01)
                << color.name().toStdString();
    }
}

TEST_F(HighContrastTest, InvertColorPreservesAlpha) {
    const QColor translucent(10, 20, 30, 42);
    EXPECT_EQ(42, HighContrast::invertColor(translucent).alpha());
}

TEST_F(HighContrastTest, InvertsHexValues) {
    EXPECT_EQ(QStringLiteral("#Deck { background-color: #ffffff; }"),
            invert(QStringLiteral("#Deck { background-color: #000000; }")));
    // Short form expands, which Qt parses the same way.
    EXPECT_EQ(QStringLiteral("#Deck { color: #000000; }"),
            invert(QStringLiteral("#Deck { color: #fff; }")));
}

TEST_F(HighContrastTest, LeavesSelectorsAlone) {
    // An id selector is not a hex color, and the ':' of a pseudo-state does not
    // open a declaration value.
    const QString sheet = QStringLiteral(
            "#SettingsSegmentLeft[displayValue=\"1\"]:hover,\n"
            "#CuePad[light=\"true\"] { border: 2px solid #000000; }");
    EXPECT_EQ(QStringLiteral(
                      "#SettingsSegmentLeft[displayValue=\"1\"]:hover,\n"
                      "#CuePad[light=\"true\"] { border: 2px solid #ffffff; }"),
            invert(sheet));
}

TEST_F(HighContrastTest, InvertsNamedColors) {
    EXPECT_EQ(QStringLiteral("#Mixxx { background-color: #ffffff; }"),
            invert(QStringLiteral("#Mixxx { background-color: black; }")));
    // lightgray is #d3d3d3.
    EXPECT_EQ(QStringLiteral("QLabel { border: 1px solid #2c2c2c; }"),
            invert(QStringLiteral("QLabel { border: 1px solid lightgray; }")));
}

TEST_F(HighContrastTest, KeepsNonColorKeywords) {
    const QString sheet = QStringLiteral(
            "#Sampler { border: 2px dashed #ffffff; text-transform: uppercase; }");
    EXPECT_EQ(QStringLiteral(
                      "#Sampler { border: 2px dashed #000000; text-transform: uppercase; }"),
            invert(sheet));
}

TEST_F(HighContrastTest, KeepsTransparent) {
    EXPECT_EQ(QStringLiteral("#Deck { background-color: transparent; }"),
            invert(QStringLiteral("#Deck { background-color: transparent; }")));
}

TEST_F(HighContrastTest, InvertsRgbaKeepingAlphaLiteral) {
    // The alpha token is re-emitted verbatim: Qt accepts both 0-255 and
    // 0.0-1.0, and re-serializing it could change what the sheet means.
    EXPECT_EQ(QStringLiteral("#DeckKey { background-color: rgba(30, 147, 247, 0.1); }"),
            invert(QStringLiteral("#DeckKey { background-color: rgba(8, 125, 225, 0.1); }")));
    EXPECT_EQ(QStringLiteral("#Deck { color: rgb(255, 255, 255); }"),
            invert(QStringLiteral("#Deck { color: rgb(0, 0, 0); }")));
}

TEST_F(HighContrastTest, LeavesCommentsAndQuotedStringsAlone) {
    const QString sheet = QStringLiteral(
            "/* black is the page background */\n"
            "* { font-family: 'Monospace'; qproperty-alignment: 'AlignCenter'; }");
    EXPECT_EQ(sheet, invert(sheet));
}

TEST_F(HighContrastTest, LeavesUrlsAloneWithoutAnIconCache) {
    const QString sheet = QStringLiteral(
            "#Btn { image: url(skin:icons/keylock.svg) no-repeat center center; }");
    EXPECT_EQ(sheet, invert(sheet));
}

TEST_F(HighContrastTest, NoInvertMarkerExemptsTheNextRule) {
    // The cue-pad label is paired against the cue's own colour, which is never
    // inverted, so inverting the label would flip it to the wrong polarity.
    const QString sheet = QStringLiteral(
            "/* no-invert */\n"
            "#CuePad[light=\"true\"] { color: #101010; }\n"
            "#CuePad { background-color: #1a1a1a; }");
    EXPECT_EQ(QStringLiteral(
                      "/* no-invert */\n"
                      "#CuePad[light=\"true\"] { color: #101010; }\n"
                      "#CuePad { background-color: #e5e5e5; }"),
            invert(sheet));
}

TEST_F(HighContrastTest, IsAnInvolution) {
    // Toggling the mode off restores the original sheet, but the transform
    // itself should also round-trip so a double-inverted sheet is not drifting.
    const QString sheet = QStringLiteral(
            "#SettingsRow { background-color: #151515; color: #e5e6ea; }");
    EXPECT_EQ(sheet.toLower(), invert(invert(sheet)).toLower());
}

} // namespace
