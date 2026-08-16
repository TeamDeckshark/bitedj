#include "library/dao/fsmetaoverridestore.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QStorageInfo>

#include "test/mixxxtest.h"
#include "track/track.h"

namespace {

// The store only writes to drives the DJ can pull out, so every case here needs
// a filesystem mounted under one of the removable roots. Without root,
// `unshare` provides one:
//
//   unshare -Umr --propagation private sh -c \
//     'mount -t tmpfs tmpfs /mnt && mkdir -p /mnt/usbtest && \
//      mount -t tmpfs tmpfs /mnt/usbtest && \
//      QT_QPA_PLATFORM=offscreen ./mixxx-test \
//        --gtest_filter="FsMetaOverrideStoreTest.*"'
//
// The tests skip themselves when that mount is not there, so an ordinary run of
// the suite still passes.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

/// Covers the per-drive rating store: what it writes, when it declines to
/// write, what a fresh load gets back and what the settings action leaves
/// behind.
class FsMetaOverrideStoreTest : public MixxxTest {
  protected:
    static bool usbIsMounted() {
        // An unmounted path reports itself as its own root, so validity is what
        // tells a real mount from a missing one.
        const QStorageInfo usb(kFakeUsb);
        return usb.isValid() && usb.isReady() && usb.rootPath() == kFakeUsb;
    }

    /// A track file on the pretend drive, on a store cleared of earlier runs.
    QString prepareUsbTrack(const QString& fileName) {
        const QString trackPath = kFakeUsb + QStringLiteral("/") + fileName;
        QFile::remove(trackPath);
        EXPECT_TRUE(FsMetaOverrideStore::clearFilesystemOverrides(kFakeUsb));
        EXPECT_TRUE(QFile::copy(
                getTestDir().filePath(QStringLiteral("sine-30.wav")), trackPath));
        return trackPath;
    }

    static TrackPointer trackAt(const QString& trackPath) {
        return Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(trackPath)));
    }

    static QString storePath() {
        return kFakeUsb + QStringLiteral("/.bitedj/meta.sqlite");
    }
};

// The whole cycle: a first load that writes nothing, a rating that mirrors
// itself to the stick, and a later load — with a different rating imported from
// the source library — that gets the stored one back.
TEST_F(FsMetaOverrideStoreTest, WritesAndReadsBackFromDrive) {
    if (!usbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    const QString trackPath = prepareUsbTrack(QStringLiteral("rated.wav"));

    // Nothing stored yet: the load takes a baseline and writes nothing.
    const TrackPointer pFirst = trackAt(trackPath);
    FsMetaOverrideStore::applyOverrides(pFirst.get());
    EXPECT_EQ(0, pFirst->getRating());
    FsMetaOverrideStore::flushIfChanged(*pFirst);
    EXPECT_FALSE(QFile::exists(storePath()));

    // The DJ rates it; the save mirrors that to the stick.
    pFirst->setRating(4);
    FsMetaOverrideStore::flushIfChanged(*pFirst);
    ASSERT_TRUE(QFile::exists(storePath()));

    // Next load, with the source library's own rating already on the track:
    // the stored one wins.
    const TrackPointer pSecond = trackAt(trackPath);
    pSecond->setRating(2);
    FsMetaOverrideStore::applyOverrides(pSecond.get());
    EXPECT_EQ(4, pSecond->getRating());

    // ...and is baselined, so a load on its own never writes again.
    QFile::remove(storePath());
    FsMetaOverrideStore::flushIfChanged(*pSecond);
    EXPECT_FALSE(QFile::exists(storePath()));
}

// Taking the stars off is an override in its own right, not an absent one: the
// track has to come back unrated rather than with the rating its source library
// exported.
TEST_F(FsMetaOverrideStoreTest, ClearedRatingIsStoredRatherThanForgotten) {
    if (!usbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    const QString trackPath = prepareUsbTrack(QStringLiteral("unrated.wav"));

    const TrackPointer pRated = trackAt(trackPath);
    FsMetaOverrideStore::applyOverrides(pRated.get());
    pRated->setRating(5);
    FsMetaOverrideStore::flushIfChanged(*pRated);
    ASSERT_TRUE(QFile::exists(storePath()));

    // The DJ takes the stars off again.
    pRated->setRating(0);
    FsMetaOverrideStore::flushIfChanged(*pRated);

    const TrackPointer pReloaded = trackAt(trackPath);
    pReloaded->setRating(3);
    FsMetaOverrideStore::applyOverrides(pReloaded.get());
    EXPECT_EQ(0, pReloaded->getRating());
}

// What the Rekordbox scan asks for: every rating on one drive in a single pass,
// keyed by the path the track sits at inside it.
TEST_F(FsMetaOverrideStoreTest, ReadsEveryRatingOnTheDriveAtOnce) {
    if (!usbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    const QString firstPath = prepareUsbTrack(QStringLiteral("one.wav"));
    const QString secondPath = kFakeUsb + QStringLiteral("/two.wav");
    QFile::remove(secondPath);
    ASSERT_TRUE(QFile::copy(
            getTestDir().filePath(QStringLiteral("sine-30.wav")), secondPath));

    // A drive nothing has been rated on reads as "no opinion", so every rating
    // the device exported is left exactly as it is.
    FsMetaOverrideStore::MountRatings ratings =
            FsMetaOverrideStore::readMountRatings(kFakeUsb);
    EXPECT_TRUE(ratings.isEmpty());
    EXPECT_EQ(3, ratings.ratingFor(firstPath, 3));

    ASSERT_TRUE(FsMetaOverrideStore::storeRating(firstPath, 5));
    ASSERT_TRUE(FsMetaOverrideStore::storeRating(secondPath, 1));

    ratings = FsMetaOverrideStore::readMountRatings(kFakeUsb);
    EXPECT_EQ(2, ratings.byRelPath.size());
    EXPECT_EQ(5, ratings.ratingFor(firstPath, 3));
    EXPECT_EQ(1, ratings.ratingFor(secondPath, 3));
    // A track on the drive that was never rated here keeps what it came with.
    EXPECT_EQ(3,
            ratings.ratingFor(kFakeUsb + QStringLiteral("/untouched.wav"), 3));

    QFile::remove(secondPath);
    ASSERT_TRUE(FsMetaOverrideStore::clearFilesystemOverrides(kFakeUsb));
}

// What Settings → Clear → Meta does to a track that is still in a deck: the
// rating its source library exported is put back, not blanked, and the track
// must not write its override out again on the way past.
TEST_F(FsMetaOverrideStoreTest, ClearRestoresTheImportedRating) {
    if (!usbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    const QString trackPath = prepareUsbTrack(QStringLiteral("imported.wav"));

    // An earlier session stored the DJ's rating on the drive.
    ASSERT_TRUE(FsMetaOverrideStore::storeRating(trackPath, 5));

    // This session: the track loads with the rating its source library gives
    // it, and the override goes on top.
    const TrackPointer pLoaded = trackAt(trackPath);
    pLoaded->setRating(2);
    FsMetaOverrideStore::applyOverrides(pLoaded.get());
    ASSERT_EQ(5, pLoaded->getRating());
    EXPECT_TRUE(FsMetaOverrideStore::overriddenLocations().contains(trackPath));

    EXPECT_TRUE(FsMetaOverrideStore::clearFilesystemOverrides(kFakeUsb));
    EXPECT_FALSE(QFile::exists(storePath()));
    FsMetaOverrideStore::suppressPendingSaves();
    ASSERT_TRUE(FsMetaOverrideStore::restoreImportedRating(pLoaded.get()));
    EXPECT_EQ(2, pLoaded->getRating());

    // The track is still loaded; its save must not re-create what was cleared.
    FsMetaOverrideStore::flushIfChanged(*pLoaded);
    EXPECT_FALSE(QFile::exists(storePath()));
}

// A track the drive held no override for carries none of this unit's ratings,
// so clearing has nothing to take off it and must leave it alone.
TEST_F(FsMetaOverrideStoreTest, RestoreIsANoOpForATrackThatNeverHadAnOverride) {
    if (!usbIsMounted()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    const QString trackPath = prepareUsbTrack(QStringLiteral("untouched.wav"));

    const TrackPointer pTrack = trackAt(trackPath);
    pTrack->setRating(3);
    FsMetaOverrideStore::applyOverrides(pTrack.get());

    EXPECT_FALSE(FsMetaOverrideStore::restoreImportedRating(pTrack.get()));
    EXPECT_EQ(3, pTrack->getRating());
}

// A track that is not on removable media must never get a store of its own.
TEST_F(FsMetaOverrideStoreTest, IgnoresTracksOnTheBootVolume) {
    const TrackPointer pTrack = Track::newTemporary(mixxx::FileAccess(
            mixxx::FileInfo(getTestDir().filePath(QStringLiteral("sine-30.wav")))));
    pTrack->setRating(4);

    FsMetaOverrideStore::applyOverrides(pTrack.get());
    FsMetaOverrideStore::flushIfChanged(*pTrack);
    EXPECT_FALSE(FsMetaOverrideStore::storeRating(pTrack->getLocation(), 4));

    const QStorageInfo storage(pTrack->getLocation());
    EXPECT_FALSE(QFile::exists(storage.rootPath() + QStringLiteral("/.bitedj/meta.sqlite")));
}

} // namespace
