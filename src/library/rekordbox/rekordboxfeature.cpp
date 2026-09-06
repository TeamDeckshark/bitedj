#include "library/rekordbox/rekordboxfeature.h"

#include "library/rekordbox/rekordboxanlz.h"

#include <mp3guessenc.h>
#include <rekordbox_anlz.h>
#include <rekordbox_pdb.h>

#include <QDir>
#include <QMap>
#include <QMessageBox>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTextCodec>
#include <QtDebug>
#include <vector>

#include "engine/engine.h"
#include "library/dao/fscueoverridestore.h"
#include "library/dao/fsmetaoverridestore.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "library/starrating.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_rekordboxfeature.cpp"
#include "notifications/notifications.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/globaltrackcache.h"
#include "track/keyfactory.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/color/color.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/sandbox.h"
#include "util/usbdevice.h"
#include "waveform/waveform.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

#define IS_RECORDBOX_DEVICE "::isRecordboxDevice::"
#define IS_NOT_RECORDBOX_DEVICE "::isNotRecordboxDevice::"

namespace {

const QString kRekordboxLibraryTable = QStringLiteral("rekordbox_library");
const QString kRekordboxPlaylistsTable = QStringLiteral("rekordbox_playlists");
const QString kRekordboxPlaylistTracksTable = QStringLiteral("rekordbox_playlist_tracks");

// depending on the filesystem of the external media, rekordbox seems
// to store its metadata in different paths:
const QStringList kPdbPaths = {
        QStringLiteral("PIONEER/rekordbox/export.pdb"),  // FAT32/exFat
        QStringLiteral(".PIONEER/rekordbox/export.pdb"), // HFS+ media
};
const QString kPLaylistPathDelimiter = QStringLiteral("-->");

// Consecutive empty background enumerations required before a device is
// removed from the sidebar and its rows cleared.
constexpr int kBgEmptyScansBeforeRemoval = 3;

QString findRekordboxPdbPath(const QString& devicePath) {
    const QDir deviceDir(devicePath);
    for (const auto& pdbPath : kPdbPaths) {
        const QFileInfo pdbFileInfo(deviceDir.filePath(pdbPath));
        if (pdbFileInfo.exists() && pdbFileInfo.isFile()) {
            return pdbFileInfo.filePath();
        }
    }
    return {};
}

enum class IDForColor : uint8_t {
    Pink = 1,
    Red,
    Orange,
    Yellow,
    Green,
    Aqua,
    Blue,
    Purple
};

constexpr mixxx::RgbColor kColorForIDPink(0xF870F8);
constexpr mixxx::RgbColor kColorForIDRed(0xF870900);
constexpr mixxx::RgbColor kColorForIDOrange(0xF8A030);
constexpr mixxx::RgbColor kColorForIDYellow(0xF8E331);
constexpr mixxx::RgbColor kColorForIDGreen(0x1EE000);
constexpr mixxx::RgbColor kColorForIDAqua(0x16C0F8);
constexpr mixxx::RgbColor kColorForIDBlue(0x0150F8);
constexpr mixxx::RgbColor kColorForIDPurple(0x9808F8);
constexpr mixxx::RgbColor kColorForIDNoColor(0x0);

struct memory_cue_loop_t {
    mixxx::audio::FramePos startPosition;
    mixxx::audio::FramePos endPosition;
    QString comment;
    mixxx::RgbColor::optional_t color;
};

bool createLibraryTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox library table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    rb_id INTEGER,"
            "    artist TEXT,"
            "    title TEXT,"
            "    album TEXT,"
            "    year INTEGER,"
            "    genre TEXT,"
            "    tracknumber TEXT,"
            "    location TEXT UNIQUE,"
            "    comment TEXT,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            "    rating INTEGER,"
            // Bite DJ: `rating` is what the view shows and may hold an
            // override this unit stored on the drive; this keeps what the
            // device's own database said, so Settings -> Clear -> Meta can put
            // it back without re-parsing the PDB.
            "    source_rating INTEGER,"
            "    analyze_path TEXT UNIQUE,"
            "    device TEXT,"
            "    color INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistsTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlists table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY,"
            "    name TEXT UNIQUE"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistTracksTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Rekordbox playlist tracks table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    playlist_id INTEGER REFERENCES rekordbox_playlists(id),"
            "    track_id INTEGER REFERENCES rekordbox_library(id),"
            "    position INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool dropTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Dropping Rekordbox table: " << tableName;

    QSqlQuery query(database);
    query.prepare("DROP TABLE IF EXISTS " + tableName);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

// This function is executed in a separate thread other than the main thread
// The returned list owns the pointers, but we can't use a unique_ptr because
// the result is passed by a const reference inside QFuture and than copied
// to the main thread requiring a copy-able object.
QList<TreeItem*> findRekordboxDevices() {
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    QList<TreeItem*> foundDevices;

#if defined(__WINDOWS__)
    // Repopulate drive list
    QFileInfoList drives = QDir::drives();
    // show drive letters
    foreach (QFileInfo drive, drives) {
        // Using drive.filePath() instead of drive.canonicalPath() as it
        // freezes interface too much if there is a network share mounted
        // (drive letter assigned) but unavailable
        //
        // drive.canonicalPath() make a system call to the underlying filesystem
        // introducing delay if it is unreadable.
        // drive.filePath() doesn't make any access to the filesystem and consequently
        // shorten the delay

        if (!findRekordboxPdbPath(drive.filePath()).isEmpty()) {
            QString displayPath = drive.filePath();
            if (displayPath.endsWith("/")) {
                displayPath.chop(1);
            }
            QList<QString> data;
            data << drive.filePath();
            data << IS_RECORDBOX_DEVICE;
            auto* pFoundDevice = new TreeItem(
                    std::move(displayPath),
                    QVariant(data));
            foundDevices << pFoundDevice;
        }
    }
#elif defined(__LINUX__)
    // To get devices on Linux, we look for directories under /media and
    // /run/media/$USER.
    QFileInfoList devices;

    // Add folders under /media to devices.
    devices += QDir(QStringLiteral("/media")).entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot);

    // The per-user mount roots only make sense when we actually know the
    // user. When USER is unset (e.g. the appliance session running as PID 1
    // with no exported USER), "/media/" + user is just "/media" again, so
    // every device would be enumerated twice and appear as two identical
    // sidebar entries whose pdb parses race each other. Same guard as
    // browsefeature's removableDriveRootPaths().
    const QString user = QString::fromLocal8Bit(qgetenv("USER"));
    if (!user.isEmpty()) {
        // Add folders under /media/$USER to devices.
        QDir mediaUserDir(QStringLiteral("/media/") + user);
        devices += mediaUserDir.entryInfoList(
                QDir::AllDirs | QDir::NoDotAndDotDot);

        // Add folders under /run/media/$USER to devices.
        QDir runMediaUserDir(QStringLiteral("/run/media/") + user);
        devices += runMediaUserDir.entryInfoList(
                QDir::AllDirs | QDir::NoDotAndDotDot);
    }

    // The scan roots can still alias each other (symlinks, bind mounts), so
    // collapse mount points that resolve to the same canonical directory.
    QSet<QString> seenMountPoints;
    foreach (QFileInfo device, devices) {
        QString canonicalPath = device.canonicalFilePath();
        if (canonicalPath.isEmpty()) {
            canonicalPath = device.absoluteFilePath();
        }
        if (seenMountPoints.contains(canonicalPath)) {
            continue;
        }
        seenMountPoints.insert(canonicalPath);

        if (!findRekordboxPdbPath(device.filePath()).isEmpty()) {
            auto* pFoundDevice = new TreeItem(
                    device.fileName(),
                    QVariant(QList<QString>{device.filePath(), IS_RECORDBOX_DEVICE}));
            foundDevices << pFoundDevice;
        }
    }
#else // __APPLE__
    QFileInfoList devices = QDir(QStringLiteral("/Volumes")).entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot);

    foreach (QFileInfo device, devices) {
        if (!findRekordboxPdbPath(device.filePath()).isEmpty()) {
            QList<QString> data;
            data << device.filePath();
            data << IS_RECORDBOX_DEVICE;
            auto* pFoundDevice = new TreeItem(
                    device.fileName(),
                    QVariant(data));
            foundDevices << pFoundDevice;
        }
    }
#endif

    return foundDevices;
}

template<typename Base, typename T>
inline bool instanceof (const T* ptr) {
    return dynamic_cast<const Base*>(ptr) != nullptr;
}

QString fromUtf16LeString(const std::string& toConvert) {
    // Kaitai uses std::string as single container for all string encodings.
    return QTextCodec::codecForName("UTF-16LE")
            ->toUnicode(toConvert.data(), static_cast<int>(toConvert.length()));
}

QString fromUtf16BeString(const std::string& toConvert) {
    // Kaitai uses std::string as single container for all string encodings.
    int length = static_cast<int>(toConvert.length()) - 2; // strip off trailing nullbyte
    return QTextCodec::codecForName("UTF-16BE")->toUnicode(toConvert.data(), length);
}

// Functions getText and parseDeviceDB are roughly based on the following Java file:
// https://github.com/Deep-Symmetry/crate-digger/commit/f09fa9fc097a2a428c43245ddd542ac1370c1adc
// getText is needed because the strings in the PDB file "have a variety of obscure representations".

QString getText(rekordbox_pdb_t::device_sql_string_t* deviceString) {
    QString text;

    if (instanceof <rekordbox_pdb_t::device_sql_short_ascii_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_short_ascii_t* shortAsciiString =
                static_cast<rekordbox_pdb_t::device_sql_short_ascii_t*>(deviceString->body());
        text = QString::fromStdString(shortAsciiString->text());
    } else if (instanceof <rekordbox_pdb_t::device_sql_long_ascii_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_long_ascii_t* longAsciiString =
                static_cast<rekordbox_pdb_t::device_sql_long_ascii_t*>(deviceString->body());
        text = QString::fromStdString(longAsciiString->text());
    } else if (instanceof <rekordbox_pdb_t::device_sql_long_utf16le_t>(deviceString->body())) {
        rekordbox_pdb_t::device_sql_long_utf16le_t* longUtf16leString =
                static_cast<rekordbox_pdb_t::device_sql_long_utf16le_t*>(deviceString->body());
        text = fromUtf16LeString(longUtf16leString->text());
    }

    // Some strings read from Rekordbox *.PDB files contain random null characters
    // which if not removed cause Mixxx to crash when attempting to read file paths
    return text.remove(QChar('\x0'));
}

int createDevicePlaylist(QSqlDatabase& database, const QString& devicePath) {
    int playlistID = kInvalidPlaylistId;

    QSqlQuery queryInsertIntoDevicePlaylist(database);
    queryInsertIntoDevicePlaylist.prepare(
            "INSERT OR IGNORE INTO " + kRekordboxPlaylistsTable +
            " (name) "
            "VALUES (:name)");

    queryInsertIntoDevicePlaylist.bindValue(":name", devicePath);

    if (!queryInsertIntoDevicePlaylist.exec()) {
        LOG_FAILED_QUERY(queryInsertIntoDevicePlaylist)
                << "devicePath: " << devicePath;
        return playlistID;
    }

    QSqlQuery idQuery(database);
    idQuery.prepare("select id from " + kRekordboxPlaylistsTable + " where name=:path");
    idQuery.bindValue(":path", devicePath);

    if (!idQuery.exec()) {
        LOG_FAILED_QUERY(idQuery)
                << "devicePath: " << devicePath;
        return playlistID;
    }

    while (idQuery.next()) {
        playlistID = idQuery.value(idQuery.record().indexOf("id")).toInt();
    }

    // The name may have pre-existed (leftovers from an unclean removal, or a
    // second parse of the same device); adopt that row's id and drop its
    // stale track links so this parse starts from an empty playlist.
    if (playlistID != kInvalidPlaylistId) {
        QSqlQuery clearTracksQuery(database);
        clearTracksQuery.prepare("delete from " + kRekordboxPlaylistTracksTable +
                " where playlist_id=:playlist_id");
        clearTracksQuery.bindValue(":playlist_id", playlistID);

        if (!clearTracksQuery.exec()) {
            LOG_FAILED_QUERY(clearTracksQuery)
                    << "devicePath: " << devicePath;
        }
    }

    return playlistID;
}

mixxx::RgbColor colorFromID(int colorID) {
    switch (static_cast<IDForColor>(colorID)) {
    case IDForColor::Pink:
        return kColorForIDPink;
    case IDForColor::Red:
        return kColorForIDRed;
    case IDForColor::Orange:
        return kColorForIDOrange;
    case IDForColor::Yellow:
        return kColorForIDYellow;
    case IDForColor::Green:
        return kColorForIDGreen;
    case IDForColor::Aqua:
        return kColorForIDAqua;
    case IDForColor::Blue:
        return kColorForIDBlue;
    case IDForColor::Purple:
        return kColorForIDPurple;
    }
    return kColorForIDNoColor;
}

void insertTrack(
        QSqlDatabase& database,
        rekordbox_pdb_t::track_row_t* track,
        QSqlQuery& query,
        QSqlQuery& queryInsertIntoDevicePlaylistTracks,
        QMap<uint32_t, QString>& artistsMap,
        QMap<uint32_t, QString>& albumsMap,
        QMap<uint32_t, QString>& genresMap,
        QMap<uint32_t, QString>& keysMap,
        const QString& devicePath,
        const QString& device,
        const FsMetaOverrideStore::MountRatings& storedRatings,
        int audioFilesCount) {
    int rbID = static_cast<int>(track->id());
    QString title = getText(track->title());
    QString artist = artistsMap[track->artist_id()];
    QString album = albumsMap[track->album_id()];
    QString year = QString::number(track->year());
    QString genre = genresMap[track->genre_id()];
    QString location = devicePath + getText(track->file_path());
    float bpm = static_cast<float>(track->tempo() / 100.0);
    int bitrate = static_cast<int>(track->bitrate());
    QString key = keysMap[track->key_id()];
    int playtime = static_cast<int>(track->duration());
    int sourceRating = static_cast<int>(track->rating());
    // A rating the DJ changed on this unit was written to the drive and wins
    // over the device's own DJ Rating, so the same stars come back on the next
    // insertion of the stick.
    int rating = storedRatings.ratingFor(location, sourceRating);
    QString comment = getText(track->comment());
    QString tracknumber = QString::number(track->track_number());
    QString anlzPath = devicePath + getText(track->analyze_path());

    query.bindValue(":rb_id", rbID);
    query.bindValue(":artist", artist);
    query.bindValue(":title", title);
    query.bindValue(":album", album);
    query.bindValue(":genre", genre);
    query.bindValue(":year", year);
    query.bindValue(":duration", playtime);
    query.bindValue(":location", location);
    query.bindValue(":rating", rating);
    query.bindValue(":source_rating", sourceRating);
    query.bindValue(":comment", comment);
    query.bindValue(":tracknumber", tracknumber);
    query.bindValue(":key", key);
    query.bindValue(":bpm", bpm);
    query.bindValue(":bitrate", bitrate);
    query.bindValue(":analyze_path", anlzPath);
    query.bindValue(":device", device);
    query.bindValue(":color",
            mixxx::RgbColor::toQVariant(
                    colorFromID(static_cast<int>(track->color_id()))));

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    int trackID = -1;
    QSqlQuery finderQuery(database);
    finderQuery.prepare("select id from " + kRekordboxLibraryTable +
            " where rb_id=:rb_id and device=:device");
    finderQuery.bindValue(":rb_id", rbID);
    finderQuery.bindValue(":device", device);

    if (!finderQuery.exec()) {
        LOG_FAILED_QUERY(finderQuery)
                << "rbID:" << rbID;
    }

    if (finderQuery.next()) {
        trackID = finderQuery.value(finderQuery.record().indexOf("id")).toInt();
    }

    // Insert into device all tracks playlist
    queryInsertIntoDevicePlaylistTracks.bindValue(":track_id", trackID);
    queryInsertIntoDevicePlaylistTracks.bindValue(":position", audioFilesCount);

    if (!queryInsertIntoDevicePlaylistTracks.exec()) {
        LOG_FAILED_QUERY(queryInsertIntoDevicePlaylistTracks)
                << "trackID:" << trackID
                << "position:" << audioFilesCount;
    }
}

void buildPlaylistTree(
        QSqlDatabase& database,
        TreeItem* parent,
        uint32_t parentID,
        QMap<uint32_t, QString>& playlistNameMap,
        QMap<uint32_t, bool>& playlistIsFolderMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTreeMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTrackMap,
        const QString& playlistPath,
        const QString& device);

QString parseDeviceDB(mixxx::DbConnectionPoolPtr dbConnectionPool, TreeItem* deviceItem) {
    QString device = deviceItem->getLabel();
    QString devicePath = deviceItem->getData().toList()[0].toString();

    qDebug() << "parseDeviceDB device: " << device << " devicePath: " << devicePath;

    const QString dbPath = findRekordboxPdbPath(devicePath);

    if (dbPath.isEmpty()) {
        return devicePath;
    }

    // Every rating this (or another) Bite DJ unit stored on the drive, read in
    // one pass before the walk: the PDB holds thousands of tracks and cannot
    // afford a store lookup per track. An empty result is the ordinary case and
    // leaves every rating exactly as the device exported it.
    const FsMetaOverrideStore::MountRatings storedRatings =
            FsMetaOverrideStore::readMountRatings(devicePath);

    // The pooler limits the lifetime all thread-local connections,
    // that should be closed immediately before exiting this function.
    const mixxx::DbConnectionPooler dbConnectionPooler(dbConnectionPool);
    QSqlDatabase database = mixxx::DbConnectionPooled(dbConnectionPool);

    //Open the database connection in this thread.
    VERIFY_OR_DEBUG_ASSERT(database.isOpen()) {
        qDebug() << "Failed to open database for Rekordbox parser."
                 << database.lastError();
        return QString();
    }

    //Give thread a low priority
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    // Bite DJ: the PDB is walked first and written second, deliberately.
    //
    // Reading the PDB means seeking around a file on a USB stick, which on this
    // hardware takes seconds for a large library. Writing means holding the
    // library database's single writer slot. Doing both at once -- as this used
    // to, with one transaction wrapped around the whole walk -- pins the write
    // lock for the entire duration of the USB read, and every other connection
    // that wants to write (an eject clearing this device's rows, a rating being
    // stored, the sidebar merging a newly found device) blocks on the GUI
    // thread until the stick is done. So: no transaction is open below, and
    // nothing here touches the database until the walk has finished.
    //
    // kaitai reads each page into memory in one go (page_ref_t::body()), so the
    // row pointers collected here stay valid -- and the lazy string accessors
    // insertTrack() uses read from that in-memory page, not from the device --
    // for as long as `rekordboxDB` is alive.
    mixxx::FileInfo fileInfo(dbPath);
    if (!Sandbox::askForAccess(&fileInfo)) {
        return QString();
    }
    std::ifstream ifs(dbPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_pdb_t rekordboxDB = rekordbox_pdb_t(&ks);

    // There are other types of tables (eg. COLOR), these are the only ones we are
    // interested at the moment. Perhaps when/if
    // https://github.com/mixxxdj/mixxx/issues/6852
    // is completed, this can be revisited.
    // Attempt was made to also recover HISTORY
    // playlists (which are found on removable Rekordbox devices), however
    // they didn't appear to contain valid row_ref_t structures.
    constexpr int totalTables = 8;

    rekordbox_pdb_t::page_type_t tableOrder[totalTables] = {
            rekordbox_pdb_t::PAGE_TYPE_KEYS,
            rekordbox_pdb_t::PAGE_TYPE_GENRES,
            rekordbox_pdb_t::PAGE_TYPE_ARTISTS,
            rekordbox_pdb_t::PAGE_TYPE_ALBUMS,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES,
            rekordbox_pdb_t::PAGE_TYPE_TRACKS,
            rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE,
            rekordbox_pdb_t::PAGE_TYPE_HISTORY};

    QMap<uint32_t, QString> keysMap;
    QMap<uint32_t, QString> genresMap;
    QMap<uint32_t, QString> artistsMap;
    QMap<uint32_t, QString> albumsMap;
    QMap<uint32_t, QString> playlistNameMap;
    QMap<uint32_t, bool> playlistIsFolderMap;
    QMap<uint32_t, QMap<uint32_t, uint32_t>> playlistTreeMap;
    QMap<uint32_t, QMap<uint32_t, uint32_t>> playlistTrackMap;
    std::vector<rekordbox_pdb_t::track_row_t*> trackRows;

    bool folderOrPlaylistFound = false;

    for (int tableOrderIndex = 0; tableOrderIndex < totalTables; tableOrderIndex++) {
        for (const auto& table : *rekordboxDB.tables()) {
            if (table->type() == tableOrder[tableOrderIndex]) {
                uint16_t lastIndex = table->last_page()->index();
                rekordbox_pdb_t::page_ref_t* currentRef = table->first_page();

                while (true) {
                    rekordbox_pdb_t::page_t* page = currentRef->body();

                    if (page->is_data_page()) {
                        for (const auto& rowgroup : *page->row_groups()) {
                            for (const auto& rowRef : *rowgroup->rows()) {
                                if (rowRef->present()) {
                                    switch (tableOrder[tableOrderIndex]) {
                                    case rekordbox_pdb_t::PAGE_TYPE_KEYS: {
                                        auto* key =
                                                static_cast<rekordbox_pdb_t::key_row_t*>(
                                                        rowRef->body());
                                        keysMap[key->id()] = getText(key->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_GENRES: {
                                        auto* genre =
                                                static_cast<rekordbox_pdb_t::genre_row_t*>(
                                                        rowRef->body());
                                        genresMap[genre->id()] = getText(genre->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_ARTISTS: {
                                        auto* artist =
                                                static_cast<rekordbox_pdb_t::artist_row_t*>(
                                                        rowRef->body());
                                        artistsMap[artist->id()] = getText(artist->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_ALBUMS: {
                                        auto* album =
                                                static_cast<rekordbox_pdb_t::album_row_t*>(
                                                        rowRef->body());
                                        albumsMap[album->id()] = getText(album->name());
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_ENTRIES: {
                                        auto* playlistEntry =
                                                static_cast<rekordbox_pdb_t::playlist_entry_row_t*>(
                                                        rowRef->body());
                                        playlistTrackMap
                                                [playlistEntry->playlist_id()]
                                                [playlistEntry->entry_index()] =
                                                        playlistEntry
                                                                ->track_id();
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_TRACKS: {
                                        // Written out below, once the device
                                        // is no longer being read from.
                                        trackRows.push_back(
                                                static_cast<rekordbox_pdb_t::track_row_t*>(
                                                        rowRef->body()));
                                    } break;
                                    case rekordbox_pdb_t::PAGE_TYPE_PLAYLIST_TREE: {
                                        auto* playlistTree =
                                                static_cast<rekordbox_pdb_t::playlist_tree_row_t*>(
                                                        rowRef->body());

                                        playlistNameMap[playlistTree->id()] =
                                                getText(playlistTree->name());
                                        playlistIsFolderMap[playlistTree
                                                                    ->id()] =
                                                playlistTree->is_folder();
                                        playlistTreeMap
                                                [playlistTree->parent_id()]
                                                [playlistTree->sort_order()] =
                                                        playlistTree->id();

                                        folderOrPlaylistFound = true;
                                    } break;
                                    default:
                                        // we currently don't handle any other
                                        // data, even though there is more.
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (currentRef->index() == lastIndex) {
                        break;
                    } else {
                        currentRef = page->next_page();
                    }
                }
            }
        }
    }

    // The device has been read; everything below is local database work only,
    // so the write lock is held for as short a time as the inserts take.
    ScopedTransaction transaction(database);

    QSqlQuery query(database);
    query.prepare("INSERT OR IGNORE INTO " + kRekordboxLibraryTable +
            " (rb_id, artist, title, album, year,"
            "genre,comment,tracknumber,bpm, bitrate,duration, location,"
            "rating,source_rating,key,analyze_path,device,color) VALUES "
            "(:rb_id, :artist, "
            ":title, :album, :year,:genre,"
            ":comment, :tracknumber,:bpm, :bitrate,:duration, :location,"
            ":rating,:source_rating,:key,:analyze_path,:device,:color)");

    // Create a playlist for all the tracks on a device
    int playlistID = createDevicePlaylist(database, devicePath);

    QSqlQuery queryInsertIntoDevicePlaylistTracks(database);
    queryInsertIntoDevicePlaylistTracks.prepare(
            "INSERT INTO " + kRekordboxPlaylistTracksTable +
            " (playlist_id, track_id, position) "
            "VALUES (:playlist_id, :track_id, :position)");

    queryInsertIntoDevicePlaylistTracks.bindValue(":playlist_id", playlistID);

    int audioFilesCount = 0;
    for (rekordbox_pdb_t::track_row_t* trackRow : trackRows) {
        insertTrack(database,
                trackRow,
                query,
                queryInsertIntoDevicePlaylistTracks,
                artistsMap,
                albumsMap,
                genresMap,
                keysMap,
                devicePath,
                device,
                storedRatings,
                audioFilesCount);

        audioFilesCount++;
    }

    if (audioFilesCount > 0 || folderOrPlaylistFound) {
        // If we have found anything, recursively build playlist/folder TreeItem children
        // for the original device TreeItem
        buildPlaylistTree(database,
                deviceItem,
                0,
                playlistNameMap,
                playlistIsFolderMap,
                playlistTreeMap,
                playlistTrackMap,
                devicePath,
                device);
    }

    qDebug() << "Found: " << audioFilesCount << " audio files in Rekordbox device " << device;

    transaction.commit();

    return devicePath;
}

void buildPlaylistTree(
        QSqlDatabase& database,
        TreeItem* parent,
        uint32_t parentID,
        QMap<uint32_t, QString>& playlistNameMap,
        QMap<uint32_t, bool>& playlistIsFolderMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTreeMap,
        QMap<uint32_t, QMap<uint32_t, uint32_t>>& playlistTrackMap,
        const QString& playlistPath,
        const QString& device) {
    for (uint32_t childIndex = 0;
            childIndex < (uint32_t)playlistTreeMap[parentID].size();
            childIndex++) {
        uint32_t childID = playlistTreeMap[parentID][childIndex];
        if (childID == 0) {
            continue;
        }
        QString playlistItemName = playlistNameMap[childID];

        QString currentPath = playlistPath + kPLaylistPathDelimiter + playlistItemName;

        TreeItem* child = parent->appendChild(playlistItemName,
                QVariant(QList<QString>{currentPath, IS_NOT_RECORDBOX_DEVICE}));

        // Create a playlist for this child. A failure here must not abort the
        // sibling loop — that would silently drop every playlist after it.
        QSqlQuery queryInsertIntoPlaylist(database);
        queryInsertIntoPlaylist.prepare(
                "INSERT OR IGNORE INTO " + kRekordboxPlaylistsTable +
                " (name) "
                "VALUES (:name)");

        queryInsertIntoPlaylist.bindValue(":name", currentPath);

        if (!queryInsertIntoPlaylist.exec()) {
            LOG_FAILED_QUERY(queryInsertIntoPlaylist)
                    << "currentPath" << currentPath;
            continue;
        }

        QSqlQuery idQuery(database);
        idQuery.prepare("select id from " + kRekordboxPlaylistsTable + " where name=:path");
        idQuery.bindValue(":path", currentPath);

        if (!idQuery.exec()) {
            LOG_FAILED_QUERY(idQuery)
                    << "currentPath" << currentPath;
            continue;
        }

        int playlistID = kInvalidPlaylistId;
        while (idQuery.next()) {
            playlistID = idQuery.value(idQuery.record().indexOf("id")).toInt();
        }

        // The name may have pre-existed (leftovers from an unclean removal);
        // adopt that row's id and drop its stale track links so this parse
        // starts from an empty playlist.
        if (playlistID != kInvalidPlaylistId) {
            QSqlQuery clearTracksQuery(database);
            clearTracksQuery.prepare("delete from " + kRekordboxPlaylistTracksTable +
                    " where playlist_id=:playlist_id");
            clearTracksQuery.bindValue(":playlist_id", playlistID);

            if (!clearTracksQuery.exec()) {
                LOG_FAILED_QUERY(clearTracksQuery)
                        << "currentPath" << currentPath;
            }
        }

        QSqlQuery queryInsertIntoPlaylistTracks(database);
        queryInsertIntoPlaylistTracks.prepare(
                "INSERT INTO " + kRekordboxPlaylistTracksTable +
                " (playlist_id, track_id, position) "
                "VALUES (:playlist_id, :track_id, :position)");

        if (playlistID != kInvalidPlaylistId && playlistTrackMap.contains(childID)) {
            // Add playlist tracks for children
            for (uint32_t trackIndex = 1; trackIndex <=
                    static_cast<uint32_t>(playlistTrackMap[childID].size());
                    trackIndex++) {
                uint32_t rbTrackID = playlistTrackMap[childID][trackIndex];

                int trackID = -1;
                QSqlQuery finderQuery(database);
                finderQuery.prepare("select id from " + kRekordboxLibraryTable +
                        " where rb_id=:rb_id and device=:device");
                finderQuery.bindValue(":rb_id", rbTrackID);
                finderQuery.bindValue(":device", device);

                if (!finderQuery.exec()) {
                    LOG_FAILED_QUERY(finderQuery)
                            << "rbTrackID:" << rbTrackID
                            << "device:" << device;
                    continue;
                }

                if (finderQuery.next()) {
                    trackID = finderQuery.value(finderQuery.record().indexOf("id")).toInt();
                }

                queryInsertIntoPlaylistTracks.bindValue(":playlist_id", playlistID);
                queryInsertIntoPlaylistTracks.bindValue(":track_id", trackID);
                queryInsertIntoPlaylistTracks.bindValue(":position", static_cast<int>(trackIndex));

                if (!queryInsertIntoPlaylistTracks.exec()) {
                    LOG_FAILED_QUERY(queryInsertIntoPlaylistTracks)
                            << "playlistID:" << playlistID
                            << "trackID:" << trackID
                            << "trackIndex:" << trackIndex;
                }
            }
        }

        if (playlistIsFolderMap[childID]) {
            // If this child is a folder (playlists are only leaf nodes), build playlist tree for it
            buildPlaylistTree(database,
                    child,
                    childID,
                    playlistNameMap,
                    playlistIsFolderMap,
                    playlistTreeMap,
                    playlistTrackMap,
                    currentPath,
                    device);
        }
    }
}

// Playlist names are the device path itself (the "all tracks" playlist) or
// "<devicePath>-->Folder-->Playlist", so the leading section is always the
// device's mount directory.
QString devicePathOfPlaylist(const QString& playlist) {
    return playlist.section(kPLaylistPathDelimiter, 0, 0);
}

void clearDeviceTables(QSqlDatabase& database, TreeItem* child) {
    ScopedTransaction transaction(database);

    // Playlist names are the device path itself (the "all tracks" playlist)
    // or prefixed with "<devicePath>-->", so a prefix match reaches every row
    // for this device — including folders and empty playlists, which have no
    // playlist_tracks links and would leak if discovered via tracks.
    const QList<QVariant> data = child->getData().toList();
    VERIFY_OR_DEBUG_ASSERT(!data.isEmpty() && !data[0].toString().isEmpty()) {
        return;
    }
    const QString devicePath = data[0].toString();
    QString likePattern = devicePath;
    likePattern.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    likePattern.replace(QLatin1Char('%'), QLatin1String("\\%"));
    likePattern.replace(QLatin1Char('_'), QLatin1String("\\_"));
    likePattern += kPLaylistPathDelimiter + QLatin1Char('%');

    QSqlQuery deletePlaylistTracksQuery(database);
    deletePlaylistTracksQuery.prepare("delete from " + kRekordboxPlaylistTracksTable +
            " where playlist_id in (select id from " + kRekordboxPlaylistsTable +
            " where name=:path or name like :pattern escape '\\')");
    deletePlaylistTracksQuery.bindValue(":path", devicePath);
    deletePlaylistTracksQuery.bindValue(":pattern", likePattern);

    if (!deletePlaylistTracksQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistTracksQuery)
                << "devicePath:" << devicePath;
    }

    QSqlQuery deletePlaylistsQuery(database);
    deletePlaylistsQuery.prepare("delete from " + kRekordboxPlaylistsTable +
            " where name=:path or name like :pattern escape '\\'");
    deletePlaylistsQuery.bindValue(":path", devicePath);
    deletePlaylistsQuery.bindValue(":pattern", likePattern);

    if (!deletePlaylistsQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistsQuery)
                << "devicePath:" << devicePath;
    }

    QSqlQuery deleteTracksQuery(database);
    deleteTracksQuery.prepare("delete from " + kRekordboxLibraryTable + " where device=:device");
    deleteTracksQuery.bindValue(":device", child->getLabel());

    if (!deleteTracksQuery.exec()) {
        LOG_FAILED_QUERY(deleteTracksQuery)
                << "device:" << child->getLabel();
    }

    transaction.commit();
}

void setHotCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int id,
        const QString& label,
        mixxx::RgbColor::optional_t color,
        QSet<int>* pImportedIndices) {
    if (pImportedIndices) {
        pImportedIndices->insert(id);
    }
    CuePointer pCue;
    const QList<CuePointer> cuePoints = track->getCuePoints();
    for (const CuePointer& trackCue : cuePoints) {
        if (trackCue->getHotCue() == id) {
            pCue = trackCue;
            break;
        }
    }

    mixxx::CueType type = mixxx::CueType::HotCue;
    if (endPosition.isValid()) {
        type = mixxx::CueType::Loop;
    }

    if (pCue) {
        pCue->setStartAndEndPosition(startPosition, endPosition);
        // A pad can change between a plain cue and a saved loop in rekordbox
        // without changing slot, so the type has to follow the end position.
        pCue->setType(type);
    } else {
        pCue = track->createAndAddCue(
                type,
                id,
                startPosition,
                endPosition);
    }
    pCue->setLabel(label);
    if (color) {
        pCue->setColor(*color);
    }
}

} // anonymous namespace

namespace mixxx {
namespace rekordbox {

void readAnalyze(TrackPointer track,
        mixxx::audio::SampleRate sampleRate,
        int timingOffset,
        bool ignoreCues,
        const QString& anlzPath) {
    if (!QFile(anlzPath).exists()) {
        return;
    }

    qDebug() << "Rekordbox ANLZ path:" << anlzPath << " for: " << track->getTitle();

    std::ifstream ifs(anlzPath.toStdString(), std::ifstream::binary);
    kaitai::kstream ks(&ifs);

    rekordbox_anlz_t anlz = rekordbox_anlz_t(&ks);

    const double sampleRateKhz = sampleRate / 1000.0;

    QList<memory_cue_loop_t> memoryCuesAndLoops;

    // Slots this pass writes. Anything else the track is still carrying was
    // deleted in rekordbox, or was left in a slot the old banking used, and is
    // pruned at the end. Cues that survive are updated in place rather than
    // recreated: readAnalyze runs on every getTrack, which hands back the same
    // cached Track a deck may already be playing, so rebuilding them wholesale
    // would blank that deck's pads and drop an active saved loop mid-set.
    QSet<int> importedHotcueIndices;

    for (const auto& section : *anlz.sections()) {
        switch (section->fourcc()) {
        case rekordbox_anlz_t::SECTION_TAGS_BEAT_GRID: {
            if (!ignoreCues) {
                break;
            }

            auto* beatGridTag =
                    static_cast<rekordbox_anlz_t::beat_grid_tag_t*>(
                            section->body());

            QVector<mixxx::audio::FramePos> beats;

            for (const auto& beat : *beatGridTag->beats()) {
                int time = static_cast<int>(beat->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                beats << mixxx::audio::FramePos(sampleRateKhz * static_cast<double>(time));
            }

            const auto pBeats = mixxx::Beats::fromBeatPositions(
                    sampleRate,
                    beats,
                    mixxx::rekordboxconstants::beatsSubversion);
            track->trySetBeats(pBeats);
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES: {
            if (ignoreCues) {
                break;
            }

            auto* cuesTag =
                    static_cast<rekordbox_anlz_t::cue_tag_t*>(
                            section->body());

            for (const auto& cueEntry : *cuesTag->cues()) {
                int time = static_cast<int>(cueEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime = static_cast<int>(cueEntry->loop_time()) - timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.color = mixxx::RgbColor::nullopt();
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueEntry->hot_cue() - 1);
                    if (hotCueIndex < mixxx::kHotCueBankStart ||
                            hotCueIndex >= mixxx::kHotCueBankStart +
                                            mixxx::kHotCueBankSize) {
                        break;
                    }
                    // A hot cue pad can hold a saved loop; carry its end
                    // position over so it imports as CueType::Loop rather than
                    // collapsing to a plain cue point.
                    mixxx::audio::FramePos endPosition =
                            mixxx::audio::kInvalidFramePos;
                    if (cueEntry->type() ==
                            rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP) {
                        int endTime = static_cast<int>(cueEntry->loop_time()) - timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }
                        endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                    }
                    setHotCue(
                            track,
                            position,
                            endPosition,
                            hotCueIndex,
                            QString(),
                            mixxx::RgbColor::nullopt(),
                            &importedHotcueIndices);
                } break;
                }
            }
        } break;
        case rekordbox_anlz_t::SECTION_TAGS_CUES_2: {
            if (ignoreCues) {
                break;
            }

            auto* cuesExtendedTag =
                    static_cast<rekordbox_anlz_t::cue_extended_tag_t*>(
                            section->body());

            for (const auto& cueExtendedEntry : *cuesExtendedTag->cues()) {
                int time = static_cast<int>(cueExtendedEntry->time()) - timingOffset;
                // Ensure no offset times are less than 1
                if (time < 1) {
                    time = 1;
                }
                const auto position = mixxx::audio::FramePos(
                        sampleRateKhz * static_cast<double>(time));

                switch (cuesExtendedTag->type()) {
                case rekordbox_anlz_t::CUE_LIST_TYPE_MEMORY_CUES: {
                    switch (cueExtendedEntry->type()) {
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_MEMORY_CUE: {
                        memory_cue_loop_t memoryCue;
                        memoryCue.startPosition = position;
                        memoryCue.endPosition = mixxx::audio::kInvalidFramePos;
                        memoryCue.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        memoryCue.color = colorFromID(static_cast<int>(
                                cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << memoryCue;
                    } break;
                    case rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP: {
                        int endTime =
                                static_cast<int>(
                                        cueExtendedEntry->loop_time()) -
                                timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }

                        memory_cue_loop_t loop;
                        loop.startPosition = position;
                        loop.endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                        loop.comment = fromUtf16BeString(cueExtendedEntry->comment());
                        loop.color = colorFromID(static_cast<int>(cueExtendedEntry->color_id()));
                        memoryCuesAndLoops << loop;
                    } break;
                    }
                } break;
                case rekordbox_anlz_t::CUE_LIST_TYPE_HOT_CUES: {
                    int hotCueIndex = static_cast<int>(cueExtendedEntry->hot_cue() - 1);
                    if (hotCueIndex < mixxx::kHotCueBankStart ||
                            hotCueIndex >= mixxx::kHotCueBankStart +
                                            mixxx::kHotCueBankSize) {
                        break;
                    }
                    // A hot cue pad can hold a saved loop; carry its end
                    // position over so it imports as CueType::Loop rather than
                    // collapsing to a plain cue point.
                    mixxx::audio::FramePos endPosition =
                            mixxx::audio::kInvalidFramePos;
                    if (cueExtendedEntry->type() ==
                            rekordbox_anlz_t::CUE_ENTRY_TYPE_LOOP) {
                        int endTime =
                                static_cast<int>(cueExtendedEntry->loop_time()) -
                                timingOffset;
                        // Ensure no offset times are less than 1
                        if (endTime < 1) {
                            endTime = 1;
                        }
                        endPosition = mixxx::audio::FramePos(
                                sampleRateKhz * static_cast<double>(endTime));
                    }
                    setHotCue(track,
                            position,
                            endPosition,
                            hotCueIndex,
                            fromUtf16BeString(cueExtendedEntry->comment()),
                            mixxx::RgbColor(qRgb(
                                    static_cast<int>(
                                            cueExtendedEntry->color_red()),
                                    static_cast<int>(
                                            cueExtendedEntry->color_green()),
                                    static_cast<int>(cueExtendedEntry
                                                    ->color_blue()))),
                            &importedHotcueIndices);
                } break;
                }
            }
        } break;
        default:
            break;
        }
    }

    if (memoryCuesAndLoops.size() > 0) {
        std::sort(memoryCuesAndLoops.begin(),
                memoryCuesAndLoops.end(),
                [](const memory_cue_loop_t& a, const memory_cue_loop_t& b)
                        -> bool { return a.startPosition < b.startPosition; });

        bool mainCueFound = false;
        int memoryCueBankIndex = mixxx::kMemoryCueBankStart;
        const int memoryCueBankEnd =
                mixxx::kMemoryCueBankStart + mixxx::kMemoryCueBankSize;

        // Add memory cues and loops. They land in their own bank of hotcue
        // slots rather than trailing the hot cues, so that a track's pads
        // stay put no matter how many memory cues it carries.
        for (const memory_cue_loop_t& memoryCueOrLoop :
                std::as_const(memoryCuesAndLoops)) {
            if (!mainCueFound && !memoryCueOrLoop.endPosition.isValid()) {
                // Set first chronological memory cue as Mixxx MainCue
                track->setMainCuePosition(memoryCueOrLoop.startPosition);
                CuePointer pMainCue = track->findCueByType(mixxx::CueType::MainCue);
                pMainCue->setLabel(memoryCueOrLoop.comment);
                if (memoryCueOrLoop.color) {
                    pMainCue->setColor(*memoryCueOrLoop.color);
                }
                mainCueFound = true;
            }

            // The main cue keeps its slot in the bank as well, so it stays
            // callable from the memory pads the way it is on a CDJ.
            if (memoryCueBankIndex >= memoryCueBankEnd) {
                continue;
            }
            setHotCue(
                    track,
                    memoryCueOrLoop.startPosition,
                    memoryCueOrLoop.endPosition,
                    memoryCueBankIndex,
                    memoryCueOrLoop.comment,
                    memoryCueOrLoop.color,
                    &importedHotcueIndices);
            memoryCueBankIndex++;
        }
    }

    if (!ignoreCues) {
        // Drop the slots this pass didn't write: cues deleted in rekordbox
        // since the last load, and memory cues left behind in the hot cue
        // slots by the banking this replaced.
        QList<CuePointer> staleCues;
        const QList<CuePointer> cuePoints = track->getCuePoints();
        for (const CuePointer& pCue : cuePoints) {
            const int hotcueIndex = pCue->getHotCue();
            if (hotcueIndex == Cue::kNoHotCue ||
                    importedHotcueIndices.contains(hotcueIndex)) {
                continue;
            }
            const mixxx::CueType type = pCue->getType();
            if (type == mixxx::CueType::HotCue || type == mixxx::CueType::Loop) {
                staleCues << pCue;
            }
        }
        for (const CuePointer& pCue : std::as_const(staleCues)) {
            track->removeCue(pCue);
        }
    }
}

} // namespace rekordbox
} // namespace mixxx

RekordboxPlaylistModel::RekordboxPlaylistModel(QObject* parent,
        TrackCollectionManager* trackCollectionManager,
        QSharedPointer<BaseTrackCache> trackSource)
        : BaseExternalPlaylistModel(parent,
                  trackCollectionManager,
                  "mixxx.db.model.rekordbox.playlistmodel",
                  kRekordboxPlaylistsTable,
                  kRekordboxPlaylistTracksTable,
                  trackSource) {
}

void RekordboxPlaylistModel::initSortColumnMapping() {
    // Add a bijective mapping between the SortColumnIds and column indices
    for (int i = 0; i < static_cast<int>(TrackModel::SortColumnId::IdMax); ++i) {
        m_columnIndexBySortColumnId[i] = -1;
    }

    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Artist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Title)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TITLE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Album)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::AlbumArtist)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Year)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Genre)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GENRE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Composer)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GROUPING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TrackNumber)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileType)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::NativeLocation)] =
            fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Comment)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMMENT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Duration)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DURATION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::BitRate)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Bpm)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::ReplayGain)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::DateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TimesPlayed)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::LastPlayedAt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Rating)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Key)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Preview)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Color)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COLOR);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::CoverArt)] =
            fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Position)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION);
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::PlaylistDateTimeAdded)] =
            fieldIndex(ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED);

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        TrackModel::SortColumnId sortColumn = static_cast<TrackModel::SortColumnId>(i);
        m_sortColumnIdByColumnIndex.insert(
                m_columnIndexBySortColumnId[static_cast<int>(sortColumn)],
                sortColumn);
    }
}

TrackPointer RekordboxPlaylistModel::getTrack(const QModelIndex& index) const {
    qDebug() << "RekordboxTrackModel::getTrack";

    TrackPointer track = BaseExternalPlaylistModel::getTrack(index);
    QString location = getFieldVariant(
            index, ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION)
                               .toString();

    if (!track || !QFile(location).exists()) {
        return track;
    }

    // getTrack() hands back the very same Track a deck may already be playing,
    // and readAnalyze() below rewrites its cues from the ANLZ file. Store any
    // cue the DJ has set since the track was loaded before that happens, or
    // the rekordbox import would erase it before it was ever saved.
    FsCueOverrideStore::flushIfChanged(*track);

    // The following code accounts for timing offsets required to
    // correctly align timing information (cue points, loops, beatgrids)
    // exported from Rekordbox. This is caused by different MP3
    // decoders treating MP3s encoded in a variety of different cases
    // differently. The mp3guessenc library is used to determine which
    // case the MP3 is classified in. See the following PR for more
    // detailed information:
    // https://github.com/mixxxdj/mixxx/pull/2119

    int timingOffset = 0;

    if (location.endsWith(".mp3", Qt::CaseInsensitive)) {
        int timingShiftCase = mp3guessenc_timing_shift_case(location.toStdString().c_str());

        qDebug() << "Timing shift case:" << timingShiftCase << "for MP3 file:" << location;

        switch (timingShiftCase) {
#ifdef __COREAUDIO__
        case EXIT_CODE_CASE_A:
            timingOffset = 12;
            break;
        case EXIT_CODE_CASE_B:
            timingOffset = 13;
            break;
        case EXIT_CODE_CASE_C:
            timingOffset = 26;
            break;
        case EXIT_CODE_CASE_D:
            timingOffset = 50;
            break;
#elif defined(__MAD__)
        case EXIT_CODE_CASE_A:
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#elif defined(__FFMPEG__)
        case EXIT_CODE_CASE_D:
            timingOffset = 26;
            break;
#endif
        }
    }

#ifdef __COREAUDIO__
    if (location.toLower().endsWith(".m4a")) {
        timingOffset = 48;
    }
#endif

    mixxx::audio::SampleRate sampleRate = track->getSampleRate();

    QString anlzPath =
            getFieldVariant(index, ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH)
                    .toString();
    QString anlzPathExt = anlzPath.left(anlzPath.length() - 3) + "EXT";

    if (QFile(anlzPathExt).exists()) {
        // Beatgrids appear to be only correct in legacy ANLZ file
        mixxx::rekordbox::readAnalyze(track, sampleRate, timingOffset, true, anlzPath);
        mixxx::rekordbox::readAnalyze(track, sampleRate, timingOffset, false, anlzPathExt);
    } else {
        mixxx::rekordbox::readAnalyze(track, sampleRate, timingOffset, false, anlzPath);
    }

    // Cues stored on the drive by this unit are the DJ's own and outrank the
    // ones rekordbox exported, so they go on last — after the ANLZ import has
    // had its say on every slot.
    FsCueOverrideStore::applyOverrides(track.get());
    // Likewise the rating: the stars this view shows come from the drive once
    // the DJ has changed them, so a deck loaded from here shows the same ones.
    FsMetaOverrideStore::applyOverrides(track.get());

    // Assume that the key of the file the has been analyzed in Recordbox is correct
    // and prevent the AnalyzerKey from re-analyzing.
    // Form 5.4.3 Key format depends on the preferences option:
    // Classic: Abm,B,Ebm,F#,Bbm,Db,Fm,Ab,…,F#m,A,Dbm,E
    // Alphanumeric (Camelot): 1A,1B,2A,2B,3A,3B,4A,4B,…,11A,11B,12A,12B
    // Not reckognized: 1m, 01A
    // Earlier versions allow any format
    // Decision: We normalize the KeyText here to not write garbage to the
    // file metadata and it is unlikely to loose extra info.
    track->setKeys(KeyFactory::makeBasicKeysNormalized(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_KEY).toString(),
            mixxx::track::io::key::USER));

    track->setColor(mixxx::RgbColor::fromQVariant(
            getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_COLOR)));

    return track;
}

bool RekordboxPlaylistModel::isColumnHiddenByDefault(int column) {
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE)) {
        return true;
    }
    return BaseSqlTableModel::isColumnHiddenByDefault(column);
}

bool RekordboxPlaylistModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_REKORDBOX_ANALYZE_PATH) ||
            BaseExternalPlaylistModel::isColumnInternal(column);
}

Qt::ItemFlags RekordboxPlaylistModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags itemFlags = readOnlyFlags(index);
    if (index.column() == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING)) {
        itemFlags |= Qt::ItemIsEditable;
    }
    return itemFlags;
}

bool RekordboxPlaylistModel::setData(
        const QModelIndex& index, const QVariant& value, int role) {
    if (role == Qt::EditRole && index.isValid() &&
            index.column() == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING)) {
        return setRatingOverride(index, value);
    }
    return BaseExternalPlaylistModel::setData(index, value, role);
}

bool RekordboxPlaylistModel::setRatingOverride(
        const QModelIndex& index, const QVariant& value) {
    const int rating = value.value<StarRating>().starCount();
    if (!mixxx::TrackRecord::isValidRating(rating)) {
        return false;
    }
    const QString location = getTrackLocation(index);
    if (location.isEmpty()) {
        return false;
    }

    // A track that is in a deck right now has to hear about this too, but
    // finding out must not pull one that is not into memory: the global cache
    // is consulted, never the database (getTrack() here imports the track and
    // re-reads its ANLZ file).
    const TrackPointer pTrack = GlobalTrackCacheLocker().lookupTrackByRef(
            TrackRef::fromFilePath(location));
    if (pTrack) {
        // What this track carried before the DJ's first rating on this unit,
        // so that Settings -> Clear -> Meta can put it back.
        FsMetaOverrideStore::noteImportedRating(location, pTrack->getRating());
    }

    // The drive is the only place a rekordbox track's rating can live: the
    // stars in this view come from the device's exported database, which this
    // unit does not write to. A stick that refuses the write keeps its rating.
    if (!FsMetaOverrideStore::storeRating(location, rating)) {
        if (Notifications* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(
                    tr("Could not save the rating to the USB drive"),
                    Notifications::Severity::Warning);
        }
        return false;
    }
    if (pTrack) {
        pTrack->setRating(rating);
    }
    // The cell itself is repainted from the scanned copy of the device library,
    // which RekordboxFeature updates off the store's ratingStored signal.
    return true;
}

RekordboxFeature::RekordboxFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("rekordbox")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    QString tableName = kRekordboxLibraryTable;
    QString idColumn = LIBRARYTABLE_ID;
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

    const QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            tableName,
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pRekordboxPlaylistModel = make_parented<RekordboxPlaylistModel>(
            this, pLibrary->trackCollectionManager(), m_trackSource);

    m_title = tr("Rekordbox");

    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    // Drop any leftover temporary Rekordbox database tables if they exist
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);

    // Create new temporary Rekordbox database tables
    createLibraryTable(database, kRekordboxLibraryTable);
    createPlaylistsTable(database, kRekordboxPlaylistsTable);
    createPlaylistTracksTable(database, kRekordboxPlaylistTracksTable);
    transaction.commit();

    connect(&m_devicesFutureWatcher,
            &QFutureWatcher<QList<TreeItem*>>::finished,
            this,
            &RekordboxFeature::onRekordboxDevicesFound);
    connect(&m_tracksFutureWatcher,
            &QFutureWatcher<QString>::finished,
            this,
            &RekordboxFeature::onTracksFound);
    // Bite DJ: drop a device from the sidebar the moment its drive is
    // unmounted (in-skin Eject, physical yank, external unmount), rather than
    // waiting for the background poll to notice it has gone.
    connect(pLibrary,
            &Library::mountEjected,
            this,
            &RekordboxFeature::ejectDevice);
    // Bite DJ: the ratings in these views are a scanned copy of the device's
    // own database, so they do not follow a track the way the rest of the
    // library does. Track both ends of that: a rating stored on a drive (from
    // this view or from a deck) and the settings action that wipes them all.
    // Touching the notifier here also gives it this thread's affinity, so a
    // store write from a worker thread arrives queued.
    connect(&FsMetaOverrideNotifier::instance(),
            &FsMetaOverrideNotifier::ratingStored,
            this,
            &RekordboxFeature::onRatingOverrideStored);
    connect(pLibrary,
            &Library::metaOverridesCleared,
            this,
            &RekordboxFeature::onMetaOverridesCleared);
    // initialize the model
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));

    // Background polling: surface mounted Rekordbox drives and pre-parse
    // their PDB without requiring a user tap. Wired after foreground
    // watcher connections so the first tick always finds connections in
    // place.
    connect(&m_bgDevicesFutureWatcher,
            &QFutureWatcher<QList<TreeItem*>>::finished,
            this,
            &RekordboxFeature::onBackgroundRekordboxDevicesFound);
    connect(&m_bgTracksFutureWatcher,
            &QFutureWatcher<QString>::finished,
            this,
            &RekordboxFeature::onBackgroundTracksFound);
    m_bgPollTimer.setInterval(5000);
    m_bgPollTimer.setSingleShot(false);
    connect(&m_bgPollTimer,
            &QTimer::timeout,
            this,
            &RekordboxFeature::onBackgroundPollTick);
    m_bgPollTimer.start();
    // The sidebar entry stays hidden until a device is found, so don't sit
    // on a full poll interval before surfacing a drive that is already
    // mounted at startup — kick the first enumeration immediately.
    QTimer::singleShot(0, this, &RekordboxFeature::onBackgroundPollTick);
}

RekordboxFeature::~RekordboxFeature() {
    // Stop the timer first so no new background work is queued, then drain
    // all four futures before dropping the tables — a still-running
    // parseDeviceDB() would otherwise write into tables about to be dropped.
    m_bgPollTimer.stop();
    m_devicesFuture.waitForFinished();
    m_tracksFuture.waitForFinished();
    m_bgDevicesFuture.waitForFinished();
    m_bgTracksFuture.waitForFinished();

    // Drop temporary Rekordbox database tables on shutdown
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    dropTable(database, kRekordboxPlaylistTracksTable);
    dropTable(database, kRekordboxPlaylistsTable);
    dropTable(database, kRekordboxLibraryTable);
    transaction.commit();
}

void RekordboxFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    parented_ptr<WLibraryTextBrowser> pEdit = make_parented<WLibraryTextBrowser>(pLibraryWidget);
    pEdit->setHtml(formatRootViewHtml());
    pEdit->setOpenLinks(false);
    connect(pEdit, &WLibraryTextBrowser::anchorClicked, this, &RekordboxFeature::htmlLinkClicked);
    pLibraryWidget->registerView("REKORDBOXHOME", pEdit);
}

void RekordboxFeature::htmlLinkClicked(const QUrl& link) {
    if (QString(link.path()) == "refresh") {
        activate();
    } else {
        qDebug() << "Unknown link clicked" << link;
    }
}

std::unique_ptr<BaseSqlTableModel>
RekordboxFeature::createPlaylistModelForPlaylist(const QVariant& data) {
    VERIFY_OR_DEBUG_ASSERT(data.canConvert<QVariantList>()) {
        return {};
    }
    QVariantList playlists = data.toList();
    VERIFY_OR_DEBUG_ASSERT(playlists.size() > 0) {
        return {};
    }
    auto pModel = std::make_unique<RekordboxPlaylistModel>(
            this, m_pLibrary->trackCollectionManager(), m_trackSource);
    pModel->setPlaylist(playlists.at(0).toString());
    return pModel;
}

QVariant RekordboxFeature::title() {
    return m_title;
}

bool RekordboxFeature::isSupported() {
    return true;
}

TreeItemModel* RekordboxFeature::sidebarModel() const {
    return m_pSidebarModel;
}

QString RekordboxFeature::formatRootViewHtml() const {
    QString title = tr("Rekordbox");
    QString summary = tr(
            "Reads databases exported for Pioneer CDJ / XDJ players using "
            "the Rekordbox Export mode.<br/>"
            "Rekordbox can only export to USB or SD devices with a FAT or "
            "HFS file system.<br/>"
            "Mixxx can read a database from any device that contains the "
            "database folders (<tt>PIONEER</tt> and <tt>Contents</tt>).<br/>"
            "Not supported are Rekordbox databases that have been moved to "
            "an external device via<br/>"
            "<i>Preferences > Advanced > Database management</i>.<br/>"
            "<br/>"
            "The following data is read:");

    QStringList items;

    items
            << tr("Folders")
            << tr("Playlists")
            << tr("Beatgrids")
            << tr("Hot cues")
            << tr("Memory cues")
            << tr("Loops (only the first loop is currently usable in Mixxx)");

    QString html;
    QString refreshLink = tr("Check for attached Rekordbox USB / SD devices (refresh)");
    html.append(QString("<h2>%1</h2>").arg(title));
    html.append(QString("<p>%1</p>").arg(summary));
    html.append(QString("<ul>"));
    for (const auto& item : std::as_const(items)) {
        html.append(QString("<li>%1</li>").arg(item));
    }
    html.append(QString("</ul>"));

    //Colorize links in lighter blue, instead of QT default dark blue.
    //Links are still different from regular text, but readable on dark/light backgrounds.
    //https://github.com/mixxxdj/mixxx/issues/9103
    html.append(QString("<a style=\"color:#0496FF;\" href=\"refresh\">%1</a>")
                        .arg(refreshLink));
    return html;
}

void RekordboxFeature::refreshLibraryModels() {
}

void RekordboxFeature::refreshScannedTracks(const QSet<TrackId>& trackIds) {
    if (trackIds.isEmpty() || !m_trackSource) {
        return;
    }
    // Re-reads those rows and tells the models, which repaint them. Batched:
    // clearing the overrides can touch every track on the drive.
    m_trackSource->slotTracksAddedOrChanged(trackIds);
}

void RekordboxFeature::onRatingOverrideStored(const QString& trackLocation, int rating) {
    if (trackLocation.isEmpty()) {
        return;
    }
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);
    // Nothing matches for a track that is not on a scanned device (every
    // rating stored anywhere comes through here, most of them for tracks no
    // rekordbox device holds) or for one already showing this rating. Either
    // way it costs one lookup on `location`, which is UNIQUE and so indexed.
    query.prepare(QStringLiteral("SELECT id FROM ") + kRekordboxLibraryTable +
            QStringLiteral(" WHERE location=:location AND rating IS NOT :rating"));
    query.bindValue(QStringLiteral(":location"), trackLocation);
    query.bindValue(QStringLiteral(":rating"), rating);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }
    QSet<TrackId> trackIds;
    while (query.next()) {
        trackIds.insert(TrackId(query.value(0)));
    }
    if (trackIds.isEmpty()) {
        return;
    }

    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE ") + kRekordboxLibraryTable +
            QStringLiteral(" SET rating=:rating WHERE location=:location"));
    update.bindValue(QStringLiteral(":rating"), rating);
    update.bindValue(QStringLiteral(":location"), trackLocation);
    if (!update.exec()) {
        LOG_FAILED_QUERY(update);
        return;
    }
    refreshScannedTracks(trackIds);
}

void RekordboxFeature::onMetaOverridesCleared() {
    QSqlDatabase database = m_pTrackCollection->database();
    QSqlQuery query(database);
    // `IS NOT` rather than `<>` so a row whose device exported no rating at all
    // (NULL) counts as differing from an override that put stars on it.
    query.prepare(QStringLiteral("SELECT id FROM ") + kRekordboxLibraryTable +
            QStringLiteral(" WHERE rating IS NOT source_rating"));
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return;
    }
    QSet<TrackId> trackIds;
    while (query.next()) {
        trackIds.insert(TrackId(query.value(0)));
    }
    if (trackIds.isEmpty()) {
        return;
    }

    QSqlQuery update(database);
    // Back to what the device's own database said, without re-parsing it: the
    // drives have just lost the stores those overrides came from.
    if (!update.exec(QStringLiteral("UPDATE ") + kRekordboxLibraryTable +
                QStringLiteral(" SET rating=source_rating WHERE rating IS NOT source_rating"))) {
        LOG_FAILED_QUERY(update);
        return;
    }
    refreshScannedTracks(trackIds);
}

void RekordboxFeature::activate() {
    qDebug() << "RekordboxFeature::activate()";

    // Let a worker thread do the XML parsing
    m_devicesFuture = QtConcurrent::run(findRekordboxDevices);
    m_devicesFutureWatcher.setFuture(m_devicesFuture);
    m_title = tr("(loading) Rekordbox");
    //calls a slot in the sidebar model such that 'Rekordbox (isLoading)' is displayed.
    emit featureIsLoading(this, true);

    emit enableCoverArtDisplay(true);
    emit switchToView("REKORDBOXHOME");
    emit disableSearch();
}

void RekordboxFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }

    //access underlying TreeItem object
    TreeItem* item = static_cast<TreeItem*>(index.internalPointer());
    if (!(item && item->getData().isValid())) {
        return;
    }

    // TreeItem list data holds 2 values in a QList and have different meanings.
    // If the 2nd QList element IS_RECORDBOX_DEVICE, the 1st element is the
    // filesystem device path, and the parseDeviceDB concurrent thread to parse
    // the Rekcordbox database is initiated. If the 2nd element is
    // IS_NOT_RECORDBOX_DEVICE, the 1st element is the playlist path and it is
    // activated.
    QList<QVariant> data = item->getData().toList();
    QString playlist = data[0].toString();
    bool doParseDeviceDB = data[1].toString() == IS_RECORDBOX_DEVICE;

    qDebug() << "RekordboxFeature::activateChild " << item->getLabel()
             << " playlist: " << playlist << " doParseDeviceDB: " << doParseDeviceDB;

    if (doParseDeviceDB) {
        qDebug() << "Parse Rekordbox Device DB: " << playlist;

        // Let a worker thread do the XML parsing
        m_tracksFuture = QtConcurrent::run(parseDeviceDB, static_cast<Library*>(parent())->dbConnectionPool(), item);
        m_tracksFutureWatcher.setFuture(m_tracksFuture);

        // This device is now a playlist element, future activations should treat is
        // as such
        data[1] = QVariant(IS_NOT_RECORDBOX_DEVICE);
        item->setData(QVariant(data));
    } else {
        qDebug() << "Activate Rekordbox Playlist: " << playlist;
        m_pRekordboxPlaylistModel->setPlaylist(playlist);
        m_pRekordboxPlaylistModel->setBackingLocation(devicePathOfPlaylist(playlist));
        emit showTrackModel(m_pRekordboxPlaylistModel);
    }
}

void RekordboxFeature::onRekordboxDevicesFound() {
    const QList<TreeItem*> result = m_devicesFuture.result();
    auto foundDevices = std::vector<std::unique_ptr<TreeItem>>(result.cbegin(), result.cend());

    mergeFoundDevicesIntoSidebar(std::move(foundDevices), /*allowTableTruncate=*/true);

    // calls a slot in the sidebarmodel such that 'isLoading' is removed from the feature title.
    m_title = tr("Rekordbox");
    emit featureLoadingFinished(this);
}

void RekordboxFeature::mergeFoundDevicesIntoSidebar(
        std::vector<std::unique_ptr<TreeItem>> foundDevices,
        bool allowTableTruncate) {
    clearLastRightClickedIndex();

    TreeItem* root = m_pSidebarModel->getRootItem();
    QSqlDatabase database = m_pTrackCollection->database();

    if (foundDevices.size() == 0) {
        if (allowTableTruncate) {
            // Foreground (user-initiated) scan: tear down immediately.
            ScopedTransaction transaction(database);

            dropTable(database, kRekordboxPlaylistTracksTable);
            dropTable(database, kRekordboxPlaylistsTable);
            dropTable(database, kRekordboxLibraryTable);

            // Create new temporary Rekordbox database tables
            createLibraryTable(database, kRekordboxLibraryTable);
            createPlaylistsTable(database, kRekordboxPlaylistsTable);
            createPlaylistTracksTable(database, kRekordboxPlaylistTracksTable);

            transaction.commit();
        } else if (++m_bgConsecutiveEmptyScans < kBgEmptyScansBeforeRemoval) {
            // Background poll: a single empty enumeration is often a transient
            // hiccup right after a (re)mount. Wait for several consecutive
            // empty scans before removing, so the device isn't needlessly
            // re-parsed when it reappears on the next tick.
            return;
        }

        // Nothing is mounted any more, so no staged device can still be
        // waiting for its playlists either.
        const QStringList staged = stagedDeviceLabels();
        for (const QString& label : staged) {
            dropStagedDevice(label);
        }

        if (root->childRows() > 0) {
            // Devices have since been unmounted. Clear their rows too —
            // otherwise a later rediscovery re-parses into a non-empty table
            // and collides on every row's UNIQUE constraint.
            for (int deviceIndex = 0; deviceIndex < root->childRows(); deviceIndex++) {
                clearDeviceTables(database, root->child(deviceIndex));
            }
            m_pSidebarModel->removeRows(0, root->childRows());
        }
        m_bgConsecutiveEmptyScans = 0;
        emit requestSidebarVisibility(this, false);
        return;
    }

    m_bgConsecutiveEmptyScans = 0;

    // Iterate backwards so removing a row doesn't shift an unvisited device
    // into the slot the loop has already passed.
    for (int deviceIndex = root->childRows() - 1; deviceIndex >= 0; deviceIndex--) {
        TreeItem* child = root->child(deviceIndex);
        bool removeChild = true;

        for (const auto& pDeviceFound : foundDevices) {
            if (pDeviceFound->getLabel() == child->getLabel()) {
                removeChild = false;
                break;
            }
        }

        if (removeChild) {
            // Device has since been unmounted, cleanup DB
            clearDeviceTables(database, child);

            m_pSidebarModel->removeRows(deviceIndex, 1);
        }
    }

    if (root->childRows() == 0) {
        // Every parsed device went away; the staged ones aren't shown yet, so
        // the feature has nothing left to display.
        emit requestSidebarVisibility(this, false);
    }

    // Forget devices that disappeared again before their parse finished.
    const QStringList staged = stagedDeviceLabels();
    for (const QString& label : staged) {
        bool stillMounted = false;
        for (const auto& pDeviceFound : foundDevices) {
            if (pDeviceFound->getLabel() == label) {
                stillMounted = true;
                break;
            }
        }
        if (!stillMounted) {
            dropStagedDevice(label);
        }
    }

    for (auto&& pDeviceFound : foundDevices) {
        const QString label = pDeviceFound->getLabel();
        if (findDeviceByLabel(label) || findStagedDevice(label)) {
            // Already shown, or already staged for parsing — don't add or
            // parse it again.
            continue;
        }
        // Bite DJ: a newly found device is staged here rather than inserted
        // into the sidebar. Its playlists only exist once parseDeviceDB() has
        // run, and a device row that can't be expanded is confusing, so the
        // row is added by promoteCompletedDrives() once the whole drive is
        // parsed.
        StagedDevice stagedDevice;
        stagedDevice.driveKey = driveKeyOfDevice(pDeviceFound.get());
        stagedDevice.pItem = std::move(pDeviceFound);
        m_stagedDevices.push_back(std::move(stagedDevice));
    }

    // A device dropped above may have been the last unparsed volume holding
    // its drive's siblings back.
    promoteCompletedDrives();
    pumpBackgroundParseQueue();
}

QString RekordboxFeature::driveKeyOfDevice(const TreeItem* pDevice) const {
    const QList<QVariant> data = pDevice->getData().toList();
    const QString devicePath = data.isEmpty() ? QString() : data[0].toString();
    // Volumes of one physical drive share a USB device node, which is what
    // holds them together until the last of them has been parsed. A volume
    // that doesn't resolve to one (a non-USB mount, or sysfs not telling us)
    // is its own group, keyed by its mount point so it can't collide.
    const QString usbDeviceNode = mixxx::usbDeviceNodeForMountPoint(devicePath);
    return usbDeviceNode.isEmpty() ? QDir::cleanPath(devicePath) : usbDeviceNode;
}

RekordboxFeature::StagedDevice* RekordboxFeature::findStagedDevice(const QString& label) {
    for (auto& stagedDevice : m_stagedDevices) {
        if (stagedDevice.pItem->getLabel() == label) {
            return &stagedDevice;
        }
    }
    return nullptr;
}

QStringList RekordboxFeature::stagedDeviceLabels() const {
    QStringList labels;
    labels.reserve(static_cast<int>(m_stagedDevices.size()));
    for (const auto& stagedDevice : m_stagedDevices) {
        labels.append(stagedDevice.pItem->getLabel());
    }
    return labels;
}

std::unique_ptr<TreeItem> RekordboxFeature::takeStagedDevice(const QString& label) {
    for (auto it = m_stagedDevices.begin(); it != m_stagedDevices.end(); ++it) {
        if (it->pItem->getLabel() == label) {
            std::unique_ptr<TreeItem> pDevice = std::move(it->pItem);
            m_stagedDevices.erase(it);
            return pDevice;
        }
    }
    return nullptr;
}

void RekordboxFeature::dropStagedDevice(const QString& label) {
    if (m_bgParseInFlight && m_bgParseLabel == label) {
        // A worker thread is writing into this item right now, so it has to
        // outlive the parse. onBackgroundTracksFound() discards it instead of
        // inserting it into the sidebar.
        m_bgParseAbandoned = true;
        return;
    }
    // A staged device that hasn't been parsed has never written a row, but one
    // that is only waiting for a sibling volume has.
    std::unique_ptr<TreeItem> pDevice = takeStagedDevice(label);
    if (pDevice) {
        QSqlDatabase database = m_pTrackCollection->database();
        clearDeviceTables(database, pDevice.get());
    }
}

void RekordboxFeature::promoteCompletedDrives() {
    // A drive with several volumes (e.g. a Rekordbox export partition plus a
    // second data partition) mounts as one sidebar row per volume. Showing the
    // first volume as soon as it is parsed would leave its siblings appearing
    // late, so hold every volume back until the whole drive is done.
    QSet<QString> incompleteDrives;
    for (const auto& stagedDevice : m_stagedDevices) {
        if (!stagedDevice.parsed) {
            incompleteDrives.insert(stagedDevice.driveKey);
        }
    }

    std::vector<std::unique_ptr<TreeItem>> childrenToAdd;
    for (auto it = m_stagedDevices.begin(); it != m_stagedDevices.end();) {
        if (incompleteDrives.contains(it->driveKey)) {
            ++it;
            continue;
        }
        childrenToAdd.push_back(std::move(it->pItem));
        it = m_stagedDevices.erase(it);
    }

    if (childrenToAdd.empty()) {
        return;
    }

    // Surface the feature's root row (no-op if already visible) BEFORE
    // inserting, so the row insertion has a live parent index in the sidebar
    // to attach to.
    emit requestSidebarVisibility(this, true);

    m_pSidebarModel->insertTreeItemRows(
            std::move(childrenToAdd), m_pSidebarModel->getRootItem()->childRows());
}

void RekordboxFeature::ejectDevice(const QString& mountPoint) {
    // Runs on the GUI thread (Library::mountEjected is emitted from
    // SystemSettings, same thread), so it is safe to mutate the sidebar model
    // and the temp DB tables directly here.
    TreeItem* root = m_pSidebarModel->getRootItem();
    if (!root) {
        return;
    }

    // SystemSettings reports the cleaned filesystem rootPath; findRekordboxDevices
    // stores the device's mount directory as data[0]. Normalise both before
    // comparing so a trailing slash or symlink-free spelling still matches.
    const QString wanted = QDir::cleanPath(mountPoint);

    // The device may still be staged — parsing, waiting for a sibling volume
    // on the same drive, or queued behind another parse — in which case it has
    // no sidebar row to remove yet.
    const QStringList staged = stagedDeviceLabels();
    for (const QString& label : staged) {
        const StagedDevice* pStagedDevice = findStagedDevice(label);
        const QList<QVariant> stagedData = pStagedDevice->pItem->getData().toList();
        if (!stagedData.isEmpty() &&
                QDir::cleanPath(stagedData[0].toString()) == wanted) {
            dropStagedDevice(label);
            // This may have been the last unparsed volume of its drive.
            promoteCompletedDrives();
            m_bgConsecutiveEmptyScans = 0;
            return;
        }
    }

    for (int deviceIndex = 0; deviceIndex < root->childRows(); ++deviceIndex) {
        TreeItem* child = root->child(deviceIndex);
        if (!child) {
            continue;
        }
        const QList<QVariant> data = child->getData().toList();
        if (data.isEmpty() ||
                QDir::cleanPath(data[0].toString()) != wanted) {
            continue;
        }

        // A right-clicked index cached against the old row layout would dangle
        // once we remove a row; clear it as mergeFoundDevicesIntoSidebar() does.
        clearLastRightClickedIndex();

        QSqlDatabase database = m_pTrackCollection->database();
        clearDeviceTables(database, child);
        m_pSidebarModel->removeRows(deviceIndex, 1);

        if (root->childRows() == 0) {
            // That was the last Rekordbox device; retire the sidebar entry.
            emit requestSidebarVisibility(this, false);
        }

        // This may have been the last device; reset the poll's empty-scan guard
        // so a stale count doesn't linger into the next enumeration.
        m_bgConsecutiveEmptyScans = 0;
        return;
    }
}

void RekordboxFeature::onTracksFound() {
    qDebug() << "onTracksFound";
    m_pSidebarModel->triggerRepaint();

    QString devicePlaylist;
    try {
        devicePlaylist = m_tracksFuture.result();
    } catch (const std::exception& e) {
        qWarning() << "Failed to load Rekordbox database:" << e.what();
        pumpBackgroundParseQueue();
        return;
    }

    qDebug() << "Show Rekordbox Device Playlist: " << devicePlaylist;

    m_pRekordboxPlaylistModel->setPlaylist(devicePlaylist);
    m_pRekordboxPlaylistModel->setBackingLocation(devicePathOfPlaylist(devicePlaylist));
    emit showTrackModel(m_pRekordboxPlaylistModel);

    // A background queue that yielded to this foreground parse may have
    // stalled — kick it forward now that the foreground slot is free.
    pumpBackgroundParseQueue();
}

void RekordboxFeature::onBackgroundPollTick() {
    // Yield to any foreground or already-running background scan.
    if (m_devicesFutureWatcher.isRunning()) {
        return;
    }
    if (m_bgDevicesFutureWatcher.isRunning()) {
        return;
    }
    m_bgDevicesFuture = QtConcurrent::run(findRekordboxDevices);
    m_bgDevicesFutureWatcher.setFuture(m_bgDevicesFuture);
}

void RekordboxFeature::onBackgroundRekordboxDevicesFound() {
    const QList<TreeItem*> result = m_bgDevicesFuture.result();
    auto foundDevices = std::vector<std::unique_ptr<TreeItem>>(
            result.cbegin(), result.cend());

    // allowTableTruncate=false so a transient empty scan during an
    // in-flight parse never wipes tables out from under that parse.
    mergeFoundDevicesIntoSidebar(std::move(foundDevices), /*allowTableTruncate=*/false);
}

void RekordboxFeature::onBackgroundTracksFound() {
    try {
        (void)m_bgTracksFuture.result();
    } catch (const std::exception& e) {
        // Show the device anyway, with whatever playlists the parse got
        // through: discarding it here would only have the next poll tick
        // rediscover it and fail again, forever.
        qWarning() << "Background Rekordbox parse failed:" << e.what();
    }
    m_bgParseInFlight = false;

    const QString label = m_bgParseLabel;
    m_bgParseLabel.clear();

    if (m_bgParseAbandoned) {
        // The drive went away mid-parse; drop whatever the parse managed to
        // write instead of showing a row for a device that is gone.
        m_bgParseAbandoned = false;
        std::unique_ptr<TreeItem> pDevice = takeStagedDevice(label);
        if (pDevice) {
            QSqlDatabase database = m_pTrackCollection->database();
            clearDeviceTables(database, pDevice.get());
        }
    } else if (StagedDevice* pStagedDevice = findStagedDevice(label)) {
        pStagedDevice->parsed = true;
    }

    // The device enters the sidebar only now, with its playlists attached —
    // and only once every other volume of the same drive is parsed too.
    promoteCompletedDrives();
    m_pSidebarModel->triggerRepaint();

    pumpBackgroundParseQueue();
}

void RekordboxFeature::pumpBackgroundParseQueue() {
    if (m_bgParseInFlight) {
        return;
    }
    if (m_tracksFutureWatcher.isRunning()) {
        // Yield to a user-driven parse so we don't double-up SQL writers
        // for the same device.
        return;
    }
    bool skippedUnparseable = false;
    for (auto& stagedDevice : m_stagedDevices) {
        if (stagedDevice.parsed) {
            continue;
        }
        TreeItem* item = stagedDevice.pItem.get();
        const QString label = item->getLabel();
        QList<QVariant> data = item->getData().toList();
        if (data.size() < 2 || data[1].toString() != IS_RECORDBOX_DEVICE) {
            // Not a parseable device row; it would never gain playlists, so
            // count it as done rather than blocking its drive forever.
            stagedDevice.parsed = true;
            skippedUnparseable = true;
            continue;
        }
        // Flip the flag BEFORE kick-off to mirror activateChild() — by the
        // time the device reaches the sidebar it is a plain playlist row
        // pointing at the device's "all tracks" playlist.
        data[1] = QVariant(IS_NOT_RECORDBOX_DEVICE);
        item->setData(QVariant(data));

        m_bgTracksFuture = QtConcurrent::run(parseDeviceDB,
                static_cast<Library*>(parent())->dbConnectionPool(),
                item);
        m_bgTracksFutureWatcher.setFuture(m_bgTracksFuture);
        m_bgParseInFlight = true;
        m_bgParseLabel = label;
        return;
    }

    if (skippedUnparseable) {
        // Nothing left to parse, and a drive may have been waiting on one of
        // the rows just written off.
        promoteCompletedDrives();
    }
}

TreeItem* RekordboxFeature::findDeviceByLabel(const QString& label) const {
    TreeItem* root = m_pSidebarModel->getRootItem();
    if (!root) {
        return nullptr;
    }
    for (int i = 0; i < root->childRows(); ++i) {
        TreeItem* child = root->child(i);
        if (child && child->getLabel() == label) {
            return child;
        }
    }
    return nullptr;
}
