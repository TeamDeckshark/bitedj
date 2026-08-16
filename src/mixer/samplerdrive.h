#pragma once

#include <QAtomicPointer>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

#include "preferences/usersettings.h"
#include "track/track_decl.h"

class ControlObject;
class ControlProxy;
class PlayerManager;

/// Owns the one USB drive the samplers live on.
///
/// The DJ picks a drive on the Samplers tab and everything about the grid
/// follows from that one choice: the 16 slots are filled from banks stored on
/// that drive, every edit is written back to it, a sample can only be loaded
/// from a file that is *on* it, and the grid empties when it is unplugged and
/// refills when it comes back. There is no other source. That is the whole
/// point of the explicit selection — the previous design inferred a drive per
/// deck from whatever track that deck happened to be playing, which meant
/// nothing on screen said where a sample had come from or where it would go.
///
/// The grid is still split into two banks of eight — samplers 1-8 are the top
/// row, 9-16 the bottom, matching `samplers.xml` — because that is how the
/// skin lays them out and how they are stored (`FsSamplerBankStore`, bank 1 and
/// bank 2). Both banks live on the *same* drive now, so the rows are two halves
/// of one set rather than two decks' private property.
///
/// Nothing is ever pulled out from under the DJ mid-set: a slot that is
/// **playing** when its row is replaced or its drive disappears keeps its
/// sample and is given the new one (or emptied) the moment it stops. Until
/// then the row is *treated* as though the change had landed, so what gets
/// written to the drive is what the DJ asked for, not the transient state.
class SamplerDrive : public QObject {
    Q_OBJECT
  public:
    /// Slots in one bank. **Must** match the grid `samplers.xml` declares, and
    /// the two together must cover every sampler the appliance creates.
    static constexpr int kSamplersPerBank = 8;
    static constexpr int kBankCount = 2;

    SamplerDrive(UserSettingsPointer pConfig, PlayerManager* pPlayerManager);
    ~SamplerDrive() override;

    /// Owned by CoreServices, so the eject path and the track players can reach
    /// it without being handed a pointer. Null in builds and tests that never
    /// construct one, which is what keeps callers optional.
    static SamplerDrive* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    /// Mounted removable drives the DJ can choose between, in the same order
    /// as `selectDriveAt()`'s index and `SystemSettings::usbMountPoints()`.
    QStringList driveLabels() const;
    /// Index of the selected drive in that list, or -1 when the selection is
    /// unmounted or nothing is selected yet.
    int selectedDriveIndex() const;
    /// Name of the selected drive, remembered while it is unplugged so the UI
    /// and the refusal messages can still say which stick the samples are on.
    /// Empty when no drive has ever been chosen.
    QString selectedDriveName() const {
        return m_selection.label;
    }
    /// Mount root of the selected drive, empty while it is not mounted.
    QString mountRoot() const {
        return m_mountRoot;
    }

    /// Make the drive at `index` the sample drive: the current rows are saved
    /// to the drive they belong to and replaced by whatever the new one holds.
    /// Out-of-range clears the selection (and the grid with it).
    void selectDriveAt(int index);

    /// Whether a sample can be loaded from the deck the SOURCE selector points
    /// at — that is, whether that deck holds a track that lives on the selected
    /// drive. What the LOAD buttons grey out on.
    bool canLoadFromSourceDeck() const;

    /// Gate for every load into a sampler, wherever it comes from: the LOAD
    /// button, the library, a controller. Returns true (and publishes why) when
    /// `group` is a sampler and `location` is not on the selected drive, which
    /// is the caller's cue to abandon the load. False for a deck, a preview
    /// deck, or a sample that is where it belongs.
    ///
    /// The rule this enforces is the one the DJ can see: a slot holds a file
    /// from the sample drive, so unplugging that drive empties the grid and
    /// plugging it back in refills it, on this unit or any other.
    bool refuseSampleLoad(const QString& group, const QString& location);

    /// Stop saving anything to the drive mounted at `mountRoot`: it is being
    /// ejected, and the unload that is about to empty the grid is the eject's
    /// doing, not the DJ's.
    ///
    /// Without this the eject would write the emptied rows back over the banks
    /// it is meant to be preserving — the drive is still mounted while its
    /// tracks are unloaded, so the write would succeed. Must be called *before*
    /// the tracks on the mount are unloaded. The selection itself is kept, so
    /// the banks come back when the drive does.
    void suppressSavesTo(const QString& mountRoot);

  signals:
    /// Emitted whenever the drive list or the selection changes. Carries the
    /// labels and the selected index so `WSamplerDrive` can rebuild in one go
    /// (CO transport carries doubles only — same string-alongside-signal
    /// pattern as `SystemSettings::usbRowsChanged`).
    void drivesChanged(const QStringList& labels, int selectedIndex);

  private:
    /// A mounted removable drive.
    struct Drive {
        QString mountPoint;
        /// Filesystem UUID, empty when the volume has none (a tmpfs under test,
        /// an exotic filesystem). The selection falls back to the mount point
        /// then, which is weaker but no worse than having no identity at all.
        QString uuid;
        QString label;
    };

    /// What the DJ chose, as persisted. Survives an unplug and a restart.
    struct Selection {
        QString uuid;
        QString mountPoint;
        QString label;

        bool isEmpty() const {
            return uuid.isEmpty() && mountPoint.isEmpty();
        }
        /// Whether `drive` is the drive this selection names. UUID decides
        /// whenever both have one: a re-plugged stick can land on a different
        /// mount point, and — worse — a *different* stick can land on the same
        /// one, since the automounter names the path after the volume label.
        bool matches(const Drive& drive) const;
    };

    /// One row of the grid and what is known about it on the drive.
    struct Bank {
        /// The row as last read from (or written to) the drive. Comparing
        /// against it is what makes "did this actually change?" cheap.
        QByteArray baseline;
        /// True while the loads issued by a restore are still settling.
        bool restoring = false;
        /// The row the outstanding restore asked for, serialized. Becomes the
        /// baseline once the restore lands: it is what the drive holds.
        QByteArray restoreTarget;
        /// The same row unserialized, so an arriving slot can be checked
        /// against what *it* was asked for.
        QStringList restoreExpected;
        /// Slots (0-based within the bank) whose load has been issued but has
        /// not landed yet. The restore ends when this empties, rather than when
        /// the row as a whole equals the target — a slot the DJ fills while the
        /// restore is settling would otherwise mean the row never matches, and
        /// the bank would stay write-locked until the watchdog gave up, taking
        /// that edit with it. Per-slot also survives the synchronous unload
        /// inside slotLoadTrack, which reports a slot that is emphatically not
        /// there yet.
        QSet<int> restoreAwaiting;
        /// Bumped by every restore, so a watchdog can tell whether the restore
        /// it was armed for is still the current one.
        int restoreGeneration = 0;
    };

    /// Connect the decks and samplers that exist and are not connected yet.
    void rebuildConnections();

    void onMountsChanged();
    void onSamplerChanged(int samplerIndex);
    void onSamplerPlayChanged(int samplerIndex, bool playing);
    void onRestoreTimeout(int bank, int generation);

    /// Re-enumerate the mounted drives and re-resolve the selection against
    /// them, restoring the grid when the selected drive has appeared and
    /// clearing it when it has gone. Emits drivesChanged().
    void refreshDrives();
    /// Point the grid at `mountRoot` (empty to detach it from every drive),
    /// saving the rows to the drive they are leaving first.
    void setMountRoot(const QString& mountRoot);
    QList<Drive> enumerateDrives() const;
    void storeSelection();
    void loadSelection();

    /// Read both banks off the drive and load them into the grid.
    void restoreAllBanks();
    /// Load `stored` into one row's samplers and arm the restore bookkeeping.
    void applyBank(int bank, const QStringList& stored);
    /// Empty the whole grid, deferring the slots that are playing.
    void clearAllSlots();
    /// Write a row to the drive if it differs from the baseline.
    void flushBank(int bank);
    /// Send `location` (empty to unload) to one sampler.
    void loadSlot(int samplerIndex, const QString& location);
    /// Give a slot that was playing the sample it was owed once it stopped.
    void applyPending(int samplerIndex);

    /// The row as it will stand once every deferred slot has been dealt with:
    /// what a playing slot has been *promised*, and what the others hold. This
    /// — not the literal state of the samplers — is what is stored, compared
    /// and converged against, so a sample left running does not write itself
    /// into a bank it is not part of.
    QStringList effectiveBankLocations(int bank) const;
    /// One slot's contribution to that row: what it has been promised while it
    /// plays out, or what it actually holds.
    QString effectiveSlotLocation(int samplerIndex) const;
    QString samplerLocation(int samplerIndex) const;
    bool hasBank(int bank) const;
    bool isSamplerPlaying(int samplerIndex) const;
    /// Whether `location` names a file on the selected drive. False whenever
    /// no drive is mounted — with no drive there is nowhere a sample can
    /// legitimately come from.
    bool isOnSelectedDrive(const QString& location) const;

    /// Recompute [Samplers],can_load from the source deck and the drive.
    void updateCanLoad();
    /// Explain, on the DJ's own tap, why LOAD is greyed out. Only ever called
    /// from the SOURCE selector: a deck loading a track of its own is not a
    /// request to put it in a sampler, so it stays silent.
    void notifySourceUnusable();

    /// Banks are stored 1-based, so bank 1 is the top row when read off the drive.
    static int bankIndexFor(int bank) {
        return bank + 1;
    }

    UserSettingsPointer m_pConfig;
    PlayerManager* const m_pPlayerManager;

    QList<Drive> m_drives;
    Selection m_selection;
    /// Mount root of the selected drive while it is mounted, else empty. The
    /// single piece of state every save and load is gated on.
    QString m_mountRoot;

    QVector<Bank> m_banks;
    /// Sampler index -> the location that slot is owed (empty to be emptied),
    /// for the slots that were playing when their row changed. Applied when
    /// playback stops; dropped if the DJ loads something there first.
    QHash<int, QString> m_pendingSlots;

    int m_connectedDecks = 0;
    int m_connectedSamplers = 0;

    /// [Samplers],source_deck_index — which deck LOAD clones from. Created
    /// here rather than by the skin so it exists before the skin parses (which
    /// then reuses it) and so its changes can be watched from here.
    std::unique_ptr<ControlObject> m_pCoSourceDeck;
    /// [Samplers],can_load — read-only, what the LOAD buttons bind `enabled` to.
    std::unique_ptr<ControlObject> m_pCoCanLoad;
    /// [Samplers],drive_mounted — read-only, 1 while the selected drive is
    /// present. Lets the skin say why the grid is empty.
    std::unique_ptr<ControlObject> m_pCoDriveMounted;
    /// Per-sampler `play`, for the deferred slots.
    QVector<ControlProxy*> m_samplerPlayProxies;

    static QAtomicPointer<SamplerDrive> s_pInstance;
};
