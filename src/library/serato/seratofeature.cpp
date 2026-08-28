#include "library/serato/seratofeature.h"

#include <QBuffer>
#include <QDir>
#include <QMap>
#include <QSet>
#include <QStandardPaths>
#include <QTextCodec>
#include <QtConcurrentRun>
#include <QtDebug>
#include <QtEndian>
#include <utility>
#include <vector>

#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/queryutil.h"
#include "library/serato/seratoplaylistmodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_seratofeature.cpp"
#include "util/assert.h"
#include "util/db/dbconnectionpooled.h"
#include "util/db/dbconnectionpooler.h"
#include "util/usbdevice.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

namespace {

// Serato Database Field IDs
// The "magic" value is the short 4 byte ascii code interpreted as quint32, so
// that we can use the value in a switch statement instead of going through
// a strcmp if/else ladder.
enum class FieldId : quint32 {
    Version = 0x7672736e,        // vrsn
    Track = 0x6f74726b,          // otrk
    FileType = 0x74747970,       // ttyp
    FilePath = 0x7066696c,       // pfil
    SongTitle = 0x74736e67,      // tsng
    Artist = 0x74617274,         // tart
    Album = 0x74616c62,          // talb
    Genre = 0x7467656e,          // tgen
    Comment = 0x74636f6d,        // tcom
    Grouping = 0x74677270,       // tgrp
    Label = 0x746c626c,          // tlbl
    Year = 0x74747972,           // ttyr
    Length = 0x746c656e,         // tlen
    Bitrate = 0x74626974,        // tbit
    SampleRate = 0x74736d70,     // tsmp
    Bpm = 0x7462706d,            // tbpm
    DateAddedText = 0x74616464,  // tadd
    DateAdded = 0x75616464,      // uadd
    Key = 0x746b6579,            // tkey
    BeatgridLocked = 0x6262676c, // bbgl
    FileTime = 0x75746d65,       // utme
    Missing = 0x626d6973,        // bmis
    Sorting = 0x7472736f,        // osrt
    ReverseOrder = 0x62726576,   // brev
    ColumnTitle = 0x6f766374,    // ovct
    ColumnName = 0x7476636e,     // tvcn
    ColumnWidth = 0x74766377,    // tvcw
    TrackPath = 0x7074726b,      // ptrk
};

struct serato_track_t {
    QString filetype;
    QString location;
    QString title;
    QString artist;
    QString album;
    QString genre;
    QString comment;
    QString grouping;
    QString label;
    int year = -1;
    int duration = 0;
    QString bitrate;
    QString samplerate;
    double bpm = -1.0;
    QString key;
    bool beatgridlocked = false;
    bool missing = false;
    quint32 filetime = 0;
    quint32 datetimeadded = 0;
};

const QString kDatabaseDirectory = QStringLiteral("_Serato_");
const QString kDatabaseFilename = QStringLiteral("database V2");
const QString kCrateDirectory = QStringLiteral("Subcrates");
const QString kCrateFilter = QStringLiteral("*.crate");
const QString kSmartCrateDirectory = QStringLiteral("Smart Crates");
const QString kSmartCrateFilter = QStringLiteral("*.scrate");

const QString kSeratoLibraryTable = QStringLiteral("serato_library");
const QString kSeratoPlaylistsTable = QStringLiteral("serato_playlists");
const QString kSeratoPlaylistTracksTable = QStringLiteral("serato_playlist_tracks");

constexpr int kHeaderSize = 2 * sizeof(quint32);

// Consecutive empty background enumerations required before a database entry
// is removed from the sidebar.
constexpr int kBgEmptyScansBeforeRemoval = 3;

// Remove all rows previously imported from this database (keyed by the
// _Serato_ directory path bound as serato_db on every row) so a re-parse —
// device re-plugged, or a different drive mounted under a reused name —
// starts clean instead of stacking duplicate INSERTs.
void clearDatabaseRows(const QSqlDatabase& database, const QString& databasePath) {
    QSqlQuery deletePlaylistTracksQuery(database);
    deletePlaylistTracksQuery.prepare(
            "DELETE FROM " + kSeratoPlaylistTracksTable +
            " WHERE playlist_id IN (SELECT id FROM " + kSeratoPlaylistsTable +
            " WHERE serato_db=:serato_db)");
    deletePlaylistTracksQuery.bindValue(":serato_db", databasePath);
    if (!deletePlaylistTracksQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistTracksQuery)
                << "databasePath:" << databasePath;
    }

    QSqlQuery deletePlaylistsQuery(database);
    deletePlaylistsQuery.prepare(
            "DELETE FROM " + kSeratoPlaylistsTable + " WHERE serato_db=:serato_db");
    deletePlaylistsQuery.bindValue(":serato_db", databasePath);
    if (!deletePlaylistsQuery.exec()) {
        LOG_FAILED_QUERY(deletePlaylistsQuery)
                << "databasePath:" << databasePath;
    }

    QSqlQuery deleteTracksQuery(database);
    deleteTracksQuery.prepare(
            "DELETE FROM " + kSeratoLibraryTable + " WHERE serato_db=:serato_db");
    deleteTracksQuery.bindValue(":serato_db", databasePath);
    if (!deleteTracksQuery.exec()) {
        LOG_FAILED_QUERY(deleteTracksQuery)
                << "databasePath:" << databasePath;
    }
}

int createPlaylist(const QSqlDatabase& database, const QString& name, const QString& databasePath) {
    QSqlQuery query(database);
    query.prepare(
            "INSERT INTO serato_playlists (name, serato_db)"
            "VALUES (:name, :serato_db)");
    query.bindValue(":name", name);
    query.bindValue(":serato_db", databasePath);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query) << "databasePath: " << databasePath;
        return -1;
    }

    return query.lastInsertId().toInt();
}

int insertTrackIntoPlaylist(const QSqlDatabase& database, int playlistId, int trackId, int position) {
    QSqlQuery query(database);
    query.prepare(
            "INSERT INTO serato_playlist_tracks (playlist_id, track_id, position) "
            "VALUES (:playlist_id, :track_id, :position)");
    query.bindValue(":playlist_id", playlistId);
    query.bindValue(":track_id", trackId);
    query.bindValue(":position", position);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return -1;
    }

    return query.lastInsertId().toInt();
}

inline QString utf16beToQString(const QByteArray& data, const quint32 size) {
    return QTextCodec::codecForName("UTF-16BE")->toUnicode(data, size);
}

inline bool bytesToBoolean(const QByteArray& data) {
    VERIFY_OR_DEBUG_ASSERT(!data.isEmpty()) {
        return false;
    }
    return data.at(0) != 0;
}

inline quint32 bytesToUInt32(const QByteArray& data) {
    VERIFY_OR_DEBUG_ASSERT(data.size() >= static_cast<int>(sizeof(quint32))) {
        return 0;
    }
    return qFromBigEndian<quint32>(data.constData());
}

inline bool parseTrack(serato_track_t* track, QIODevice* buffer) {
    QByteArray headerData = buffer->read(kHeaderSize);
    while (headerData.length() == kHeaderSize) {
        quint32 fieldId = bytesToUInt32(headerData.mid(0, sizeof(quint32)));
        quint32 fieldSize = bytesToUInt32(headerData.mid(sizeof(quint32), kHeaderSize));

        // Read field data
        QByteArray data = buffer->read(fieldSize);
        if (static_cast<quint32>(data.length()) != fieldSize) {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qWarning() << "Failed to read "
                       << fieldSize
                       << " bytes for "
                       << fieldName
                       << " field.";
            return false;
        }

        // Parse field data
        switch (static_cast<FieldId>(fieldId)) {
        case FieldId::FileType:
            track->filetype = utf16beToQString(data, fieldSize);
            break;
        case FieldId::FilePath:
            track->location = utf16beToQString(data, fieldSize);
            break;
        case FieldId::SongTitle:
            track->title = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Artist:
            track->artist = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Album:
            track->album = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Genre:
            track->genre = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Length: {
            bool ok;
            int duration = utf16beToQString(data, fieldSize).toInt(&ok);
            if (ok) {
                track->duration = duration;
            }
            break;
        }
        case FieldId::Bitrate:
            track->bitrate = utf16beToQString(data, fieldSize);
            break;
        case FieldId::SampleRate:
            track->samplerate = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Bpm: {
            bool ok;
            double bpm = utf16beToQString(data, fieldSize).toDouble(&ok);
            if (ok) {
                track->bpm = bpm;
            }
            break;
        }
        case FieldId::Comment:
            track->comment = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Grouping:
            track->grouping = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Label:
            track->label = utf16beToQString(data, fieldSize);
            break;
        case FieldId::Year: {
            // 4-digit year as string (YYYY)
            bool ok;
            int year = utf16beToQString(data, fieldSize).toInt(&ok);
            if (ok) {
                track->year = year;
            }
            break;
        }
        case FieldId::Key:
            track->key = utf16beToQString(data, fieldSize);
            break;
        case FieldId::BeatgridLocked:
            track->beatgridlocked = bytesToBoolean(data);
            break;
        case FieldId::Missing:
            if (fieldSize == 1) {
                track->missing = bytesToBoolean(data);
            }
            break;
        case FieldId::FileTime:
            // POSIX timestamp
            if (fieldSize == sizeof(quint32)) {
                track->filetime = bytesToUInt32(data);
            }
            break;
        case FieldId::DateAdded:
            // POSIX timestamp
            if (fieldSize == sizeof(quint32)) {
                track->datetimeadded = bytesToUInt32(data);
            }
            break;
        case FieldId::DateAddedText:
            // Ignore this field, but do not print a debug message. It's the
            // same as the regular DateAdded field, but this time the timestamp
            // is a string instead of an unsigned integer. Since we already
            // parse the integer version, it doesn't make sense to parse this.
            break;
        default: {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qDebug() << "Ignoring unknown field "
                     << fieldName
                     << " ("
                     << fieldSize
                     << " bytes).";
        }
        }

        headerData = buffer->read(kHeaderSize);
    }

    if (headerData.length() != 0) {
        qWarning() << "Found "
                   << headerData.length()
                   << " extra bytes at end of track definition.";
        return false;
    }

    // Ignore tracks with empty location fields. The track location is used as
    // identifier by Serato (e.g. it's also used to reference them in Crates).
    if (track->location.isEmpty()) {
        qWarning() << "Found track with empty location field.";
        return false;
    }

    return true;
}

inline QString parseCrateTrackPath(QIODevice* buffer) {
    QString location;
    QByteArray headerData = buffer->read(kHeaderSize);
    while (headerData.length() == kHeaderSize) {
        quint32 fieldId = bytesToUInt32(headerData.mid(0, sizeof(quint32)));
        quint32 fieldSize = bytesToUInt32(headerData.mid(sizeof(quint32), kHeaderSize));

        // Read field data
        QByteArray data = buffer->read(fieldSize);
        if (static_cast<quint32>(data.length()) != fieldSize) {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qWarning() << "Failed to read "
                       << fieldSize
                       << " bytes for "
                       << fieldName
                       << " field.";
            return QString();
        }

        // Parse field data
        switch (static_cast<FieldId>(fieldId)) {
        case FieldId::TrackPath:
            location = utf16beToQString(data, fieldSize);
            break;
        default: {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qDebug() << "Ignoring unknown field "
                     << fieldName
                     << " ("
                     << fieldSize
                     << " bytes).";
        }
        }

        headerData = buffer->read(kHeaderSize);
    }

    if (headerData.length() != 0) {
        qWarning() << "Found "
                   << headerData.length()
                   << " extra bytes at end of track definition.";
        return QString();
    }

    return location;
}

// Bite DJ: a crate read off the drive, held until the write phase. See
// parseDatabase() for why reading and writing are kept apart.
struct serato_crate_t {
    QString name;
    QString filePath;
    // In file order, empty paths dropped. These are the paths as the crate
    // spells them, which is the key trackIdMap is built with.
    QStringList trackLocations;
};

// Reads one .crate file. Touches the drive but not the database. Returns false
// if the crate could not be read, in which case nothing about it is written --
// unlike the old read-and-insert-at-once version, which left behind a playlist
// row holding however many tracks it got through before the read failed, with
// no sidebar entry pointing at it.
bool readCrateFile(const QString& crateFilePath, serato_crate_t* pCrate) {
    pCrate->name = QFileInfo(crateFilePath).baseName();
    pCrate->filePath = crateFilePath;
    qDebug() << "Parsing crate"
             << pCrate->name
             << "at" << crateFilePath;

    mixxx::FileInfo fileInfo(crateFilePath);
    QFile crateFile(crateFilePath);
    if (!Sandbox::askForAccess(&fileInfo) || !crateFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open file "
                   << crateFilePath
                   << " for reading:"
                   << crateFile.errorString();
        return false;
    }

    QByteArray headerData = crateFile.read(kHeaderSize);
    while (headerData.length() == kHeaderSize) {
        quint32 fieldId = bytesToUInt32(headerData.mid(0, sizeof(quint32)));
        quint32 fieldSize = bytesToUInt32(headerData.mid(sizeof(quint32), kHeaderSize));

        // Read field data
        QByteArray data = crateFile.read(fieldSize);
        if (static_cast<quint32>(data.length()) != fieldSize) {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qWarning() << "Failed to read "
                       << fieldSize
                       << " bytes for "
                       << fieldName
                       << " field from "
                       << crateFilePath
                       << ".";
            return false;
        }

        // Parse field data
        switch (static_cast<FieldId>(fieldId)) {
        case FieldId::Version: {
            QString version = utf16beToQString(data, fieldSize);
            qDebug() << "Serato Database Version: "
                     << version;
            break;
        }
        case FieldId::Track: {
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            QString location = parseCrateTrackPath(&buffer);
            if (!location.isEmpty()) {
                pCrate->trackLocations.append(location);
            }
            break;
        }
        default: {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qDebug() << "Ignoring unknown field "
                     << fieldName
                     << " ("
                     << fieldSize
                     << " bytes) in database "
                     << crateFilePath
                     << ".";
        }
        }

        headerData = crateFile.read(kHeaderSize);
    }

    if (headerData.length() != 0) {
        qWarning() << "Found "
                   << headerData.length()
                   << " extra bytes at end of Serato database file "
                   << crateFilePath
                   << ".";
    }

    return true;
}

// Writes an already-read crate. Database only -- no drive access.
QString insertCrate(
        const QSqlDatabase& database,
        const QString& databasePath,
        const serato_crate_t& crate,
        const QMap<QString, int>& trackIdMap) {
    int playlistId = createPlaylist(database, crate.filePath, databasePath);
    if (playlistId < 0) {
        qWarning() << "Failed to create library playlist for "
                   << crate.filePath;
        return QString();
    }

    int trackCount = 0;
    for (const QString& location : crate.trackLocations) {
        int trackId = trackIdMap.value(location, -1);
        insertTrackIntoPlaylist(database, playlistId, trackId, trackCount);
        trackCount++;
    }

    return crate.name;
}

QString parseDatabase(mixxx::DbConnectionPoolPtr dbConnectionPool, TreeItem* databaseItem) {
    QString databaseName = databaseItem->getLabel();
    QString databaseFilePath = databaseItem->getData().toList()[0].toString();
    QDir databaseDir = QFileInfo(databaseFilePath).dir();

    QDir databaseRootDir = QDir(databaseDir);
    databaseRootDir.cdUp();

#if defined(__WINDOWS__)
    // On Windows, all paths are relative to drive root of the database (e.g.
    // "C:\"). Qt doesn't seem to provide a way to find it for a specific path,
    // so we just call cdUp() until it stops working.
    while (databaseRootDir.cdUp()) {
        // Nothing to do here
    }
#else
    // If the file is on an external drive, the database path are relative to
    // its mountpoint, i.e. the parent directory of the _Serato_
    // directory. This means we can just use the path as-is.
    //
    // If the file is not on an external drive, the paths are all relative to
    // the file system's root directory ("/").
    //
    // Serato does not exist on Linux, if it did, it would probably just mirror
    // the way paths are handled on OSX.
    if (databaseRootDir.canonicalPath().startsWith(QDir::homePath())) {
        databaseRootDir.setPath(QDir::rootPath());
    }
#endif

    qDebug() << "Parsing Serato database"
             << databaseName
             << "at" << databaseFilePath;

    if (!QFile(databaseFilePath).exists()) {
        qWarning() << "Serato database file not found: "
                   << databaseFilePath;
        return databaseFilePath;
    }

    // The pooler limits the lifetime all thread-local connections,
    // that should be closed immediately before exiting this function.
    const mixxx::DbConnectionPooler dbConnectionPooler(dbConnectionPool);
    QSqlDatabase database = mixxx::DbConnectionPooled(dbConnectionPool);

    //Open the database connection in this thread.
    VERIFY_OR_DEBUG_ASSERT(database.isOpen()) {
        qWarning() << "Failed to open database for Serato parser."
                   << database.lastError();
        return QString();
    }

    //Give thread a low priority
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    // Bite DJ: the drive is read first and the database written second,
    // deliberately.
    //
    // "database V2" and every .crate beside it live on a USB stick, and reading
    // them takes seconds on this hardware. This used to run with one
    // transaction wrapped around the whole thing, which pinned the library
    // database's single writer slot for the entire duration of that read --
    // and every other connection wanting to write (an eject clearing this
    // device's rows, a rating being stored, the sidebar merging a newly found
    // device) then blocked on the GUI thread until the stick was done. So
    // nothing below touches the database until the drive has been read; see
    // the write phase further down.
    mixxx::FileInfo fileInfo(databaseFilePath);
    QFile databaseFile(databaseFilePath);
    if (!Sandbox::askForAccess(&fileInfo) || !databaseFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open file "
                   << databaseFilePath
                   << " for reading.";
        return QString();
    }

    std::vector<serato_track_t> tracks;
    QByteArray headerData = databaseFile.read(kHeaderSize);
    while (headerData.length() == kHeaderSize) {
        quint32 fieldId = bytesToUInt32(headerData.mid(0, sizeof(quint32)));
        quint32 fieldSize = bytesToUInt32(headerData.mid(sizeof(quint32), kHeaderSize));

        // Read field data
        QByteArray data = databaseFile.read(fieldSize);
        if (static_cast<quint32>(data.length()) != fieldSize) {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qWarning() << "Failed to read "
                       << fieldSize
                       << " bytes for "
                       << fieldName
                       << " field from "
                       << databaseFilePath
                       << ".";
            return QString();
        }

        // Parse field data
        switch (static_cast<FieldId>(fieldId)) {
        case FieldId::Version: {
            QString version = utf16beToQString(data, fieldSize);
            qDebug() << "Serato Database Version: "
                     << version;
            break;
        }
        case FieldId::Track: {
            serato_track_t track;
            QBuffer buffer(&data);
            buffer.open(QIODevice::ReadOnly);
            if (parseTrack(&track, &buffer)) {
                tracks.push_back(std::move(track));
            }
            break;
        }
        default: {
            QString fieldName = QString(headerData.mid(0, sizeof(quint32)));
            qDebug() << "Ignoring unknown field "
                     << fieldName
                     << " ("
                     << fieldSize
                     << " bytes) in database "
                     << databaseFilePath
                     << ".";
        }
        }

        headerData = databaseFile.read(kHeaderSize);
    }

    if (headerData.length() != 0) {
        qWarning() << "Found "
                   << headerData.length()
                   << " extra bytes at end of Serato database file "
                   << databaseFilePath
                   << ".";
    }

    // Read the crates, still without touching the database.
    std::vector<serato_crate_t> crates;
    QDir crateDir = QDir(databaseDir);
    if (crateDir.cd(kCrateDirectory)) {
        QStringList filters;
        filters << kCrateFilter;
        const auto entryList = crateDir.entryList(filters);
        for (const QString& entry : entryList) {
            serato_crate_t crate;
            if (readCrateFile(crateDir.filePath(entry), &crate)) {
                crates.push_back(std::move(crate));
            }
        }
    } else {
        qWarning() << "Failed to open crate directory: "
                   << databaseDir.filePath(kCrateDirectory);
    }

    // TODO: Parse Smart Crates

    // The drive has been read; everything below is local database work only, so
    // the write lock is held for as short a time as the inserts take.
    ScopedTransaction transaction(database);

    clearDatabaseRows(database, databaseDir.path());

    QSqlQuery query(database);
    query.prepare(
            "INSERT INTO " +
            kSeratoLibraryTable + " (" +
            LIBRARYTABLE_TITLE + ", " +
            LIBRARYTABLE_ARTIST + ", " +
            LIBRARYTABLE_ALBUM + ", " +
            LIBRARYTABLE_GENRE + ", " +
            LIBRARYTABLE_COMMENT + ", " +
            LIBRARYTABLE_GROUPING + ", " +
            LIBRARYTABLE_YEAR + ", " +
            LIBRARYTABLE_DURATION + ", " +
            LIBRARYTABLE_BITRATE + ", " +
            LIBRARYTABLE_SAMPLERATE + ", " +
            LIBRARYTABLE_BPM + ", " +
            LIBRARYTABLE_KEY + ", " +
            TRACKLOCATIONSTABLE_LOCATION + ", " +
            LIBRARYTABLE_BPM_LOCK + ", " +
            LIBRARYTABLE_DATETIMEADDED +
            ", "
            "label, "
            "serato_db"
            ") VALUES ("
            ":title, "
            ":artist, "
            ":album, "
            ":genre, "
            ":comment, "
            ":grouping, "
            ":year, "
            ":duration, "
            ":bitrate, "
            ":samplerate, "
            ":bpm, "
            ":key, "
            ":location, "
            ":bpm_lock, "
            ":datetime_added, "
            ":label, "
            ":serato_db"
            ")");

    int playlistId = createPlaylist(database, databaseFilePath, databaseDir.path());
    if (playlistId < 0) {
        qWarning() << "Failed to create library playlist for "
                   << databaseFilePath;
        return QString();
    }

    int trackCount = 0;
    QMap<QString, int> trackIdMap;
    for (const serato_track_t& track : tracks) {
        QString location = databaseRootDir.absoluteFilePath(track.location);
        query.bindValue(":title", track.title);
        query.bindValue(":artist", track.artist);
        query.bindValue(":album", track.album);
        query.bindValue(":genre", track.genre);
        query.bindValue(":comment", track.comment);
        query.bindValue(":grouping", track.grouping);
        query.bindValue(":year", track.year);
        query.bindValue(":duration", track.duration);
        query.bindValue(":bitrate", track.bitrate);
        query.bindValue(":samplerate", track.samplerate);
        query.bindValue(":bpm", track.bpm);
        query.bindValue(":key", track.key);
        query.bindValue(":location", location);
        query.bindValue(":bpm_lock", track.beatgridlocked);
        query.bindValue(":datetime_added", track.datetimeadded);
        query.bindValue(":label", track.label);
        query.bindValue(":serato_db", databaseDir.path());

        if (!query.exec()) {
            LOG_FAILED_QUERY(query);
        } else {
            int trackId = query.lastInsertId().toInt();
            insertTrackIntoPlaylist(database, playlistId, trackId, trackCount);
            // Keyed by the path as the database spells it, which is what the
            // crates reference -- not the absolute path bound above.
            trackIdMap.insert(track.location, trackId);
            trackCount++;
        }
    }

    for (const serato_crate_t& crate : crates) {
        QString crateName = insertCrate(
                database,
                databaseDir.path(),
                crate,
                trackIdMap);
        if (!crateName.isEmpty()) {
            TreeItem* crateItem = databaseItem->appendChild(crateName,
                    QList<QVariant>{
                            QVariant(crate.filePath), QVariant(true)});
            crateItem->setIcon(QIcon(":/images/library/ic_library_crates.svg"));
        }
    }

    transaction.commit();

    return databaseFilePath;
}

// This function is executed in a separate thread other than the main thread
// The returned list owns the pointers, but we can't use a unique_ptr because
// the result is passed by a const reference inside QFuture and than copied
// to the main thread requiring a copy-able object.
QList<TreeItem*> findSeratoDatabases() {
    QThread* thisThread = QThread::currentThread();
    thisThread->setPriority(QThread::LowPriority);

    // Build a list of directories that could contain the _Serato_ directory
    QFileInfoList databaseLocations;
    foreach (const QString& musicDir, QStandardPaths::standardLocations(QStandardPaths::MusicLocation)) {
        databaseLocations.append(QFileInfo(musicDir));
    }
#if defined(__WINDOWS__)
    // Repopulate drive list
    // Using drive.filePath() instead of drive.canonicalPath() as it
    // freezes interface too much if there is a network share mounted
    // (drive letter assigned) but unavailable
    //
    // drive.canonicalPath() make a system call to the underlying filesystem
    // introducing delay if it is unreadable.
    // drive.filePath() doesn't make any access to the filesystem and consequently
    // shorten the delay
    databaseLocations.append(QDir::drives());
#elif defined(__LINUX__)
    // To get devices on Linux, we look for directories under /media and
    // /run/media/$USER.
    const QString userName = QString::fromLocal8Bit(qgetenv("USER"));

    // Add folders under /media to devices.
    QDir mediaDir = QDir(QStringLiteral("/media/"));
    databaseLocations.append(
            mediaDir.entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot));

    // Add folders under /media/$USER to devices. When USER is unset,
    // cd("") is a no-op that "succeeds" in place, which would re-list
    // /media itself and duplicate every device — skip the per-user roots
    // in that case (same guard as browsefeature/rekordboxfeature).
    if (!userName.isEmpty() && mediaDir.cd(userName)) {
        databaseLocations.append(
                mediaDir.entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot));
    }

    // Add folders under /run/media/$USER to devices.
    QDir runMediaDir = QDir(QStringLiteral("/run/media/"));
    if (runMediaDir.cd(userName)) {
        databaseLocations.append(
                runMediaDir.entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot));
    }
#elif defined(__APPLE__)
    QDir volumesDir = QDir(QStringLiteral("/Volumes"));
    databaseLocations.append(
            volumesDir.entryInfoList(QDir::AllDirs | QDir::NoDotAndDotDot));
#endif

    QList<TreeItem*> foundDatabases;
    for (const QFileInfo& databaseLocation : std::as_const(databaseLocations)) {
        QDir databaseDir = QDir(databaseLocation.filePath());
        if (!databaseDir.cd(kDatabaseDirectory)) {
            continue;
        }

        if (!databaseDir.exists(kDatabaseFilename)) {
            continue;
        }

        QString displayPath = databaseLocation.filePath();
        if (displayPath.endsWith("/")) {
            displayPath.chop(1);
        }

        TreeItem* foundDatabase = new TreeItem(std::move(displayPath),
                QVariant(QList<QVariant>{
                        QVariant(databaseDir.filePath(kDatabaseFilename)),
                        QVariant(false)}));

        foundDatabases << foundDatabase;
    }

    return foundDatabases;
}

bool createLibraryTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Serato library table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    title TEXT,"
            "    artist TEXT,"
            "    album TEXT,"
            "    genre TEXT,"
            "    comment TEXT,"
            "    grouping TEXT,"
            "    year INTEGER,"
            "    duration INTEGER,"
            "    bitrate TEXT,"
            "    samplerate TEXT,"
            "    bpm FLOAT,"
            "    key TEXT,"
            "    location TEXT,"
            "    bpm_lock INTEGER,"
            "    datetime_added DEFAULT CURRENT_TIMESTAMP,"
            "    label TEXT,"
            "    composer TEXT,"
            "    filename TEXT,"
            "    filetype TEXT,"
            "    remixer TEXT,"
            "    size INTEGER,"
            "    tracknumber TEXT,"
            "    serato_db TEXT"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistsTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Serato playlists table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY,"
            "    name TEXT,"
            "    serato_db TEXT"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool createPlaylistTracksTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Creating Serato playlist tracks table: " << tableName;

    QSqlQuery query(database);
    query.prepare(
            "CREATE TABLE IF NOT EXISTS " + tableName +
            " ("
            "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "    playlist_id INTEGER REFERENCES serato_playlists(id),"
            "    track_id INTEGER REFERENCES serato_library(id),"
            "    position INTEGER"
            ");");

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

bool dropTable(QSqlDatabase& database, const QString& tableName) {
    qDebug() << "Dropping Serato table: " << tableName;

    QSqlQuery query(database);
    query.prepare("DROP TABLE IF EXISTS " + tableName);

    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
        return false;
    }

    return true;
}

} // anonymous namespace

SeratoFeature::SeratoFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig)
        : BaseExternalLibraryFeature(pLibrary, pConfig, QStringLiteral("serato")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)) {
    QString idColumn = LIBRARYTABLE_ID;
    QStringList columns = {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_GROUPING,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_SAMPLERATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_KEY,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_BPM_LOCK};
    QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_GENRE,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_GROUPING};

    m_trackSource = QSharedPointer<BaseTrackCache>::create(
            m_pTrackCollection,
            kSeratoLibraryTable,
            std::move(idColumn),
            std::move(columns),
            std::move(searchColumns),
            false);
    m_pSeratoPlaylistModel = new SeratoPlaylistModel(
            this, pLibrary->trackCollectionManager(), m_trackSource);

    m_title = tr("Serato");

    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    // Drop any leftover temporary Serato database tables if they exist
    dropTable(database, kSeratoPlaylistTracksTable);
    dropTable(database, kSeratoPlaylistsTable);
    dropTable(database, kSeratoLibraryTable);

    // Create new temporary Serato database tables
    createLibraryTable(database, kSeratoLibraryTable);
    createPlaylistsTable(database, kSeratoPlaylistsTable);
    createPlaylistTracksTable(database, kSeratoPlaylistTracksTable);
    transaction.commit();

    connect(&m_databasesFutureWatcher,
            &QFutureWatcher<QList<TreeItem*>>::finished,
            this,
            &SeratoFeature::onSeratoDatabasesFound);
    connect(&m_tracksFutureWatcher,
            &QFutureWatcher<QString>::finished,
            this,
            &SeratoFeature::onTracksFound);
    // Bite DJ: drop a database from the sidebar the moment its drive is
    // unmounted (in-skin Eject, physical yank, external unmount), rather than
    // waiting for the background poll to notice it has gone.
    connect(pLibrary,
            &Library::mountEjected,
            this,
            &SeratoFeature::ejectDevice);

    // initialize the model
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));

    // Background polling: surface mounted Serato databases and pre-parse
    // them without requiring a user tap. Required now that the sidebar
    // entry is hidden while no database is present — there is no root item
    // to tap to trigger the first scan. Mirrors RekordboxFeature.
    connect(&m_bgDatabasesFutureWatcher,
            &QFutureWatcher<QList<TreeItem*>>::finished,
            this,
            &SeratoFeature::onBackgroundSeratoDatabasesFound);
    connect(&m_bgTracksFutureWatcher,
            &QFutureWatcher<QString>::finished,
            this,
            &SeratoFeature::onBackgroundTracksFound);
    m_bgPollTimer.setInterval(5000);
    m_bgPollTimer.setSingleShot(false);
    connect(&m_bgPollTimer,
            &QTimer::timeout,
            this,
            &SeratoFeature::onBackgroundPollTick);
    m_bgPollTimer.start();
    // The sidebar entry stays hidden until a database is found, so don't sit
    // on a full poll interval before surfacing a drive that is already
    // mounted at startup — kick the first enumeration immediately.
    QTimer::singleShot(0, this, &SeratoFeature::onBackgroundPollTick);
}

SeratoFeature::~SeratoFeature() {
    // Stop the timer first so no new background work is queued, then drain
    // all four futures before dropping the tables — a still-running
    // parseDatabase() would otherwise write into tables about to be dropped.
    m_bgPollTimer.stop();
    m_databasesFuture.waitForFinished();
    m_tracksFuture.waitForFinished();
    m_bgDatabasesFuture.waitForFinished();
    m_bgTracksFuture.waitForFinished();

    // Drop temporary Serato database tables on shutdown
    QSqlDatabase database = m_pTrackCollection->database();
    ScopedTransaction transaction(database);
    dropTable(database, kSeratoPlaylistTracksTable);
    dropTable(database, kSeratoPlaylistsTable);
    dropTable(database, kSeratoLibraryTable);
    transaction.commit();

    delete m_pSeratoPlaylistModel;
}

void SeratoFeature::bindLibraryWidget(WLibrary* libraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    WLibraryTextBrowser* edit = new WLibraryTextBrowser(libraryWidget);
    edit->setHtml(formatRootViewHtml());
    edit->setOpenLinks(false);
    connect(edit, &WLibraryTextBrowser::anchorClicked, this, &SeratoFeature::htmlLinkClicked);
    libraryWidget->registerView("SERATOHOME", edit);
}

void SeratoFeature::htmlLinkClicked(const QUrl& link) {
    if (QString(link.path()) == "refresh") {
        activate();
    } else {
        qDebug() << "Unknown link clicked" << link;
    }
}

std::unique_ptr<BaseSqlTableModel>
SeratoFeature::createPlaylistModelForPlaylist(const QVariant& data) {
    VERIFY_OR_DEBUG_ASSERT(data.canConvert<QVariantList>()) {
        return {};
    }
    QVariantList playlists = data.toList();
    VERIFY_OR_DEBUG_ASSERT(playlists.size() > 0) {
        return {};
    }
    auto pModel = std::make_unique<SeratoPlaylistModel>(
            this, m_pLibrary->trackCollectionManager(), m_trackSource);
    pModel->setPlaylist(playlists.at(0).toString());
    return pModel;
}

QVariant SeratoFeature::title() {
    return m_title;
}

bool SeratoFeature::isSupported() {
    return true;
}

TreeItemModel* SeratoFeature::sidebarModel() const {
    return m_pSidebarModel;
}

QString SeratoFeature::formatRootViewHtml() const {
    QString title = tr("Serato");
    QString summary = tr("Reads the following from the Serato Music directory and removable devices:");
    QStringList items;

    items << tr("Tracks")
          << tr("Crates");

    QString html;
    QString refreshLink = tr("Check for Serato databases (refresh)");
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

void SeratoFeature::refreshLibraryModels() {
}

void SeratoFeature::activate() {
    qDebug() << "SeratoFeature::activate()";

    // Let a worker thread do the parsing
    m_databasesFuture = QtConcurrent::run(findSeratoDatabases);
    m_databasesFutureWatcher.setFuture(m_databasesFuture);
    m_title = tr("(loading) Serato");
    //calls a slot in the sidebar model such that 'Serato (isLoading)' is displayed.
    emit featureIsLoading(this, true);

    emit enableCoverArtDisplay(true);
    emit switchToView("SERATOHOME");
    emit disableSearch();
}

void SeratoFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }

    //access underlying TreeItem object
    TreeItem* item = static_cast<TreeItem*>(index.internalPointer());
    if (!(item && item->getData().isValid())) {
        return;
    }

    // TreeItem list data holds 2 values in a QList:
    //
    //     1. Playlist Name/Path (QString)
    //     2. isPlaylist (boolean)
    //
    // If the second element is false, then the database does still have to be
    // parsed.
    QList<QVariant> data = item->getData().toList();
    VERIFY_OR_DEBUG_ASSERT(data.size() == 2) {
        return;
    }
    QString playlist = data[0].toString();
    bool isPlaylist = data[1].toBool();

    qDebug() << "SeratoFeature::activateChild " << item->getLabel();

    if (!isPlaylist) {
        // Let a worker thread do the parsing
        m_tracksFuture = QtConcurrent::run(parseDatabase, static_cast<Library*>(parent())->dbConnectionPool(), item);
        m_tracksFutureWatcher.setFuture(m_tracksFuture);

        // This device is now a playlist element, future activations should
        // treat is as such
        data[1] = QVariant(true);
        item->setData(QVariant(data));
    } else {
        qDebug() << "Activate Serato Playlist: " << playlist;
        emit saveModelState();
        m_pSeratoPlaylistModel->setPlaylist(playlist);
        // Playlist names are filesystem paths — the database file for the
        // device-level playlist, the .crate file for a crate — so they double
        // as the location tying this view to its (removable) drive.
        m_pSeratoPlaylistModel->setBackingLocation(playlist);
        emit showTrackModel(m_pSeratoPlaylistModel);
    }
}

void SeratoFeature::onSeratoDatabasesFound() {
    const QList<TreeItem*> result = m_databasesFuture.result();
    auto foundDatabases = std::vector<std::unique_ptr<TreeItem>>(result.cbegin(), result.cend());

    mergeFoundDatabasesIntoSidebar(std::move(foundDatabases), /*isBackgroundScan=*/false);

    // calls a slot in the sidebarmodel such that 'isLoading' is removed from the feature title.
    m_title = tr("Serato");
    emit featureLoadingFinished(this);
}

void SeratoFeature::mergeFoundDatabasesIntoSidebar(
        std::vector<std::unique_ptr<TreeItem>> foundDatabases,
        bool isBackgroundScan) {
    clearLastRightClickedIndex();

    TreeItem* root = m_pSidebarModel->getRootItem();

    if (foundDatabases.size() == 0) {
        // No Serato databases found

        if (isBackgroundScan &&
                ++m_bgConsecutiveEmptyScans < kBgEmptyScansBeforeRemoval) {
            // Background poll: a single empty enumeration is often a transient
            // hiccup right after a (re)mount. Wait for several consecutive
            // empty scans before removing, so the database isn't needlessly
            // re-parsed when it reappears on the next tick.
            return;
        }

        // Nothing is mounted any more, so no staged database can still be
        // waiting for its crates either.
        const QStringList staged = stagedDatabaseLabels();
        for (const QString& label : staged) {
            dropStagedDatabase(label);
        }

        if (root->childRows() > 0) {
            // Devices have since been unmounted
            m_pSidebarModel->removeRows(0, root->childRows());
        }
        m_bgConsecutiveEmptyScans = 0;
        emit requestSidebarVisibility(this, false);
        return;
    }

    m_bgConsecutiveEmptyScans = 0;

    // Iterate backwards so removing a row doesn't shift an unvisited database
    // into the slot the loop has already passed.
    for (int databaseIndex = root->childRows() - 1; databaseIndex >= 0; databaseIndex--) {
        TreeItem* child = root->child(databaseIndex);
        bool removeChild = true;

        for (const auto& pDatabaseFound : foundDatabases) {
            if (pDatabaseFound->getLabel() == child->getLabel()) {
                removeChild = false;
                break;
            }
        }
        if (removeChild) {
            // Device has since been unmounted, cleanup DB
            m_pSidebarModel->removeRows(databaseIndex, 1);
        }
    }

    if (root->childRows() == 0) {
        // Every parsed database went away; the staged ones aren't shown yet,
        // so the feature has nothing left to display.
        emit requestSidebarVisibility(this, false);
    }

    // Forget databases that disappeared again before their parse finished.
    const QStringList staged = stagedDatabaseLabels();
    for (const QString& label : staged) {
        bool stillMounted = false;
        for (const auto& pDatabaseFound : foundDatabases) {
            if (pDatabaseFound->getLabel() == label) {
                stillMounted = true;
                break;
            }
        }
        if (!stillMounted) {
            dropStagedDatabase(label);
        }
    }

    for (auto&& pDatabaseFound : foundDatabases) {
        const QString label = pDatabaseFound->getLabel();
        if (findDatabaseByLabel(label) || findStagedDatabase(label)) {
            // Already shown, or already staged for parsing — don't add or
            // parse it again.
            continue;
        }
        // Bite DJ: a newly found database is staged here rather than inserted
        // into the sidebar. Its crates only exist once parseDatabase() has
        // run, and a device row that can't be expanded is confusing, so the
        // row is added by promoteCompletedDrives() once the whole drive is
        // parsed.
        StagedDatabase stagedDatabase;
        stagedDatabase.driveKey = driveKeyOfDatabase(pDatabaseFound.get());
        stagedDatabase.pItem = std::move(pDatabaseFound);
        m_stagedDatabases.push_back(std::move(stagedDatabase));
    }

    // A database dropped above may have been the last unparsed volume holding
    // its drive's siblings back.
    promoteCompletedDrives();
    pumpBackgroundParseQueue();
}

QString SeratoFeature::driveKeyOfDatabase(const TreeItem* pDatabase) const {
    // The label is the mount point the _Serato_ directory was found under
    // (or the Music folder for a local library).
    const QString mountPoint = pDatabase->getLabel();
    // Volumes of one physical drive share a USB device node, which is what
    // holds them together until the last of them has been parsed. A volume
    // that doesn't resolve to one (the local Music library, or sysfs not
    // telling us) is its own group, keyed by its path so it can't collide.
    const QString usbDeviceNode = mixxx::usbDeviceNodeForMountPoint(mountPoint);
    return usbDeviceNode.isEmpty() ? QDir::cleanPath(mountPoint) : usbDeviceNode;
}

SeratoFeature::StagedDatabase* SeratoFeature::findStagedDatabase(const QString& label) {
    for (auto& stagedDatabase : m_stagedDatabases) {
        if (stagedDatabase.pItem->getLabel() == label) {
            return &stagedDatabase;
        }
    }
    return nullptr;
}

QStringList SeratoFeature::stagedDatabaseLabels() const {
    QStringList labels;
    labels.reserve(static_cast<int>(m_stagedDatabases.size()));
    for (const auto& stagedDatabase : m_stagedDatabases) {
        labels.append(stagedDatabase.pItem->getLabel());
    }
    return labels;
}

std::unique_ptr<TreeItem> SeratoFeature::takeStagedDatabase(const QString& label) {
    for (auto it = m_stagedDatabases.begin(); it != m_stagedDatabases.end(); ++it) {
        if (it->pItem->getLabel() == label) {
            std::unique_ptr<TreeItem> pDatabase = std::move(it->pItem);
            m_stagedDatabases.erase(it);
            return pDatabase;
        }
    }
    return nullptr;
}

void SeratoFeature::dropStagedDatabase(const QString& label) {
    if (m_bgParseInFlight && m_bgParseLabel == label) {
        // A worker thread is writing into this item right now, so it has to
        // outlive the parse. onBackgroundTracksFound() discards it instead of
        // inserting it into the sidebar.
        m_bgParseAbandoned = true;
        return;
    }
    takeStagedDatabase(label);
}

void SeratoFeature::promoteCompletedDrives() {
    // A drive with several volumes (e.g. a Serato partition plus a second data
    // partition) mounts as one sidebar row per volume. Showing the first
    // volume as soon as it is parsed would leave its siblings appearing late,
    // so hold every volume back until the whole drive is done.
    QSet<QString> incompleteDrives;
    for (const auto& stagedDatabase : m_stagedDatabases) {
        if (!stagedDatabase.parsed) {
            incompleteDrives.insert(stagedDatabase.driveKey);
        }
    }

    std::vector<std::unique_ptr<TreeItem>> childrenToAdd;
    for (auto it = m_stagedDatabases.begin(); it != m_stagedDatabases.end();) {
        if (incompleteDrives.contains(it->driveKey)) {
            ++it;
            continue;
        }
        childrenToAdd.push_back(std::move(it->pItem));
        it = m_stagedDatabases.erase(it);
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

void SeratoFeature::ejectDevice(const QString& mountPoint) {
    // Runs on the GUI thread (Library::mountEjected is emitted from
    // SystemSettings, same thread), so it is safe to mutate the sidebar
    // model directly here.
    TreeItem* root = m_pSidebarModel->getRootItem();
    if (!root) {
        return;
    }

    // findSeratoDatabases() labels each entry with the device's mount path
    // (trailing slash removed), so match the ejected mount point on the
    // label. Normalise both so a trailing slash still matches.
    const QString wanted = QDir::cleanPath(mountPoint);

    // The database may still be staged — parsing, waiting for a sibling volume
    // on the same drive, or queued behind another parse — in which case it has
    // no sidebar row to remove yet.
    const QStringList staged = stagedDatabaseLabels();
    for (const QString& label : staged) {
        if (QDir::cleanPath(label) == wanted) {
            dropStagedDatabase(label);
            // This may have been the last unparsed volume of its drive.
            promoteCompletedDrives();
            m_bgConsecutiveEmptyScans = 0;
            return;
        }
    }

    for (int databaseIndex = 0; databaseIndex < root->childRows(); ++databaseIndex) {
        TreeItem* child = root->child(databaseIndex);
        if (!child || QDir::cleanPath(child->getLabel()) != wanted) {
            continue;
        }

        // A right-clicked index cached against the old row layout would dangle
        // once we remove a row; clear it as mergeFoundDatabasesIntoSidebar() does.
        clearLastRightClickedIndex();

        m_pSidebarModel->removeRows(databaseIndex, 1);

        if (root->childRows() == 0) {
            // That was the last Serato database; retire the sidebar entry.
            emit requestSidebarVisibility(this, false);
        }

        // This may have been the last database; reset the poll's empty-scan
        // guard so a stale count doesn't linger into the next enumeration.
        m_bgConsecutiveEmptyScans = 0;
        return;
    }
}

void SeratoFeature::onTracksFound() {
    qDebug() << "onTracksFound";
    m_pSidebarModel->triggerRepaint();

    QString databasePlaylist = m_tracksFuture.result();

    qDebug() << "Show Serato Database Playlist: " << databasePlaylist;
    emit saveModelState();
    m_pSeratoPlaylistModel->setPlaylist(databasePlaylist);
    m_pSeratoPlaylistModel->setBackingLocation(databasePlaylist);
    emit showTrackModel(m_pSeratoPlaylistModel);

    // A background queue that yielded to this foreground parse may have
    // stalled — kick it forward now that the foreground slot is free.
    pumpBackgroundParseQueue();
}

void SeratoFeature::onBackgroundPollTick() {
    // Yield to any foreground or already-running background scan.
    if (m_databasesFutureWatcher.isRunning()) {
        return;
    }
    if (m_bgDatabasesFutureWatcher.isRunning()) {
        return;
    }
    m_bgDatabasesFuture = QtConcurrent::run(findSeratoDatabases);
    m_bgDatabasesFutureWatcher.setFuture(m_bgDatabasesFuture);
}

void SeratoFeature::onBackgroundSeratoDatabasesFound() {
    const QList<TreeItem*> result = m_bgDatabasesFuture.result();
    auto foundDatabases = std::vector<std::unique_ptr<TreeItem>>(
            result.cbegin(), result.cend());

    mergeFoundDatabasesIntoSidebar(std::move(foundDatabases), /*isBackgroundScan=*/true);
}

void SeratoFeature::onBackgroundTracksFound() {
    try {
        (void)m_bgTracksFuture.result();
    } catch (const std::exception& e) {
        // Show the database anyway, with whatever crates the parse got
        // through: discarding it here would only have the next poll tick
        // rediscover it and fail again, forever.
        qWarning() << "Background Serato parse failed:" << e.what();
    }
    m_bgParseInFlight = false;

    const QString label = m_bgParseLabel;
    m_bgParseLabel.clear();

    if (m_bgParseAbandoned) {
        // The drive went away mid-parse; don't show a row for a device that
        // is gone.
        m_bgParseAbandoned = false;
        takeStagedDatabase(label);
    } else if (StagedDatabase* pStagedDatabase = findStagedDatabase(label)) {
        pStagedDatabase->parsed = true;
    }

    // The database enters the sidebar only now, with its crates attached —
    // and only once every other volume of the same drive is parsed too.
    promoteCompletedDrives();
    m_pSidebarModel->triggerRepaint();

    pumpBackgroundParseQueue();
}

void SeratoFeature::pumpBackgroundParseQueue() {
    if (m_bgParseInFlight) {
        return;
    }
    if (m_tracksFutureWatcher.isRunning()) {
        // Yield to a user-driven parse so we don't double-up SQL writers
        // for the same database.
        return;
    }
    bool skippedUnparseable = false;
    for (auto& stagedDatabase : m_stagedDatabases) {
        if (stagedDatabase.parsed) {
            continue;
        }
        TreeItem* item = stagedDatabase.pItem.get();
        const QString label = item->getLabel();
        QList<QVariant> data = item->getData().toList();
        if (data.size() < 2 || data[1].toBool()) {
            // Not a parseable database row; it would never gain crates, so
            // count it as done rather than blocking its drive forever.
            stagedDatabase.parsed = true;
            skippedUnparseable = true;
            continue;
        }
        // Flip the flag BEFORE kick-off to mirror activateChild() — by the
        // time the database reaches the sidebar it is a plain playlist row
        // pointing at the device's "all tracks" playlist.
        data[1] = QVariant(true);
        item->setData(QVariant(data));

        m_bgTracksFuture = QtConcurrent::run(parseDatabase,
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

TreeItem* SeratoFeature::findDatabaseByLabel(const QString& label) const {
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
