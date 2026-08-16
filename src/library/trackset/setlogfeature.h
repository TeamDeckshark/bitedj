#pragma once

#include <QHash>
#include <QPointer>
#include <QTimer>

#include "library/trackset/baseplaylistfeature.h"
#include "preferences/usersettings.h"

class Library;
class QAction;

/// Bite DJ: the history is USB-centric.
///
/// Its top level is the drives, not the sessions: every mounted USB volume gets
/// a node, and under it are the sessions that were played off *that* drive,
/// read from (and written to) the drive's own `.bitedj/history.sqlite` (see
/// FsHistoryStore). Pull the stick, plug it into another Bite DJ unit, and the
/// sets it played come with it. A fixed "This Unit" node keeps the sessions of
/// tracks that were not on a removable drive, which are still ordinary setlog
/// playlists in this box's library database.
///
/// Sidebar item payloads are therefore no longer all playlist ids. The nodes
/// backed by a drive carry a string (see the k*Prefix constants in the .cpp),
/// which BasePlaylistFeature's helpers read as "not a playlist" — QVariant::
/// toInt() fails on them — so its playlist actions decline them instead of
/// acting on some unrelated playlist. Everything under "This Unit" is an
/// ordinary playlist id and behaves exactly as it did before.
class SetlogFeature : public BasePlaylistFeature {
    Q_OBJECT

  public:
    SetlogFeature(Library* pLibrary,
            UserSettingsPointer pConfig);
    virtual ~SetlogFeature();

    QVariant title() override;

    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;
    void activatePlaylist(int playlistId) override;

  public slots:
    void onRightClick(const QPoint& globalPos) override;
    void onRightClickChild(const QPoint& globalPos, const QModelIndex& index) override;
    void slotJoinWithPrevious();
    void slotMarkAllTracksPlayed();
    void slotLockAllChildPlaylists();
    void slotUnlockAllChildPlaylists();
    void slotDeletePlaylist() override;
    void slotGetNewPlaylist();
    void activate() override;
    void activateChild(const QModelIndex& index) override;

  protected:
    QModelIndex constructChildModel(int selectedId);
    void decorateChild(TreeItem* pChild, int playlistId) override;

  private slots:
    void slotPlayingTrackChanged(TrackPointer currentPlayingTrack);
    void slotPlaylistTableChanged(int playlistId) override;
    void slotPlaylistContentOrLockChanged(const QSet<int>& playlistIds) override;
    void slotPlaylistTableRenamed(int playlistId, const QString& newName) override;
    void slotDeleteAllUnlockedChildPlaylists();
    /// Re-reads the mounted drives and rebuilds the tree when they changed.
    void slotRefreshUsbVolumes();
    /// A drive is gone: drop its node (and the view, if it was showing one of
    /// its sessions) now rather than on the next poll tick.
    void slotMountEjected(const QString& mountPoint);
    /// Starts a new session on the drive that was right-clicked. The next track
    /// played off it opens the session; nothing is written until then.
    void slotStartNewDriveSession();
    /// Deletes every session stored on the drive that was right-clicked.
    void slotDeleteDriveHistory();

  private:
    void deleteAllUnlockedPlaylistsWithFewerTracks();
    void lockOrUnlockAllChildPlaylists(bool lock);
    QString getRootViewHtml() const override;

    /// Mount root of the drive `trackLocation` lives on, or empty when it is
    /// not on one of the currently mounted removable volumes.
    QString mountRootForLocation(const QString& trackLocation) const;
    /// The session tracks played off `mountRoot` are being logged to, opening
    /// one if this is the first track since the drive was mounted. Empty when
    /// the drive cannot record history (unavailable or write-protected).
    QString currentSessionOnDrive(const QString& mountRoot);
    /// Logs a track played off a drive to that drive and updates the sidebar.
    void logTrackToDrive(const QString& mountRoot, const TrackPointer& pTrack);

    /// Shows one drive session in the track view, resolving its stored paths
    /// against the library. `sessionName` may be empty, which shows an empty
    /// view (a drive that carries no history yet).
    void showDriveSession(const QString& mountRoot,
            const QString& sessionName,
            const QModelIndex& index);
    /// Replaces the contents of the scratch playlist the drive sessions are
    /// shown through.
    void fillDriveViewPlaylist(const QList<TrackId>& trackIds);
    /// Drops the session on screen, emptying its view, because the drive it was
    /// read from is gone.
    void forgetShownDriveSession();
    /// Relabels (or inserts) the sidebar item of one session in place, so
    /// logging a track does not collapse the tree the DJ is browsing.
    void updateDriveSessionItem(const QString& mountRoot, const QString& sessionName);
    /// Row of the volume node of `mountRoot`, or an invalid index.
    QModelIndex indexOfVolumeNode(const QString& mountRoot);
    QModelIndex indexOfItemData(const QVariant& data);

    std::list<TrackId> m_recentTracks;
    QAction* m_pJoinWithPreviousAction;
    QAction* m_pMarkTracksPlayedAction;
    QAction* m_pStartNewPlaylist;
    QAction* m_pLockAllChildPlaylists;
    QAction* m_pUnlockAllChildPlaylists;
    QAction* m_pDeleteAllChildPlaylists;
    QAction* m_pStartNewDriveSessionAction;
    QAction* m_pDeleteDriveHistoryAction;
    QAction* m_pDeleteDriveSessionAction;

    int m_currentPlaylistId;
    /// Hidden, locked playlist the drive sessions are shown through: it is
    /// refilled from the drive's store on every activation, which is what lets
    /// them use the ordinary playlist track view (loading to a deck, cover art,
    /// ratings) without living in this unit's library.
    int m_driveViewPlaylistId;
    /// Mount roots of the drives currently shown in the sidebar, in that order.
    QStringList m_usbMountPoints;
    /// Mount root -> the session being logged to on that drive right now.
    QHash<QString, QString> m_currentSessionByMount;
    /// What the scratch playlist currently holds, so a track logged to the
    /// session on screen can be appended to the view as well.
    QString m_shownMountRoot;
    QString m_shownSessionName;
    /// Backstop for drives appearing: SystemSettings emits nothing this object
    /// can connect to at construction time (it is built after the library), and
    /// an eject arrives through Library::mountEjected but a plug-in does not.
    QTimer m_usbPollTimer;

    Library* m_pLibrary;
    UserSettingsPointer m_pConfig;
};
