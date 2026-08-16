#include "mixer/samplerdrive.h"

#include <gtest/gtest.h>
#include <sys/mount.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QStorageInfo>
#include <QTest>

#include "control/controlindicatortimer.h"
#include "control/controlobject.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/dao/fssamplerbankstore.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/deck.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "mixer/sampler.h"
#include "preferences/systemsettings.h"
#include "recording/recordingmanager.h"
#include "soundio/soundmanager.h"
#include "sources/soundsourceproxy.h"
#include "test/mixxxdbtest.h"
#include "test/soundsourceproviderregistration.h"
#include "track/track.h"

namespace {

// The feature is about drives being plugged in and pulled out, so the test
// mounts and unmounts them for real. That needs a writable /mnt and the
// privileges to mount inside it, which `unshare` provides without root:
//
//   unshare -Umr --propagation private sh -c
//     'mount -t tmpfs tmpfs /mnt &&
//      QT_QPA_PLATFORM=offscreen ./mixxx-test
//        --gtest_filter="SamplerDriveTest.*"'
//
// Each "drive" is a directory bind-mounted onto its mount point, so unmounting
// it leaves its contents intact and mounting it again brings back the same
// volume — which is exactly what re-plugging a stick has to look like. Every
// test skips when that is not available.
const QString kUsbA = QStringLiteral("/mnt/usbA");
const QString kUsbB = QStringLiteral("/mnt/usbB");
const QString kBackingA = QStringLiteral("/mnt/backingA");
const QString kBackingB = QStringLiteral("/mnt/backingB");

const QString kSourceTrack = QStringLiteral("id3-test-data/cover-test-png.mp3");
const QString kSourceSample = QStringLiteral("id3-test-data/cover-test-vbr.mp3");

constexpr int kSlots = SamplerDrive::kSamplersPerBank;
constexpr int kSamplerCount = SamplerDrive::kBankCount * kSlots;
constexpr int kPumpTimeoutMillis = 10000;

bool bindMount(const QString& backing, const QString& mountPoint) {
    QDir().mkpath(backing);
    QDir().mkpath(mountPoint);
    return ::mount(backing.toLocal8Bit().constData(),
                   mountPoint.toLocal8Bit().constData(),
                   nullptr,
                   MS_BIND,
                   nullptr) == 0;
}

bool unmountDrive(const QString& mountPoint) {
    const QByteArray path = mountPoint.toLocal8Bit();
    if (::umount(path.constData()) == 0) {
        return true;
    }
    // Busy, because a sampler is still holding a file on it — which is exactly
    // the state a drive is in when a DJ pulls it out mid-set. A lazy unmount
    // detaches it from the mount table there and then, so the filesystem
    // disappears under the still-open descriptors the way a yank does.
    return ::umount2(path.constData(), MNT_DETACH) == 0;
}

bool isMounted(const QString& mountPoint) {
    const QStorageInfo info(mountPoint);
    return info.isValid() && info.isReady() &&
            QDir::cleanPath(info.rootPath()) == mountPoint;
}

void deleteTrack(Track* pTrack) {
    // Delete track objects directly in unit tests with no main event loop.
    delete pTrack;
}

} // namespace

/// Drives the real PlayerManager, decks and samplers against two drives that
/// are genuinely mounted and unmounted, to cover what SamplerDrive does with
/// the DJ's choice of drive: what it restores, what it saves where, what it
/// refuses to load, and what happens to the grid when the stick goes away and
/// comes back.
class SamplerDriveTest : public MixxxDbTest, SoundSourceProviderRegistration {
  public:
    SamplerDriveTest()
            : MixxxDbTest(true) {
    }

    /// Mount two empty volumes for this test, discarding anything a previous
    /// one left behind. False when this build cannot mount at all, which is
    /// what every test skips on.
    static bool haveMountControl() {
        unmountDrive(kUsbA);
        unmountDrive(kUsbB);
        QDir(kBackingA).removeRecursively();
        QDir(kBackingB).removeRecursively();
        return bindMount(kBackingA, kUsbA) && bindMount(kBackingB, kUsbB);
    }

    void SetUp() override {
        if (!haveMountControl()) {
            return;
        }
        m_setUp = true;
        // Mirrors coreservices, minus the local disk / settings setup.
        auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();
        m_pEffectsManager = std::make_shared<EffectsManager>(m_pConfig, pChannelHandleFactory);
        m_pEngine = std::make_shared<EngineMixer>(m_pConfig,
                "[Master]",
                m_pEffectsManager.get(),
                pChannelHandleFactory,
                true);
        m_pSoundManager = std::make_shared<SoundManager>(m_pConfig, m_pEngine.get());
        m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(nullptr);
        m_pEngine->registerNonEngineChannelSoundIO(m_pSoundManager.get());

        CoverArtCache::createInstance();

        m_pPlayerManager = std::make_shared<PlayerManager>(m_pConfig,
                m_pSoundManager.get(),
                m_pEffectsManager.get(),
                m_pEngine.get());
        m_pPlayerManager->addConfiguredDecks();
        for (int i = 0; i < kSamplerCount; ++i) {
            m_pPlayerManager->addSampler();
        }
        PlayerInfo::create();
        m_pEffectsManager->setup();

        const auto dbConnection = mixxx::DbConnectionPooled(dbConnectionPooler());
        ASSERT_TRUE(MixxxDb::initDatabaseSchema(dbConnection));
        m_pTrackCollectionManager = std::make_unique<TrackCollectionManager>(
                nullptr, m_pConfig, dbConnectionPooler(), deleteTrack);
        m_pRecordingManager = std::make_shared<RecordingManager>(m_pConfig, m_pEngine.get());
        m_pLibrary = std::make_shared<Library>(nullptr,
                m_pConfig,
                dbConnectionPooler(),
                m_pTrackCollectionManager.get(),
                m_pPlayerManager.get(),
                m_pRecordingManager.get());
        m_pPlayerManager->bindToLibrary(m_pLibrary.get());

        // Both drives start clean, so a rerun in the same namespace still counts.
        ASSERT_TRUE(FsSamplerBankStore::clearFilesystemBanks(kUsbA));
        ASSERT_TRUE(FsSamplerBankStore::clearFilesystemBanks(kUsbB));

        // SamplerDrive enumerates drives and hears about plugs and unplugs
        // through this, exactly as it does in the running appliance.
        m_pSystemSettings = std::make_unique<SystemSettings>(
                m_pConfig, m_pPlayerManager, m_pRecordingManager);
        m_pSamplerDrive = std::make_unique<SamplerDrive>(m_pConfig, m_pPlayerManager.get());
    }

    ~SamplerDriveTest() override {
        if (!m_setUp) {
            return;
        }
        m_pSamplerDrive.reset();
        m_pSystemSettings.reset();
        m_pSoundManager.reset();
        m_pPlayerManager.reset();
        PlayerInfo::destroy();
        m_pLibrary.reset();
        m_pRecordingManager.reset();
        m_pEngine.reset();
        m_pEffectsManager.reset();
        m_pTrackCollectionManager.reset();
        m_pControlIndicatorTimer.reset();
        CoverArtCache::destroy();

        unmountDrive(kUsbA);
        unmountDrive(kUsbB);
        QDir(kBackingA).removeRecursively();
        QDir(kBackingB).removeRecursively();
    }

  protected:
    /// Copy one of the test files onto `mountRoot` under `relPath` so it can be
    /// loaded from a drive, and return its absolute path.
    QString placeOnDrive(
            const QString& mountRoot, const QString& relPath, const QString& source) const {
        const QString path = mountRoot + QDir::separator() + relPath;
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile::remove(path);
        EXPECT_TRUE(QFile::copy(getTestDir().filePath(source), path));
        return path;
    }

    /// Turn the engine over and pump the event queue until `predicate` holds.
    /// Track loading finishes on the engine's terms, and the restore that
    /// follows is a second round of the same.
    bool pumpUntil(const std::function<bool()>& predicate) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kPumpTimeoutMillis) {
            m_pEngine->process(1024);
            QCoreApplication::processEvents();
            if (predicate()) {
                return true;
            }
            QTest::qSleep(10);
        }
        return predicate();
    }

    /// Turn the engine over for a fixed spell, to give anything that was *not*
    /// supposed to happen the chance to happen anyway.
    void pumpFor(int millis) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < millis) {
            m_pEngine->process(1024);
            QCoreApplication::processEvents();
            QTest::qSleep(10);
        }
    }

    /// Re-enumerate the mounted drives now, instead of waiting for the poll
    /// that would catch the change a few seconds later on the appliance.
    void refreshMounts() {
        ControlObject::set(ConfigKey("[System]", "usb_refresh"), 1.0);
        QCoreApplication::processEvents();
    }

    void selectDrive(const QString& mountPoint) {
        const int index = m_pSamplerDrive->driveLabels().indexOf(QDir(mountPoint).dirName());
        ASSERT_NE(-1, index) << qPrintable(mountPoint) << " is not in the drive list";
        m_pSamplerDrive->selectDriveAt(index);
    }

    void loadDeck(int deckIndex, const QString& location) {
        m_pPlayerManager->slotLoadLocationToPlayer(
                location, PlayerManager::groupForDeck(deckIndex), false);
        ASSERT_TRUE(pumpUntil([this, deckIndex] {
            return m_pPlayerManager->getDeck(deckIndex)->getLoadedTrack() != nullptr;
        }));
    }

    void loadSampler(int samplerIndex, const QString& location) {
        m_pPlayerManager->slotLoadLocationToPlayer(
                location, PlayerManager::groupForSampler(samplerIndex), false);
        ASSERT_TRUE(pumpUntil([this, samplerIndex] {
            return !samplerLocation(samplerIndex).isEmpty();
        }));
        // Let the caching reader finish prefetching before anything unloads
        // this slot again: unloading with a chunk read in flight trips a
        // DEBUG_ASSERT in stock CachingReader::process that has nothing to do
        // with what is under test here.
        pumpFor(500);
    }

    QString samplerLocation(int samplerIndex) const {
        Sampler* pSampler = m_pPlayerManager->getSampler(samplerIndex);
        const TrackPointer pTrack = pSampler ? pSampler->getLoadedTrack() : TrackPointer();
        return pTrack ? pTrack->getLocation() : QString();
    }

    /// Wait for the drive to hold `expected` as bank `bankIndex`. The write
    /// happens as the sampler settles, which is an engine round away.
    bool waitForStoredBank(
            const QString& mountRoot, int bankIndex, const QStringList& expected) {
        return pumpUntil([&] {
            QStringList stored;
            return FsSamplerBankStore::readBank(mountRoot, bankIndex, kSlots, &stored) &&
                    stored == expected;
        });
    }

    static QStringList bankOf(const QStringList& locations) {
        QStringList bank = locations;
        while (bank.size() < kSlots) {
            bank.append(QString());
        }
        return bank;
    }

    std::shared_ptr<EffectsManager> m_pEffectsManager;
    std::shared_ptr<mixxx::ControlIndicatorTimer> m_pControlIndicatorTimer;
    std::shared_ptr<EngineMixer> m_pEngine;
    std::shared_ptr<SoundManager> m_pSoundManager;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::unique_ptr<TrackCollectionManager> m_pTrackCollectionManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;
    std::shared_ptr<Library> m_pLibrary;
    std::unique_ptr<SystemSettings> m_pSystemSettings;
    std::unique_ptr<SamplerDrive> m_pSamplerDrive;
    bool m_setUp = false;
};

#define SKIP_WITHOUT_DRIVES()                                                  \
    if (!m_setUp) {                                                            \
        GTEST_SKIP() << "needs mountable filesystems at " << qPrintable(kUsbA) \
                     << " and " << qPrintable(kUsbB);                          \
    }

// Choosing a drive is what fills the grid: both banks come off the drive that
// was picked, and nothing else has any say in what the slots hold.
TEST_F(SamplerDriveTest, SelectingADriveFillsBothBanksFromIt) {
    SKIP_WITHOUT_DRIVES();

    const QString top = placeOnDrive(kUsbA, QStringLiteral("Samples/top.mp3"), kSourceSample);
    const QString bottom = placeOnDrive(kUsbA, QStringLiteral("Samples/bottom.mp3"), kSourceSample);
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kUsbA, 1, bankOf({top})));
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kUsbA, 2, bankOf({bottom})));

    selectDrive(kUsbA);

    EXPECT_TRUE(pumpUntil([this, &top] { return samplerLocation(0) == top; }));
    EXPECT_TRUE(pumpUntil([this, &bottom] { return samplerLocation(kSlots) == bottom; }));
}

// Every edit goes to the selected drive, whichever row it lands in.
TEST_F(SamplerDriveTest, EditsAreSavedToTheSelectedDrive) {
    SKIP_WITHOUT_DRIVES();

    const QString one = placeOnDrive(kUsbA, QStringLiteral("Samples/one.mp3"), kSourceSample);
    const QString two = placeOnDrive(kUsbA, QStringLiteral("Samples/two.mp3"), kSourceSample);

    selectDrive(kUsbA);
    loadSampler(0, one);
    loadSampler(kSlots, two);

    EXPECT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({one})));
    EXPECT_TRUE(waitForStoredBank(kUsbA, 2, bankOf({two})));
}

// The rule the greyed-out LOAD button stands for: a slot can only hold a file
// that is on the selected drive. A sample on the *other* stick is refused
// wherever the load comes from, so the grid can never end up holding something
// the drive does not carry.
TEST_F(SamplerDriveTest, SamplesFromAnotherDriveAreRefused) {
    SKIP_WITHOUT_DRIVES();

    const QString onA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);
    const QString onB = placeOnDrive(kUsbB, QStringLiteral("Samples/b.mp3"), kSourceSample);
    const QString internal = getTestDir().filePath(kSourceSample);

    selectDrive(kUsbA);

    EXPECT_TRUE(m_pSamplerDrive->refuseSampleLoad(
            PlayerManager::groupForSampler(0), onB));
    EXPECT_TRUE(m_pSamplerDrive->refuseSampleLoad(
            PlayerManager::groupForSampler(0), internal));
    EXPECT_FALSE(m_pSamplerDrive->refuseSampleLoad(
            PlayerManager::groupForSampler(0), onA));
    // A deck is not policed: it plays from wherever the DJ likes.
    EXPECT_FALSE(m_pSamplerDrive->refuseSampleLoad(
            PlayerManager::groupForDeck(0), onB));

    // And the refusal actually stops the load, rather than only reporting it.
    m_pPlayerManager->slotLoadLocationToPlayer(
            onB, PlayerManager::groupForSampler(0), false);
    pumpFor(500);
    EXPECT_TRUE(samplerLocation(0).isEmpty());
}

// LOAD is offered when the source deck is playing something off the selected
// drive, and only then.
TEST_F(SamplerDriveTest, LoadIsOnlyOfferedForATrackOnTheSelectedDrive) {
    SKIP_WITHOUT_DRIVES();

    const QString trackA = placeOnDrive(kUsbA, QStringLiteral("track.mp3"), kSourceTrack);

    selectDrive(kUsbA);
    EXPECT_FALSE(m_pSamplerDrive->canLoadFromSourceDeck()) << "empty deck";

    loadDeck(0, trackA);
    EXPECT_TRUE(m_pSamplerDrive->canLoadFromSourceDeck());

    // The same deck, once the samples are expected to come from elsewhere.
    selectDrive(kUsbB);
    EXPECT_FALSE(m_pSamplerDrive->canLoadFromSourceDeck());
}

// Switching drives hands the grid over wholesale: the new drive's banks
// replace the rows, and the drive being left keeps what it had.
TEST_F(SamplerDriveTest, SwitchingDrivesReplacesTheGrid) {
    SKIP_WITHOUT_DRIVES();

    const QString sampleA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);
    const QString sampleB = placeOnDrive(kUsbB, QStringLiteral("Samples/b.mp3"), kSourceSample);
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kUsbB, 1, bankOf({sampleB})));

    selectDrive(kUsbA);
    loadSampler(0, sampleA);
    ASSERT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({sampleA})));

    selectDrive(kUsbB);
    EXPECT_TRUE(pumpUntil([this, &sampleB] { return samplerLocation(0) == sampleB; }));

    QStringList stored;
    ASSERT_TRUE(FsSamplerBankStore::readBank(kUsbA, 1, kSlots, &stored));
    EXPECT_EQ(bankOf({sampleA}), stored);
}

// A drive with no banks yet empties the grid rather than letting the previous
// drive's samples pass themselves off as its own. The first slot the DJ fills
// is what starts its bank.
TEST_F(SamplerDriveTest, SwitchingToAFreshDriveEmptiesTheGrid) {
    SKIP_WITHOUT_DRIVES();

    const QString sampleA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);

    selectDrive(kUsbA);
    loadSampler(0, sampleA);
    ASSERT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({sampleA})));

    selectDrive(kUsbB);
    EXPECT_TRUE(pumpUntil([this] { return samplerLocation(0).isEmpty(); }));
}

// The headline case: pulling the stick empties the grid, and plugging it back
// in refills it — without the DJ touching anything.
TEST_F(SamplerDriveTest, UnpluggingClearsTheGridAndReplugRestoresIt) {
    SKIP_WITHOUT_DRIVES();

    const QString sampleA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);

    selectDrive(kUsbA);
    loadSampler(0, sampleA);
    ASSERT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({sampleA})));

    ASSERT_TRUE(unmountDrive(kUsbA));
    ASSERT_FALSE(isMounted(kUsbA));
    refreshMounts();
    EXPECT_TRUE(pumpUntil([this] { return samplerLocation(0).isEmpty(); }));
    EXPECT_TRUE(m_pSamplerDrive->mountRoot().isEmpty());
    // The DJ still knows which stick their samples are on.
    EXPECT_EQ(QStringLiteral("usbA"), m_pSamplerDrive->selectedDriveName());

    ASSERT_TRUE(bindMount(kBackingA, kUsbA));
    refreshMounts();
    EXPECT_TRUE(pumpUntil([this, &sampleA] { return samplerLocation(0) == sampleA; }));
}

// The grid emptying itself when a drive disappears must not be mistaken for
// the DJ clearing their samples: the bank on the drive survives it.
TEST_F(SamplerDriveTest, UnpluggingDoesNotWipeTheBankOnTheDrive) {
    SKIP_WITHOUT_DRIVES();

    const QString sampleA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);

    selectDrive(kUsbA);
    loadSampler(0, sampleA);
    ASSERT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({sampleA})));

    ASSERT_TRUE(unmountDrive(kUsbA));
    refreshMounts();
    ASSERT_TRUE(pumpUntil([this] { return samplerLocation(0).isEmpty(); }));
    pumpFor(300);

    ASSERT_TRUE(bindMount(kBackingA, kUsbA));
    QStringList stored;
    ASSERT_TRUE(FsSamplerBankStore::readBank(kUsbA, 1, kSlots, &stored));
    EXPECT_EQ(bankOf({sampleA}), stored);
}

// Ejecting from Settings unloads the drive's tracks while it is still mounted,
// so without the suppression the emptied rows would be written straight back
// over the banks the eject exists to preserve.
TEST_F(SamplerDriveTest, EjectDoesNotWipeTheBankItIsPreserving) {
    SKIP_WITHOUT_DRIVES();

    const QString sampleA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);

    selectDrive(kUsbA);
    loadSampler(0, sampleA);
    ASSERT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({sampleA})));

    // What SystemSettings::ejectMountPoint does before unloading the mount.
    m_pSamplerDrive->suppressSavesTo(kUsbA);

    m_pPlayerManager->slotLoadTrackToPlayer(
            TrackPointer(), PlayerManager::groupForSampler(0), false);
    ASSERT_TRUE(pumpUntil([this] { return samplerLocation(0).isEmpty(); }));
    pumpFor(300);

    QStringList stored;
    ASSERT_TRUE(FsSamplerBankStore::readBank(kUsbA, 1, kSlots, &stored));
    EXPECT_EQ(bankOf({sampleA}), stored);
}

// Nothing is pulled out from under a DJ mid-set: a slot that is playing when
// its row is replaced keeps its sample and takes the change when it stops.
// Meanwhile the *new* drive is told what the row will hold, not what it holds
// on the way there — the sample still running belongs to the other drive.
TEST_F(SamplerDriveTest, APlayingSlotKeepsItsSampleUntilItStops) {
    SKIP_WITHOUT_DRIVES();

    const QString sampleA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);
    const QString otherA = placeOnDrive(kUsbA, QStringLiteral("Samples/other.mp3"), kSourceSample);

    selectDrive(kUsbA);
    loadSampler(0, sampleA);
    ASSERT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({sampleA})));

    const ConfigKey playKey(PlayerManager::groupForSampler(0), QStringLiteral("play"));
    ControlObject::set(playKey, 1.0);
    pumpFor(100);
    ASSERT_TRUE(ControlObject::toBool(playKey)) << "sample did not start";

    // Drive B has no bank, so the row is on its way to empty.
    selectDrive(kUsbB);
    pumpFor(300);
    EXPECT_EQ(sampleA, samplerLocation(0)) << "the playing sample was cut off";
    // ...and B is never told it owns a sample that is not on it: the row it is
    // given is the one the slot has been promised, empty.
    QStringList storedB;
    if (FsSamplerBankStore::readBank(kUsbB, 1, kSlots, &storedB)) {
        EXPECT_EQ(QStringList(kSlots, QString()), storedB);
    }

    ControlObject::set(playKey, 0.0);
    EXPECT_TRUE(pumpUntil([this] { return samplerLocation(0).isEmpty(); }));

    // Drive A kept the bank it had all along.
    QStringList stored;
    ASSERT_TRUE(FsSamplerBankStore::readBank(kUsbA, 1, kSlots, &stored));
    EXPECT_EQ(bankOf({sampleA}), stored);
    // Going back to A brings its row back, and the slot that was playing takes
    // part in it again like any other.
    selectDrive(kUsbA);
    EXPECT_TRUE(pumpUntil([this, &sampleA] { return samplerLocation(0) == sampleA; }));
    loadSampler(1, otherA);
    EXPECT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({sampleA, otherA})));
}

// A restore suppresses writes until its loads land, and the DJ does not wait
// for it: a slot filled while the row is still settling must still be saved
// once it has. Waiting for the row as a whole to equal what was asked for
// never happens in that case — the extra sample is not in it — so the bank
// would stay write-locked until the watchdog gave up, taking the edit with it.
TEST_F(SamplerDriveTest, AnEditMadeWhileTheRestoreSettlesIsStillSaved) {
    SKIP_WITHOUT_DRIVES();

    const QString stored = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);
    const QString added = placeOnDrive(kUsbA, QStringLiteral("Samples/added.mp3"), kSourceSample);
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kUsbA, 1, bankOf({stored})));

    selectDrive(kUsbA);
    // No pumping in between: the restore's load for slot 0 has been issued and
    // is still in flight, which is exactly the window under test.
    loadSampler(1, added);

    EXPECT_TRUE(waitForStoredBank(kUsbA, 1, bankOf({stored, added})));
}

// With one stick in the unit there is nothing to choose between, so the
// samplers work without a trip to the drive picker.
TEST_F(SamplerDriveTest, ASingleDriveSelectsItself) {
    SKIP_WITHOUT_DRIVES();

    const QString sampleA = placeOnDrive(kUsbA, QStringLiteral("Samples/a.mp3"), kSourceSample);
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kUsbA, 1, bankOf({sampleA})));

    ASSERT_TRUE(unmountDrive(kUsbB));
    refreshMounts();
    if (m_pSamplerDrive->driveLabels().size() != 1) {
        // Another filesystem is mounted under the removable roots (the fake
        // drive the store tests want, say), so this is not the one-stick case.
        GTEST_SKIP() << "needs exactly one mounted drive";
    }

    EXPECT_TRUE(pumpUntil([this, &sampleA] { return samplerLocation(0) == sampleA; }));
    EXPECT_EQ(QStringLiteral("usbA"), m_pSamplerDrive->selectedDriveName());
}
