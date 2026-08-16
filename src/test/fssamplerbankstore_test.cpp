#include "library/dao/fssamplerbankstore.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QStorageInfo>
#include <QStringList>

#include "mixer/samplerdrive.h"
#include "test/mixxxtest.h"

namespace {

// The store only writes to drives the DJ can pull out, so its database half
// needs a filesystem mounted under one of the removable roots. Without root,
// `unshare` provides one:
//
//   unshare -Umr --propagation private sh -c \
//     'mount -t tmpfs tmpfs /mnt && mkdir -p /mnt/usbtest && \
//      mount -t tmpfs tmpfs /mnt/usbtest && \
//      QT_QPA_PLATFORM=offscreen ./mixxx-test \
//        --gtest_filter="FsSamplerBankStoreTest.*"'
//
// The test skips those cases when that mount is not there, so an ordinary run
// of the suite still covers the codec.
const QString kFakeUsb = QStringLiteral("/mnt/usbtest");

constexpr int kSlots = SamplerDrive::kSamplersPerBank;

/// Covers the payload codec the per-drive sampler bank store round-trips
/// through — which paths travel with the stick and which cannot — plus the
/// database half, for which the last cases need a removable mount.
class FsSamplerBankStoreTest : public MixxxTest {
  protected:
    static bool haveFakeUsb() {
        // An unmounted path reports itself as its own root, so validity is what
        // tells a real mount from a missing one.
        const QStorageInfo usb(kFakeUsb);
        return usb.isValid() && usb.isReady() && usb.rootPath() == kFakeUsb;
    }

    /// A bank of `kSlots` slots with `locations` at the front and the rest empty.
    static QStringList bankOf(const QStringList& locations) {
        QStringList bank = locations;
        while (bank.size() < kSlots) {
            bank.append(QString());
        }
        return bank;
    }
};

} // anonymous namespace

// A sample that lives on the same stick is stored relative to the mount root,
// which is what lets the whole bank come back when the drive is mounted
// somewhere else or plugged into another unit.
TEST_F(FsSamplerBankStoreTest, StoresDriveLocalPathsRelative) {
    const QString root = QStringLiteral("/mnt/stick");
    const QByteArray payload = FsSamplerBankStore::serializeBank(root,
            bankOf({QStringLiteral("/mnt/stick/Samples/airhorn.wav")}));

    EXPECT_TRUE(payload.contains("Samples/airhorn.wav"));
    EXPECT_FALSE(payload.contains("/mnt/stick"));

    const QStringList parsed = FsSamplerBankStore::parseBank(root, payload, kSlots);
    EXPECT_EQ(QStringLiteral("/mnt/stick/Samples/airhorn.wav"), parsed.at(0));
}

// One that does not can only be named absolutely. It simply fails to load
// where that path does not exist, which is the honest outcome.
TEST_F(FsSamplerBankStoreTest, StoresForeignPathsAbsolute) {
    const QString root = QStringLiteral("/mnt/stick");
    const QString foreign = QStringLiteral("/home/dj/Music/vox.wav");
    const QByteArray payload =
            FsSamplerBankStore::serializeBank(root, bankOf({foreign}));

    EXPECT_TRUE(payload.contains(foreign.toUtf8()));

    const QStringList parsed = FsSamplerBankStore::parseBank(root, payload, kSlots);
    EXPECT_EQ(foreign, parsed.at(0));
}

// A bank read back on a drive mounted at a different path resolves against the
// root it is read with, not the one it was written on.
TEST_F(FsSamplerBankStoreTest, DriveLocalPathsFollowTheMountPoint) {
    const QByteArray payload =
            FsSamplerBankStore::serializeBank(QStringLiteral("/mnt/stick"),
                    bankOf({QStringLiteral("/mnt/stick/Samples/siren.wav")}));

    const QStringList parsed = FsSamplerBankStore::parseBank(
            QStringLiteral("/media/usb0"), payload, kSlots);
    EXPECT_EQ(QStringLiteral("/media/usb0/Samples/siren.wav"), parsed.at(0));
}

// Empty slots are part of the bank: a row with a gap in the middle comes back
// with the same gap rather than closing up.
TEST_F(FsSamplerBankStoreTest, KeepsEmptySlotsInPlace) {
    const QString root = QStringLiteral("/mnt/stick");
    QStringList bank(kSlots, QString());
    bank[0] = QStringLiteral("/mnt/stick/a.wav");
    bank[3] = QStringLiteral("/mnt/stick/b.wav");

    const QStringList parsed = FsSamplerBankStore::parseBank(
            root, FsSamplerBankStore::serializeBank(root, bank), kSlots);
    EXPECT_EQ(bank, parsed);
}

// SamplerDrive compares payloads to decide whether a row is worth writing,
// so the same set of slots always has to serialize to the same bytes.
TEST_F(FsSamplerBankStoreTest, SameBankSerializesIdentically) {
    const QString root = QStringLiteral("/mnt/stick");
    const QStringList bank = bankOf({QStringLiteral("/mnt/stick/a.wav"),
            QStringLiteral("/home/dj/b.wav")});

    EXPECT_EQ(FsSamplerBankStore::serializeBank(root, bank),
            FsSamplerBankStore::serializeBank(root, bank));
}

// A bank stored by a build with a different grid is fitted to this one rather
// than rejected, so a change of bank size costs slots, not the whole bank.
TEST_F(FsSamplerBankStoreTest, FitsBanksOfAnotherSize) {
    const QString root = QStringLiteral("/mnt/stick");
    QStringList oversized;
    for (int i = 0; i < kSlots + 4; ++i) {
        oversized.append(root + QStringLiteral("/s%1.wav").arg(i));
    }

    const QStringList parsed = FsSamplerBankStore::parseBank(
            root, FsSamplerBankStore::serializeBank(root, oversized), kSlots);
    ASSERT_EQ(kSlots, parsed.size());
    EXPECT_EQ(root + QStringLiteral("/s0.wav"), parsed.at(0));

    const QStringList undersized = {root + QStringLiteral("/only.wav")};
    const QStringList padded = FsSamplerBankStore::parseBank(
            root, FsSamplerBankStore::serializeBank(root, undersized), kSlots);
    ASSERT_EQ(kSlots, padded.size());
    EXPECT_EQ(root + QStringLiteral("/only.wav"), padded.at(0));
    EXPECT_TRUE(padded.at(1).isEmpty());
}

TEST_F(FsSamplerBankStoreTest, UnreadablePayloadYieldsAnEmptyBank) {
    const QStringList parsed = FsSamplerBankStore::parseBank(
            QStringLiteral("/mnt/stick"), QByteArrayLiteral("not json"), kSlots);
    ASSERT_EQ(kSlots, parsed.size());
    for (const QString& location : parsed) {
        EXPECT_TRUE(location.isEmpty());
    }
}

// The whole cycle against a real store on a real (pretend) drive: a drive with
// no bank, a bank saved and read back, the two decks' banks kept apart, an
// emptied bank that is still a bank, and the action that deletes them.
TEST_F(FsSamplerBankStoreTest, WritesAndReadsBackFromDrive) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }

    // Start from a clean drive, so a rerun in the same namespace still counts.
    ASSERT_TRUE(FsSamplerBankStore::clearFilesystemBanks(kFakeUsb));
    const QString dbPath = kFakeUsb + QStringLiteral("/.bitedj/samplers.sqlite");

    // A drive with no bank is not an empty bank: the row is left alone.
    QStringList readBack;
    EXPECT_FALSE(FsSamplerBankStore::readBank(kFakeUsb, 1, kSlots, &readBack));
    EXPECT_FALSE(QFile::exists(dbPath));

    const QStringList deck1 = bankOf({kFakeUsb + QStringLiteral("/Samples/airhorn.wav"),
            QString(),
            kFakeUsb + QStringLiteral("/Samples/siren.wav")});
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kFakeUsb, 1, deck1));
    ASSERT_TRUE(QFile::exists(dbPath));

    ASSERT_TRUE(FsSamplerBankStore::readBank(kFakeUsb, 1, kSlots, &readBack));
    EXPECT_EQ(deck1, readBack);

    // Deck 2's bank is stored separately, which is what keeps the two rows
    // independent when both decks are playing off the same stick.
    EXPECT_FALSE(FsSamplerBankStore::readBank(kFakeUsb, 2, kSlots, &readBack));
    const QStringList deck2 = bankOf({kFakeUsb + QStringLiteral("/Samples/vox.wav")});
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kFakeUsb, 2, deck2));
    ASSERT_TRUE(FsSamplerBankStore::readBank(kFakeUsb, 2, kSlots, &readBack));
    EXPECT_EQ(deck2, readBack);
    ASSERT_TRUE(FsSamplerBankStore::readBank(kFakeUsb, 1, kSlots, &readBack));
    EXPECT_EQ(deck1, readBack);

    // A bank the DJ emptied is stored in its own right, and must not read back
    // as "no bank" — that would leave the row loaded instead of clearing it.
    const QStringList empty(kSlots, QString());
    ASSERT_TRUE(FsSamplerBankStore::writeBank(kFakeUsb, 1, empty));
    ASSERT_TRUE(FsSamplerBankStore::readBank(kFakeUsb, 1, kSlots, &readBack));
    EXPECT_EQ(empty, readBack);

    // Clearing takes the whole store with it.
    ASSERT_TRUE(FsSamplerBankStore::clearFilesystemBanks(kFakeUsb));
    EXPECT_FALSE(QFile::exists(dbPath));
    EXPECT_FALSE(FsSamplerBankStore::readBank(kFakeUsb, 1, kSlots, &readBack));
    EXPECT_FALSE(FsSamplerBankStore::readBank(kFakeUsb, 2, kSlots, &readBack));
}

// The bank is keyed by the drive, so a sample on the stick is stored by its
// path inside it and resolves back to wherever the stick is mounted now.
TEST_F(FsSamplerBankStoreTest, BankTravelsWithTheDrive) {
    if (!haveFakeUsb()) {
        GTEST_SKIP() << "needs a filesystem mounted at " << qPrintable(kFakeUsb);
    }
    ASSERT_TRUE(FsSamplerBankStore::clearFilesystemBanks(kFakeUsb));

    ASSERT_TRUE(FsSamplerBankStore::writeBank(kFakeUsb,
            1,
            bankOf({kFakeUsb + QStringLiteral("/Samples/horn.wav")})));

    // Read the payload back through the codec against another mount point: the
    // stored path is relative, so it follows the drive rather than pinning it.
    QStringList readBack;
    ASSERT_TRUE(FsSamplerBankStore::readBank(kFakeUsb, 1, kSlots, &readBack));
    const QByteArray payload =
            FsSamplerBankStore::serializeBank(kFakeUsb, readBack);
    const QStringList elsewhere = FsSamplerBankStore::parseBank(
            QStringLiteral("/media/other"), payload, kSlots);
    EXPECT_EQ(QStringLiteral("/media/other/Samples/horn.wav"), elsewhere.at(0));

    ASSERT_TRUE(FsSamplerBankStore::clearFilesystemBanks(kFakeUsb));
}

// A path that is not a live mount root must never resolve to the boot volume it
// is now a plain directory on — an ejected stick would otherwise have its bank
// written to whatever is left behind at the mount point.
TEST_F(FsSamplerBankStoreTest, RefusesMountPointsThatAreNotThere) {
    QStringList readBack;
    EXPECT_FALSE(FsSamplerBankStore::readBank(
            QStringLiteral("/mnt/definitely-not-mounted"), 1, kSlots, &readBack));
    EXPECT_FALSE(FsSamplerBankStore::writeBank(
            QStringLiteral("/mnt/definitely-not-mounted"),
            1,
            QStringList(kSlots, QString())));
    // And a directory that merely sits under a removable root belongs to the
    // filesystem it is a folder on, not to a drive of its own.
    EXPECT_FALSE(FsSamplerBankStore::writeBank(
            getTestDir().absolutePath(), 1, QStringList(kSlots, QString())));
}
