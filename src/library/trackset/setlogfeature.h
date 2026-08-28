#pragma once

#include <QHash>
#include <QPointer>
#include <QTimer>

#include "library/dao/fshistoryworker.h"
#include "library/trackset/baseplaylistfeature.h"
#include "preferences/usersettings.h"

class Library;
class QAction;

/// Bite DJ: the history is USB-centric, and *only* USB-centric.
///
/// Its top level is the drives, not the sessions: every mounted USB volume gets
/// a node, and under it are the sessions that were played off *that* drive,
/// read from (and written to) the drive's own `.bitedj/history.sqlite` (see
/// FsHistoryStore). Pull the stick, plug it into another Bite DJ unit, and the
/// sets it played come with it.
///
/// Nothing is logged to this unit. A track played from somewhere other than a
/// removable drive has no stick to be written to, and rather than pile up
/// setlog playlists in the box's own library — a list that only ever grows, on
/// an appliance whose sidebar has to stay short — it is simply not recorded.
/// Any setlogs an earlier version left behind are purged at startup.
///
/// Sidebar item payloads are therefore not playlist ids at all: they carry a
/// string (see the k*Prefix constants in the .cpp), which BasePlaylistFeature's
/// helpers read as "not a playlist" — QVariant::toInt() fails on them — so its
/// playlist actions decline them instead of acting on some unrelated playlist.
class SetlogFeature : public BasePlaylistFeature {
    Q_OBJECT

  public:
    SetlogFeature(Library* pLibrary,
            UserSettingsPointer pConfig);
    virtual ~SetlogFeature();

    QVariant title() override;

    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

  public slots:
    void onRightClick(const QPoint& globalPos) override;
    void onRightClickChild(const QPoint& globalPos, const QModelIndex& index) override;
    void slotDeletePlaylist() override;
    void activate() override;
    void activateChild(const QModelIndex& index) override;

  protected:
    /// Rebuilds the sidebar tree from this unit's copy of the mounted drives.
    void constructChildModel();
    void decorateChild(TreeItem* pChild, int playlistId) override;

  private slots:
    void slotPlayingTrackChanged(TrackPointer currentPlayingTrack);
    /// No sidebar item of this feature is backed by a playlist any more, so the
    /// playlist table telling us it changed says nothing about the tree.
    void slotPlaylistTableChanged(int playlistId) override;
    void slotPlaylistContentOrLockChanged(const QSet<int>& playlistIds) override;
    void slotPlaylistTableRenamed(int playlistId, const QString& newName) override;
    /// Re-reads the mounted drives and rebuilds the tree when they changed.
    void slotRefreshUsbVolumes();
    /// A drive is gone: drop its node (and the view, if it was showing one of
    /// its sessions) now rather than on the next poll tick.
    void slotMountEjected(const QString& mountPoint);
    /// A track reached a drive's history. Carries the session's new totals, so
    /// relabelling the sidebar costs no read of its own.
    void slotTrackLogged(const QString& mountRoot,
            const FsHistorySession& session,
            TrackId trackId);
    /// A drive's session was closed by the worker thread: whatever it was still
    /// logging has landed, so the sidebar can stop calling it current.
    void slotSessionClosed(const QString& mountRoot);
    /// What a drive holds, as just read off it. Replaces this unit's copy and
    /// redraws the tree from it.
    void slotSessionsRead(const QString& mountRoot,
            const QList<FsHistorySession>& sessions);
    /// The tracks of a session, as just read off the drive: fills the view, if
    /// that session is still the one on screen.
    void slotSessionTracksRead(const QString& mountRoot,
            const QString& sessionName,
            const QStringList& locations);
    /// Starts a new session on the drive that was right-clicked. The next track
    /// played off it opens the session; nothing is written until then.
    void slotStartNewDriveSession();
    /// Deletes every session stored on the drive that was right-clicked.
    void slotDeleteDriveHistory();

  private:
    /// Drops every setlog playlist in this unit's library. They are no longer
    /// written, so the only ones that can exist were left by a version that
    /// still logged locally; this is what stops them showing up forever.
    void purgeLocalSetlogPlaylists();
    QString getRootViewHtml() const override;

    /// Mount root of the drive `trackLocation` lives on, or empty when it is
    /// not on one of the currently mounted removable volumes.
    QString mountRootForLocation(const QString& trackLocation) const;
    /// Hands a track played off a drive to the worker thread. Which session it
    /// lands in is decided there (see FsHistoryWorker), and the sidebar is
    /// updated from slotTrackLogged() once the row is actually on the drive.
    void logTrackToDrive(const QString& mountRoot, const TrackPointer& pTrack);

    /// Shows one drive session in the track view. The session's tracks are read
    /// off the drive on the worker thread, so the view opens empty and fills in
    /// from slotSessionTracksRead(). `sessionName` may be empty, which shows an
    /// empty view (a drive that carries no history yet).
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
    void updateDriveSessionItem(const QString& mountRoot, const FsHistorySession& session);
    /// Row of the volume node of `mountRoot`, or an invalid index.
    QModelIndex indexOfVolumeNode(const QString& mountRoot);
    QModelIndex indexOfItemData(const QVariant& data);

    std::list<TrackId> m_recentTracks;
    QAction* m_pStartNewDriveSessionAction;
    QAction* m_pDeleteDriveHistoryAction;
    QAction* m_pDeleteDriveSessionAction;

    /// Hidden, locked playlist the drive sessions are shown through: it is
    /// refilled from the drive's store on every activation, which is what lets
    /// them use the ordinary playlist track view (loading to a deck, cover art,
    /// ratings) without living in this unit's library.
    int m_driveViewPlaylistId;
    /// Mount roots of the drives currently shown in the sidebar, in that order.
    QStringList m_usbMountPoints;
    /// Mount root -> the sessions stored on that drive, newest first. This
    /// unit's copy of what the worker thread last read off each drive: the
    /// sidebar is built from it rather than from the sticks, so plugging one in
    /// or logging a track redraws the tree without touching a filesystem.
    QHash<QString, QList<FsHistorySession>> m_sessionsByMount;
    /// Mount root -> the session being logged to on that drive right now. A
    /// copy for display only: the worker thread decides which session a track
    /// lands in (a new name has to be read off the drive), and this follows it
    /// through slotTrackLogged().
    QHash<QString, QString> m_currentSessionByMount;
    /// Appends the history of a drive off the event loop; a USB stick can take
    /// seconds to accept one.
    FsHistoryWorker m_historyWorker;
    /// A drive whose node was activated before this unit had read it, so that
    /// the set it turns out to hold can be opened when the answer arrives.
    /// Empty the rest of the time.
    QString m_driveAwaitingActivation;
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
