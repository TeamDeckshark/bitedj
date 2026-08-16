// This feature reads tracks, playlists and folders from removable Recordbox
// prepared devices (USB drives, etc), by parsing the binary *.PDB files
// stored on each removable device. It does not read the locally stored
// Rekordbox database (Collection).

// It draws heavily from the hard work completed here:

//      https://github.com/Deep-Symmetry/crate-digger

// And uses the C++ Kaitai Struct binary parsing libraries:

//      http://kaitai.io
//      https://github.com/kaitai-io/kaitai_struct
//      https://github.com/kaitai-io/kaitai_struct_cpp_stl_runtime

// The *.PDB C++ files:

//      rekordbox_pdb.h
//      rekordbox_pdb.cpp

// Were generated from the following structure definition file:

//      https://github.com/Deep-Symmetry/crate-digger/blob/master/src/main/kaitai/rekordbox_pdb.ksy

#pragma once

#include <QFuture>
#include <QFutureWatcher>
#include <QSet>
#include <QStringListModel>
#include <QTimer>
#include <QtConcurrentRun>
#include <fstream>

#include "library/baseexternallibraryfeature.h"
#include "library/baseexternalplaylistmodel.h"
#include "library/baseexternaltrackmodel.h"
#include "library/treeitemmodel.h"
#include "track/trackid.h"
#include "util/parented_ptr.h"

class TrackCollectionManager;
class BaseExternalPlaylistModel;

class RekordboxPlaylistModel : public BaseExternalPlaylistModel {
    Q_OBJECT
  public:
    RekordboxPlaylistModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager,
            QSharedPointer<BaseTrackCache> trackSource);
    TrackPointer getTrack(const QModelIndex& index) const override;
    bool isColumnHiddenByDefault(int column) override;
    bool isColumnInternal(int column) override;

    // Bite DJ: the star rating is the one cell a Rekordbox playlist lets the DJ
    // change. Everything else here is a read-only mirror of what the device
    // exported; a rating is the DJ's own judgement of the track, so it is
    // stored on the drive and travels with it (FsMetaOverrideStore).
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index,
            const QVariant& value,
            int role = Qt::EditRole) override;

  protected:
    void initSortColumnMapping() override;

  private:
    /// Store the rating for the track at `index` on its drive. Returns false
    /// when the drive would not take it, in which case the cell keeps the
    /// rating it had.
    bool setRatingOverride(const QModelIndex& index, const QVariant& value);
};

class RekordboxFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    RekordboxFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~RekordboxFeature() override;

    QVariant title() override;
    static bool isSupported();
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;

    // Bite DJ: hidden until the background poll (or a foreground scan)
    // finds a mounted Rekordbox device; hides again when the last device
    // disappears. See requestSidebarVisibility.
    bool isSidebarVisibleByDefault() const override {
        return false;
    }

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void refreshLibraryModels();
    void onRekordboxDevicesFound();
    void onTracksFound();
    // Bite DJ: connected to Library::mountEjected. Removes the sidebar device
    // whose mountpoint matches `mountPoint` (and clears its temp DB tables) the
    // instant the drive is unmounted, so a dead volume can't be tapped in the
    // window before the background poll would have culled it.
    void ejectDevice(const QString& mountPoint);

  private slots:
    void htmlLinkClicked(const QUrl& link);
    // Bite DJ: a rating was written to a drive — by this view, by a deck, by
    // anything. The scanned copy of the device library is what the playlist
    // views paint from, so it has to take the new value.
    void onRatingOverrideStored(const QString& trackLocation, int rating);
    // Bite DJ: Settings -> General -> Clear -> Meta wiped the drives; put the
    // scanned ratings back to what the devices themselves exported.
    void onMetaOverridesCleared();
    void onBackgroundPollTick();
    void onBackgroundRekordboxDevicesFound();
    void onBackgroundTracksFound();

  private:
    QString formatRootViewHtml() const;
    /// Re-read the rows the given ids name from the scanned device library, so
    /// every open playlist view repaints them.
    void refreshScannedTracks(const QSet<TrackId>& trackIds);
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    void mergeFoundDevicesIntoSidebar(
            std::vector<std::unique_ptr<TreeItem>> foundDevices,
            bool allowTableTruncate);
    void pumpBackgroundParseQueue();
    TreeItem* findDeviceByLabel(const QString& label) const;

    // A device found on disk but deliberately withheld from the sidebar, see
    // m_stagedDevices.
    struct StagedDevice {
        std::unique_ptr<TreeItem> pItem;
        // Identifies the physical drive this volume sits on. Volumes sharing a
        // key are shown together, once the last of them has been parsed.
        QString driveKey;
        bool parsed = false;
    };
    QString driveKeyOfDevice(const TreeItem* pDevice) const;
    StagedDevice* findStagedDevice(const QString& label);
    QStringList stagedDeviceLabels() const;
    std::unique_ptr<TreeItem> takeStagedDevice(const QString& label);
    void dropStagedDevice(const QString& label);
    // Moves every staged device whose drive is fully parsed into the sidebar.
    void promoteCompletedDrives();

    parented_ptr<TreeItemModel> m_pSidebarModel;
    parented_ptr<RekordboxPlaylistModel> m_pRekordboxPlaylistModel;

    QFutureWatcher<QList<TreeItem*>> m_devicesFutureWatcher;
    QFuture<QList<TreeItem*>> m_devicesFuture;
    QFutureWatcher<QString> m_tracksFutureWatcher;
    QFuture<QString> m_tracksFuture;
    QString m_title;

    // Background polling: surfaces newly-inserted USB drives and parses their
    // PDB without requiring a user tap. Devices in the sidebar without
    // children are treated as leaves by the patched WLibrarySidebar and would
    // collapse the sidebar on tap.
    QTimer m_bgPollTimer;
    QFutureWatcher<QList<TreeItem*>> m_bgDevicesFutureWatcher;
    QFuture<QList<TreeItem*>> m_bgDevicesFuture;
    QFutureWatcher<QString> m_bgTracksFutureWatcher;
    QFuture<QString> m_bgTracksFuture;
    // Devices found on disk but deliberately withheld from the sidebar until
    // their PDB has been parsed, so a device row never appears before the
    // playlists it is supposed to expand into. Parsed front-first; the items
    // move into the sidebar model in promoteCompletedDrives(), a whole drive
    // at a time so the volumes of a multi-partition stick appear together.
    std::vector<StagedDevice> m_stagedDevices;
    bool m_bgParseInFlight = false;
    // Label of the staged device the in-flight parse is writing into.
    QString m_bgParseLabel;
    // Set when that device is unmounted mid-parse: the item can't be freed
    // while the worker thread writes into it, so it is discarded once the
    // future completes rather than being inserted into the sidebar.
    bool m_bgParseAbandoned = false;
    // A single empty background enumeration is often a transient hiccup right
    // after a (re)mount. Require several consecutive empty scans before tearing
    // down a device, so it isn't needlessly re-parsed when it reappears.
    int m_bgConsecutiveEmptyScans = 0;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
