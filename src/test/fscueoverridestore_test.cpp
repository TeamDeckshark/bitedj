#include "library/dao/fscueoverridestore.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QStorageInfo>
#include <algorithm>

#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/cueinfo.h"
#include "track/track.h"

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);

// The store itself only writes to drives the DJ can pull out, so its database
// half needs a filesystem mounted under one of the removable roots. Without
// root, `unshare` provides one:
//
//   unshare -Umr --propagation private sh -c \
//     'mount -t tmpfs tmpfs /mnt && mkdir -p /mnt/usbtest && \
//      mount -t tmpfs tmpfs /mnt/usbtest && \
//      QT_QPA_PLATFORM=offscreen ./mixxx-test \
//        --gtest_filter="FsCueOverrideStoreTest.*"'
//
// The test skips itself when that mount is not there, so an ordinary run of
// the suite still covers everything above.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

/// Covers the payload codec the per-drive cue store round-trips through —
/// which cues follow the stick, and how they land back on a track — plus the
/// database half, for which the last two cases need a removable mount.
class FsCueOverrideStoreTest : public MixxxTest {
  protected:
    TrackPointer createTrack() {
        const auto pTrack = Track::newTemporary(mixxx::FileAccess(
                mixxx::FileInfo(getTestDir().filePath(QStringLiteral("sine-30.wav")))));
        pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(2),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        return pTrack;
    }

    static mixxx::audio::FramePos framesForSeconds(double seconds) {
        return mixxx::audio::FramePos(seconds * kSampleRate);
    }

    static CuePointer findHotcue(const TrackPointer& pTrack, int hotcueIndex) {
        const QList<CuePointer> cues = pTrack->getCuePoints();
        for (const CuePointer& pCue : cues) {
            if (pCue->getHotCue() == hotcueIndex) {
                return pCue;
            }
        }
        return CuePointer();
    }

    static QList<int> hotcueIndices(const TrackPointer& pTrack) {
        QList<int> indices;
        const QList<CuePointer> cues = pTrack->getCuePoints();
        for (const CuePointer& pCue : cues) {
            if (pCue->getHotCue() != Cue::kNoHotCue) {
                indices << pCue->getHotCue();
            }
        }
        std::sort(indices.begin(), indices.end());
        return indices;
    }
};

// What the DJ set on the pads comes back on the next load: both banks, saved
// loops with their range, labels, colours and the main cue.
TEST_F(FsCueOverrideStoreTest, RoundTripsBothBanksAndMainCue) {
    const TrackPointer pSaved = createTrack();
    pSaved->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            framesForSeconds(12.5),
            mixxx::audio::kInvalidFramePos,
            mixxx::RgbColor(0xff0000));
    findHotcue(pSaved, mixxx::kHotCueBankStart)->setLabel(QStringLiteral("drop"));
    pSaved->createAndAddCue(mixxx::CueType::Loop,
            mixxx::kHotCueBankStart + 3,
            framesForSeconds(30.0),
            framesForSeconds(34.0),
            mixxx::RgbColor(0x00ff00));
    pSaved->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kMemoryCueBankStart + 1,
            framesForSeconds(64.0),
            mixxx::audio::kInvalidFramePos,
            mixxx::RgbColor(0x0000ff));
    pSaved->setMainCuePosition(framesForSeconds(4.0));

    const QByteArray payload = FsCueOverrideStore::serializeCues(*pSaved);

    const TrackPointer pLoaded = createTrack();
    FsCueOverrideStore::applyPayload(pLoaded.get(), payload);

    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart,
                      mixxx::kHotCueBankStart + 3,
                      mixxx::kMemoryCueBankStart + 1}),
            hotcueIndices(pLoaded));

    const CuePointer pHotCue = findHotcue(pLoaded, mixxx::kHotCueBankStart);
    ASSERT_TRUE(pHotCue);
    EXPECT_EQ(mixxx::CueType::HotCue, pHotCue->getType());
    EXPECT_EQ(framesForSeconds(12.5), pHotCue->getPosition());
    EXPECT_EQ(QStringLiteral("drop"), pHotCue->getLabel());
    EXPECT_EQ(mixxx::RgbColor(0xff0000), pHotCue->getColor());

    const CuePointer pLoop = findHotcue(pLoaded, mixxx::kHotCueBankStart + 3);
    ASSERT_TRUE(pLoop);
    EXPECT_EQ(mixxx::CueType::Loop, pLoop->getType());
    EXPECT_EQ(framesForSeconds(30.0), pLoop->getPosition());
    EXPECT_EQ(framesForSeconds(34.0), pLoop->getEndPosition());

    const CuePointer pMemoryCue = findHotcue(pLoaded, mixxx::kMemoryCueBankStart + 1);
    ASSERT_TRUE(pMemoryCue);
    EXPECT_EQ(framesForSeconds(64.0), pMemoryCue->getPosition());

    EXPECT_EQ(framesForSeconds(4.0), pLoaded->getMainCuePosition());
}

// The whole point of the store: the cues on the drive replace the ones
// rekordbox exported, including the slots rekordbox filled and the DJ cleared.
TEST_F(FsCueOverrideStoreTest, OverrideReplacesImportedCues) {
    const TrackPointer pSaved = createTrack();
    pSaved->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            framesForSeconds(20.0),
            mixxx::audio::kInvalidFramePos);
    const QByteArray payload = FsCueOverrideStore::serializeCues(*pSaved);

    // A track as the rekordbox import leaves it: a cue in the same slot at a
    // different position, plus two slots the override does not know about.
    const TrackPointer pImported = createTrack();
    pImported->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            framesForSeconds(1.0),
            mixxx::audio::kInvalidFramePos);
    pImported->createAndAddCue(mixxx::CueType::Loop,
            mixxx::kHotCueBankStart + 1,
            framesForSeconds(2.0),
            framesForSeconds(3.0));
    pImported->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kMemoryCueBankStart,
            framesForSeconds(5.0),
            mixxx::audio::kInvalidFramePos);

    FsCueOverrideStore::applyPayload(pImported.get(), payload);

    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart}), hotcueIndices(pImported));
    EXPECT_EQ(framesForSeconds(20.0),
            findHotcue(pImported, mixxx::kHotCueBankStart)->getPosition());
}

// A track loaded by the rekordbox model is the same Track a deck may already
// be playing, so a slot that survives the override has to be updated in place
// rather than recreated — otherwise the deck's pad goes dead mid-set.
TEST_F(FsCueOverrideStoreTest, SurvivingSlotKeepsItsCueObject) {
    const TrackPointer pSaved = createTrack();
    pSaved->createAndAddCue(mixxx::CueType::Loop,
            mixxx::kHotCueBankStart,
            framesForSeconds(20.0),
            framesForSeconds(24.0));
    const QByteArray payload = FsCueOverrideStore::serializeCues(*pSaved);

    const TrackPointer pPlaying = createTrack();
    pPlaying->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            framesForSeconds(1.0),
            mixxx::audio::kInvalidFramePos);
    const CuePointer pCueBefore = findHotcue(pPlaying, mixxx::kHotCueBankStart);

    FsCueOverrideStore::applyPayload(pPlaying.get(), payload);

    const CuePointer pCueAfter = findHotcue(pPlaying, mixxx::kHotCueBankStart);
    ASSERT_TRUE(pCueAfter);
    EXPECT_EQ(pCueBefore.get(), pCueAfter.get());
    // A pad can change from a plain cue to a saved loop, so the type follows.
    EXPECT_EQ(mixxx::CueType::Loop, pCueAfter->getType());
    EXPECT_EQ(framesForSeconds(24.0), pCueAfter->getEndPosition());
}

// Deleting every cue is an override in its own right, not an absent one: an
// empty payload has to clear the pads instead of leaving rekordbox's in place.
TEST_F(FsCueOverrideStoreTest, EmptyOverrideClearsManagedCues) {
    const TrackPointer pTrack = createTrack();
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            framesForSeconds(1.0),
            mixxx::audio::kInvalidFramePos);
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kMemoryCueBankStart,
            framesForSeconds(2.0),
            mixxx::audio::kInvalidFramePos);

    FsCueOverrideStore::applyPayload(pTrack.get(), QByteArrayLiteral("[]"));

    EXPECT_TRUE(hotcueIndices(pTrack).isEmpty());
}

// Only the cues the DJ can set from the pads follow the drive. Intro/outro and
// the analyzer's own ranges belong to the analysis, which has its own cache.
TEST_F(FsCueOverrideStoreTest, LeavesUnmanagedCuesAlone) {
    const TrackPointer pTrack = createTrack();
    pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            framesForSeconds(0.5),
            framesForSeconds(8.0));
    pTrack->createAndAddCue(mixxx::CueType::N60dBSound,
            Cue::kNoHotCue,
            framesForSeconds(0.1),
            framesForSeconds(179.0));

    EXPECT_EQ(QByteArrayLiteral("[]"), FsCueOverrideStore::serializeCues(*pTrack));

    FsCueOverrideStore::applyPayload(pTrack.get(), QByteArrayLiteral("[]"));

    EXPECT_TRUE(pTrack->findCueByType(mixxx::CueType::Intro));
    EXPECT_TRUE(pTrack->findCueByType(mixxx::CueType::N60dBSound));
}

// A track the drive held no override for carries none of this unit's cues, so
// clearing must leave it completely alone. Getting this wrong is how a Serato
// track loses its markers for good: they are imported from the file's tags once
// and the library database is authoritative from then on.
TEST_F(FsCueOverrideStoreTest, RestoreIsANoOpForATrackThatNeverHadAnOverride) {
    const TrackPointer pTrack = createTrack();
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            framesForSeconds(12.0),
            mixxx::audio::kInvalidFramePos);
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kMemoryCueBankStart + 1,
            framesForSeconds(64.0),
            mixxx::audio::kInvalidFramePos);
    pTrack->setMainCuePosition(framesForSeconds(4.0));
    const QByteArray before = FsCueOverrideStore::serializeCues(*pTrack);

    EXPECT_FALSE(FsCueOverrideStore::restoreImportedCues(pTrack.get()));
    EXPECT_EQ(before, FsCueOverrideStore::serializeCues(*pTrack));
}

// Positions are stored in seconds so the cues land in the same place on a unit
// that decodes the file at a different rate.
TEST_F(FsCueOverrideStoreTest, PositionsAreSampleRateIndependent) {
    const TrackPointer pSaved = createTrack();
    pSaved->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            framesForSeconds(10.0),
            mixxx::audio::kInvalidFramePos);
    const QByteArray payload = FsCueOverrideStore::serializeCues(*pSaved);

    const TrackPointer pLoaded = createTrack();
    constexpr auto kOtherSampleRate = mixxx::audio::SampleRate(48000);
    pLoaded->setAudioProperties(
            mixxx::audio::ChannelCount(2),
            kOtherSampleRate,
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(180));

    FsCueOverrideStore::applyPayload(pLoaded.get(), payload);

    const CuePointer pCue = findHotcue(pLoaded, mixxx::kHotCueBankStart);
    ASSERT_TRUE(pCue);
    EXPECT_DOUBLE_EQ(10.0 * kOtherSampleRate, pCue->getPosition().value());
}

// The whole cycle against a real store on a real (pretend) drive: a first load
// that writes nothing, a save that mirrors the DJ's cues, a reload that puts
// them back over imported ones, and the settings action that clears them.
TEST_F(FsCueOverrideStoreTest, WritesAndReadsBackFromDrive) {
    // An unmounted path reports itself as its own root, so validity is what
    // tells a real mount from a missing one.
    const QStorageInfo usb(kFakeUsb);
    if (!usb.isValid() || !usb.isReady() || usb.rootPath() != kFakeUsb) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    const QString trackPath = kFakeUsb + QStringLiteral("/cued.wav");
    // Start from a clean drive, so a rerun in the same namespace still counts.
    QFile::remove(trackPath);
    ASSERT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
    ASSERT_TRUE(QFile::copy(getTestDir().filePath(QStringLiteral("sine-30.wav")), trackPath));
    const QString dbPath = kFakeUsb + QStringLiteral("/.bitedj/cues.sqlite");

    const auto makeTrack = [&] {
        auto pTrack = Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(trackPath)));
        pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        return pTrack;
    };

    // Nothing stored yet: the load takes a baseline and writes nothing.
    const TrackPointer pFirst = makeTrack();
    FsCueOverrideStore::applyOverrides(pFirst.get());
    EXPECT_FALSE(QFile::exists(dbPath));
    FsCueOverrideStore::flushIfChanged(*pFirst);
    EXPECT_FALSE(QFile::exists(dbPath));

    // The DJ sets a hot cue and a memory loop; the save mirrors them to the stick.
    pFirst->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart + 2,
            mixxx::audio::FramePos(11.0 * kSampleRate),
            mixxx::audio::kInvalidFramePos,
            mixxx::RgbColor(0xabcdef));
    pFirst->createAndAddCue(mixxx::CueType::Loop,
            mixxx::kMemoryCueBankStart,
            mixxx::audio::FramePos(20.0 * kSampleRate),
            mixxx::audio::FramePos(28.0 * kSampleRate));
    FsCueOverrideStore::flushIfChanged(*pFirst);
    ASSERT_TRUE(QFile::exists(dbPath));

    // Next load, with rekordbox-style cues already imported: the stored ones win.
    const TrackPointer pSecond = makeTrack();
    pSecond->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            mixxx::audio::FramePos(1.0 * kSampleRate),
            mixxx::audio::kInvalidFramePos);
    FsCueOverrideStore::applyOverrides(pSecond.get());
    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart + 2, mixxx::kMemoryCueBankStart}),
            hotcueIndices(pSecond));
    const CuePointer pRestored = findHotcue(pSecond, mixxx::kHotCueBankStart + 2);
    ASSERT_TRUE(pRestored);
    EXPECT_DOUBLE_EQ(11.0 * kSampleRate, pRestored->getPosition().value());
    EXPECT_EQ(mixxx::RgbColor(0xabcdef), pRestored->getColor());

    // Clearing wipes the drive's store, and a track still loaded must not
    // recreate it.
    EXPECT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
    EXPECT_FALSE(QFile::exists(dbPath));
    FsCueOverrideStore::suppressPendingSaves();
    FsCueOverrideStore::flushIfChanged(*pFirst);
    EXPECT_FALSE(QFile::exists(dbPath));

    // A fresh load gets the imported cues back, and is baselined so later edits
    // are stored again.
    const TrackPointer pThird = makeTrack();
    pThird->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            mixxx::audio::FramePos(1.0 * kSampleRate),
            mixxx::audio::kInvalidFramePos);
    FsCueOverrideStore::applyOverrides(pThird.get());
    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart}), hotcueIndices(pThird));
    EXPECT_FALSE(QFile::exists(dbPath));
    pThird->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart + 5,
            mixxx::audio::FramePos(9.0 * kSampleRate),
            mixxx::audio::kInvalidFramePos);
    FsCueOverrideStore::flushIfChanged(*pThird);
    EXPECT_TRUE(QFile::exists(dbPath));
}

// What Settings → Clear does to a track that is still in a deck. Only the DJ's
// own edits come off: the cues the source library exported — a rekordbox ANLZ
// import, or Serato markers the library database now holds — are put back
// rather than blanked along with them.
TEST_F(FsCueOverrideStoreTest, ClearRestoresImportedCuesInsteadOfBlankingThem) {
    const QStorageInfo usb(kFakeUsb);
    if (!usb.isValid() || !usb.isReady() || usb.rootPath() != kFakeUsb) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    const QString trackPath = kFakeUsb + QStringLiteral("/imported.wav");
    QFile::remove(trackPath);
    ASSERT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
    ASSERT_TRUE(QFile::copy(getTestDir().filePath(QStringLiteral("sine-30.wav")), trackPath));

    const auto makeTrack = [&] {
        auto pTrack = Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(trackPath)));
        pTrack->setAudioProperties(mixxx::audio::ChannelCount(2),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        return pTrack;
    };
    // The cues the source library exports for this track, on every load.
    const auto addImportedCues = [&](const TrackPointer& pTrack) {
        pTrack->createAndAddCue(mixxx::CueType::HotCue,
                mixxx::kHotCueBankStart,
                mixxx::audio::FramePos(3.0 * kSampleRate),
                mixxx::audio::kInvalidFramePos,
                mixxx::RgbColor(0x112233));
        pTrack->createAndAddCue(mixxx::CueType::HotCue,
                mixxx::kMemoryCueBankStart,
                mixxx::audio::FramePos(40.0 * kSampleRate),
                mixxx::audio::kInvalidFramePos);
        pTrack->setMainCuePosition(mixxx::audio::FramePos(2.0 * kSampleRate));
    };

    // An earlier session: the DJ moved the imported hot cue and added one of
    // their own, which stores the whole picture as an override.
    const TrackPointer pEdited = makeTrack();
    addImportedCues(pEdited);
    findHotcue(pEdited, mixxx::kHotCueBankStart)
            ->setStartAndEndPosition(mixxx::audio::FramePos(7.0 * kSampleRate),
                    mixxx::audio::kInvalidFramePos);
    pEdited->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart + 4,
            mixxx::audio::FramePos(25.0 * kSampleRate),
            mixxx::audio::kInvalidFramePos);
    FsCueOverrideStore::flushIfChanged(*pEdited);

    // This session: the track loads with its imported cues and the override
    // goes on top, so the deck shows the DJ's edits.
    const TrackPointer pLoaded = makeTrack();
    addImportedCues(pLoaded);
    const QByteArray importedPayload = FsCueOverrideStore::serializeCues(*pLoaded);
    FsCueOverrideStore::applyOverrides(pLoaded.get());
    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart,
                      mixxx::kHotCueBankStart + 4,
                      mixxx::kMemoryCueBankStart}),
            hotcueIndices(pLoaded));
    EXPECT_DOUBLE_EQ(7.0 * kSampleRate,
            findHotcue(pLoaded, mixxx::kHotCueBankStart)->getPosition().value());

    // Clearing puts the track back to exactly what the source library exported.
    EXPECT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
    FsCueOverrideStore::suppressPendingSaves();
    ASSERT_TRUE(FsCueOverrideStore::restoreImportedCues(pLoaded.get()));

    EXPECT_EQ(importedPayload, FsCueOverrideStore::serializeCues(*pLoaded));
    // The DJ's own pad is gone...
    EXPECT_FALSE(findHotcue(pLoaded, mixxx::kHotCueBankStart + 4));
    // ...while the imported ones survive, back at their imported positions.
    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart, mixxx::kMemoryCueBankStart}),
            hotcueIndices(pLoaded));
    EXPECT_DOUBLE_EQ(3.0 * kSampleRate,
            findHotcue(pLoaded, mixxx::kHotCueBankStart)->getPosition().value());
    EXPECT_EQ(mixxx::RgbColor(0x112233),
            findHotcue(pLoaded, mixxx::kHotCueBankStart)->getColor());
    EXPECT_DOUBLE_EQ(2.0 * kSampleRate, pLoaded->getMainCuePosition().value());

    QFile::remove(trackPath);
    ASSERT_TRUE(FsCueOverrideStore::clearFilesystemOverrides(kFakeUsb));
}

// A track that is not on removable media must never get a store of its own.
TEST_F(FsCueOverrideStoreTest, IgnoresTracksOnTheBootVolume) {
    const TrackPointer pTrack = createTrack();
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart,
            mixxx::audio::FramePos(1.0 * kSampleRate),
            mixxx::audio::kInvalidFramePos);

    FsCueOverrideStore::applyOverrides(pTrack.get());
    FsCueOverrideStore::flushIfChanged(*pTrack);

    const QStorageInfo storage(pTrack->getLocation());
    EXPECT_FALSE(QFile::exists(storage.rootPath() + QStringLiteral("/.bitedj/cues.sqlite")));
}

} // namespace
