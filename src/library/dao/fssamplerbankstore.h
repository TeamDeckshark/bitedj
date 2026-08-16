#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

/// Portable, per-filesystem store for the sampler banks a DJ builds on this
/// unit.
///
/// The sampler grid is split into two banks of eight (see SamplerDrive), both
/// saved to the one drive the DJ picked, into the self-contained SQLite
/// database `<mountRoot>/.bitedj/samplers.sqlite` next to the analysis cache
/// and the cue overrides. A bank is stored as the list of its slots' file paths
/// — relative to the mount root, since a sample can only be one that lives on
/// that stick, so the whole set travels with the drive and comes back on any
/// other Bite DJ unit.
///
/// A stored bank with no slots filled is meaningful — that is a bank the DJ
/// emptied — which is why the store distinguishes "no bank" from "an empty
/// bank": a drive that has never held a bank is not one whose owner cleared it.
///
/// Like FsCueOverrideStore this store does not keep its connections open; see
/// ScopedFsStore for why a lingering file descriptor would break eject.
class FsSamplerBankStore {
  public:
    /// Read bank `bankIndex` stored on the drive mounted at `mountRoot` into
    /// `pLocations`, as absolute paths with an empty string for every empty
    /// slot. The list is always resized to `slotCount`: a bank stored by a
    /// build with a different bank size is truncated or padded rather than
    /// rejected.
    ///
    /// Returns false when the drive is unavailable or holds no bank at that
    /// index, leaving `pLocations` untouched.
    static bool readBank(const QString& mountRoot,
            int bankIndex,
            int slotCount,
            QStringList* pLocations);

    /// Store `locations` (absolute paths, empty string for an empty slot) as
    /// bank `bankIndex` on the drive mounted at `mountRoot`.
    static bool writeBank(const QString& mountRoot,
            int bankIndex,
            const QStringList& locations);

    /// Delete the sampler bank database of the filesystem mounted at
    /// `mountPoint`. Returns false only when one exists but could not be
    /// deleted; a drive without banks counts as success.
    static bool clearFilesystemBanks(const QString& mountPoint);

    /// The stored payload: one JSON array of slot paths, relative to
    /// `mountRoot` for the samples that live on that drive and absolute for the
    /// ones that do not (which SamplerDrive no longer allows into a slot, but
    /// a bank written by an older build can still name). Also serves as the comparison value that decides
    /// whether a bank changed and is worth writing, so the same set of slots
    /// always has to serialize to the same bytes.
    ///
    /// Public so the codec can be exercised without a removable drive under the
    /// test, and so SamplerDrive can compare against a bank it just read.
    static QByteArray serializeBank(const QString& mountRoot, const QStringList& locations);

    /// Inverse of serializeBank(): absolute paths, exactly `slotCount` of them.
    static QStringList parseBank(const QString& mountRoot,
            const QByteArray& payload,
            int slotCount);
};
