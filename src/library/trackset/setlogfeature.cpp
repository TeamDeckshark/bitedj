#include "library/trackset/setlogfeature.h"

#include <QDateTime>
#include <QDir>
#include <QMenu>
#include <QMessageBox>

#include "library/dao/fshistorystore.h"
#include "library/dao/fshistoryworker.h"
#include "library/library.h"
#include "library/library_prefs.h"
#include "library/playlisttablemodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "mixer/playerinfo.h"
#include "moc_setlogfeature.cpp"
#include "preferences/systemsettings.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/make_const_iterator.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wtracktableview.h"

namespace {

/// Sidebar payloads of the nodes that are not playlists. They deliberately do
/// not parse as an integer, which is how BasePlaylistFeature's helpers tell
/// them apart from a playlist id (see the class comment).
const QString kVolumeNodePrefix = QStringLiteral("bitedj:history:drive:");
const QString kSessionNodePrefix = QStringLiteral("bitedj:history:session:");
/// Neither a mount point nor a session name can contain a newline.
const QChar kSessionNodeSeparator = QLatin1Char('\n');

/// Backstop for a drive being plugged in; an eject is reported to us directly.
constexpr int kUsbPollIntervalMillis = 5000;

const QString kCurrentSessionIcon =
        QStringLiteral(":/images/library/ic_library_history_current.svg");
const QString kDriveIcon = QStringLiteral(":/images/library/ic_library_computer.svg");

QString volumeNodeData(const QString& mountRoot) {
    return kVolumeNodePrefix + mountRoot;
}

QString sessionNodeData(const QString& mountRoot, const QString& sessionName) {
    return kSessionNodePrefix + mountRoot + kSessionNodeSeparator + sessionName;
}

bool parseVolumeNodeData(const QVariant& data, QString* pMountRoot) {
    const QString text = data.toString();
    if (!text.startsWith(kVolumeNodePrefix)) {
        return false;
    }
    *pMountRoot = text.mid(kVolumeNodePrefix.size());
    return !pMountRoot->isEmpty();
}

bool parseSessionNodeData(const QVariant& data, QString* pMountRoot, QString* pSessionName) {
    const QString text = data.toString();
    if (!text.startsWith(kSessionNodePrefix)) {
        return false;
    }
    const QString payload = text.mid(kSessionNodePrefix.size());
    const int separator = payload.indexOf(kSessionNodeSeparator);
    if (separator <= 0) {
        return false;
    }
    *pMountRoot = payload.left(separator);
    *pSessionName = payload.mid(separator + 1);
    return !pSessionName->isEmpty();
}

/// What a drive is called in the sidebar: the name the automounter gave its
/// mount point, which is the volume label the DJ wrote on the stick.
QString volumeLabel(const QString& mountRoot) {
    const QString name = QDir(mountRoot).dirName();
    return name.isEmpty() ? mountRoot : name;
}

} // namespace

using namespace mixxx::library::prefs;

SetlogFeature::SetlogFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig)
        : BasePlaylistFeature(
                  pLibrary,
                  pConfig,
                  new PlaylistTableModel(
                          nullptr,
                          pLibrary->trackCollectionManager(),
                          "mixxx.db.model.setlog",
                          /*keep hidden tracks*/ true),
                  QStringLiteral("SETLOGHOME"),
                  QStringLiteral("history"),
                  QStringLiteral("SetlogCountsDurations"),
                  /*keep hidden tracks*/ true),
          m_driveViewPlaylistId(kInvalidPlaylistId),
          m_pLibrary(pLibrary),
          m_pConfig(pConfig) {
    // Whatever an earlier version logged into this unit's own library goes
    // here: sets live on the drive they were played from, and a local list that
    // only ever grows is exactly what this sidebar must not carry.
    purgeLocalSetlogPlaylists();

    QString placeholderName = "historyPlaceholder";
    // remove previously created placeholder playlists
    const QList<QPair<int, QString>> pls = m_playlistDao.getPlaylists(PlaylistDAO::PLHT_UNKNOWN);
    QStringList plsToDelete;
    for (const QPair<int, QString>& pl : pls) {
        if (pl.second.startsWith(placeholderName)) {
            plsToDelete.append(QString::number(pl.first));
        }
    }
    m_playlistDao.deletePlaylists(plsToDelete);

    // Create the empty placeholder playlist the drive sessions are shown
    // through. It is refilled from a drive's own store on every activation and
    // deleted again on shutdown, so nothing of a stick's history is left behind
    // in this unit's library.
    m_driveViewPlaylistId = m_playlistDao.createUniquePlaylist(&placeholderName,
            PlaylistDAO::PLHT_UNKNOWN);
    DEBUG_ASSERT(m_driveViewPlaylistId != kInvalidPlaylistId);
    // The view onto a drive session is read-only: it is a record of what was
    // played, and edits to it would have nowhere to go.
    m_playlistDao.setPlaylistLocked(m_driveViewPlaylistId, true);

    m_usbMountPoints = SystemSettings::usbMountPoints();

    //construct child model
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    constructChildModel();

    m_pStartNewDriveSessionAction = new QAction(tr("Start new session"), this);
    connect(m_pStartNewDriveSessionAction,
            &QAction::triggered,
            this,
            &SetlogFeature::slotStartNewDriveSession);

    m_pDeleteDriveHistoryAction = new QAction(tr("Delete all sessions on this drive"), this);
    connect(m_pDeleteDriveHistoryAction,
            &QAction::triggered,
            this,
            &SetlogFeature::slotDeleteDriveHistory);

    m_pDeleteDriveSessionAction = new QAction(tr("Delete session"), this);
    connect(m_pDeleteDriveSessionAction,
            &QAction::triggered,
            this,
            &SetlogFeature::slotDeletePlaylist);

    // A drive that goes away takes its node with it right away; one that
    // arrives is picked up by the poll below.
    connect(pLibrary,
            &Library::mountEjected,
            this,
            &SetlogFeature::slotMountEjected);
    connect(&m_historyWorker,
            &FsHistoryWorker::trackLogged,
            this,
            &SetlogFeature::slotTrackLogged);
    connect(&m_historyWorker,
            &FsHistoryWorker::sessionClosed,
            this,
            &SetlogFeature::slotSessionClosed);
    connect(&m_historyWorker,
            &FsHistoryWorker::sessionsRead,
            this,
            &SetlogFeature::slotSessionsRead);
    connect(&m_historyWorker,
            &FsHistoryWorker::sessionTracksRead,
            this,
            &SetlogFeature::slotSessionTracksRead);
    m_usbPollTimer.setInterval(kUsbPollIntervalMillis);
    m_usbPollTimer.setSingleShot(false);
    connect(&m_usbPollTimer,
            &QTimer::timeout,
            this,
            &SetlogFeature::slotRefreshUsbVolumes);
    m_usbPollTimer.start();

    // What the drives that are already plugged in hold. Asked here, after the
    // connections above, and answered off the worker thread, so opening the
    // app does not wait on however many sticks are in: the sidebar shows the
    // drives immediately and their sets a moment later.
    for (const QString& mountRoot : std::as_const(m_usbMountPoints)) {
        m_historyWorker.readSessions(mountRoot);
    }
}

SetlogFeature::~SetlogFeature() {
    // Delete the placeholder
    m_playlistDao.deletePlaylist(m_driveViewPlaylistId);
}

QVariant SetlogFeature::title() {
    return tr("History");
}

void SetlogFeature::bindLibraryWidget(
        WLibrary* pLibraryWidget, KeyboardEventFilter* pKeyboard) {
    BasePlaylistFeature::bindLibraryWidget(pLibraryWidget, pKeyboard);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingTrackChanged,
            this,
            &SetlogFeature::slotPlayingTrackChanged);
    m_pLibraryWidget = QPointer(pLibraryWidget);
}

void SetlogFeature::purgeLocalSetlogPlaylists() {
    const QList<QPair<int, QString>> setlogs =
            m_playlistDao.getPlaylists(PlaylistDAO::PLHT_SET_LOG);
    if (setlogs.isEmpty()) {
        return;
    }
    QStringList ids;
    ids.reserve(setlogs.size());
    for (const QPair<int, QString>& setlog : setlogs) {
        ids.append(QString::number(setlog.first));
    }
    // Locked ones go too: a lock was a way of keeping a local setlog around,
    // and there is no longer anywhere for it to be kept.
    qInfo() << "History: dropping" << ids.size()
            << "setlog playlist(s) left in this unit's library";
    m_playlistDao.deletePlaylists(ids);
}

void SetlogFeature::slotDeletePlaylist() {
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }
    TreeItem* pItem = static_cast<TreeItem*>(m_lastRightClickedIndex.internalPointer());
    if (!pItem) {
        return;
    }

    QString mountRoot;
    QString sessionName;
    if (parseSessionNodeData(pItem->getData(), &mountRoot, &sessionName)) {
        // A session on a drive: it lives in that drive's store, not in a
        // playlist table.
        QMessageBox::StandardButton btn = QMessageBox::question(nullptr,
                tr("Confirm Deletion"),
                //: %1 is the session name, %2 the drive it was played from
                tr("Do you really want to delete the session <b>%1</b> "
                   "on <b>%2</b>?")
                        .arg(sessionName, volumeLabel(mountRoot)),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
        if (btn != QMessageBox::Yes) {
            return;
        }
        // Queued like everything else that touches the drive, so a track
        // logged moments ago cannot land after the delete and put the session
        // back. The row goes now rather than when the drive confirms — the
        // sessionsRead() that answers this restores it if the delete failed.
        m_historyWorker.deleteSession(mountRoot, sessionName);
        QList<FsHistorySession>& sessions = m_sessionsByMount[mountRoot];
        sessions.erase(std::remove_if(sessions.begin(),
                               sessions.end(),
                               [&sessionName](const FsHistorySession& session) {
                                   return session.name == sessionName;
                               }),
                sessions.end());
        if (m_shownMountRoot == mountRoot && m_shownSessionName == sessionName) {
            showDriveSession(mountRoot, QString(), QModelIndex());
        }
        constructChildModel();
        return;
    }
    // Nothing else in this tree is deletable: a volume node is the drive
    // itself, and no item here is backed by a playlist.
}

void SetlogFeature::onRightClick(const QPoint& globalPos) {
    Q_UNUSED(globalPos);
    m_lastRightClickedIndex = QModelIndex();

    // Create the right-click menu
    // QMenu menu(NULL);
    // menu.addAction(m_pCreatePlaylistAction);
    // TODO(DASCHUER) add something like disable logging
    // menu.exec(globalPos);
}

void SetlogFeature::onRightClickChild(const QPoint& globalPos, const QModelIndex& index) {
    //Save the model index so we can get it in the action slots...
    m_lastRightClickedIndex = index;

    TreeItem* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (!pItem) {
        return;
    }
    const QVariant itemData = pItem->getData();

    QMenu menu(m_pSidebarWidget);

    QString mountRoot;
    QString sessionName;
    if (parseSessionNodeData(itemData, &mountRoot, &sessionName)) {
        // A session stored on a drive. It has no playlist behind it, so the
        // playlist actions (rename, lock, Auto DJ, export) do not apply.
        menu.addAction(m_pDeleteDriveSessionAction);
    } else if (parseVolumeNodeData(itemData, &mountRoot)) {
        menu.addAction(m_pStartNewDriveSessionAction);
        menu.addSeparator();
        menu.addAction(m_pDeleteDriveHistoryAction);
    } else {
        // Nothing else is a real entry.
        return;
    }

    menu.exec(globalPos);
}

/// Purpose: When plugging or pulling a drive we require the sidebar model not
/// to reset, so this inserts the rows dynamically rather than resetting.
///
/// The tree is exactly the drives: one node per mounted USB volume, holding the
/// sessions that drive carries. Nothing else — a set that was not played off a
/// removable drive is not recorded anywhere.
void SetlogFeature::constructChildModel() {
    clearChildModel();
    std::vector<std::unique_ptr<TreeItem>> itemList;
    itemList.reserve(m_usbMountPoints.size());

    // The drives, newest session first under each of them.
    for (const QString& mountRoot : std::as_const(m_usbMountPoints)) {
        auto pVolumeItem = std::make_unique<TreeItem>(
                volumeLabel(mountRoot), volumeNodeData(mountRoot));
        pVolumeItem->setIcon(QIcon(kDriveIcon));

        // From this unit's copy, not from the stick: the tree is rebuilt on
        // every mount change, and none of those is worth a round trip to a USB
        // drive on the GUI thread. The copy is kept current by
        // slotSessionsRead().
        const QList<FsHistorySession> sessions = m_sessionsByMount.value(mountRoot);
        const QString currentSession = m_currentSessionByMount.value(mountRoot);
        for (const FsHistorySession& session : sessions) {
            TreeItem* pSessionItem = pVolumeItem->appendChild(
                    createPlaylistLabel(session.name,
                            session.trackCount,
                            session.durationSeconds),
                    sessionNodeData(mountRoot, session.name));
            if (session.name == currentSession) {
                pSessionItem->setIcon(QIcon(kCurrentSessionIcon));
            }
        }
        itemList.push_back(std::move(pVolumeItem));
    }

    // Append all the newly created TreeItems in a dynamic way to the childmodel
    m_pSidebarModel->insertTreeItemRows(std::move(itemList), 0);
}

void SetlogFeature::decorateChild(TreeItem* pChild, int playlistId) {
    // Every item in this tree is a drive or one of its sessions, neither of
    // which is a playlist, so BasePlaylistFeature never gets here.
    Q_UNUSED(pChild);
    Q_UNUSED(playlistId);
}

QModelIndex SetlogFeature::indexOfItemData(const QVariant& data) {
    QModelIndexList results = m_pSidebarModel->match(
            m_pSidebarModel->getRootIndex(),
            TreeItemModel::kDataRole,
            data,
            1,
            Qt::MatchWrap | Qt::MatchExactly | Qt::MatchRecursive);
    if (!results.isEmpty()) {
        return results.front();
    }
    return QModelIndex();
}

QModelIndex SetlogFeature::indexOfVolumeNode(const QString& mountRoot) {
    return indexOfItemData(volumeNodeData(mountRoot));
}

QString SetlogFeature::mountRootForLocation(const QString& trackLocation) const {
    if (trackLocation.isEmpty()) {
        return QString();
    }
    const QString cleaned = QDir::cleanPath(trackLocation);
    for (const QString& mountRoot : m_usbMountPoints) {
        if (cleaned.startsWith(mountRoot + QLatin1Char('/'))) {
            return mountRoot;
        }
    }
    return QString();
}

void SetlogFeature::logTrackToDrive(const QString& mountRoot, const TrackPointer& pTrack) {
    // Queued, not written here: the store is on a USB stick, one append has
    // been measured at over three seconds, and this runs from the track change
    // — the DJ must not be holding a dead screen while the drive thinks about
    // it. Everything the sidebar needs comes back through slotTrackLogged().
    m_historyWorker.logTrack(mountRoot,
            pTrack->getLocation(),
            static_cast<int>(pTrack->getDuration()),
            pTrack->getId());
}

void SetlogFeature::slotTrackLogged(const QString& mountRoot,
        const FsHistorySession& session,
        TrackId trackId) {
    if (!m_usbMountPoints.contains(mountRoot)) {
        // The drive was pulled while the row was being written. Its node is
        // already gone from the tree, and its session is not this unit's to
        // show any more.
        return;
    }
    m_currentSessionByMount.insert(mountRoot, session.name);
    // Keep this unit's copy of the drive in step without re-reading it: the
    // answer already carries the session's new totals.
    QList<FsHistorySession>& sessions = m_sessionsByMount[mountRoot];
    const auto it = std::find_if(sessions.begin(),
            sessions.end(),
            [&session](const FsHistorySession& stored) {
                return stored.name == session.name;
            });
    if (it != sessions.end()) {
        *it = session;
    } else {
        // Its first track, so it is the newest set on the drive.
        sessions.prepend(session);
    }
    updateDriveSessionItem(mountRoot, session);

    if (m_shownMountRoot != mountRoot || m_shownSessionName != session.name) {
        return;
    }
    // The session on screen just grew. Keep the selection the DJ may be working
    // with (see the same dance in the local branch of slotPlayingTrackChanged).
    if (!trackId.isValid()) {
        return;
    }
    WTrackTableView* pView = m_pLibraryWidget
            ? dynamic_cast<WTrackTableView*>(m_pLibraryWidget->getActiveView())
            : nullptr;
    if (pView) {
        const QList<TrackId> trackIds = pView->getSelectedTrackIds();
        m_playlistDao.appendTrackToPlaylist(trackId, m_driveViewPlaylistId);
        pView->setSelectedTracks(trackIds);
    } else {
        m_playlistDao.appendTrackToPlaylist(trackId, m_driveViewPlaylistId);
    }
}

void SetlogFeature::updateDriveSessionItem(
        const QString& mountRoot, const FsHistorySession& session) {
    const QModelIndex volumeIndex = indexOfVolumeNode(mountRoot);
    if (!volumeIndex.isValid()) {
        return;
    }
    TreeItem* pVolumeItem = m_pSidebarModel->getItem(volumeIndex);
    if (!pVolumeItem) {
        return;
    }

    const QString label = createPlaylistLabel(
            session.name, session.trackCount, session.durationSeconds);
    const QString itemData = sessionNodeData(mountRoot, session.name);

    const QList<TreeItem*> sessionItems = pVolumeItem->children();
    for (TreeItem* pSessionItem : sessionItems) {
        if (pSessionItem->getData().toString() != itemData) {
            continue;
        }
        pSessionItem->setLabel(label);
        m_pSidebarModel->triggerRepaint();
        return;
    }

    // The session's first track: give it a row of its own rather than rebuilding
    // the tree, which would collapse whatever the DJ is browsing.
    auto pSessionItem = std::make_unique<TreeItem>(label, itemData);
    pSessionItem->setIcon(QIcon(kCurrentSessionIcon));
    std::vector<std::unique_ptr<TreeItem>> rows;
    rows.push_back(std::move(pSessionItem));
    m_pSidebarModel->insertTreeItemRows(std::move(rows), 0, volumeIndex);
}

void SetlogFeature::showDriveSession(const QString& mountRoot,
        const QString& sessionName,
        const QModelIndex& index) {
    // Whatever this shows settles what was on screen, so no earlier click is
    // still waiting for its drive to be read (activateChild re-arms this when
    // the click it is handling is one).
    m_driveAwaitingActivation.clear();
    if (index.isValid()) {
        m_lastClickedIndex = index;
        m_lastRightClickedIndex = QModelIndex();
    }

    // The view opens on what is on screen now, which is nothing: the session's
    // tracks are on the drive, and reading them there is the worker thread's
    // job. slotSessionTracksRead() fills the rows in when they arrive.
    fillDriveViewPlaylist({});
    m_shownMountRoot = mountRoot;
    m_shownSessionName = sessionName;
    if (!sessionName.isEmpty()) {
        m_historyWorker.readSessionTracks(mountRoot, sessionName);
    }

    emit saveModelState();
    m_pPlaylistTableModel->selectPlaylist(m_driveViewPlaylistId);
    emit showTrackModel(m_pPlaylistTableModel);
    emit enableCoverArtDisplay(true);
}

void SetlogFeature::slotSessionTracksRead(const QString& mountRoot,
        const QString& sessionName,
        const QStringList& locations) {
    if (m_shownMountRoot != mountRoot || m_shownSessionName != sessionName) {
        // The DJ moved on while the drive was being read.
        return;
    }
    QList<TrackId> trackIds;
    trackIds.reserve(locations.size());
    for (const QString& location : locations) {
        // The stored paths are relative to the drive, so they resolve here even
        // when the stick was played on another unit. A track that is no longer
        // on it simply drops out of the view.
        const TrackPointer pTrack = m_pLibrary->trackCollectionManager()->getOrAddTrack(
                TrackRef::fromFilePath(location));
        if (pTrack && pTrack->getId().isValid()) {
            trackIds.append(pTrack->getId());
        }
    }
    fillDriveViewPlaylist(trackIds);
}

void SetlogFeature::slotSessionsRead(
        const QString& mountRoot, const QList<FsHistorySession>& sessions) {
    if (!m_usbMountPoints.contains(mountRoot)) {
        // Read of a drive that has since been pulled.
        m_sessionsByMount.remove(mountRoot);
        return;
    }
    // A re-read that found nothing new redraws nothing: slotRefreshUsbVolumes()
    // asks every drive on every change to the list, and rebuilding the tree
    // would only collapse whatever the DJ is browsing. The answer is still
    // recorded, so a drive that holds nothing counts as read.
    const bool changed = m_sessionsByMount.value(mountRoot) != sessions;
    m_sessionsByMount.insert(mountRoot, sessions);
    if (changed) {
        constructChildModel();
    }

    if (m_driveAwaitingActivation != mountRoot) {
        return;
    }
    m_driveAwaitingActivation.clear();
    if (!sessions.isEmpty() && m_shownMountRoot == mountRoot &&
            m_shownSessionName.isEmpty()) {
        // The drive's node was activated before this unit knew what it held;
        // its most recent set is what that click asked for.
        showDriveSession(mountRoot, sessions.first().name, QModelIndex());
    }
}

void SetlogFeature::fillDriveViewPlaylist(const QList<TrackId>& trackIds) {
    if (m_driveViewPlaylistId == kInvalidPlaylistId) {
        return;
    }
    const int maxPosition = m_playlistDao.getMaxPosition(m_driveViewPlaylistId);
    if (maxPosition > 0) {
        QList<int> positions;
        positions.reserve(maxPosition);
        for (int position = 1; position <= maxPosition; ++position) {
            positions.append(position);
        }
        m_playlistDao.removeTracksFromPlaylist(m_driveViewPlaylistId, positions);
    }
    if (!trackIds.isEmpty()) {
        m_playlistDao.appendTracksToPlaylist(trackIds, m_driveViewPlaylistId);
    }
}

void SetlogFeature::slotRefreshUsbVolumes() {
    const QStringList mountPoints = SystemSettings::usbMountPoints();
    if (mountPoints == m_usbMountPoints) {
        return;
    }
    m_usbMountPoints = mountPoints;

    // A drive that went away ends its session; plugging it back in starts a new
    // one rather than continuing the set it was pulled out of.
    for (auto it = m_currentSessionByMount.begin();
            it != m_currentSessionByMount.end();) {
        if (mountPoints.contains(it.key())) {
            ++it;
        } else {
            m_historyWorker.forgetSession(it.key());
            it = m_currentSessionByMount.erase(it);
        }
    }
    // Ask every drive on the list what it holds. A stick that is merely still
    // plugged in answers with what this unit already has, which slotSessionsRead
    // drops on the floor; one that was swapped for another on the same mount
    // point answers with the new stick's sets.
    for (const QString& mountRoot : std::as_const(m_usbMountPoints)) {
        m_historyWorker.readSessions(mountRoot);
    }
    for (auto it = m_sessionsByMount.begin(); it != m_sessionsByMount.end();) {
        if (mountPoints.contains(it.key())) {
            ++it;
        } else {
            it = m_sessionsByMount.erase(it);
        }
    }
    if (!m_shownMountRoot.isEmpty() && !mountPoints.contains(m_shownMountRoot)) {
        forgetShownDriveSession();
    }

    constructChildModel();
}

void SetlogFeature::forgetShownDriveSession() {
    m_shownMountRoot.clear();
    m_shownSessionName.clear();
    m_driveAwaitingActivation.clear();
    // Empty the view as well. Library::slotMountEjected only dismisses a view
    // that is reading the dead mount directly (a browse listing), and a session
    // is shown through a playlist, so the rows would otherwise sit there naming
    // files that are no longer on the box.
    fillDriveViewPlaylist({});
}

void SetlogFeature::slotMountEjected(const QString& mountPoint) {
    const QString cleaned = QDir::cleanPath(mountPoint);
    m_currentSessionByMount.remove(cleaned);
    m_sessionsByMount.remove(cleaned);
    // The stick that comes back on this mount point is not necessarily the one
    // that just left, so the set it was recording ends here.
    m_historyWorker.forgetSession(cleaned);
    if (m_shownMountRoot == cleaned) {
        forgetShownDriveSession();
    }
    if (m_usbMountPoints.removeAll(cleaned) > 0) {
        constructChildModel();
    }
}

void SetlogFeature::slotStartNewDriveSession() {
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }
    TreeItem* pItem = static_cast<TreeItem*>(m_lastRightClickedIndex.internalPointer());
    QString mountRoot;
    if (!pItem || !parseVolumeNodeData(pItem->getData(), &mountRoot)) {
        return;
    }
    // Forgetting the drive's session is all it takes: the next track played off
    // it opens a new one. Nothing is written to the stick until then, so a set
    // that never happens leaves no empty session behind. The sidebar follows in
    // slotSessionClosed(), so that a track still being written lands in the
    // session it was actually played into before the marker moves off it.
    m_historyWorker.forgetSession(mountRoot);
}

void SetlogFeature::slotSessionClosed(const QString& mountRoot) {
    if (m_currentSessionByMount.remove(mountRoot) > 0) {
        // Redraws the "current session" marker, which no session on this drive
        // carries any more.
        constructChildModel();
    }
}

void SetlogFeature::slotDeleteDriveHistory() {
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }
    TreeItem* pItem = static_cast<TreeItem*>(m_lastRightClickedIndex.internalPointer());
    QString mountRoot;
    if (!pItem || !parseVolumeNodeData(pItem->getData(), &mountRoot)) {
        return;
    }

    QMessageBox::StandardButton btn = QMessageBox::warning(nullptr,
            tr("Confirm Deletion"),
            //: %1 is the name of the drive
            //: <b> + </b> are used to make the text in between bold in the popup
            //: <br> is a linebreak
            tr("Do you really want to delete the whole history stored on "
               "<b>%1</b>?<br><br>")
                    .arg(volumeLabel(mountRoot)),
            QMessageBox::Ok | QMessageBox::Cancel,
            QMessageBox::Cancel);
    if (btn != QMessageBox::Ok) {
        return;
    }

    // As in slotDeletePlaylist(): queued, so the delete is the last word on
    // whatever was already on its way to the drive, and the tree is emptied
    // now rather than when the drive confirms.
    m_historyWorker.clearHistory(mountRoot);
    m_sessionsByMount.insert(mountRoot, QList<FsHistorySession>());
    if (m_shownMountRoot == mountRoot) {
        showDriveSession(mountRoot, QString(), QModelIndex());
    }
    constructChildModel();
}

void SetlogFeature::slotPlayingTrackChanged(TrackPointer currentPlayingTrack) {
    if (!currentPlayingTrack) {
        return;
    }

    TrackId currentPlayingTrackId(currentPlayingTrack->getId());
    bool track_played_recently = false;
    if (currentPlayingTrackId.isValid()) {
        // Remove the track from the recent tracks list if it's present and put
        // at the front of the list.
        const auto it = std::find(
                m_recentTracks.cbegin(),
                m_recentTracks.cend(),
                currentPlayingTrackId);
        if (it == m_recentTracks.cend()) {
            track_played_recently = false;
        } else {
            track_played_recently = true;
            constErase(&m_recentTracks, it);
        }
        m_recentTracks.push_front(currentPlayingTrackId);

        // Keep a window of 6 tracks (inspired by 2 decks, 4 samplers)
        const unsigned int recentTrackWindow = m_pConfig->getValue(
                kHistoryTrackDuplicateDistanceConfigKey,
                kHistoryTrackDuplicateDistanceDefault);
        while (m_recentTracks.size() > recentTrackWindow) {
            m_recentTracks.pop_back();
        }
    }

    // If the track was recently played, don't increment the playcount or
    // add it to the history.
    if (track_played_recently) {
        return;
    }

    // If the track is not present in the recent tracks list, mark it
    // played and update its playcount.
    currentPlayingTrack->updatePlayCounter();

    // A track played off a drive belongs to that drive's history, which lives
    // on the drive itself — no library id needed, because the store keys on the
    // path relative to the mount root.
    const QString mountRoot = mountRootForLocation(currentPlayingTrack->getLocation());
    if (mountRoot.isEmpty()) {
        // Played off this unit's own storage: it has no stick to be written to,
        // and nothing of it is kept here. The play counter above is the whole
        // record of it.
        return;
    }
    logTrackToDrive(mountRoot, currentPlayingTrack);
}

void SetlogFeature::slotPlaylistTableChanged(int playlistId) {
    // No item in this tree is backed by a playlist: the sidebar is built from
    // the mounted drives, and the only playlist this feature owns is the hidden
    // scratch one the drive sessions are shown through.
    Q_UNUSED(playlistId);
}

void SetlogFeature::slotPlaylistContentOrLockChanged(const QSet<int>& playlistIds) {
    Q_UNUSED(playlistIds);
}

void SetlogFeature::slotPlaylistTableRenamed(int playlistId, const QString& newName) {
    Q_UNUSED(playlistId);
    Q_UNUSED(newName);
}

void SetlogFeature::activate() {
    // The root has nothing of its own to show: this unit keeps no history, so
    // the sets are all one level down, under the drive they were played from.
    m_lastClickedIndex = m_pSidebarModel->getRootIndex();
    m_lastRightClickedIndex = QModelIndex();
    m_shownMountRoot.clear();
    m_shownSessionName.clear();
    m_driveAwaitingActivation.clear();
    BaseTrackSetFeature::activate();
}

void SetlogFeature::activateChild(const QModelIndex& index) {
    // qDebug() << "SetlogFeature::activateChild()" << index;
    TreeItem* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (!pItem) {
        return;
    }
    const QVariant itemData = pItem->getData();

    QString mountRoot;
    QString sessionName;
    if (parseSessionNodeData(itemData, &mountRoot, &sessionName)) {
        showDriveSession(mountRoot, sessionName, index);
        return;
    }
    if (parseVolumeNodeData(itemData, &mountRoot)) {
        // A drive shows its most recent session, or an empty view when it
        // carries no history yet. From this unit's copy of the drive; a drive
        // it has not read yet (clicked in the moment between the stick
        // appearing and being read) opens its set from slotSessionsRead().
        const QList<FsHistorySession> sessions = m_sessionsByMount.value(mountRoot);
        const bool driveHasBeenRead = m_sessionsByMount.contains(mountRoot);
        showDriveSession(mountRoot,
                sessions.isEmpty() ? QString() : sessions.first().name,
                index);
        if (!driveHasBeenRead) {
            m_driveAwaitingActivation = mountRoot;
        }
        return;
    }

    // Anything else is not a real entry.
}

QString SetlogFeature::getRootViewHtml() const {
    const QString title = tr("History");
    const QString summary =
            tr("Every set is stored on the USB drive it was played from. "
               "Pick a drive to see the sets it carries.");
    return QStringLiteral("<h2>%1</h2><p>%2</p>").arg(title, summary);
}
