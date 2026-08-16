#include "library/dao/fshistorystore.h"

#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>

#include "library/dao/fsstore.h"

namespace {

// Per-filesystem layout, alongside the analysis cache, the cue overrides and
// the sampler banks in the same .bitedj dir.
const QString kStoreDbName = QStringLiteral("history.sqlite");
const char kLogTag[] = "FsHistoryStore";

// One row per played track. The session is identified by its name rather than
// by a surrogate id: names are unique on a drive (newSessionName() makes them
// so), they are what the sidebar shows, and keeping them here means a session
// needs no second table — which is what lets the whole store be created by the
// single DDL statement ScopedFsStore::open() runs.
const QString kCreateTableDdl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS history ("
        "session TEXT NOT NULL, "
        "position INTEGER NOT NULL, "
        "location TEXT NOT NULL, "
        "duration_seconds INTEGER, "
        "played_at TEXT, "
        "PRIMARY KEY (session, position))");

/// Open the store on `mountRoot` for reading. Fails without touching the drive
/// when it carries no history at all.
bool openForRead(const QString& mountRoot, ScopedFsStore* pStore, FsStoreTarget* pTarget) {
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, pTarget)) {
        return false;
    }
    return pStore->open(*pTarget, QString());
}

/// Open the store on `mountRoot` for writing, creating it as needed.
bool openForWrite(const QString& mountRoot, ScopedFsStore* pStore, FsStoreTarget* pTarget) {
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, pTarget)) {
        return false;
    }
    if (!pTarget->writable) {
        // A write-protected stick is an expected case, not a failure worth
        // logging on every track change.
        return false;
    }
    return pStore->open(*pTarget, kCreateTableDdl);
}

} // anonymous namespace

// static
bool FsHistoryStore::readSessions(
        const QString& mountRoot, QList<FsHistorySession>* pSessions) {
    FsStoreTarget target;
    ScopedFsStore store(kLogTag);
    if (!openForRead(mountRoot, &store, &target)) {
        return false;
    }

    QSqlQuery query(store.database());
    if (!query.exec(QStringLiteral(
                "SELECT session, MIN(played_at), COUNT(*), "
                "  COALESCE(SUM(duration_seconds), 0) "
                "FROM history GROUP BY session "
                "ORDER BY MIN(played_at) DESC, session DESC"))) {
        // Also the path taken by a store written by a future schema.
        qDebug() << kLogTag << ": cannot read sessions on" << mountRoot
                 << query.lastError().text();
        return false;
    }

    QList<FsHistorySession> sessions;
    while (query.next()) {
        FsHistorySession session;
        session.name = query.value(0).toString();
        session.startedAt = QDateTime::fromString(
                query.value(1).toString(), Qt::ISODate);
        session.trackCount = query.value(2).toInt();
        session.durationSeconds = query.value(3).toInt();
        sessions.append(std::move(session));
    }
    if (sessions.isEmpty()) {
        return false;
    }
    *pSessions = std::move(sessions);
    return true;
}

// static
bool FsHistoryStore::readSessionTracks(const QString& mountRoot,
        const QString& sessionName,
        QStringList* pLocations) {
    FsStoreTarget target;
    ScopedFsStore store(kLogTag);
    if (!openForRead(mountRoot, &store, &target)) {
        return false;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "SELECT location FROM history "
            "WHERE session = :session ORDER BY position"));
    query.bindValue(QStringLiteral(":session"), sessionName);
    if (!query.exec()) {
        qDebug() << kLogTag << ": cannot read session" << sessionName << "on"
                 << mountRoot << query.lastError().text();
        return false;
    }

    const QDir rootDir(target.rootPath);
    QStringList locations;
    while (query.next()) {
        const QString stored = query.value(0).toString();
        if (stored.isEmpty()) {
            continue;
        }
        locations.append(QDir::isAbsolutePath(stored)
                        ? QDir::cleanPath(stored)
                        : rootDir.absoluteFilePath(stored));
    }
    if (locations.isEmpty()) {
        return false;
    }
    *pLocations = std::move(locations);
    return true;
}

// static
bool FsHistoryStore::readSessionSummary(const QString& mountRoot,
        const QString& sessionName,
        FsHistorySession* pSession) {
    FsStoreTarget target;
    ScopedFsStore store(kLogTag);
    if (!openForRead(mountRoot, &store, &target)) {
        return false;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "SELECT MIN(played_at), COUNT(*), COALESCE(SUM(duration_seconds), 0) "
            "FROM history WHERE session = :session"));
    query.bindValue(QStringLiteral(":session"), sessionName);
    if (!query.exec() || !query.next()) {
        return false;
    }
    const int trackCount = query.value(1).toInt();
    if (trackCount <= 0) {
        return false;
    }
    pSession->name = sessionName;
    pSession->startedAt = QDateTime::fromString(query.value(0).toString(), Qt::ISODate);
    pSession->trackCount = trackCount;
    pSession->durationSeconds = query.value(2).toInt();
    return true;
}

// static
QString FsHistoryStore::newSessionName(const QString& mountRoot) {
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, &target)) {
        return QString();
    }
    if (!target.writable) {
        return QString();
    }

    const QString today = QDate::currentDate().toString(Qt::ISODate);

    // Reading is done without creating anything: a drive that has never been
    // played from gets its database on the first append, not on the first
    // track that merely might be logged to it.
    ScopedFsStore store(kLogTag);
    if (!store.open(target, QString())) {
        return today;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "SELECT DISTINCT session FROM history WHERE session LIKE :prefix"));
    query.bindValue(QStringLiteral(":prefix"), QString(today + QStringLiteral("%")));
    if (!query.exec()) {
        qDebug() << kLogTag << ": cannot list today's sessions on" << mountRoot
                 << query.lastError().text();
        return today;
    }
    QStringList taken;
    while (query.next()) {
        taken.append(query.value(0).toString());
    }

    QString name = today;
    int suffix = 1;
    while (taken.contains(name)) {
        name = today + QStringLiteral(" #%1").arg(++suffix);
    }
    return name;
}

// static
bool FsHistoryStore::appendTrack(const QString& mountRoot,
        const QString& sessionName,
        const QString& trackLocation,
        int durationSeconds) {
    if (sessionName.isEmpty()) {
        return false;
    }
    FsStoreTarget target;
    ScopedFsStore store(kLogTag);
    if (!openForWrite(mountRoot, &store, &target)) {
        return false;
    }

    const QDir rootDir(target.rootPath);
    const QString absPath = QFileInfo(trackLocation).absoluteFilePath();
    const QString relPath = rootDir.relativeFilePath(absPath);
    if (relPath.isEmpty() || relPath.startsWith(QLatin1String("..")) ||
            QDir::isAbsolutePath(relPath)) {
        // Not a track on this drive. Storing it absolutely would write a path
        // that means nothing on the next unit, so the caller picked the wrong
        // drive and there is nothing useful to record.
        qWarning() << kLogTag << ":" << absPath << "is not on" << target.rootPath;
        return false;
    }

    int position = 1;
    QSqlQuery maxQuery(store.database());
    maxQuery.prepare(QStringLiteral(
            "SELECT COALESCE(MAX(position), 0) FROM history WHERE session = :session"));
    maxQuery.bindValue(QStringLiteral(":session"), sessionName);
    if (maxQuery.exec() && maxQuery.next()) {
        position = maxQuery.value(0).toInt() + 1;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "INSERT INTO history "
            "(session, position, location, duration_seconds, played_at) "
            "VALUES (:session, :position, :location, :duration, :played_at)"));
    query.bindValue(QStringLiteral(":session"), sessionName);
    query.bindValue(QStringLiteral(":position"), position);
    query.bindValue(QStringLiteral(":location"), relPath);
    query.bindValue(QStringLiteral(":duration"), durationSeconds);
    query.bindValue(QStringLiteral(":played_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!query.exec()) {
        qWarning() << kLogTag << ": cannot log" << relPath << "to session"
                   << sessionName << "on" << mountRoot << query.lastError().text();
        return false;
    }
    return true;
}

// static
bool FsHistoryStore::deleteSession(const QString& mountRoot, const QString& sessionName) {
    FsStoreTarget target;
    ScopedFsStore store(kLogTag);
    if (!openForWrite(mountRoot, &store, &target)) {
        return false;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral("DELETE FROM history WHERE session = :session"));
    query.bindValue(QStringLiteral(":session"), sessionName);
    if (!query.exec()) {
        qWarning() << kLogTag << ": cannot delete session" << sessionName << "on"
                   << mountRoot << query.lastError().text();
        return false;
    }
    qInfo() << kLogTag << ": deleted session" << sessionName << "on" << mountRoot;
    return true;
}

// static
bool FsHistoryStore::clearFilesystemHistory(const QString& mountPoint) {
    return fsStoreRemove(mountPoint, kStoreDbName, kLogTag);
}
