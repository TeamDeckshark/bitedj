// Tests for the Bite DJ "played this session" track tint: the session registry
// itself, and the fact that it colours a Rekordbox playlist row — an external
// model with no `played` database column, which is what the appliance browses
// most of the time.
#include "library/playedtracks.h"

#include <gtest/gtest.h>

#include <QColor>
#include <QSignalSpy>
#include <QSqlQuery>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "library/basetrackcache.h"
#include "library/basetracktablemodel.h"
#include "library/dao/trackschema.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/trackcollection.h"
#include "mixer/playerinfo.h"
#include "test/librarytest.h"
#include "track/track.h"
#include "widget/wtracktableview.h"

namespace {

const QString kTrackLocation = QStringLiteral("/media/USB1/a.mp3");
const QString kOtherLocation = QStringLiteral("/media/USB1/b.mp3");

class PlayedTracksTest : public LibraryTest {
  protected:
    PlayedTracksTest()
            : m_crossfader(ConfigKey("[Master]", "crossfader"), -1.0, 1.0),
              m_numDecks(ConfigKey("[App]", "num_decks")),
              m_numSamplers(ConfigKey("[App]", "num_samplers")),
              m_numPreviewDecks(ConfigKey("[App]", "num_preview_decks")) {
        m_numDecks.set(2);
        m_numPreviewDecks.set(1);
        PlayerInfo::create();
        // Singletons, so an earlier test in the same binary may have left both
        // a stale registry and a cleared colour flag behind.
        PlayedTracks::destroy();
        PlayedTracks::instance();
        BaseTrackTableModel::setApplyPlayedTrackColor(true);
    }
    ~PlayedTracksTest() override {
        PlayedTracks::destroy();
        PlayerInfo::destroy();
    }

    /// Minimal Rekordbox USB library holding a single track at kTrackLocation.
    void createRekordboxTables() {
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
                kTrackLocation +
                "', 'c', 180, '320', 124.5,"
                " 'Am', 3, '/media/USB1/PIONEER/a.DAT', 'USB1', 1)"));
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

TEST_F(PlayedTracksTest, AudibleDeckMarksItsTrackPlayed) {
    // Only deck 1 exists here, so PlayerInfo doesn't poll controls we haven't
    // created.
    m_numDecks.set(1);
    // The controls PlayerInfo polls to decide which deck is the audible one.
    ControlObject play(ConfigKey("[Channel1]", "play"));
    ControlObject pregain(ConfigKey("[Channel1]", "pregain"));
    ControlObject volume(ConfigKey("[Channel1]", "volume"));
    ControlObject orientation(ConfigKey("[Channel1]", "orientation"));
    pregain.set(1.0);
    volume.set(1.0);
    orientation.set(1.0); // centre, unaffected by the crossfader

    PlayedTracks& playedTracks = PlayedTracks::instance();
    const auto pTrack = Track::newTemporary(
            mixxx::FileAccess(mixxx::FileInfo(kTrackLocation)));

    // Merely loading a track into a stopped deck is not "played".
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel1]"), pTrack);
    EXPECT_FALSE(playedTracks.isPlayed(pTrack->getLocation()));

    // Once the deck is audible, re-announcing the load marks it.
    play.set(1.0);
    PlayerInfo::instance().setTrackInfo(QStringLiteral("[Channel1]"), pTrack);
    EXPECT_TRUE(playedTracks.isPlayed(pTrack->getLocation()));
}

TEST_F(PlayedTracksTest, RegistryTracksAndResets) {
    PlayedTracks& playedTracks = PlayedTracks::instance();
    QSignalSpy spy(&playedTracks, &PlayedTracks::playedTracksChanged);

    EXPECT_TRUE(playedTracks.isEmpty());
    EXPECT_FALSE(playedTracks.isPlayed(kTrackLocation));

    playedTracks.markPlayed(kTrackLocation);
    EXPECT_FALSE(playedTracks.isEmpty());
    EXPECT_TRUE(playedTracks.isPlayed(kTrackLocation));
    EXPECT_FALSE(playedTracks.isPlayed(kOtherLocation));
    EXPECT_EQ(1, spy.count());

    // Playing the same track again is not a change: no redundant repaint.
    playedTracks.markPlayed(kTrackLocation);
    EXPECT_EQ(1, spy.count());

    // An empty location (e.g. a track with no file) is ignored.
    playedTracks.markPlayed(QString());
    EXPECT_EQ(1, spy.count());

    playedTracks.clear();
    EXPECT_TRUE(playedTracks.isEmpty());
    EXPECT_FALSE(playedTracks.isPlayed(kTrackLocation));
    EXPECT_EQ(2, spy.count());

    // Resetting an already-empty session is a no-op.
    playedTracks.clear();
    EXPECT_EQ(2, spy.count());
}

TEST_F(PlayedTracksTest, ExternalModelRowIsTintedWhilePlayed) {
    createRekordboxTables();

    auto trackSource = createRekordboxTrackSource();
    RekordboxPlaylistModel model(nullptr, trackCollectionManager(), trackSource);
    model.setPlaylist(QStringLiteral("/media/USB1-->Playlist A"));
    model.select();
    ASSERT_EQ(1, model.rowCount());

    const int titleCol =
            model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    ASSERT_GE(titleCol, 0);
    const QModelIndex titleIndex = model.index(0, titleCol);
    ASSERT_EQ(kTrackLocation, model.getTrackLocation(titleIndex));

    // Untouched rows keep the skin's own text colour (no ForegroundRole).
    EXPECT_FALSE(model.data(titleIndex, Qt::ForegroundRole)
                         .canConvert<QColor>());

    PlayedTracks& playedTracks = PlayedTracks::instance();
    QSignalSpy repaintSpy(&model, &QAbstractItemModel::dataChanged);

    playedTracks.markPlayed(kTrackLocation);
    const QVariant played = model.data(titleIndex, Qt::ForegroundRole);
    ASSERT_TRUE(played.canConvert<QColor>());
    EXPECT_EQ(QColor(WTrackTableView::kDefaultTrackPlayedColor),
            played.value<QColor>());
    // The model asked the view to repaint rather than waiting for a reselect.
    EXPECT_GE(repaintSpy.count(), 1);

    playedTracks.clear();
    EXPECT_FALSE(model.data(titleIndex, Qt::ForegroundRole)
                         .canConvert<QColor>());
}
