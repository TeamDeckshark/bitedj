#include "library/rekordbox/rekordboxanalysis.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <cmath>

#include "library/dao/fsanalysiscache.h"
#include "library/rekordbox/rekordboxanlz.h"
#include "library/rekordbox/rekordboxconstants.h"
#include "test/mixxxtest.h"
#include "track/beats.h"
#include "track/keyfactory.h"
#include "track/keys.h"
#include "track/track.h"

namespace {

// The analysis cache only lives on drives the DJ can pull out, so these cases
// need a filesystem mounted under one of the removable roots. Without root:
//
//   unshare -Umr --propagation private sh -c \
//     'mount -t tmpfs tmpfs /mnt && mkdir -p /mnt/usbtest && \
//      mount -t tmpfs tmpfs /mnt/usbtest && \
//      QT_QPA_PLATFORM=offscreen ./mixxx-test \
//        --gtest_filter="RekordboxAnalysisTest.*"'
//
// The tests skip themselves when that mount is not there.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);

/// A legacy `.DAT` holding just a PQTZ beat grid, one beat per time given, laid
/// out as lib/rekordbox-metadata/rekordbox_anlz.ksy reads it (big endian).
/// Each beat's tempo (hundredths of a BPM) is taken from `tempos`, 120.00 when
/// it has none for that beat.
QByteArray anlzWithBeatGrid(const QList<uint32_t>& beatTimesMs,
        const QList<uint16_t>& tempos = {}) {
    const auto appendU32 = [](QByteArray* pOut, uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            pOut->append(static_cast<char>((value >> shift) & 0xff));
        }
    };
    const auto appendU16 = [](QByteArray* pOut, uint16_t value) {
        pOut->append(static_cast<char>((value >> 8) & 0xff));
        pOut->append(static_cast<char>(value & 0xff));
    };
    QByteArray body;
    appendU32(&body, 0);
    appendU32(&body, 0x80000);
    appendU32(&body, static_cast<uint32_t>(beatTimesMs.size()));
    uint16_t beatNumber = 1;
    for (int i = 0; i < beatTimesMs.size(); ++i) {
        appendU16(&body, beatNumber);
        appendU16(&body, tempos.value(i, 12000));
        appendU32(&body, beatTimesMs[i]);
        beatNumber = beatNumber % 4 + 1;
    }
    QByteArray section;
    section.append("PQTZ", 4);
    appendU32(&section, 12);
    appendU32(&section, static_cast<uint32_t>(12 + body.size()));
    section.append(body);

    QByteArray header;
    header.append("PMAI", 4);
    appendU32(&header, 0x1c);
    appendU32(&header, static_cast<uint32_t>(0x1c + section.size()));
    header.append(0x1c - header.size(), '\0');
    return header + section;
}

/// Bite DJ's own key detection always carries a sub-version.
Keys detectedKeys(mixxx::track::io::key::ChromaticKey key) {
    Keys keys = KeyFactory::makeBasicKeys(key, mixxx::track::io::key::ANALYZER);
    keys.setSubVersion(QStringLiteral("vamp_plugin_id=qm-keydetector:0"));
    return keys;
}

/// A key as a rekordbox export puts it on a track.
Keys rekordboxKeysFor(const QString& keyText) {
    Keys keys = KeyFactory::makeBasicKeysNormalized(keyText, mixxx::track::io::key::USER);
    keys.setSubVersion(mixxx::rekordboxconstants::keysSubversion);
    return keys;
}

class RekordboxAnalysisTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        if (!usbIsMounted()) {
            return;
        }
        // Every case gets a drive of its own: no analysis cache, no export.
        FsAnalysisCache::clearFilesystemCache(kFakeUsb);
        QDir(kFakeUsb).removeRecursively();
        QDir().mkpath(kFakeUsb + QStringLiteral("/PIONEER/USBANLZ"));
        m_pCache = std::make_unique<FsAnalysisCache>(config());
    }

    void TearDown() override {
        RekordboxAnalysis::setLookup({});
        m_pCache.reset();
        if (usbIsMounted()) {
            FsAnalysisCache::clearFilesystemCache(kFakeUsb);
        }
        MixxxTest::TearDown();
    }

    static bool usbIsMounted() {
        const QStorageInfo usb(kFakeUsb);
        return usb.isValid() && usb.isReady() && usb.rootPath() == kFakeUsb;
    }

    /// An audio file on the drive, which the export says rekordbox analysed
    /// with `keyText` and the grid in `anlzBytes` (none when empty).
    TrackPointer exportedTrack(const QString& fileName,
            const QString& keyText,
            const QByteArray& anlzBytes) {
        const QString trackPath = kFakeUsb + QStringLiteral("/") + fileName;
        EXPECT_TRUE(QFile::copy(
                getTestDir().filePath(QStringLiteral("sine-30.wav")), trackPath));
        const QString anlzPath = kFakeUsb + QStringLiteral("/PIONEER/USBANLZ/") +
                fileName + QStringLiteral(".DAT");
        if (!anlzBytes.isEmpty()) {
            writeAnlz(anlzPath, anlzBytes);
        }
        m_entries.insert(trackPath, RekordboxAnalysis::Entry{anlzPath, keyText});
        RekordboxAnalysis::setLookup([this](const QString& location)
                                             -> std::optional<RekordboxAnalysis::Entry> {
            const auto it = m_entries.constFind(location);
            if (it == m_entries.constEnd()) {
                return std::nullopt;
            }
            return *it;
        });
        return trackAt(trackPath);
    }

    static void writeAnlz(const QString& path, const QByteArray& bytes) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(bytes);
    }

    /// A fresh Track for the file, as a new session would build it.
    static TrackPointer trackAt(const QString& trackPath) {
        const auto pTrack = Track::newTemporary(mixxx::FileAccess(mixxx::FileInfo(trackPath)));
        pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(2),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(30));
        return pTrack;
    }

    /// What a player loading the track does.
    RekordboxAnalysis::Result load(const TrackPointer& pTrack) {
        return RekordboxAnalysis::applySource(pTrack, config());
    }

    /// What tapping RB / BITE on a deck holding the track does.
    RekordboxAnalysis::Result choose(const TrackPointer& pTrack, AnalysisSource source) {
        return RekordboxAnalysis::chooseSource(pTrack, source, config());
    }

    /// What TrackDAO does with a track's analysis on save.
    void save(const TrackPointer& pTrack) {
        m_pCache->saveTrackBeats(pTrack->getLocation(), pTrack->getBeats());
        m_pCache->saveTrackKeys(pTrack->getLocation(), pTrack->getKeys());
    }

    /// A fresh Track for the file with Bite DJ's stored analysis on it, as
    /// TrackDAO hands a player the track in a later session.
    TrackPointer reloaded(const QString& trackPath) {
        const TrackPointer pTrack = trackAt(trackPath);
        if (const auto pBeats = m_pCache->getTrackBeats(trackPath, kSampleRate)) {
            pTrack->trySetBeats(pBeats);
        }
        if (const auto keys = m_pCache->getTrackKeys(trackPath)) {
            pTrack->setKeys(*keys);
        }
        return pTrack;
    }

    QHash<QString, RekordboxAnalysis::Entry> m_entries;
    std::unique_ptr<FsAnalysisCache> m_pCache;
};

#define SKIP_WITHOUT_USB()                                                         \
    if (!usbIsMounted()) {                                                         \
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb); \
    }

// A track nobody exported is left exactly as it is, whatever the DJ picks.
TEST_F(RekordboxAnalysisTest, TrackWithoutExportIsUntouched) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = trackAt(getTestDir().filePath(QStringLiteral("sine-30.wav")));
    const auto pBeats = mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(124));
    pTrack->trySetBeats(pBeats);

    for (const auto result : {load(pTrack),
                 choose(pTrack, AnalysisSource::Rekordbox),
                 choose(pTrack, AnalysisSource::BiteDJ)}) {
        EXPECT_FALSE(result.rekordboxAvailable);
        EXPECT_EQ(AnalysisSource::BiteDJ, result.source);
        EXPECT_FALSE(result.needsAnalysis);
        EXPECT_EQ(pBeats, pTrack->getBeats());
    }
}

// Until the DJ picks, a track plays with rekordbox's grid and key, tagged so
// that the analyzers and the cache can tell them apart.
TEST_F(RekordboxAnalysisTest, RekordboxIsTheDefault) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("a.wav"),
            QStringLiteral("8A"),
            anlzWithBeatGrid({500, 1000, 1500, 2000}));
    pTrack->trySetBeats(mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(124)));
    pTrack->setKeys(detectedKeys(mixxx::track::io::key::C_MAJOR));

    const auto result = load(pTrack);

    EXPECT_TRUE(result.rekordboxAvailable);
    EXPECT_EQ(AnalysisSource::Rekordbox, result.source);
    EXPECT_FALSE(result.needsAnalysis);
    ASSERT_TRUE(RekordboxAnalysis::isRekordboxBeats(pTrack->getBeats()));
    EXPECT_NEAR(120.0, pTrack->getBpm(), 0.01);
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, pTrack->getKeys().getGlobalKey());
    EXPECT_EQ(mixxx::rekordboxconstants::keysSubversion, pTrack->getKeys().getSubVersion());
    // Undo must not take the deck back to the grid it replaced.
    EXPECT_FALSE(pTrack->canUndoBeatsChange());
}

// The DJ's pick belongs to the track and is kept on its drive: the next load
// of the same file comes back with it, and other tracks are not affected.
TEST_F(RekordboxAnalysisTest, ChoiceIsRememberedPerTrack) {
    SKIP_WITHOUT_USB();
    const QByteArray anlz = anlzWithBeatGrid({500, 1000, 1500, 2000});
    const TrackPointer pPicked = exportedTrack(QStringLiteral("picked.wav"), QString(), anlz);
    const TrackPointer pOther = exportedTrack(QStringLiteral("other.wav"), QString(), anlz);
    pPicked->trySetBeats(mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(124)));
    save(pPicked);

    EXPECT_EQ(AnalysisSource::BiteDJ, choose(pPicked, AnalysisSource::BiteDJ).source);
    save(pPicked);

    const TrackPointer pPickedAgain = reloaded(pPicked->getLocation());
    const auto result = load(pPickedAgain);
    EXPECT_TRUE(result.rekordboxAvailable);
    EXPECT_EQ(AnalysisSource::BiteDJ, result.source);
    EXPECT_NEAR(124.0, pPickedAgain->getBpm(), 0.01);

    EXPECT_EQ(AnalysisSource::Rekordbox, load(reloaded(pOther->getLocation())).source);

    // And back: RB is remembered just the same.
    choose(pPickedAgain, AnalysisSource::Rekordbox);
    EXPECT_EQ(AnalysisSource::Rekordbox, load(reloaded(pPicked->getLocation())).source);
}

// The point of keeping both: saving a track that carries rekordbox's analysis
// leaves Bite DJ's on the drive, and switching back finds it there.
TEST_F(RekordboxAnalysisTest, BothAnalysesSurviveSwitching) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("b.wav"),
            QStringLiteral("8A"),
            anlzWithBeatGrid({500, 1000, 1500, 2000}));
    const auto pOwnBeats = mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(124));
    pTrack->trySetBeats(pOwnBeats);
    pTrack->setKeys(detectedKeys(mixxx::track::io::key::C_MAJOR));
    save(pTrack);

    load(pTrack);
    save(pTrack);

    const auto pStored = m_pCache->getTrackBeats(pTrack->getLocation(), kSampleRate);
    ASSERT_TRUE(pStored);
    EXPECT_FALSE(RekordboxAnalysis::isRekordboxBeats(pStored));
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR,
            m_pCache->getTrackKeys(pTrack->getLocation())->getGlobalKey());

    const auto result = choose(pTrack, AnalysisSource::BiteDJ);
    EXPECT_FALSE(result.needsAnalysis);
    EXPECT_NEAR(124.0, pTrack->getBpm(), 0.01);
    EXPECT_EQ(mixxx::track::io::key::C_MAJOR, pTrack->getKeys().getGlobalKey());
    EXPECT_FALSE(pTrack->canUndoBeatsChange());

    choose(pTrack, AnalysisSource::Rekordbox);
    EXPECT_NEAR(120.0, pTrack->getBpm(), 0.01);
    EXPECT_EQ(mixxx::track::io::key::A_MINOR, pTrack->getKeys().getGlobalKey());
    EXPECT_FALSE(pTrack->canUndoBeatsChange());
}

// A grid corrected in BITE is Bite DJ's own grid, kept on the drive, and is
// what the track comes back with next time.
TEST_F(RekordboxAnalysisTest, BiteDJGridEditIsKept) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("edit.wav"),
            QString(),
            anlzWithBeatGrid({500, 1000, 1500, 2000}));
    const auto pOwn = mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(124));
    pTrack->trySetBeats(pOwn);
    save(pTrack);
    load(pTrack);
    choose(pTrack, AnalysisSource::BiteDJ);
    // Read back from the drive, so an equal grid rather than the same object.
    const auto pRestored = pTrack->getBeats();
    ASSERT_TRUE(pRestored);
    ASSERT_FALSE(RekordboxAnalysis::isRekordboxBeats(pRestored));
    ASSERT_EQ(pOwn->firstBeat(), pRestored->firstBeat());

    const auto pEdited = pRestored->tryTranslate(441);
    ASSERT_TRUE(pEdited);
    pTrack->trySetBeats(*pEdited);
    save(pTrack);

    const TrackPointer pNextSession = reloaded(pTrack->getLocation());
    EXPECT_EQ(AnalysisSource::BiteDJ, load(pNextSession).source);
    ASSERT_TRUE(pNextSession->getBeats());
    EXPECT_FALSE(RekordboxAnalysis::isRekordboxBeats(pNextSession->getBeats()));
    EXPECT_EQ(pOwn->firstBeat() + 441, pNextSession->getBeats()->firstBeat());
}

// Rekordbox's grid is read from the export on every load and never written to
// the drive, not even over Bite DJ's slot.
TEST_F(RekordboxAnalysisTest, RekordboxGridIsNeverStored) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("e.wav"),
            QString(),
            anlzWithBeatGrid({500, 1000, 1500, 2000}));
    load(pTrack);
    const auto pImported = pTrack->getBeats();
    ASSERT_TRUE(RekordboxAnalysis::isRekordboxBeats(pImported));
    // Not something a deck lets the DJ do (BpmControl refuses), but whatever
    // gets there is not kept.
    pTrack->trySetBeats(*pImported->tryTranslate(441));
    save(pTrack);

    EXPECT_FALSE(m_pCache->getTrackBeats(pTrack->getLocation(), kSampleRate));
    const TrackPointer pNextSession = reloaded(pTrack->getLocation());
    load(pNextSession);
    ASSERT_TRUE(pNextSession->getBeats());
    EXPECT_EQ(pImported->firstBeat(), pNextSession->getBeats()->firstBeat());
}

// Asking for Bite DJ's analysis of a track it never analysed takes rekordbox's
// off, so that the analyzer runs.
TEST_F(RekordboxAnalysisTest, BiteDJSourceWithoutOwnAnalysisNeedsAnalysis) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("c.wav"),
            QStringLiteral("8A"),
            anlzWithBeatGrid({500, 1000, 1500, 2000}));
    load(pTrack);
    ASSERT_TRUE(pTrack->getBeats());

    const auto result = choose(pTrack, AnalysisSource::BiteDJ);

    EXPECT_EQ(AnalysisSource::BiteDJ, result.source);
    EXPECT_TRUE(result.needsAnalysis);
    EXPECT_FALSE(pTrack->getBeats());
    EXPECT_EQ(mixxx::track::io::key::INVALID, pTrack->getKeys().getGlobalKey());
}

// Drives written by an older build hold the rekordbox import where Bite DJ's
// analysis goes: the grid tagged as rekordbox's, the key as a plain untagged
// one. Neither may pass for Bite DJ's own.
TEST_F(RekordboxAnalysisTest, LegacyImportInOwnSlotIsNotBiteDJsAnalysis) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("d.wav"),
            QStringLiteral("8A"),
            anlzWithBeatGrid({500, 1000, 1500, 2000}));
    // What an older build left behind, written past the new save rules.
    {
        const auto pLegacyTrack = trackAt(pTrack->getLocation());
        mixxx::rekordbox::readAnalyzeFiles(pLegacyTrack,
                kSampleRate,
                0,
                m_entries.value(pTrack->getLocation()).anlzPath);
        ASSERT_TRUE(RekordboxAnalysis::isRekordboxBeats(pLegacyTrack->getBeats()));
    }
    const Keys legacyKeys = KeyFactory::makeBasicKeysNormalized(
            QStringLiteral("8A"), mixxx::track::io::key::USER);
    ASSERT_TRUE(m_pCache->saveTrackKeys(pTrack->getLocation(), legacyKeys));
    pTrack->setKeys(legacyKeys);
    pTrack->trySetBeats(mixxx::rekordbox::readBeatGrid(
            m_entries.value(pTrack->getLocation()).anlzPath, kSampleRate, 0));

    const auto result = choose(pTrack, AnalysisSource::BiteDJ);

    EXPECT_TRUE(result.needsAnalysis);
    EXPECT_FALSE(pTrack->getBeats());
    EXPECT_EQ(mixxx::track::io::key::INVALID, pTrack->getKeys().getGlobalKey());
}

// A grid the DJ locked is theirs, and it is not rekordbox's.
TEST_F(RekordboxAnalysisTest, LockedGridIsNeverReplaced) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("g.wav"),
            QString(),
            anlzWithBeatGrid({500, 1000, 1500, 2000}));
    const auto pLocked = mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(124));
    pTrack->trySetAndLockBeats(pLocked);

    const auto result = load(pTrack);
    EXPECT_EQ(AnalysisSource::BiteDJ, result.source);
    EXPECT_EQ(pLocked, pTrack->getBeats());
}

// An export entry for a track rekordbox never analysed has no grid: the track
// plays with Bite DJ's analysis, and RB cannot be picked.
TEST_F(RekordboxAnalysisTest, UnanalysedExportPlaysWithBiteDJ) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("h.wav"), QString(), QByteArray());
    const auto pOwn = mixxx::Beats::fromConstTempo(
            kSampleRate, mixxx::audio::kStartFramePos, mixxx::Bpm(124));
    pTrack->trySetBeats(pOwn);

    const auto result = load(pTrack);
    EXPECT_FALSE(result.rekordboxAvailable);
    EXPECT_EQ(AnalysisSource::BiteDJ, result.source);
    EXPECT_EQ(pOwn, pTrack->getBeats());

    EXPECT_EQ(AnalysisSource::BiteDJ, choose(pTrack, AnalysisSource::Rekordbox).source);
    EXPECT_FALSE(m_pCache->getAnalysisSource(pTrack->getLocation()));
}

// The same for a grid that is there but cannot be read: Bite DJ's analysis,
// and the rekordbox key with it goes too.
TEST_F(RekordboxAnalysisTest, UnreadableRekordboxGridFallsBackToBiteDJ) {
    SKIP_WITHOUT_USB();
    const TrackPointer pTrack = exportedTrack(QStringLiteral("bad.wav"),
            QStringLiteral("8A"),
            QByteArray("this is not an ANLZ file at all"));
    pTrack->setKeys(rekordboxKeysFor(QStringLiteral("8A")));

    const auto result = load(pTrack);
    EXPECT_FALSE(result.rekordboxAvailable);
    EXPECT_EQ(AnalysisSource::BiteDJ, result.source);
    EXPECT_TRUE(result.needsAnalysis);
    EXPECT_EQ(mixxx::track::io::key::INVALID, pTrack->getKeys().getGlobalKey());
}

/// `count` beats at `bpm` from `firstMs`, rounded to whole milliseconds the
/// way rekordbox stores them.
QList<uint32_t> roundedBeatTimes(double firstMs, double bpm, int count) {
    QList<uint32_t> times;
    for (int i = 0; i < count; ++i) {
        times << static_cast<uint32_t>(std::lround(firstMs + i * 60000.0 / bpm));
    }
    return times;
}

mixxx::BeatsPointer readGrid(const QByteArray& anlzBytes) {
    QTemporaryDir dir;
    EXPECT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ANLZ0000.DAT"));
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(anlzBytes);
    file.close();
    return mixxx::rekordbox::readBeatGrid(path, kSampleRate, 0);
}

// A constant tempo whose beats only wander by the export's millisecond
// rounding comes back as one grid at the exact exported tempo, not a marker
// per beat chasing 491/492 ms intervals.
TEST(RekordboxBeatGridTest, RoundedConstantGridKeepsExactTempo) {
    const auto pBeats = readGrid(anlzWithBeatGrid(
            roundedBeatTimes(1000, 122, 64), QList<uint16_t>(64, 12200)));
    ASSERT_TRUE(pBeats);
    EXPECT_TRUE(pBeats->hasConstantTempo());
    EXPECT_DOUBLE_EQ(122.0, pBeats->getLastMarkerBpm().value());
    // Anchored on rekordbox's first beat (a constant grid also extends back
    // before it, so that is not firstBeat()).
    EXPECT_EQ(mixxx::audio::FramePos(44100),
            pBeats->findClosestBeat(mixxx::audio::FramePos(44100)));
    // The 64th beat is still on the exported grid: no drift from rounding.
    EXPECT_NEAR(44100 + 63 * 44100 * 60.0 / 122,
            pBeats->findClosestBeat(mixxx::audio::FramePos(44100 + 63 * 21688.5)).value(),
            1.0);
    EXPECT_TRUE(RekordboxAnalysis::isRekordboxBeats(pBeats));
}

// A real tempo change keeps every beat where rekordbox put it.
TEST(RekordboxBeatGridTest, TempoChangeKeepsBeatPositions) {
    QList<uint32_t> times = roundedBeatTimes(1000, 120, 8);
    times << roundedBeatTimes(times.last() + 60000.0 / 128, 128, 8);
    QList<uint16_t> tempos(8, 12000);
    tempos << QList<uint16_t>(8, 12800);
    const auto pBeats = readGrid(anlzWithBeatGrid(times, tempos));
    ASSERT_TRUE(pBeats);
    EXPECT_FALSE(pBeats->hasConstantTempo());
}

// One tempo throughout, but a beat further off the grid than rounding can
// explain (an edited grid): positions are kept as they are.
TEST(RekordboxBeatGridTest, OffGridBeatKeepsBeatPositions) {
    QList<uint32_t> times = roundedBeatTimes(1000, 120, 16);
    times[9] += 5;
    const auto pBeats = readGrid(anlzWithBeatGrid(times, QList<uint16_t>(16, 12000)));
    ASSERT_TRUE(pBeats);
    EXPECT_FALSE(pBeats->hasConstantTempo());
}

} // namespace
