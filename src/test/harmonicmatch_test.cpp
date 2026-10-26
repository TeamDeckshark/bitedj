// Tests for the Bite DJ compatible-key row tint: the Camelot-wheel rule, the
// reference following the audible (else playing, else last loaded) deck's
// playing key, and the tint on a Rekordbox playlist row (text-only key, no
// key_id column).
#include "library/harmonicmatch.h"

#include <gtest/gtest.h>

#include <QColor>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QSqlQuery>
#include <memory>
#include <vector>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "library/basetrackcache.h"
#include "library/basetracktablemodel.h"
#include "library/dao/trackschema.h"
#include "library/playedtracks.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/trackcollection.h"
#include "mixer/playerinfo.h"
#include "test/librarytest.h"
#include "track/keyutils.h"
#include "track/track.h"

using namespace mixxx::track::io::key;

namespace {

const QString kTrackLocation = QStringLiteral("/media/USB1/a.mp3");

class HarmonicMatchTest : public LibraryTest {
  protected:
    HarmonicMatchTest()
            : m_crossfader(ConfigKey("[Master]", "crossfader"), -1.0, 1.0),
              m_numDecks(ConfigKey("[App]", "num_decks")),
              m_numSamplers(ConfigKey("[App]", "num_samplers")),
              m_numPreviewDecks(ConfigKey("[App]", "num_preview_decks")) {
        // Only deck 1 exists, so PlayerInfo doesn't poll controls we haven't
        // created.
        m_numDecks.set(1);
        m_numPreviewDecks.set(1);
        PlayerInfo::create();
        // Singletons: an earlier test in the same binary may have left state.
        PlayedTracks::destroy();
        HarmonicMatch::destroy();
        HarmonicMatch::instance();
    }
    ~HarmonicMatchTest() override {
        HarmonicMatch::destroy();
        PlayedTracks::destroy();
        PlayerInfo::destroy();
    }

    void createRekordboxTables(const QString& key) {
        QSqlQuery q(internalCollection()->database());
        ASSERT_TRUE(q.exec(
                "CREATE TABLE IF NOT EXISTS rekordbox_library ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT, rb_id INTEGER,"
                " artist TEXT, title TEXT, album TEXT, year INTEGER,"
                " genre TEXT, tracknumber TEXT, location TEXT UNIQUE,"
                " comment TEXT, duration INTEGER, bitrate TEXT, bpm FLOAT,"
                " key TEXT, rating INTEGER, analyze_path TEXT UNIQUE,"
                " device TEXT, color INTEGER)"));
        ASSERT_TRUE(q.exec(
                "CREATE TABLE IF NOT EXISTS rekordbox_playlists ("
                " id INTEGER PRIMARY KEY, name TEXT UNIQUE)"));
        ASSERT_TRUE(q.exec(
                "CREATE TABLE IF NOT EXISTS rekordbox_playlist_tracks ("
                " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                " playlist_id INTEGER, track_id INTEGER, position INTEGER)"));
        ASSERT_TRUE(q.exec(
                "INSERT INTO rekordbox_library (rb_id, artist, title, album,"
                " year, genre, tracknumber, location, comment, duration,"
                " bitrate, bpm, key, rating, analyze_path, device, color)"
                " VALUES (101, 'Artist A', 'Title A', 'Album A', 2020,"
                " 'House', '1', '" +
                kTrackLocation + "', 'c', 180, '320', 124.5, '" + key +
                "', 3, '/media/USB1/PIONEER/a.DAT', 'USB1', 1)"));
        ASSERT_TRUE(q.exec(
                "INSERT INTO rekordbox_playlists (id, name) VALUES"
                " (1, '/media/USB1'), (2, '/media/USB1-->Playlist A')"));
        ASSERT_TRUE(q.exec(
                "INSERT INTO rekordbox_playlist_tracks (playlist_id, track_id,"
                " position) VALUES (1, 1, 1), (2, 1, 1)"));
    }

    QSharedPointer<BaseTrackCache> createRekordboxTrackSource() {
        QStringList columns = {
                LIBRARYTABLE_ID,
                LIBRARYTABLE_ARTIST,
                LIBRARYTABLE_TITLE,
                LIBRARYTABLE_ALBUM,
                LIBRARYTABLE_YEAR,
                LIBRARYTABLE_GENRE,
                LIBRARYTABLE_TRACKNUMBER,
                TRACKLOCATIONSTABLE_LOCATION,
                LIBRARYTABLE_COMMENT,
                LIBRARYTABLE_RATING,
                LIBRARYTABLE_DURATION,
                LIBRARYTABLE_BITRATE,
                LIBRARYTABLE_BPM,
                LIBRARYTABLE_KEY,
                LIBRARYTABLE_COLOR,
                REKORDBOX_ANALYZE_PATH};
        auto trackSource = QSharedPointer<BaseTrackCache>::create(
                internalCollection(),
                QStringLiteral("rekordbox_library"),
                QString(LIBRARYTABLE_ID),
                std::move(columns),
                QStringList{LIBRARYTABLE_ARTIST, LIBRARYTABLE_TITLE},
                false);
        trackSource->buildIndex();
        return trackSource;
    }

    ControlPotmeter m_crossfader;
    ControlObject m_numDecks;
    ControlObject m_numSamplers;
    ControlObject m_numPreviewDecks;
};

} // namespace

TEST_F(HarmonicMatchTest, CamelotWheelRule) {
    // A minor = 8A (OpenKey 1m).
    // Same key always matches, even at range 0.
    EXPECT_TRUE(HarmonicMatch::isCompatible(A_MINOR, A_MINOR, 0));
    // Neighbours on the same ring: E minor (9A) and D minor (7A).
    EXPECT_FALSE(HarmonicMatch::isCompatible(A_MINOR, E_MINOR, 0));
    EXPECT_TRUE(HarmonicMatch::isCompatible(A_MINOR, E_MINOR, 1));
    EXPECT_TRUE(HarmonicMatch::isCompatible(A_MINOR, D_MINOR, 1));
    // Two steps: B minor (10A) and G minor (6A).
    EXPECT_FALSE(HarmonicMatch::isCompatible(A_MINOR, B_MINOR, 1));
    EXPECT_TRUE(HarmonicMatch::isCompatible(A_MINOR, B_MINOR, 2));
    EXPECT_TRUE(HarmonicMatch::isCompatible(A_MINOR, G_MINOR, 2));
    // Relative major (C major, 8B) from one step up, never other diagonals.
    EXPECT_FALSE(HarmonicMatch::isCompatible(A_MINOR, C_MAJOR, 0));
    EXPECT_TRUE(HarmonicMatch::isCompatible(A_MINOR, C_MAJOR, 1));
    EXPECT_FALSE(HarmonicMatch::isCompatible(A_MINOR, G_MAJOR, 2));
    // The wheel wraps: B major (1B) is one step from E major (12B) and
    // F sharp major (2B).
    EXPECT_TRUE(HarmonicMatch::isCompatible(B_MAJOR, E_MAJOR, 1));
    EXPECT_TRUE(HarmonicMatch::isCompatible(B_MAJOR, F_SHARP_MAJOR, 1));
    // Opposite side of the wheel.
    EXPECT_FALSE(HarmonicMatch::isCompatible(C_MAJOR, F_SHARP_MAJOR, 2));
    // Unknown keys never match.
    EXPECT_FALSE(HarmonicMatch::isCompatible(INVALID, INVALID, 2));
    EXPECT_FALSE(HarmonicMatch::isCompatible(A_MINOR, INVALID, 2));
}

TEST_F(HarmonicMatchTest, ModeAndTextMatching) {
    HarmonicMatch& match = HarmonicMatch::instance();
    QSignalSpy spy(&match, &HarmonicMatch::matchChanged);
    match.setReferenceKey(A_MINOR);
    // Off: nothing is active and no repaint was asked for.
    EXPECT_FALSE(match.isActive());
    EXPECT_FALSE(match.matchesKeyText(QStringLiteral("Am")));
    EXPECT_EQ(0, spy.count());

    match.setMode(HarmonicMatch::Mode::OneStep);
    EXPECT_EQ(1, spy.count());
    EXPECT_TRUE(match.isActive());
    // Any notation the libraries hand us.
    EXPECT_TRUE(match.matchesKeyText(QStringLiteral("Am")));
    EXPECT_TRUE(match.matchesKeyText(QStringLiteral("9A")));
    EXPECT_TRUE(match.matchesKeyText(QStringLiteral("C")));
    EXPECT_FALSE(match.matchesKeyText(QStringLiteral("10A")));
    EXPECT_FALSE(match.matchesKeyText(QString()));
    EXPECT_FALSE(match.matchesKeyText(QStringLiteral("garbage")));

    match.setMode(HarmonicMatch::Mode::SameKey);
    EXPECT_FALSE(match.matchesKeyText(QStringLiteral("9A")));
    EXPECT_TRUE(match.matchesKeyText(QStringLiteral("8A")));
}

TEST_F(HarmonicMatchTest, ReferenceFollowsAudibleDeckKey) {
    ControlObject play(ConfigKey("[Channel1]", "play"));
    ControlObject pregain(ConfigKey("[Channel1]", "pregain"));
    ControlObject volume(ConfigKey("[Channel1]", "volume"));
    ControlObject orientation(ConfigKey("[Channel1]", "orientation"));
    ControlObject key(ConfigKey("[Channel1]", "key"));
    pregain.set(1.0);
    volume.set(1.0);
    orientation.set(1.0); // centre, unaffected by the crossfader
    key.set(KeyUtils::keyToNumericValue(A_MINOR));
    // Rebuilt now that the deck's controls exist.
    HarmonicMatch::destroy();
    HarmonicMatch& match = HarmonicMatch::instance();
    match.setMode(HarmonicMatch::Mode::OneStep);
    EXPECT_EQ(INVALID, match.referenceKey());

    const auto pTrack = Track::newTemporary(
            mixxx::FileAccess(mixxx::FileInfo(kTrackLocation)));
    play.set(1.0);
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel1]"), pTrack);
    EXPECT_EQ(A_MINOR, match.referenceKey());

    // Pitching the deck moves the reference with it.
    key.set(KeyUtils::keyToNumericValue(B_FLAT_MINOR));
    QCoreApplication::processEvents();
    EXPECT_EQ(B_FLAT_MINOR, match.referenceKey());

    // Unloading the deck leaves nothing to match against.
    play.set(0.0);
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel1]"), TrackPointer());
    QCoreApplication::processEvents();
    EXPECT_EQ(INVALID, match.referenceKey());
}

TEST_F(HarmonicMatchTest, ReferenceWithoutAudibleDeck) {
    // Two decks with their faders down, as this fork starts them: nothing is
    // ever audible, so the reference falls back to a playing deck, then to
    // the deck loaded last.
    m_numDecks.set(2);
    std::vector<std::unique_ptr<ControlObject>> controls;
    ControlObject* plays[2];
    ControlObject* keys[2];
    for (int i = 0; i < 2; ++i) {
        const QString group = QStringLiteral("[Channel%1]").arg(i + 1);
        for (const char* item : {"pregain", "volume", "orientation"}) {
            controls.push_back(std::make_unique<ControlObject>(ConfigKey(group, item)));
        }
        controls.push_back(std::make_unique<ControlObject>(ConfigKey(group, "play")));
        plays[i] = controls.back().get();
        controls.push_back(std::make_unique<ControlObject>(ConfigKey(group, "key")));
        keys[i] = controls.back().get();
    }
    keys[0]->set(KeyUtils::keyToNumericValue(A_MINOR));
    keys[1]->set(KeyUtils::keyToNumericValue(E_MINOR));
    HarmonicMatch::destroy();
    HarmonicMatch& match = HarmonicMatch::instance();
    match.setMode(HarmonicMatch::Mode::SameKey);

    const auto pTrack = Track::newTemporary(
            mixxx::FileAccess(mixxx::FileInfo(kTrackLocation)));
    // Loaded, not playing, not audible.
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel1]"), pTrack);
    EXPECT_EQ(A_MINOR, match.referenceKey());
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel2]"), pTrack);
    EXPECT_EQ(E_MINOR, match.referenceKey());

    // Deck 1 playing (fader down) wins over deck 2 merely loaded.
    plays[0]->set(1.0);
    QCoreApplication::processEvents();
    EXPECT_EQ(A_MINOR, match.referenceKey());
    EXPECT_EQ(-1, PlayerInfo::instance().getCurrentPlayingDeck());

    // Paused again: back to the deck loaded last.
    plays[0]->set(0.0);
    QCoreApplication::processEvents();
    EXPECT_EQ(E_MINOR, match.referenceKey());
}

TEST_F(HarmonicMatchTest, ExternalModelTitleIsTinted) {
    createRekordboxTables(QStringLiteral("Em"));
    auto trackSource = createRekordboxTrackSource();
    RekordboxPlaylistModel model(nullptr, trackCollectionManager(), trackSource);
    model.setPlaylist(QStringLiteral("/media/USB1-->Playlist A"));
    model.select();
    ASSERT_EQ(1, model.rowCount());

    const int titleCol = model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    const int artistCol = model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);
    ASSERT_GE(titleCol, 0);
    ASSERT_GE(artistCol, 0);
    const QModelIndex titleIndex = model.index(0, titleCol);
    const QModelIndex artistIndex = model.index(0, artistCol);

    HarmonicMatch& match = HarmonicMatch::instance();
    match.setReferenceKey(A_MINOR);
    EXPECT_FALSE(model.data(titleIndex, Qt::ForegroundRole).canConvert<QColor>());

    QSignalSpy repaintSpy(&model, &QAbstractItemModel::dataChanged);
    match.setMode(HarmonicMatch::Mode::OneStep);
    EXPECT_GE(repaintSpy.count(), 1);
    const QVariant tint = model.data(titleIndex, Qt::ForegroundRole);
    ASSERT_TRUE(tint.canConvert<QColor>());
    EXPECT_EQ(HarmonicMatch::kMatchColor, tint.value<QColor>());
    // The whole row, like the played and missing tints.
    EXPECT_EQ(HarmonicMatch::kMatchColor,
            model.data(artistIndex, Qt::ForegroundRole).value<QColor>());

    // Out of range once narrowed to the same key.
    match.setMode(HarmonicMatch::Mode::SameKey);
    EXPECT_FALSE(model.data(titleIndex, Qt::ForegroundRole).canConvert<QColor>());

    // A played track is never suggested.
    match.setMode(HarmonicMatch::Mode::OneStep);
    BaseTrackTableModel::setApplyPlayedTrackColor(true);
    PlayedTracks::instance().markPlayed(kTrackLocation);
    EXPECT_NE(HarmonicMatch::kMatchColor,
            model.data(titleIndex, Qt::ForegroundRole).value<QColor>());
}
