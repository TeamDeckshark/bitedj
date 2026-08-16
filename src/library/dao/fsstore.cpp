#include "library/dao/fsstore.h"

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>

#include "preferences/systemsettings.h"

namespace {

// Every per-filesystem store shares one directory at the root of the drive,
// alongside the analysis cache.
const QString kStoreDirName = QStringLiteral(".bitedj");

QString nextConnectionName() {
    static QAtomicInt counter;
    return QStringLiteral("fsstore-%1").arg(counter.fetchAndAddRelaxed(1));
}

/// Whether a SQLite error says the file is not a usable database at all, in
/// which case it can only be replaced.
bool isCorruptionError(const QSqlError& error) {
    // SQLITE_CORRUPT = 11 (malformed disk image), SQLITE_NOTADB = 26.
    const QString native = error.nativeErrorCode();
    if (native == QLatin1String("11") || native == QLatin1String("26")) {
        return true;
    }
    const QString text = error.text();
    return text.contains(QLatin1String("file is not a database"), Qt::CaseInsensitive) ||
            text.contains(QLatin1String("database disk image is malformed"),
                    Qt::CaseInsensitive);
}

void fillPaths(const QString& rootPath, const QString& dbName, FsStoreTarget* pTarget) {
    const QDir storeDir(rootPath + QDir::separator() + kStoreDirName);
    pTarget->rootPath = rootPath;
    pTarget->storeDirPath = storeDir.absolutePath();
    pTarget->dbPath = storeDir.absoluteFilePath(dbName);
}

} // anonymous namespace

// static
bool FsStoreTarget::resolveForFile(
        const QString& fileLocation, const QString& dbName, FsStoreTarget* pTarget) {
    const QString absPath = QFileInfo(fileLocation).absoluteFilePath();
    if (absPath.isEmpty()) {
        return false;
    }
    const QStorageInfo storage(absPath);
    if (!storage.isValid() || !storage.isReady()) {
        return false;
    }
    const QString rootPath = storage.rootPath();
    if (rootPath.isEmpty() || !SystemSettings::isOnRemovableMedia(rootPath)) {
        return false;
    }
    fillPaths(rootPath, dbName, pTarget);
    pTarget->relPath = QDir(rootPath).relativeFilePath(absPath);
    pTarget->writable = !storage.isReadOnly();
    return true;
}

// static
bool FsStoreTarget::resolveForMount(
        const QString& mountRoot, const QString& dbName, FsStoreTarget* pTarget) {
    const QString cleanRoot = QDir::cleanPath(mountRoot);
    if (cleanRoot.isEmpty() || !QFileInfo(cleanRoot).isDir()) {
        // QStorageInfo hands a path that is not there back as its own
        // rootPath, so an ejected mount would otherwise resolve to itself.
        return false;
    }
    const QStorageInfo storage(cleanRoot);
    if (!storage.isValid() || !storage.isReady()) {
        return false;
    }
    const QString rootPath = storage.rootPath();
    // Only the mount point itself, so that a folder on the boot volume that
    // happens to sit under a removable root cannot stand in for a drive.
    if (rootPath.isEmpty() || QDir::cleanPath(rootPath) != cleanRoot ||
            !SystemSettings::isOnRemovableMedia(rootPath)) {
        return false;
    }
    // Store the cleaned root, so that a caller comparing paths against
    // FsStoreTarget::rootPath sees the same string it passed in.
    fillPaths(cleanRoot, dbName, pTarget);
    pTarget->writable = !storage.isReadOnly();
    return true;
}

ScopedFsStore::ScopedFsStore(const char* logTag)
        : m_logTag(logTag) {
}

ScopedFsStore::~ScopedFsStore() {
    if (m_connectionName.isEmpty()) {
        return;
    }
    {
        QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
        if (db.isValid()) {
            db.close();
        }
    }
    // The QSqlDatabase copies above must be out of scope before removing the
    // connection, or Qt warns that it is still in use.
    QSqlDatabase::removeDatabase(m_connectionName);
}

QSqlDatabase ScopedFsStore::database() const {
    return QSqlDatabase::database(m_connectionName);
}

bool ScopedFsStore::open(const FsStoreTarget& target, const QString& createTableDdl) {
    const bool create = !createTableDdl.isEmpty();
    if (!create && !QFileInfo::exists(target.dbPath)) {
        return false;
    }
    if (create && !QDir().mkpath(target.storeDirPath)) {
        qWarning() << m_logTag << ": cannot create" << target.storeDirPath;
        return false;
    }

    m_connectionName = nextConnectionName();
    QSqlDatabase db = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), m_connectionName);
    db.setDatabaseName(target.dbPath);

    const auto openConnection = [&createTableDdl, create](
                                        QSqlDatabase& db, QSqlError* pError) {
        if (!db.open()) {
            *pError = db.lastError();
            return false;
        }
        if (!create) {
            return true;
        }
        // Creating the schema reads sqlite_master, which is the first access
        // that surfaces a corrupt file.
        QSqlQuery query(db);
        if (!query.exec(createTableDdl)) {
            *pError = query.lastError();
            return false;
        }
        return true;
    };

    for (int attempt = 0; attempt < 2; ++attempt) {
        QSqlError error;
        if (openConnection(db, &error)) {
            return true;
        }
        const bool canRecover = target.writable && attempt == 0 &&
                isCorruptionError(error) && QFileInfo::exists(target.dbPath);
        if (!canRecover) {
            qWarning() << m_logTag << ": cannot open" << target.dbPath << error.text();
            return false;
        }
        qWarning() << m_logTag << ": the store at" << target.dbPath
                   << "is unreadable (" << error.text()
                   << "); replacing it with an empty one";
        db.close();
        if (!QFile::remove(target.dbPath)) {
            qWarning() << m_logTag << ": failed to delete" << target.dbPath;
            return false;
        }
        if (!create) {
            // Nothing left to read from.
            return false;
        }
    }
    return false;
}

bool fsStoreRemove(const QString& mountPoint, const QString& dbName, const char* logTag) {
    const QDir storeDir(
            QDir::cleanPath(mountPoint) + QDir::separator() + kStoreDirName);
    // Drop the journal siblings a crash may have left behind as well: SQLite
    // would roll a stale hot journal back into a freshly created database.
    const QStringList names = {
            dbName,
            dbName + QStringLiteral("-journal"),
            dbName + QStringLiteral("-wal"),
            dbName + QStringLiteral("-shm"),
    };
    bool ok = true;
    for (const QString& name : names) {
        const QString path = storeDir.absoluteFilePath(name);
        if (!QFileInfo::exists(path)) {
            continue;
        }
        if (QFile::remove(path)) {
            qInfo() << logTag << ": deleted" << path;
        } else {
            qWarning() << logTag << ": cannot delete" << path;
            ok = false;
        }
    }
    return ok;
}
