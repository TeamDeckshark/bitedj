#include "library/dao/fsanalysiscache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QtGlobal>
#include <utility>

#include "library/rekordbox/rekordboxanalysis.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "preferences/waveformsettings.h"
#include "track/keyfactory.h"

namespace {

// Mirror AnalysisDao: default zlib compression level is the best size/CPU tradeoff.
constexpr int kCompressionLevel = -1;

// Per-filesystem cache layout, consistent with the rebranded ~/.bitedj data dir.
const QString kCacheDirName = QStringLiteral(".bitedj");
const QString kCacheDbName = QStringLiteral("analysis.sqlite");

const ConfigKey kEnabledConfigKey =
        ConfigKey(QStringLiteral("[Library]"), QStringLiteral("AnalysisCacheOnTrackFs"));

const ConfigKey kHomeCacheEnabledConfigKey =
        ConfigKey(QStringLiteral("[Library]"), QStringLiteral("AnalysisCacheInHome"));

int checksumOf(const QByteArray& data) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return qChecksum(data);
#else
    return qChecksum(data.constData(), data.length());
#endif
}

// Whether a SQLite error indicates the database file itself is unusable, i.e.
// it isn't a valid SQLite database or its image is structurally damaged. Such a
// file can never be opened or repaired, so the cache deletes and recreates it.
bool isCorruptionError(const QSqlError& error) {
    // SQLITE_CORRUPT = 11 (malformed disk image), SQLITE_NOTADB = 26 (not a
    // database / encrypted). nativeErrorCode() carries the primary result code.
    const QString native = error.nativeErrorCode();
    if (native == QLatin1String("11") || native == QLatin1String("26")) {
        return true;
    }
    const QString text = error.text();
    return text.contains(QLatin1String("file is not a database"), Qt::CaseInsensitive) ||
            text.contains(QLatin1String("database disk image is malformed"),
                    Qt::CaseInsensitive);
}

} // namespace

QMutex FsAnalysisCache::s_registryMutex;
QSet<FsAnalysisCache*> FsAnalysisCache::s_instances;

FsAnalysisCache::FsAnalysisCache(UserSettingsPointer pConfig)
        : m_pConfig(std::move(pConfig)) {
    QMutexLocker registryLocker(&s_registryMutex);
    s_instances.insert(this);
}

FsAnalysisCache::~FsAnalysisCache() {
    // Unregister first (under the registry lock) so a concurrent
    // closeFilesystemConnections() cannot dereference this while we tear down.
    {
        QMutexLocker registryLocker(&s_registryMutex);
        s_instances.remove(this);
    }
    // Drop every connection this instance opened. Connections must be closed and
    // out of scope before removeDatabase(), so iterate over the stored names.
    QMutexLocker handlesLocker(&m_handlesMutex);
    for (auto it = m_handles.begin(); it != m_handles.end();) {
        const QString connectionName = it->connectionName;
        it->db.close();
        // The stored handle must be gone before removeDatabase(), or Qt warns
        // that the connection is still in use.
        it = m_handles.erase(it);
        if (!connectionName.isEmpty()) {
            QSqlDatabase::removeDatabase(connectionName);
        }
    }
}

bool FsAnalysisCache::isEnabled() const {
    return m_pConfig->getValue(kEnabledConfigKey, true);
}

bool FsAnalysisCache::isHomeCacheEnabled() const {
    return m_pConfig->getValue(kHomeCacheEnabledConfigKey, false);
}

QSqlDatabase FsAnalysisCache::databaseForTrack(
        const QString& trackLocation, QString* pRelPath, bool* pWritable) {
    if (pWritable) {
        *pWritable = false;
    }
    const QString absPath = QFileInfo(trackLocation).absoluteFilePath();
    if (absPath.isEmpty()) {
        return QSqlDatabase();
    }

    const QStorageInfo storage(absPath);
    if (!storage.isValid() || !storage.isReady()) {
        return QSqlDatabase();
    }
    const QString rootPath = storage.rootPath();
    if (rootPath.isEmpty()) {
        return QSqlDatabase();
    }
    if (pRelPath) {
        *pRelPath = QDir(rootPath).relativeFilePath(absPath);
    }

    const auto it = m_handles.constFind(rootPath);
    if (it != m_handles.constEnd()) {
        if (pWritable) {
            *pWritable = it->writable;
        }
        return it->db;
    }

    const QDir cacheDir(rootPath + QDir::separator() + kCacheDirName);
    const QString dbPath = cacheDir.absoluteFilePath(kCacheDbName);
    const bool writable = !storage.isReadOnly();

    // On a read-only filesystem we can only use an already-existing cache.
    if (!writable && !QFileInfo::exists(dbPath)) {
        m_handles.insert(rootPath, FsHandle{QString(), QSqlDatabase(), false});
        return QSqlDatabase();
    }
    if (writable && !QDir().mkpath(cacheDir.absolutePath())) {
        qWarning() << "FsAnalysisCache: cannot create cache dir" << cacheDir.absolutePath();
        m_handles.insert(rootPath, FsHandle{QString(), QSqlDatabase(), false});
        return QSqlDatabase();
    }

    const QString connectionName =
            QStringLiteral("fsanalysis-%1-%2")
                    .arg(static_cast<qulonglong>(reinterpret_cast<quintptr>(this)))
                    .arg(static_cast<qulonglong>(qHash(rootPath)));
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    db.setDatabaseName(dbPath);

    // Open the cache, recovering once from a corrupt/undeserializable database file
    // by deleting it so a fresh one is created in its place. Only attempted on a
    // writable filesystem (a read-only stick can neither be repaired nor deleted).
    for (int attempt = 0; attempt < 2; ++attempt) {
        QSqlError error;
        if (openConnection(db, writable, &error)) {
            m_handles.insert(rootPath, FsHandle{connectionName, db, writable});
            if (pWritable) {
                *pWritable = writable;
            }
            return db;
        }

        const bool canRecover = writable && attempt == 0 &&
                isCorruptionError(error) && QFileInfo::exists(dbPath);
        if (!canRecover) {
            qWarning() << "FsAnalysisCache: cannot open" << dbPath << error.text();
            break;
        }

        qWarning() << "FsAnalysisCache: cache database" << dbPath
                   << "is corrupt or unreadable (" << error.text()
                   << "); deleting it and recreating an empty cache";
        db.close();
        if (!QFile::remove(dbPath)) {
            qWarning() << "FsAnalysisCache: failed to delete corrupt cache" << dbPath;
            break;
        }
        // Loop to reopen; db.open() recreates the file on a writable filesystem.
    }

    db.close();
    QSqlDatabase::removeDatabase(connectionName);
    m_handles.insert(rootPath, FsHandle{QString(), QSqlDatabase(), false});
    return QSqlDatabase();
}

bool FsAnalysisCache::openConnection(QSqlDatabase& db, bool writable, QSqlError* pError) {
    if (!db.open()) {
        if (pError) {
            *pError = db.lastError();
        }
        return false;
    }

    if (writable) {
        // Creating (or touching) the schema reads sqlite_master, which is the first
        // access that surfaces a corrupt/non-SQLite file.
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS waveform_cache ("
                    "relpath TEXT NOT NULL, type INTEGER NOT NULL, "
                    "description TEXT, version TEXT, data_checksum INTEGER, data BLOB, "
                    "PRIMARY KEY (relpath, type))"))) {
            if (pError) {
                *pError = query.lastError();
            }
            return false;
        }
        // Beat grid, keyed by relpath. Compressed like the waveform blobs and
        // covered by the same corrupt-file recovery path in databaseForTrack().
        // No `type` column: exactly one beats row per track. `version` is Mixxx's
        // "BeatMap-1.0" etc, needed to reconstruct via mixxx::Beats::fromByteArray.
        if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS beats_cache ("
                    "relpath TEXT NOT NULL PRIMARY KEY, "
                    "version TEXT, sub_version TEXT, "
                    "data_checksum INTEGER, data BLOB)"))) {
            if (pError) {
                *pError = query.lastError();
            }
            return false;
        }
        // Bite DJ: whose grid and key the DJ picked for a track a rekordbox
        // export has analysed (RekordboxAnalysis::AnalysisSource values). No
        // row is the default: rekordbox's, whenever its grid can be read.
        if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS analysis_source ("
                    "relpath TEXT NOT NULL PRIMARY KEY, "
                    "source INTEGER NOT NULL)"))) {
            if (pError) {
                *pError = query.lastError();
            }
            return false;
        }
        // Detected keys, same shape. Keys::toByteArray() serializes the whole
        // key-detection result, but the global key text is also stored plainly
        // so a stick can be inspected without a Keys deserializer.
        if (!query.exec(QStringLiteral(
                    "CREATE TABLE IF NOT EXISTS keys_cache ("
                    "relpath TEXT NOT NULL PRIMARY KEY, "
                    "version TEXT, sub_version TEXT, "
                    "data_checksum INTEGER, data BLOB, "
                    "global_key_id INTEGER, global_key_text TEXT)"))) {
            if (pError) {
                *pError = query.lastError();
            }
            return false;
        }
    }
    return true;
}

QList<AnalysisDao::AnalysisInfo> FsAnalysisCache::getAnalysesForTrack(
        const QString& trackLocation) {
    QList<AnalysisDao::AnalysisInfo> analyses;

    // Hold the handles lock across the whole operation so an eject-driven
    // releaseFilesystem() on another thread can't close the connection mid-query.
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath);
    if (!db.isValid() || !db.isOpen()) {
        return analyses;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT type, description, version, data_checksum, data "
            "FROM waveform_cache WHERE relpath = :relpath"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    if (!query.exec()) {
        qWarning() << "FsAnalysisCache: query failed for" << relPath
                   << query.lastError().text();
        return analyses;
    }

    while (query.next()) {
        const QByteArray compressedData = query.value(4).toByteArray();
        const int storedChecksum = query.value(3).toInt();
        if (checksumOf(compressedData) != storedChecksum) {
            qWarning() << "FsAnalysisCache: corrupt cache entry for" << relPath;
            continue;
        }
        AnalysisDao::AnalysisInfo info;
        info.type = static_cast<AnalysisDao::AnalysisType>(query.value(0).toInt());
        info.description = query.value(1).toString();
        info.version = query.value(2).toString();
        info.data = qUncompress(compressedData);
        analyses.append(info);
    }
    return analyses;
}

bool FsAnalysisCache::saveAnalysis(
        QSqlDatabase& db,
        const QString& relPath,
        AnalysisDao::AnalysisType type,
        const QString& description,
        const QString& version,
        const QByteArray& data) {
    const QByteArray compressedData = qCompress(data, kCompressionLevel);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO waveform_cache "
            "(relpath, type, description, version, data_checksum, data) "
            "VALUES (:relpath, :type, :description, :version, :data_checksum, :data)"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    query.bindValue(QStringLiteral(":type"), static_cast<int>(type));
    query.bindValue(QStringLiteral(":description"), description);
    query.bindValue(QStringLiteral(":version"), version);
    query.bindValue(QStringLiteral(":data_checksum"), checksumOf(compressedData));
    query.bindValue(QStringLiteral(":data"), compressedData);
    if (!query.exec()) {
        qWarning() << "FsAnalysisCache: cannot save analysis for" << relPath
                   << query.lastError().text();
        return false;
    }
    return true;
}

bool FsAnalysisCache::saveTrackAnalyses(
        const QString& trackLocation,
        ConstWaveformPointer pWaveform,
        ConstWaveformPointer pWaveSummary) {
    WaveformSettings waveformSettings(m_pConfig);
    if (!waveformSettings.waveformCachingEnabled()) {
        return false;
    }

    // Nothing to do unless both waveforms are valid and pending a save.
    if (!pWaveform || pWaveform->saveState() != Waveform::SaveState::SavePending ||
            !pWaveSummary || pWaveSummary->saveState() != Waveform::SaveState::SavePending) {
        return false;
    }

    // Hold the handles lock across the whole operation so an eject-driven
    // releaseFilesystem() on another thread can't close the connection mid-write.
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    bool writable = false;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath, &writable);
    if (!db.isValid() || !db.isOpen() || !writable) {
        // Read-only/unavailable filesystem: skip caching. A read-only stick is the
        // expected case and is intentionally silent; hard failures are logged in
        // databaseForTrack().
        return false;
    }

    bool ok = saveAnalysis(db,
            relPath,
            AnalysisDao::TYPE_WAVEFORM,
            pWaveform->getDescription(),
            pWaveform->getVersion(),
            pWaveform->toByteArray());
    if (ok) {
        pWaveform->setSaveState(Waveform::SaveState::Saved);
    }

    const bool summaryOk = saveAnalysis(db,
            relPath,
            AnalysisDao::TYPE_WAVESUMMARY,
            pWaveSummary->getDescription(),
            pWaveSummary->getVersion(),
            pWaveSummary->toByteArray());
    if (summaryOk) {
        pWaveSummary->setSaveState(Waveform::SaveState::Saved);
    }

    return ok && summaryOk;
}

bool FsAnalysisCache::saveTrackBeats(
        const QString& trackLocation,
        const mixxx::BeatsPointer& pBeats) {
    // Hold the handles lock across the whole operation so an eject-driven
    // releaseFilesystem() on another thread can't close the connection mid-write.
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    bool writable = false;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath, &writable);
    if (!db.isValid() || !db.isOpen() || !writable) {
        return false;
    }

    // Bite DJ: rekordbox's grid is never stored where Bite DJ's goes. It is
    // read from the drive's export on every load, it cannot be edited here
    // (BpmControl refuses), and the slot below keeps Bite DJ's own analysis
    // for the DJ to switch back to.
    if (RekordboxAnalysis::isRekordboxBeats(pBeats)) {
        return true;
    }

    // A null pBeats means the track has no beat grid — delete any stored entry
    // so a stale one from an older analyzer run doesn't come back on next load.
    if (!pBeats) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
                "DELETE FROM beats_cache WHERE relpath = :relpath"));
        query.bindValue(QStringLiteral(":relpath"), relPath);
        if (!query.exec()) {
            qWarning() << "FsAnalysisCache: cannot clear beats for" << relPath
                       << query.lastError().text();
            return false;
        }
        return true;
    }

    const QByteArray compressedData = qCompress(pBeats->toByteArray(), kCompressionLevel);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO beats_cache "
            "(relpath, version, sub_version, data_checksum, data) "
            "VALUES (:relpath, :version, :sub_version, :data_checksum, :data)"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    query.bindValue(QStringLiteral(":version"), pBeats->getVersion());
    query.bindValue(QStringLiteral(":sub_version"), pBeats->getSubVersion());
    query.bindValue(QStringLiteral(":data_checksum"), checksumOf(compressedData));
    query.bindValue(QStringLiteral(":data"), compressedData);
    if (!query.exec()) {
        qWarning() << "FsAnalysisCache: cannot save beats for" << relPath
                   << query.lastError().text();
        return false;
    }
    return true;
}

mixxx::BeatsPointer FsAnalysisCache::getTrackBeats(
        const QString& trackLocation,
        mixxx::audio::SampleRate sampleRate) {
    // Same locking as the write path — releaseFilesystem() is the concurrent
    // caller we're serializing against.
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath);
    if (!db.isValid() || !db.isOpen()) {
        return nullptr;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT version, sub_version, data_checksum, data "
            "FROM beats_cache WHERE relpath = :relpath"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    if (!query.exec() || !query.next()) {
        return nullptr;
    }

    const QByteArray compressedData = query.value(3).toByteArray();
    const int storedChecksum = query.value(2).toInt();
    if (checksumOf(compressedData) != storedChecksum) {
        qWarning() << "FsAnalysisCache: corrupt beats entry for" << relPath;
        return nullptr;
    }
    const QString version = query.value(0).toString();
    if (version.isEmpty()) {
        return nullptr;
    }
    QByteArray beatsBlob = qUncompress(compressedData);
    return mixxx::Beats::fromByteArray(
            sampleRate, version, query.value(1).toString(), beatsBlob);
}

std::optional<int> FsAnalysisCache::getAnalysisSource(const QString& trackLocation) {
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath);
    if (!db.isValid() || !db.isOpen()) {
        return std::nullopt;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT source FROM analysis_source WHERE relpath = :relpath"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    // A cache written by an older build has no such table, which fails the
    // query: that is simply no choice made.
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }
    bool ok = false;
    const int source = query.value(0).toInt(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return source;
}

bool FsAnalysisCache::saveAnalysisSource(const QString& trackLocation, int source) {
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    bool writable = false;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath, &writable);
    if (!db.isValid() || !db.isOpen() || !writable) {
        return false;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO analysis_source (relpath, source) "
            "VALUES (:relpath, :source)"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    query.bindValue(QStringLiteral(":source"), source);
    if (!query.exec()) {
        qWarning() << "FsAnalysisCache: cannot save the analysis source for"
                   << relPath << query.lastError().text();
        return false;
    }
    return true;
}

bool FsAnalysisCache::saveTrackKeys(
        const QString& trackLocation,
        const Keys& keys) {
    // Bite DJ: same rule as for the grid. Rekordbox's key comes from the
    // drive's export on every load and must not take the place of Bite DJ's.
    if (keys.getSubVersion() == mixxx::rekordboxconstants::keysSubversion) {
        return true;
    }
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    bool writable = false;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath, &writable);
    if (!db.isValid() || !db.isOpen() || !writable) {
        return false;
    }

    // An empty version means no serializable key detection: clear the entry so
    // an older stored one doesn't shadow a re-run that removed keys.
    if (keys.getVersion().isEmpty()) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
                "DELETE FROM keys_cache WHERE relpath = :relpath"));
        query.bindValue(QStringLiteral(":relpath"), relPath);
        if (!query.exec()) {
            qWarning() << "FsAnalysisCache: cannot clear keys for" << relPath
                       << query.lastError().text();
            return false;
        }
        return true;
    }

    const QByteArray compressedData = qCompress(keys.toByteArray(), kCompressionLevel);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO keys_cache "
            "(relpath, version, sub_version, data_checksum, data, "
            "global_key_id, global_key_text) "
            "VALUES (:relpath, :version, :sub_version, :data_checksum, :data, "
            ":global_key_id, :global_key_text)"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    query.bindValue(QStringLiteral(":version"), keys.getVersion());
    query.bindValue(QStringLiteral(":sub_version"), keys.getSubVersion());
    query.bindValue(QStringLiteral(":data_checksum"), checksumOf(compressedData));
    query.bindValue(QStringLiteral(":data"), compressedData);
    query.bindValue(QStringLiteral(":global_key_id"),
            static_cast<int>(keys.getGlobalKey()));
    query.bindValue(QStringLiteral(":global_key_text"), keys.getGlobalKeyText());
    if (!query.exec()) {
        qWarning() << "FsAnalysisCache: cannot save keys for" << relPath
                   << query.lastError().text();
        return false;
    }
    return true;
}

std::optional<Keys> FsAnalysisCache::getTrackKeys(const QString& trackLocation) {
    QMutexLocker locker(&m_handlesMutex);
    QString relPath;
    QSqlDatabase db = databaseForTrack(trackLocation, &relPath);
    if (!db.isValid() || !db.isOpen()) {
        return std::nullopt;
    }

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT version, sub_version, data_checksum, data "
            "FROM keys_cache WHERE relpath = :relpath"));
    query.bindValue(QStringLiteral(":relpath"), relPath);
    if (!query.exec() || !query.next()) {
        return std::nullopt;
    }

    const QByteArray compressedData = query.value(3).toByteArray();
    const int storedChecksum = query.value(2).toInt();
    if (checksumOf(compressedData) != storedChecksum) {
        qWarning() << "FsAnalysisCache: corrupt keys entry for" << relPath;
        return std::nullopt;
    }
    const QString version = query.value(0).toString();
    if (version.isEmpty()) {
        return std::nullopt;
    }
    QByteArray keysBlob = qUncompress(compressedData);
    return KeyFactory::loadKeysFromByteArray(
            version, query.value(1).toString(), &keysBlob);
}

void FsAnalysisCache::releaseFilesystem(const QString& mountPoint) {
    const QString cleanedMount = QDir::cleanPath(mountPoint);

    QMutexLocker locker(&m_handlesMutex);
    for (auto it = m_handles.begin(); it != m_handles.end();) {
        if (QDir::cleanPath(it.key()) != cleanedMount) {
            ++it;
            continue;
        }
        const QString connectionName = it.value().connectionName;
        // Close through the stored handle, not QSqlDatabase::database(): this
        // usually runs on the GUI thread, and Qt refuses to hand out a
        // connection another thread opened. m_handlesMutex, held here and by
        // every cache operation, keeps the owning thread off it meanwhile.
        it.value().db.close();
        // Forget the memoized handle either way; a later access reopens lazily.
        // Erasing drops the stored handle, which removeDatabase() needs gone.
        it = m_handles.erase(it);
        if (connectionName.isEmpty()) {
            // Memoized "unavailable" marker, no connection/file descriptor to free.
            continue;
        }
        QSqlDatabase::removeDatabase(connectionName);
        qInfo() << "FsAnalysisCache: closed cache connection on" << cleanedMount
                << "for eject";
    }
}

// static
bool FsAnalysisCache::clearFilesystemCache(const QString& mountPoint) {
    // Free the SQLite file descriptors first so the delete below removes the
    // file every instance is using, not one that lives on as an open unlinked
    // inode still receiving writes.
    closeFilesystemConnections(mountPoint);

    const QDir cacheDir(
            QDir::cleanPath(mountPoint) + QDir::separator() + kCacheDirName);
    // Also drop any journal siblings a crash may have left behind: SQLite would
    // roll a stale hot journal back into the freshly created empty database and
    // corrupt it.
    const QStringList names = {
            kCacheDbName,
            kCacheDbName + QStringLiteral("-journal"),
            kCacheDbName + QStringLiteral("-wal"),
            kCacheDbName + QStringLiteral("-shm"),
    };
    bool ok = true;
    for (const QString& name : names) {
        const QString path = cacheDir.absoluteFilePath(name);
        if (!QFileInfo::exists(path)) {
            continue;
        }
        if (QFile::remove(path)) {
            qInfo() << "FsAnalysisCache: deleted" << path;
        } else {
            qWarning() << "FsAnalysisCache: cannot delete" << path;
            ok = false;
        }
    }
    return ok;
}

// static
void FsAnalysisCache::closeFilesystemConnections(const QString& mountPoint) {
    // Hold the registry lock for the whole sweep: it blocks instance construction
    // and destruction, so every pointer we visit stays alive. Each releaseFilesystem()
    // takes the instance's own handles lock, which serializes against that instance's
    // in-flight cache operation on its home thread, so the close never races a query.
    QMutexLocker registryLocker(&s_registryMutex);
    for (FsAnalysisCache* pInstance : std::as_const(s_instances)) {
        pInstance->releaseFilesystem(mountPoint);
    }
}
