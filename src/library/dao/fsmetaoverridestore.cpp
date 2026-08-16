#include "library/dao/fsmetaoverridestore.h"

#include <QDateTime>
#include <QDir>
#include <QMutexLocker>
#include <QSqlError>
#include <QSqlQuery>

#include "library/dao/fsstore.h"
#include "moc_fsmetaoverridestore.cpp"
#include "preferences/systemsettings.h"
#include "track/track.h"
#include "track/trackrecord.h"
#include "util/assert.h"

namespace {

// Per-filesystem layout, alongside the analysis cache and the cue overrides in
// the same .bitedj dir.
const QString kStoreDbName = QStringLiteral("meta.sqlite");
const char kLogTag[] = "FsMetaOverrideStore";

const QString kCreateTableDdl = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS meta_overrides ("
        "relpath TEXT PRIMARY KEY NOT NULL, "
        "version INTEGER NOT NULL, "
        "updated_at TEXT, "
        "rating INTEGER NOT NULL)");

// Payload format version, so a future change to the stored fields (a comment, a
// track colour) can be told apart from the current one instead of being misread.
constexpr int kPayloadVersion = 1;

// Baseline marker for a track whose override was cleared while it stayed
// loaded. Outside the valid rating range, so it can never equal a real one.
constexpr int kSuppressedBaseline = -1;

bool isStorableRating(int rating) {
    return mixxx::TrackRecord::isValidRating(rating);
}

} // anonymous namespace

// static
FsMetaOverrideNotifier& FsMetaOverrideNotifier::instance() {
    static FsMetaOverrideNotifier notifier;
    return notifier;
}

QMutex FsMetaOverrideStore::s_baselineMutex;
QHash<QString, int> FsMetaOverrideStore::s_baselines;
QHash<QString, int> FsMetaOverrideStore::s_importedRatings;

int FsMetaOverrideStore::MountRatings::ratingFor(
        const QString& trackLocation, int fallback) const {
    if (rootPath.isEmpty() || byRelPath.isEmpty()) {
        return fallback;
    }
    const auto it = byRelPath.constFind(QDir(rootPath).relativeFilePath(trackLocation));
    return it == byRelPath.constEnd() ? fallback : *it;
}

// static
void FsMetaOverrideStore::applyOverrides(Track* pTrack) {
    VERIFY_OR_DEBUG_ASSERT(pTrack) {
        return;
    }
    const QString location = pTrack->getLocation();
    if (location.isEmpty() || !SystemSettings::isOnRemovableMedia(location)) {
        // A track on the internal drive keeps its rating in the library, where
        // it was already; there is no drive for it to follow.
        return;
    }

    int rating = mixxx::TrackRecord::kNoRating;
    bool found = false;
    // What the source library exported for this track, captured before the
    // override is laid over it. Settings → Clear puts exactly this back, so
    // clearing takes off the DJ's own edit and nothing else.
    const int imported = pTrack->getRating();
    bool hasImported = false;
    if (readOverride(location, &rating, &found) && found && isStorableRating(rating)) {
        hasImported = true;
        pTrack->setRating(rating);
    }

    // Baseline what the track carries now, override applied or not, so that
    // only an edit made from here on counts as a change worth storing.
    const int baseline = pTrack->getRating();
    QMutexLocker locker(&s_baselineMutex);
    s_baselines.insert(location, baseline);
    // Only the first override this track gets: a track is applied to more than
    // once (the database load, then every load out of a Rekordbox playlist),
    // and by the second time the rating on it is this unit's own.
    if (hasImported && !s_importedRatings.contains(location)) {
        s_importedRatings.insert(location, imported);
    }
}

// static
void FsMetaOverrideStore::flushIfChanged(const Track& track) {
    const QString location = track.getLocation();
    if (location.isEmpty() || !SystemSettings::isOnRemovableMedia(location)) {
        return;
    }
    const int rating = track.getRating();
    if (!isStorableRating(rating)) {
        return;
    }

    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_baselines.constFind(location);
        if (it == s_baselines.constEnd()) {
            // Never seen by applyOverrides(): treat the baseline as unrated, so
            // a track first rated in this session is stored, but one that
            // carries no stars at all does not get an entry of its own.
            if (rating == mixxx::TrackRecord::kNoRating) {
                return;
            }
        } else if (*it == kSuppressedBaseline || *it == rating) {
            return;
        }
    }

    if (!writeOverride(location, rating)) {
        return;
    }
    {
        QMutexLocker locker(&s_baselineMutex);
        s_baselines.insert(location, rating);
    }
    emit FsMetaOverrideNotifier::instance().ratingStored(location, rating);
}

// static
bool FsMetaOverrideStore::storeRating(const QString& trackLocation, int rating) {
    if (trackLocation.isEmpty() || !isStorableRating(rating) ||
            !SystemSettings::isOnRemovableMedia(trackLocation)) {
        return false;
    }
    if (!writeOverride(trackLocation, rating)) {
        return false;
    }
    {
        QMutexLocker locker(&s_baselineMutex);
        // The track may be loaded in a deck; baseline it at what the drive now
        // holds so its next save does not write the same rating again.
        s_baselines.insert(trackLocation, rating);
    }
    emit FsMetaOverrideNotifier::instance().ratingStored(trackLocation, rating);
    return true;
}

// static
void FsMetaOverrideStore::noteImportedRating(const QString& trackLocation, int rating) {
    if (trackLocation.isEmpty() || !isStorableRating(rating) ||
            !SystemSettings::isOnRemovableMedia(trackLocation)) {
        return;
    }
    QMutexLocker locker(&s_baselineMutex);
    // Only the first one: a second edit would record this unit's own override
    // as the thing to restore.
    if (!s_importedRatings.contains(trackLocation)) {
        s_importedRatings.insert(trackLocation, rating);
    }
}

// static
FsMetaOverrideStore::MountRatings FsMetaOverrideStore::readMountRatings(
        const QString& mountRoot) {
    MountRatings ratings;
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForMount(mountRoot, kStoreDbName, &target)) {
        return ratings;
    }

    ScopedFsStore store(kLogTag);
    if (!store.open(target, QString())) {
        // No store on this drive, the common case for a stick nothing has been
        // rated on.
        return ratings;
    }

    QSqlQuery query(store.database());
    query.setForwardOnly(true);
    query.prepare(QStringLiteral(
            "SELECT relpath, rating FROM meta_overrides WHERE version = :version"));
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    if (!query.exec()) {
        // Also the path taken by a store written by a future schema.
        qDebug() << kLogTag << ": cannot read overrides on" << target.rootPath
                 << query.lastError().text();
        return ratings;
    }
    while (query.next()) {
        const int rating = query.value(1).toInt();
        if (isStorableRating(rating)) {
            ratings.byRelPath.insert(query.value(0).toString(), rating);
        }
    }
    ratings.rootPath = target.rootPath;
    return ratings;
}

// static
bool FsMetaOverrideStore::readOverride(
        const QString& trackLocation, int* pRating, bool* pFound) {
    *pFound = false;
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForFile(trackLocation, kStoreDbName, &target)) {
        return false;
    }

    ScopedFsStore store(kLogTag);
    if (!store.open(target, QString())) {
        // No store on this drive (the common case for a track that has never
        // been rated here), or one that could not be opened.
        return true;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "SELECT rating FROM meta_overrides "
            "WHERE relpath = :relpath AND version = :version"));
    query.bindValue(QStringLiteral(":relpath"), target.relPath);
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    if (!query.exec()) {
        qDebug() << kLogTag << ": cannot read override for" << target.relPath
                 << query.lastError().text();
        return true;
    }
    if (query.next()) {
        *pRating = query.value(0).toInt();
        *pFound = true;
    }
    return true;
}

// static
bool FsMetaOverrideStore::writeOverride(const QString& trackLocation, int rating) {
    FsStoreTarget target;
    if (!FsStoreTarget::resolveForFile(trackLocation, kStoreDbName, &target)) {
        return false;
    }
    if (!target.writable) {
        // A write-protected stick is an expected case, not a failure worth
        // logging on every save.
        return false;
    }

    ScopedFsStore store(kLogTag);
    if (!store.open(target, kCreateTableDdl)) {
        return false;
    }

    QSqlQuery query(store.database());
    query.prepare(QStringLiteral(
            "INSERT OR REPLACE INTO meta_overrides "
            "(relpath, version, updated_at, rating) "
            "VALUES (:relpath, :version, :updated_at, :rating)"));
    query.bindValue(QStringLiteral(":relpath"), target.relPath);
    query.bindValue(QStringLiteral(":version"), kPayloadVersion);
    query.bindValue(QStringLiteral(":updated_at"),
            QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":rating"), rating);
    if (!query.exec()) {
        qWarning() << kLogTag << ": cannot save rating for" << target.relPath
                   << query.lastError().text();
        return false;
    }
    qDebug() << kLogTag << ": saved rating" << rating << "for" << target.relPath;
    return true;
}

// static
bool FsMetaOverrideStore::clearFilesystemOverrides(const QString& mountPoint) {
    return fsStoreRemove(mountPoint, kStoreDbName, kLogTag);
}

// static
void FsMetaOverrideStore::suppressPendingSaves() {
    QMutexLocker locker(&s_baselineMutex);
    for (auto it = s_baselines.begin(); it != s_baselines.end(); ++it) {
        *it = kSuppressedBaseline;
    }
}

// static
QStringList FsMetaOverrideStore::overriddenLocations() {
    QMutexLocker locker(&s_baselineMutex);
    return s_importedRatings.keys();
}

// static
bool FsMetaOverrideStore::restoreImportedRating(Track* pTrack) {
    VERIFY_OR_DEBUG_ASSERT(pTrack) {
        return false;
    }
    int imported = mixxx::TrackRecord::kNoRating;
    {
        QMutexLocker locker(&s_baselineMutex);
        const auto it = s_importedRatings.constFind(pTrack->getLocation());
        if (it == s_importedRatings.constEnd()) {
            // No override was ever applied to this track, so it carries none of
            // this unit's ratings and there is nothing for a clear to take off.
            return false;
        }
        imported = *it;
    }
    // An unrated import is meaningful here too: the source library exported no
    // stars at all, so every one on the track was put there on this unit.
    pTrack->setRating(imported);
    return true;
}
