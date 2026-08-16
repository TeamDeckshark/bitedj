#include "library/dao/fssamplerbankstore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSqlError>
#include <QSqlQuery>

#include "library/dao/fsstore.h"

namespace {

// Per-filesystem layout, alongside the analysis cache and the cue overrides in
// the same .bitedj dir.
const QString kStoreDbName = QStringLiteral("samplers.sqlite");
const char kLogTag[] = "FsSamplerBankStore";

// Payload format version, so a future change to the stored fields can be told
// apart from the current one instead of being misread.
constexpr int kPayloadVersion = 1;

const QString kCreateTableDdl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sampler_banks ("
        "bank INTEGER PRIMARY KEY NOT NULL, "
        "version INTEGER NOT NULL, "
        "updated_at TEXT, "
        "slots TEXT NOT NULL)");

} // anonymous namespace

// static
QByteArray FsSamplerBankStore::serializeBank(
        const QString& mountRoot, const QStringList& locations) {
    const QDir rootDir(mountRoot);
    QJsonArray array;
    for (const QString& location : locations) {
        if (location.isEmpty()) {
            array.append(QString());
            continue;
        }
        const QString absPath = QFileInfo(location).absoluteFilePath();
        // A sample that lives on the same stick is stored relative to its root
        // so the bank still resolves when the drive is mounted elsewhere, or on
        // another unit. One that does not (a track on the boot volume, or on a
        // second stick) can only be named absolutely; it simply fails to load
        // where that path does not exist, which is the honest outcome.
        const QString relPath = rootDir.relativeFilePath(absPath);
        const bool onThisDrive = !relPath.isEmpty() &&
                !relPath.startsWith(QLatin1String("..")) &&
                !QDir::isAbsolutePath(relPath);
        array.append(onThisDrive ? relPath : absPath);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

// static
QStringList FsSamplerBankStore::parseBank(
        const QString& mountRoot, const QByteArray& payload, int slotCount) {
    QStringList locations;
    locations.reserve(slotCount);

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        qWarning() << kLogTag << ": ignoring unreadable sampler bank on" << mountRoot
                   << parseError.errorString();
        return QStringList(slotCount, QString());
    }

    const QDir rootDir(mountRoot);
    const QJsonArray array = document.array();
    for (const QJsonValue& value : array) {
        if (locations.size() >= slotCount) {
            // A bank stored by a build with a bigger grid: keep the slots this
            // one has room for rather than refusing the whole bank.
            break;
        }
        const QString stored = value.toString();
        if (stored.isEmpty()) {
            locations.append(QString());
        } else if (QDir::isAbsolutePath(stored)) {
            locations.append(QDir::cleanPath(stored));
        } else {
            locations.append(rootDir.absoluteFilePath(stored));
        }
    }
    while (locations.size() < slotCount) {
        locations.append(QString());
    }
    return locations;
}

// static
bool FsSamplerBankStore::readBank(const QString& mountRoot,
        int bankIndex,
        int slotCount,
        QStringList* pLocations) {
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, &target)) {
        return false;
    }

    ScopedFsStore store(kLogTag);
    if (!store.open(target, QString())) {
        // No banks on this drive (the common case for a stick that has never
        // been used here), or a store that could not be opened.
        return false;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "SELECT slots FROM sampler_banks "
            "WHERE bank = :bank AND version = :version"));
    query.bindValue(QStringLiteral(":bank"), bankIndex);
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    if (!query.exec()) {
        // Also the path taken by a store written by a future schema.
        qDebug() << kLogTag << ": cannot read bank" << bankIndex << "on" << mountRoot
                 << query.lastError().text();
        return false;
    }
    if (!query.next()) {
        return false;
    }
    *pLocations = parseBank(
            target.rootPath, query.value(0).toString().toUtf8(), slotCount);
    return true;
}

// static
bool FsSamplerBankStore::writeBank(const QString& mountRoot,
        int bankIndex,
        const QStringList& locations) {
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, &target)) {
        return false;
    }
    if (!target.writable) {
        // A write-protected stick is an expected case, not a failure worth
        // logging every time a slot changes.
        return false;
    }

    ScopedFsStore store(kLogTag);
    if (!store.open(target, kCreateTableDdl)) {
        return false;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO sampler_banks "
            "(bank, version, updated_at, slots) "
            "VALUES (:bank, :version, :updated_at, :slots)"));
    query.bindValue(QStringLiteral(":bank"), bankIndex);
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    query.bindValue(QStringLiteral(":updated_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":slots"),
            QString::fromUtf8(serializeBank(target.rootPath, locations)));
    if (!query.exec()) {
        qWarning() << kLogTag << ": cannot save bank" << bankIndex << "on" << mountRoot
                   << query.lastError().text();
        return false;
    }
    qDebug() << kLogTag << ": saved sampler bank" << bankIndex << "on" << mountRoot;
    return true;
}

// static
bool FsSamplerBankStore::clearFilesystemBanks(const QString& mountPoint) {
    return fsStoreRemove(mountPoint, kStoreDbName, kLogTag);
}
