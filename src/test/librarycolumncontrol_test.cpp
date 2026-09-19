// Tests for the Bite DJ LibraryColumnControl-managed track table layout:
// managed visibility/weights from mixxx.cfg, internal-column enforcement,
// self-healing after model resets, upstream-compatible order persistence,
// and the rating column's minimum width.
#include <gtest/gtest.h>

#include <QApplication>
#include <QSqlQuery>
#include <QTableView>
#include <QVariant>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "library/basetrackcache.h"
#include "library/columncache.h"
#include "library/dao/trackschema.h"
#include "library/librarycolumncontrol.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/starrating.h"
#include "library/trackcollection.h"
#include "mixer/playerinfo.h"
#include "test/librarytest.h"
#include "widget/wtracktableviewheader.h"

class LibraryColumnControlTest : public LibraryTest {
  protected:
    LibraryColumnControlTest()
            : m_crossfader(ConfigKey("[Master]", "crossfader"), -1.0, 1.0),
              m_numDecks(ConfigKey("[App]", "num_decks")),
              m_numSamplers(ConfigKey("[App]", "num_samplers")),
              m_numPreviewDecks(ConfigKey("[App]", "num_preview_decks")) {
        m_numDecks.set(2);
        m_numPreviewDecks.set(1);
        PlayerInfo::create();
    }
    ~LibraryColumnControlTest() override {
        PlayerInfo::destroy();
    }

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
                " 'House', '1', '/media/USB1/a.mp3', 'c', 180, '320', 124.5,"
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

    void setLibraryConfig(const char* key, int value) {
        config()->set(ConfigKey("[Library]", key), ConfigValue(value));
    }

    ControlPotmeter m_crossfader;
    ControlObject m_numDecks;
    ControlObject m_numSamplers;
    ControlObject m_numPreviewDecks;
};

TEST_F(LibraryColumnControlTest, ManagedLayoutIsAppliedAndSelfHealing) {
    createRekordboxTables();

    // Mirror the appliance's mixxx.cfg [Library] entries.
    setLibraryConfig("ColumnVisible_position", 1);
    setLibraryConfig("ColumnVisible_artist", 1);
    setLibraryConfig("ColumnVisible_genre", 1);
    setLibraryConfig("ColumnVisible_rating", 1);
    setLibraryConfig("ColumnVisible_color", 0);
    setLibraryConfig("ColumnVisible_timesplayed", 0);
    setLibraryConfig("ColumnVisible_year", 0);
    setLibraryConfig("ColumnWeight_artist", 2);
    setLibraryConfig("ColumnWeight_genre", 3);
    setLibraryConfig("ColumnWeight_title", 4);

    LibraryColumnControl columnControl(config());

    auto trackSource = createRekordboxTrackSource();
    RekordboxPlaylistModel model(nullptr, trackCollectionManager(), trackSource);
    model.setPlaylist(QStringLiteral("/media/USB1-->Playlist A"));
    model.select();
    ASSERT_EQ(1, model.rowCount());

    // Mimic what WTrackTableView::setTrackTableModel does for headers.
    QTableView view;
    view.resize(800, 480);
    auto* header = new WTrackTableViewHeader(Qt::Horizontal, &view);
    view.setModel(&model);
    view.setHorizontalHeader(header);
    for (int i = 0; i < model.columnCount(); ++i) {
        if (model.isColumnInternal(i)) {
            header->hideSection(i);
        }
    }
    view.show();
    QApplication::processEvents();

    const int trackIdCol =
            model.fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_TRACKID);
    const int positionCol =
            model.fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    const int previewCol =
            model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW);
    const int ratingCol =
            model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING);
    const int titleCol =
            model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    const int artistCol =
            model.fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);

    // Managed layout per config: '#' (position), title, rating visible;
    // preview managed-invisible; internal track_id hidden.
    EXPECT_TRUE(header->isSectionHidden(trackIdCol));
    EXPECT_FALSE(header->isSectionHidden(positionCol));
    EXPECT_FALSE(header->isSectionHidden(titleCol));
    EXPECT_FALSE(header->isSectionHidden(ratingCol));
    EXPECT_TRUE(header->isSectionHidden(previewCol));

    // The rating column must fit all five stars.
    EXPECT_GE(header->sectionSize(ratingCol), StarRating().sizeHint().width());

    // Switching playlists on the same model must not disturb the layout.
    model.setPlaylist(QStringLiteral("/media/USB1"));
    model.select();
    QApplication::processEvents();
    EXPECT_TRUE(header->isSectionHidden(trackIdCol));
    EXPECT_TRUE(header->isSectionHidden(previewCol));
    EXPECT_FALSE(header->isSectionHidden(positionCol));

    // Self-healing: if the hidden state ever diverges (unhidden internal
    // track_id column = untitled numeric ids; unhidden preview column = grey
    // skinless buttons), the next model reset must re-assert the managed
    // layout, since the appliance offers no header menu to recover manually.
    header->setSectionHidden(trackIdCol, false);
    header->setSectionHidden(previewCol, false);
    model.select();
    QApplication::processEvents();
    EXPECT_TRUE(header->isSectionHidden(trackIdCol));
    EXPECT_TRUE(header->isSectionHidden(previewCol));
    EXPECT_FALSE(header->isSectionHidden(positionCol));
    EXPECT_GE(header->sectionSize(ratingCol), StarRating().sizeHint().width());

    // Column order uses the upstream per-model header state, while a restored
    // pixel width is replaced by BiteDJ's configured weight.
    const int artistVisualIndex = header->visualIndex(artistCol);
    header->moveSection(header->visualIndex(titleCol), artistVisualIndex);
    header->resizeSection(titleCol, 37);

    QTableView restoredView;
    restoredView.resize(800, 480);
    auto* restoredHeader =
            new WTrackTableViewHeader(Qt::Horizontal, &restoredView);
    restoredView.setModel(&model);
    restoredView.setHorizontalHeader(restoredHeader);
    restoredView.show();
    QApplication::processEvents();

    EXPECT_EQ(artistVisualIndex, restoredHeader->visualIndex(titleCol));
    EXPECT_NE(37, restoredHeader->sectionSize(titleCol));
    EXPECT_TRUE(restoredHeader->isSectionHidden(trackIdCol));
    EXPECT_TRUE(restoredHeader->isSectionHidden(previewCol));
}

// The settings page shows one control per column — [Library],column_size_<name>
// — because a label plus an ON/OFF toggle plus a four-segment width strip does
// not fit three columns into a 480px panel. It is a mirror of the
// visible+weight pair, not a third source of truth, so what matters is that the
// mirror holds in both directions.
namespace {
constexpr double kHidden = 0.0;

double co(const char* item) {
    return ControlObject::get(ConfigKey("[Library]", item));
}

void setCo(const char* item, double value) {
    ControlObject::set(ConfigKey("[Library]", item), value);
}
} // namespace

TEST_F(LibraryColumnControlTest, SizeCoStartsFromTheStoredVisibilityAndWeight) {
    setLibraryConfig("ColumnVisible_artist", 1);
    setLibraryConfig("ColumnWeight_artist", 3);
    setLibraryConfig("ColumnVisible_genre", 0);
    setLibraryConfig("ColumnWeight_genre", 2);

    LibraryColumnControl columnControl(config());

    EXPECT_EQ(3.0, co("column_size_artist"));
    // Hidden reads as 0 whatever width it would have had.
    EXPECT_EQ(kHidden, co("column_size_genre"));
}

TEST_F(LibraryColumnControlTest, SizeCoDrivesVisibilityAndWeight) {
    setLibraryConfig("ColumnVisible_genre", 0);
    setLibraryConfig("ColumnWeight_genre", 1);

    LibraryColumnControl columnControl(config());
    ASSERT_EQ(kHidden, co("column_size_genre"));

    // One tap past OFF: the column comes back on, at that width.
    setCo("column_size_genre", 3.0);

    EXPECT_EQ(1.0, co("column_visible_genre"));
    EXPECT_EQ(3.0, co("column_weight_genre"));
    EXPECT_EQ(1, config()->getValue(ConfigKey("[Library]", "ColumnVisible_genre"), 0));
    EXPECT_EQ(3, config()->getValue(ConfigKey("[Library]", "ColumnWeight_genre"), 0));
}

TEST_F(LibraryColumnControlTest, SizeZeroHidesButKeepsTheWidth) {
    setLibraryConfig("ColumnVisible_genre", 1);
    setLibraryConfig("ColumnWeight_genre", 4);

    LibraryColumnControl columnControl(config());
    ASSERT_EQ(4.0, co("column_size_genre"));

    setCo("column_size_genre", kHidden);

    EXPECT_EQ(kHidden, co("column_visible_genre"));
    // Turning a column back on should bring back the width it had, so hiding
    // must not touch the weight.
    EXPECT_EQ(4.0, co("column_weight_genre"));
    EXPECT_EQ(4, config()->getValue(ConfigKey("[Library]", "ColumnWeight_genre"), 0));
}

TEST_F(LibraryColumnControlTest, SizeCoFollowsVisibilityAndWeightWrittenElsewhere) {
    setLibraryConfig("ColumnVisible_genre", 1);
    setLibraryConfig("ColumnWeight_genre", 1);

    LibraryColumnControl columnControl(config());

    setCo("column_weight_genre", 4.0);
    EXPECT_EQ(4.0, co("column_size_genre"));

    setCo("column_visible_genre", 0.0);
    EXPECT_EQ(kHidden, co("column_size_genre"));

    setCo("column_visible_genre", 1.0);
    EXPECT_EQ(4.0, co("column_size_genre"));
}

TEST_F(LibraryColumnControlTest, SizeCoClampsToARealSize) {
    setLibraryConfig("ColumnVisible_genre", 1);
    setLibraryConfig("ColumnWeight_genre", 2);

    LibraryColumnControl columnControl(config());

    setCo("column_size_genre", 9.0);
    EXPECT_EQ(4.0, co("column_size_genre"));
    EXPECT_EQ(4.0, co("column_weight_genre"));

    // Nothing is narrower than hidden.
    setCo("column_size_genre", -1.0);
    EXPECT_EQ(kHidden, co("column_size_genre"));
    EXPECT_EQ(kHidden, co("column_visible_genre"));
}

TEST_F(LibraryColumnControlTest, SizeCoSnapsBackWhenHidingTheLastColumn) {
    // The table would render blank, so LibraryColumnControl refuses. The button
    // has to come back off OFF or it would lie about the column's state.
    for (const char* key : {"ColumnVisible_title",
                 "ColumnVisible_artist",
                 "ColumnVisible_bpm",
                 "ColumnVisible_key",
                 "ColumnVisible_duration"}) {
        setLibraryConfig(key, 0);
    }
    setLibraryConfig("ColumnVisible_genre", 1);
    setLibraryConfig("ColumnWeight_genre", 2);

    LibraryColumnControl columnControl(config());
    ASSERT_EQ(2.0, co("column_size_genre"));

    setCo("column_size_genre", kHidden);

    EXPECT_EQ(1.0, co("column_visible_genre"));
    EXPECT_EQ(2.0, co("column_size_genre"));
}
