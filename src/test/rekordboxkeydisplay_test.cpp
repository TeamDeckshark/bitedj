#include "library/rekordbox/rekordboxkeydisplay.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHash>
#include <QSignalSpy>

#include "library/rekordbox/rekordboxanalysis.h"
#include "test/mixxxtest.h"

using mixxx::track::io::key::ChromaticKey;

namespace {

const QString kOnExport = QStringLiteral("/mnt/usb/Contents/a.mp3");
const QString kNotOnExport = QStringLiteral("/mnt/usb/Other/b.mp3");

class RekordboxKeyDisplayTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_keyTexts.insert(kOnExport, QStringLiteral("8A"));
        RekordboxAnalysis::setLookup([this](const QString& location)
                                             -> std::optional<RekordboxAnalysis::Entry> {
            ++m_lookups;
            const auto it = m_keyTexts.constFind(location);
            if (it == m_keyTexts.constEnd()) {
                return std::nullopt;
            }
            return RekordboxAnalysis::Entry{QString(), *it};
        });
        QCoreApplication::processEvents();
        display().setEnabled(true);
        m_lookups = 0;
    }

    void TearDown() override {
        RekordboxAnalysis::setLookup({});
        QCoreApplication::processEvents();
        display().setEnabled(false);
    }

    static RekordboxKeyDisplay& display() {
        return RekordboxKeyDisplay::instance();
    }

    QHash<QString, QString> m_keyTexts;
    int m_lookups = 0;
};

TEST_F(RekordboxKeyDisplayTest, ShowsTheExportKey) {
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, display().keyFor(kOnExport));
}

TEST_F(RekordboxKeyDisplayTest, TrackOnNoExportKeepsItsOwnKey) {
    EXPECT_EQ(mixxx::track::io::key::INVALID, display().keyFor(kNotOnExport));
}

TEST_F(RekordboxKeyDisplayTest, OffShowsNothingFromRekordbox) {
    QSignalSpy changed(&display(), &RekordboxKeyDisplay::changed);
    display().setEnabled(false);
    EXPECT_EQ(1, changed.count());
    EXPECT_EQ(mixxx::track::io::key::INVALID, display().keyFor(kOnExport));
    EXPECT_EQ(0, m_lookups);
}

TEST_F(RekordboxKeyDisplayTest, UnreadableExportKeyKeepsItsOwnKey) {
    m_keyTexts.insert(kNotOnExport, QStringLiteral("not a key"));
    EXPECT_EQ(mixxx::track::io::key::INVALID, display().keyFor(kNotOnExport));
}

TEST_F(RekordboxKeyDisplayTest, LooksUpEachLocationOnce) {
    display().keyFor(kOnExport);
    display().keyFor(kOnExport);
    display().keyFor(kNotOnExport);
    display().keyFor(kNotOnExport);
    EXPECT_EQ(2, m_lookups);
}

TEST_F(RekordboxKeyDisplayTest, ExportChangeDropsTheCache) {
    EXPECT_EQ(mixxx::track::io::key::INVALID, display().keyFor(kNotOnExport));
    m_keyTexts.insert(kNotOnExport, QStringLiteral("Abm"));
    QSignalSpy changed(&display(), &RekordboxKeyDisplay::changed);
    display().exportChanged();
    QCoreApplication::processEvents();
    EXPECT_EQ(1, changed.count());
    EXPECT_EQ(mixxx::track::io::key::G_SHARP_MINOR, display().keyFor(kNotOnExport));
}

} // namespace
