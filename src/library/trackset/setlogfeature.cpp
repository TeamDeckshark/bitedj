#include "library/trackset/setlogfeature.h"

#include <QDateTime>
#include <QDir>
#include <QMenu>
#include <QMessageBox>
#include <QSqlTableModel>

#include "library/dao/fshistorystore.h"
#include "library/library.h"
#include "library/library_prefs.h"
#include "library/playlisttablemodel.h"
#include "library/queryutil.h"
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
const QString kLocalNodeData = QStringLiteral("bitedj:history:local");
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
          m_currentPlaylistId(kInvalidPlaylistId),
          m_driveViewPlaylistId(kInvalidPlaylistId),
          m_pLibrary(pLibrary),
          m_pConfig(pConfig) {
    // remove unneeded entries
    deleteAllUnlockedPlaylistsWithFewerTracks();

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
    constructChildModel(kInvalidPlaylistId);

    m_pJoinWithPreviousAction = new QAction(tr("Join with previous (below)"), this);
    connect(m_pJoinWithPreviousAction,
            &QAction::triggered,
            this,
            &SetlogFeature::slotJoinWithPrevious);

    m_pMarkTracksPlayedAction = new QAction(tr("Mark all tracks played"), this);
    connect(m_pMarkTracksPlayedAction,
            &QAction::triggered,
            this,
            &SetlogFeature::slotMarkAllTracksPlayed);

    m_pStartNewPlaylist = new QAction(tr("Finish current and start new"), this);
    connect(m_pStartNewPlaylist,
            &QAction::triggered,
            this,
            &SetlogFeature::slotGetNewPlaylist);

    m_pLockAllChildPlaylists = new QAction(tr("Lock all child playlists"), this);
    connect(m_pLockAllChildPlaylists,
            &QAction::triggered,
            this,
            &SetlogFeature::slotLockAllChildPlaylists);

    m_pUnlockAllChildPlaylists = new QAction(tr("Unlock all child playlists"), this);
    connect(m_pUnlockAllChildPlaylists,
            &QAction::triggered,
            this,
            &SetlogFeature::slotUnlockAllChildPlaylists);

    m_pDeleteAllChildPlaylists = new QAction(tr("Delete all unlocked child playlists"), this);
    connect(m_pDeleteAllChildPlaylists,
            &QAction::triggered,
            this,
            &SetlogFeature::slotDeleteAllUnlockedChildPlaylists);

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
    m_usbPollTimer.setInterval(kUsbPollIntervalMillis);
    m_usbPollTimer.setSingleShot(false);
    connect(&m_usbPollTimer,
            &QTimer::timeout,
            this,
            &SetlogFeature::slotRefreshUsbVolumes);
    m_usbPollTimer.start();

    // initialized in a new generic slot(get new history playlist purpose)
    slotGetNewPlaylist();
}

SetlogFeature::~SetlogFeature() {
    // Clean up history when shutting down in case the track threshold changed,
    // incl. potentially empty current playlist
    deleteAllUnlockedPlaylistsWithFewerTracks();
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

void SetlogFeature::deleteAllUnlockedPlaylistsWithFewerTracks() {
    ScopedTransaction transaction(m_pLibrary->trackCollectionManager()
                                          ->internalCollection()
                                          ->database());
    int minTrackCount = m_pConfig->getValue(
            kHistoryMinTracksToKeepConfigKey,
            kHistoryMinTracksToKeepDefault);
    m_playlistDao.deleteAllUnlockedPlaylistsWithFewerTracks(PlaylistDAO::PLHT_SET_LOG,
            minTrackCount);
    transaction.commit();
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
        if (!FsHistoryStore::deleteSession(mountRoot, sessionName)) {
            return;
        }
        if (m_currentSessionByMount.value(mountRoot) == sessionName) {
            m_currentSessionByMount.remove(mountRoot);
        }
        if (m_shownMountRoot == mountRoot && m_shownSessionName == sessionName) {
            showDriveSession(mountRoot, QString(), QModelIndex());
        }
        constructChildModel(kInvalidPlaylistId);
        return;
    }

    int playlistId = playlistIdFromIndex(m_lastRightClickedIndex);
    if (playlistId == kInvalidPlaylistId || playlistId == m_currentPlaylistId) {
        // the current setlog must not be deleted, and neither a volume nor the
        // "This Unit" node is a playlist
        return;
    }
    // regular setlog, call the base implementation
    BasePlaylistFeature::slotDeletePlaylist();
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
    } else if (itemData.toString() == kLocalNodeData) {
        menu.addAction(m_pLockAllChildPlaylists);
        menu.addAction(m_pUnlockAllChildPlaylists);
        menu.addSeparator();
        menu.addAction(m_pDeleteAllChildPlaylists);
    } else {
        int playlistId = playlistIdFromIndex(index);
        // not a real entry
        if (playlistId == kInvalidPlaylistId) {
            return;
        }
        // this is a playlist
        bool locked = m_playlistDao.isPlaylistLocked(playlistId);
        m_pDeletePlaylistAction->setEnabled(!locked);
        m_pRenamePlaylistAction->setEnabled(!locked);
        m_pJoinWithPreviousAction->setEnabled(!locked);
        m_pLockPlaylistAction->setText(locked ? tr("Unlock") : tr("Lock"));

        menu.addAction(m_pAddToAutoDJAction);
        menu.addAction(m_pAddToAutoDJTopAction);
        menu.addSeparator();
        menu.addAction(m_pRenamePlaylistAction);
        if (playlistId != m_currentPlaylistId) {
            // Todays playlist should not be locked or deleted
            menu.addAction(m_pDeletePlaylistAction);
            menu.addAction(m_pLockPlaylistAction);
            menu.addAction(m_pMarkTracksPlayedAction);
        }
        if (index.sibling(index.row() + 1, index.column()).isValid()) {
            // The very first (oldest) setlog cannot be joint
            menu.addAction(m_pJoinWithPreviousAction);
        }
        if (playlistId == m_currentPlaylistId) {
            // Todays playlists can change !
            m_pStartNewPlaylist->setEnabled(
                    m_playlistDao.tracksInPlaylist(m_currentPlaylistId) > 0);
            menu.addAction(m_pStartNewPlaylist);
        }
        menu.addSeparator();
        menu.addAction(m_pExportPlaylistAction);
        menu.addAction(m_pExportTrackFilesAction);
    }

    menu.exec(globalPos);
}

/// Purpose: When inserting or removing playlists,
/// we require the sidebar model not to reset.
/// This method queries the database and does dynamic insertion
///
/// The top level is the drives: one node per mounted USB volume, holding the
/// sessions that drive carries, plus a fixed "This Unit" node holding the
/// setlog playlists of tracks that were not played off a removable drive.
/// @param selectedId row which should be selected
QModelIndex SetlogFeature::constructChildModel(int selectedId) {
    // qDebug() << "SetlogFeature::constructChildModel() selected:" << selectedId;
    // Setup the sidebar playlist model
    QSqlDatabase database =
            m_pLibrary->trackCollectionManager()->internalCollection()->database();

    QString queryString = QStringLiteral(
            "CREATE TEMPORARY VIEW IF NOT EXISTS %1 "
            "AS SELECT "
            "  Playlists.id AS id, "
            "  Playlists.name AS name, "
            "  Playlists.date_created AS date_created, "
            "  LOWER(Playlists.name) AS sort_name, "
            "  max(PlaylistTracks.position) AS count,"
            "  SUM(library.duration) AS durationSeconds "
            "FROM Playlists "
            "LEFT JOIN PlaylistTracks "
            "  ON PlaylistTracks.playlist_id = Playlists.id "
            "LEFT JOIN library "
            "  ON PlaylistTracks.track_id = library.id "
            "  WHERE Playlists.hidden = %2 "
            "  GROUP BY Playlists.id")
                                  .arg(m_countsDurationTableName,
                                          QString::number(PlaylistDAO::PLHT_SET_LOG));
    ;
    queryString.append(
            mixxx::DbConnection::collateLexicographically(
                    " ORDER BY sort_name"));
    QSqlQuery query(database);
    if (!query.exec(queryString)) {
        LOG_FAILED_QUERY(query);
    }

    // Setup the sidebar playlist model
    QSqlTableModel playlistTableModel(this, database);
    playlistTableModel.setTable(m_countsDurationTableName);
    playlistTableModel.setSort(playlistTableModel.fieldIndex("id"), Qt::DescendingOrder);
    playlistTableModel.select();
    while (playlistTableModel.canFetchMore()) {
        playlistTableModel.fetchMore();
    }
    QSqlRecord record = playlistTableModel.record();
    int nameColumn = record.indexOf("name");
    int idColumn = record.indexOf("id");
    int countColumn = record.indexOf("count");
    int durationColumn = record.indexOf("durationSeconds");

    clearChildModel();
    std::vector<std::unique_ptr<TreeItem>> itemList;
    itemList.reserve(m_usbMountPoints.size() + 1);

    // The drives, newest session first under each of them.
    for (const QString& mountRoot : std::as_const(m_usbMountPoints)) {
        auto pVolumeItem = std::make_unique<TreeItem>(
                volumeLabel(mountRoot), volumeNodeData(mountRoot));
        pVolumeItem->setIcon(QIcon(kDriveIcon));

        QList<FsHistorySession> sessions;
        if (FsHistoryStore::readSessions(mountRoot, &sessions)) {
            const QString currentSession = m_currentSessionByMount.value(mountRoot);
            for (const FsHistorySession& session : std::as_const(sessions)) {
                TreeItem* pSessionItem = pVolumeItem->appendChild(
                        createPlaylistLabel(session.name,
                                session.trackCount,
                                session.durationSeconds),
                        sessionNodeData(mountRoot, session.name));
                if (session.name == currentSession) {
                    pSessionItem->setIcon(QIcon(kCurrentSessionIcon));
                }
            }
        }
        itemList.push_back(std::move(pVolumeItem));
    }

    // Everything this unit logged for itself.
    auto pLocalItem = std::make_unique<TreeItem>(tr("This Unit"), kLocalNodeData);
    for (int row = 0; row < playlistTableModel.rowCount(); ++row) {
        int id =
                playlistTableModel
                        .data(playlistTableModel.index(row, idColumn))
                        .toInt();
        QString name =
                playlistTableModel
                        .data(playlistTableModel.index(row, nameColumn))
                        .toString();
        int count = playlistTableModel
                            .data(playlistTableModel.index(row, countColumn))
                            .toInt();
        int duration =
                playlistTableModel
                        .data(playlistTableModel.index(row, durationColumn))
                        .toInt();

        TreeItem* pItem = pLocalItem->appendChild(
                createPlaylistLabel(name, count, duration), id);
        pItem->setBold(m_playlistIdsOfSelectedTrack.contains(id));
        decorateChild(pItem, id);
    }
    itemList.push_back(std::move(pLocalItem));

    // Append all the newly created TreeItems in a dynamic way to the childmodel
    m_pSidebarModel->insertTreeItemRows(std::move(itemList), 0);

    return indexFromPlaylistId(selectedId);
}

void SetlogFeature::decorateChild(TreeItem* item, int playlistId) {
    if (playlistId == m_currentPlaylistId) {
        item->setIcon(QIcon(kCurrentSessionIcon));
    } else if (m_playlistDao.isPlaylistLocked(playlistId)) {
        item->setIcon(QIcon(":/images/library/ic_library_locked.svg"));
    } else {
        item->setIcon(QIcon());
    }
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

QString SetlogFeature::currentSessionOnDrive(const QString& mountRoot) {
    const auto it = m_currentSessionByMount.constFind(mountRoot);
    if (it != m_currentSessionByMount.constEnd()) {
        return it.value();
    }
    // First track off this drive since it was plugged in: that is where one set
    // ends and the next begins.
    const QString sessionName = FsHistoryStore::newSessionName(mountRoot);
    if (sessionName.isEmpty()) {
        // Unavailable or write-protected. Nothing to log to, and nothing worth
        // saying about it on every track change.
        return QString();
    }
    m_currentSessionByMount.insert(mountRoot, sessionName);
    return sessionName;
}

void SetlogFeature::logTrackToDrive(const QString& mountRoot, const TrackPointer& pTrack) {
    const QString sessionName = currentSessionOnDrive(mountRoot);
    if (sessionName.isEmpty()) {
        return;
    }
    if (!FsHistoryStore::appendTrack(mountRoot,
                sessionName,
                pTrack->getLocation(),
                static_cast<int>(pTrack->getDuration()))) {
        return;
    }

    updateDriveSessionItem(mountRoot, sessionName);

    if (m_shownMountRoot != mountRoot || m_shownSessionName != sessionName) {
        return;
    }
    // The session on screen just grew. Keep the selection the DJ may be working
    // with (see the same dance in the local branch of slotPlayingTrackChanged).
    const TrackId trackId = pTrack->getId();
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
        const QString& mountRoot, const QString& sessionName) {
    const QModelIndex volumeIndex = indexOfVolumeNode(mountRoot);
    if (!volumeIndex.isValid()) {
        return;
    }
    TreeItem* pVolumeItem = m_pSidebarModel->getItem(volumeIndex);
    if (!pVolumeItem) {
        return;
    }

    FsHistorySession session;
    if (!FsHistoryStore::readSessionSummary(mountRoot, sessionName, &session)) {
        return;
    }
    const QString label = createPlaylistLabel(
            session.name, session.trackCount, session.durationSeconds);
    const QString itemData = sessionNodeData(mountRoot, sessionName);

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
    if (index.isValid()) {
        m_lastClickedIndex = index;
        m_lastRightClickedIndex = QModelIndex();
    }

    QList<TrackId> trackIds;
    if (!sessionName.isEmpty()) {
        QStringList locations;
        if (FsHistoryStore::readSessionTracks(mountRoot, sessionName, &locations)) {
            trackIds.reserve(locations.size());
            for (const QString& location : std::as_const(locations)) {
                // The stored paths are relative to the drive, so they resolve
                // here even when the stick was played on another unit. A track
                // that is no longer on it simply drops out of the view.
                const TrackPointer pTrack =
                        m_pLibrary->trackCollectionManager()->getOrAddTrack(
                                TrackRef::fromFilePath(location));
                if (pTrack && pTrack->getId().isValid()) {
                    trackIds.append(pTrack->getId());
                }
            }
        }
    }

    fillDriveViewPlaylist(trackIds);
    m_shownMountRoot = mountRoot;
    m_shownSessionName = sessionName;

    emit saveModelState();
    m_pPlaylistTableModel->selectPlaylist(m_driveViewPlaylistId);
    emit showTrackModel(m_pPlaylistTableModel);
    emit enableCoverArtDisplay(true);
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
            it = m_currentSessionByMount.erase(it);
        }
    }
    if (!m_shownMountRoot.isEmpty() && !mountPoints.contains(m_shownMountRoot)) {
        forgetShownDriveSession();
    }

    constructChildModel(kInvalidPlaylistId);
}

void SetlogFeature::forgetShownDriveSession() {
    m_shownMountRoot.clear();
    m_shownSessionName.clear();
    // Empty the view as well. Library::slotMountEjected only dismisses a view
    // that is reading the dead mount directly (a browse listing), and a session
    // is shown through a playlist, so the rows would otherwise sit there naming
    // files that are no longer on the box.
    fillDriveViewPlaylist({});
}

void SetlogFeature::slotMountEjected(const QString& mountPoint) {
    const QString cleaned = QDir::cleanPath(mountPoint);
    m_currentSessionByMount.remove(cleaned);
    if (m_shownMountRoot == cleaned) {
        forgetShownDriveSession();
    }
    if (m_usbMountPoints.removeAll(cleaned) > 0) {
        constructChildModel(kInvalidPlaylistId);
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
    // that never happens leaves no empty session behind.
    if (m_currentSessionByMount.remove(mountRoot) > 0) {
        constructChildModel(kInvalidPlaylistId);
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

    if (!FsHistoryStore::clearFilesystemHistory(mountRoot)) {
        return;
    }
    m_currentSessionByMount.remove(mountRoot);
    if (m_shownMountRoot == mountRoot) {
        showDriveSession(mountRoot, QString(), QModelIndex());
    }
    constructChildModel(kInvalidPlaylistId);
}

/// Invoked on startup to create new current playlist and by "Finish current and start new"
void SetlogFeature::slotGetNewPlaylist() {
    //qDebug() << "slotGetNewPlaylist() successfully triggered !";

    // create a new playlist for today
    QString set_log_name_format;
    QString set_log_name;

    set_log_name = QDate::currentDate().toString(Qt::ISODate);
    set_log_name_format = set_log_name + " #%1";
    int i = 1;

    // calculate name of the todays setlog
    while (m_playlistDao.getPlaylistIdFromName(set_log_name) != kInvalidPlaylistId) {
        set_log_name = set_log_name_format.arg(++i);
    }

    //qDebug() << "Creating session history playlist name:" << set_log_name;
    m_currentPlaylistId = m_playlistDao.createPlaylist(
            set_log_name, PlaylistDAO::PLHT_SET_LOG);

    if (m_currentPlaylistId == kInvalidPlaylistId) {
        qDebug() << "Setlog playlist Creation Failed";
        qDebug() << "An unknown error occurred while creating playlist: "
                 << set_log_name;
    } else {
        m_recentTracks.clear();
        m_playlistDao.setCurrentHistoryPlaylistId(m_currentPlaylistId);
    }

    // reload child model again because the 'added' signal fired by PlaylistDAO
    // might have triggered slotPlaylistTableChanged() before m_currentPlaylistId was set,
    // which causes the wrong playlist being decorated as 'current'
    slotPlaylistTableChanged(m_currentPlaylistId);
}

void SetlogFeature::slotJoinWithPrevious() {
    // qDebug() << "SetlogFeature::slotJoinWithPrevious() row:" << m_lastRightClickedIndex.data();
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }

    int clickedPlaylistId = playlistIdFromIndex(m_lastRightClickedIndex);
    if (clickedPlaylistId == kInvalidPlaylistId) {
        return;
    }

    if (m_playlistDao.isPlaylistLocked(clickedPlaylistId)) {
        qDebug() << "Aborting playlist join because playlist"
                 << clickedPlaylistId << "is locked.";
        return;
    }

    // Add every track from right-clicked playlist to that with the next smaller ID
    int previousPlaylistId = m_playlistDao.getPreviousPlaylist(
            clickedPlaylistId, PlaylistDAO::PLHT_SET_LOG);
    if (previousPlaylistId == kInvalidPlaylistId) {
        qDebug() << "Aborting playlist join because there's no previous playlist"
                    " for playlist"
                 << clickedPlaylistId;
        return;
    }
    if (m_playlistDao.isPlaylistLocked(previousPlaylistId)) {
        qDebug() << "Aborting playlist join because previous playlist"
                 << previousPlaylistId << "is locked.";
        return;
    }

    // Right-clicked playlist may not be loaded. Use a temporary model to
    // keep sidebar selection and table view in sync
    std::unique_ptr<PlaylistTableModel> pPlaylistTableModel =
            std::make_unique<PlaylistTableModel>(this,
                    m_pLibrary->trackCollectionManager(),
                    "mixxx.db.model.playlist_export");
    pPlaylistTableModel->selectPlaylist(previousPlaylistId);

    if (clickedPlaylistId == m_currentPlaylistId) {
        // mark all the Tracks in the previous Playlist as played
        pPlaylistTableModel->select();
        int rows = pPlaylistTableModel->rowCount();
        for (int i = 0; i < rows; ++i) {
            QModelIndex index = pPlaylistTableModel->index(i, 0);
            if (index.isValid()) {
                TrackPointer pTrack = pPlaylistTableModel->getTrack(index);
                DEBUG_ASSERT(pTrack != nullptr);
                // Do not update the play count, just set played status.
                pTrack->updatePlayedStatusKeepPlayCount(true);
            }
        }

        // Change current setlog
        m_currentPlaylistId = previousPlaylistId;
        m_playlistDao.setCurrentHistoryPlaylistId(m_currentPlaylistId);
    }
    qDebug() << "slotJoinWithPrevious() current:"
             << clickedPlaylistId
             << " previous:" << previousPlaylistId;
    if (m_playlistDao.copyPlaylistTracks(clickedPlaylistId, previousPlaylistId)) {
        m_playlistDao.deletePlaylist(clickedPlaylistId);
    }
}

void SetlogFeature::slotMarkAllTracksPlayed() {
    // qDebug() << "SetlogFeature::slotMarkAllTracksPlayed()";
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }

    int clickedPlaylistId = playlistIdFromIndex(m_lastRightClickedIndex);
    if (clickedPlaylistId == kInvalidPlaylistId) {
        return;
    }

    if (clickedPlaylistId == m_currentPlaylistId) {
        return;
    }

    // Right-clicked playlist may not be loaded. Use a temporary model to
    // keep sidebar selection and table view in sync
    std::unique_ptr<PlaylistTableModel> pPlaylistTableModel =
            std::make_unique<PlaylistTableModel>(this,
                    m_pLibrary->trackCollectionManager(),
                    "mixxx.db.model.playlist_export");
    pPlaylistTableModel->selectPlaylist(clickedPlaylistId);
    // mark all the Tracks in the previous Playlist as played
    pPlaylistTableModel->select();
    int rows = pPlaylistTableModel->rowCount();
    for (int i = 0; i < rows; ++i) {
        QModelIndex index = pPlaylistTableModel->index(i, 0);
        if (index.isValid()) {
            TrackPointer pTrack = pPlaylistTableModel->getTrack(index);
            DEBUG_ASSERT(pTrack != nullptr);
            // Do not update the play count, just set played status.
            pTrack->updatePlayedStatusKeepPlayCount(true);
        }
    }
}

void SetlogFeature::slotLockAllChildPlaylists() {
    lockOrUnlockAllChildPlaylists(true);
}

void SetlogFeature::slotUnlockAllChildPlaylists() {
    lockOrUnlockAllChildPlaylists(false);
}

void SetlogFeature::lockOrUnlockAllChildPlaylists(bool lock) {
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }
    if (lock) {
        qWarning() << "lock all child playlists of" << m_lastRightClickedIndex.data().toString();
    } else {
        qWarning() << "unlock all child playlists of" << m_lastRightClickedIndex.data().toString();
    }
    TreeItem* item = static_cast<TreeItem*>(m_lastRightClickedIndex.internalPointer());
    if (!item) {
        return;
    }
    const QList<TreeItem*> children = item->children();
    if (children.isEmpty()) {
        return;
    }

    QSet<int> ids;
    for (const auto& pChild : children) {
        bool ok = false;
        int childId = pChild->getData().toInt(&ok);
        if (ok && childId != kInvalidPlaylistId) {
            ids.insert(childId);
        }
    }
    m_playlistDao.setPlaylistsLocked(ids, lock);
}

void SetlogFeature::slotDeleteAllUnlockedChildPlaylists() {
    if (!m_lastRightClickedIndex.isValid()) {
        return;
    }
    TreeItem* item = static_cast<TreeItem*>(m_lastRightClickedIndex.internalPointer());
    if (!item) {
        return;
    }
    const QList<TreeItem*> children = item->children();
    if (children.isEmpty()) {
        return;
    }
    QString parentName = m_lastRightClickedIndex.data().toString();

    QMessageBox::StandardButton btn = QMessageBox::question(nullptr,
            tr("Confirm Deletion"),
            //: %1 is the name of the parent sidebar item
            //: <b> + </b> are used to make the text in between bold in the popup
            //: <br> is a linebreak
            tr("Do you really want to delete all unlocked playlist from <b>%1</b>?<br><br>")
                    .arg(parentName),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
    if (btn != QMessageBox::Yes) {
        return;
    }

    QStringList ids;
    int count = 0;
    for (const auto& pChild : children) {
        bool ok = false;
        int childId = pChild->getData().toInt(&ok);
        if (ok && childId != kInvalidPlaylistId) {
            ids.append(pChild->getData().toString());
            count++;
        }
    }
    // Double-check, this is a weighty decision
    btn = QMessageBox::warning(nullptr,
            tr("Confirm Deletion"),
            //: %1 is the number of playlists to be deleted
            //: %2 is the name of the parent sidebar item
            //: <b> + </b> are used to make the text in between bold in the popup
            //: <br> is a linebreak
            tr("Deleting %1 playlists from <b>%2</b>.<br><br>")
                    .arg(QString::number(count), parentName),
            QMessageBox::Ok | QMessageBox::Cancel,
            QMessageBox::Cancel);
    if (btn != QMessageBox::Ok) {
        return;
    }
    qDebug() << "History: deleting all unlocked playlists of" << parentName;
    m_playlistDao.deleteUnlockedPlaylists(std::move(ids));
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
    if (!mountRoot.isEmpty()) {
        logTrackToDrive(mountRoot, currentPlayingTrack);
        return;
    }

    // We can only add tracks that are Mixxx library tracks, not external
    // sources.
    if (!currentPlayingTrackId.isValid()) {
        return;
    }

    if (m_pPlaylistTableModel->getPlaylist() == m_currentPlaylistId) {
        // View needs a refresh

        bool hasActiveView = false;
        if (m_pLibraryWidget) {
            WTrackTableView* view = dynamic_cast<WTrackTableView*>(
                    m_pLibraryWidget->getActiveView());
            if (view != nullptr) {
                // We have a active view on the history. The user may have some
                // important active selection. For example putting track into crates
                // while the song changes through autodj. The selection is then lost
                // and dataloss occurs
                hasActiveView = true;
                const QList<TrackId> trackIds = view->getSelectedTrackIds();
                m_pPlaylistTableModel->appendTrack(currentPlayingTrackId);
                view->setSelectedTracks(trackIds);
            }
        }

        if (!hasActiveView) {
            m_pPlaylistTableModel->appendTrack(currentPlayingTrackId);
        }
    } else {
        // TODO(XXX): Care whether the append succeeded.
        m_playlistDao.appendTrackToPlaylist(
                currentPlayingTrackId, m_currentPlaylistId);
    }
}

void SetlogFeature::slotPlaylistTableChanged(int playlistId) {
    // qDebug() << "SetlogFeature::slotPlaylistTableChanged() id:" << playlistId;
    PlaylistDAO::HiddenType type = m_playlistDao.getHiddenType(playlistId);
    if (type != PlaylistDAO::PLHT_SET_LOG &&
            type != PlaylistDAO::PLHT_UNKNOWN) { // deleted Playlist
        return;
    }

    // save currently selected History sidebar item (if any)
    int selectedPlaylistId = kInvalidPlaylistId;
    QVariant selectedItemData;
    bool rootWasSelected = false;
    if (isChildIndexSelectedInSidebar(m_lastClickedIndex)) {
        int lastClickedPlaylistId = playlistIdFromIndex(m_lastClickedIndex);
        if (lastClickedPlaylistId == kInvalidPlaylistId) {
            // A drive, one of its sessions or the "This Unit" node: not a
            // playlist, so it is restored by its payload instead of by id.
            TreeItem* pItem = static_cast<TreeItem*>(m_lastClickedIndex.internalPointer());
            if (pItem) {
                selectedItemData = pItem->getData();
            }
        } else if (playlistId == lastClickedPlaylistId &&
                type == PlaylistDAO::PLHT_UNKNOWN) {
            // selected playlist was deleted, find a sibling.
            // prev/next works here because history playlists are always
            // sorted by date of creation.
            selectedPlaylistId = m_playlistDao.getPreviousPlaylist(
                    lastClickedPlaylistId,
                    PlaylistDAO::PLHT_SET_LOG);
            if (selectedPlaylistId == kInvalidPlaylistId) {
                // no previous playlist, try to get the next playlist
                selectedPlaylistId = m_playlistDao.getNextPlaylist(
                        lastClickedPlaylistId,
                        PlaylistDAO::PLHT_SET_LOG);
            }
        } else {
            selectedPlaylistId = lastClickedPlaylistId;
        }
    } else {
        rootWasSelected = m_pSidebarWidget &&
                m_pSidebarWidget->isFeatureRootIndexSelected(this);
    }

    QModelIndex newIndex = constructChildModel(selectedPlaylistId);

    if (selectedItemData.isValid()) {
        // Re-select the drive item that was selected, without re-reading its
        // session: nothing about it changed, only the playlists below it did.
        newIndex = indexOfItemData(selectedItemData);
        if (newIndex.isValid()) {
            m_lastClickedIndex = newIndex;
            emit featureSelect(this, newIndex);
        }
        return;
    }
    if (newIndex.isValid()) {
        emit featureSelect(this, newIndex);
        activateChild(newIndex);
    } else if (rootWasSelected) {
        // calling featureSelect with invalid index will select the root item
        emit featureSelect(this, newIndex);
        activate(); // to reload the new current playlist
    }
}

void SetlogFeature::slotPlaylistContentOrLockChanged(const QSet<int>& playlistIds) {
    // qDebug() << "SetlogFeature::slotPlaylistContentOrLockChanged() for"
    //          << playlistIds.count() << "playlist(s)";
    QSet<int> idsToBeUpdated;
    for (const auto playlistId : std::as_const(playlistIds)) {
        if (m_playlistDao.getHiddenType(playlistId) == PlaylistDAO::PLHT_SET_LOG) {
            idsToBeUpdated.insert(playlistId);
        }
    }
    updateChildModel(idsToBeUpdated);
}

void SetlogFeature::slotPlaylistTableRenamed(int playlistId, const QString& newName) {
    Q_UNUSED(newName);
    // qDebug() << "SetlogFeature::slotPlaylistTableRenamed() Id:" << playlistId;
    if (m_playlistDao.getHiddenType(playlistId) == PlaylistDAO::PLHT_SET_LOG) {
        updateChildModel(QSet<int>{playlistId});
    }
}

void SetlogFeature::activate() {
    // The root item was clicked, so activate the current playlist of this unit.
    m_lastClickedIndex = m_pSidebarModel->getRootIndex();
    m_lastRightClickedIndex = QModelIndex();
    activatePlaylist(m_currentPlaylistId);
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
        // carries no history yet.
        QList<FsHistorySession> sessions;
        if (FsHistoryStore::readSessions(mountRoot, &sessions) && !sessions.isEmpty()) {
            showDriveSession(mountRoot, sessions.first().name, index);
        } else {
            showDriveSession(mountRoot, QString(), index);
        }
        return;
    }

    m_shownMountRoot.clear();
    m_shownSessionName.clear();

    if (itemData.toString() == kLocalNodeData) {
        // Same as clicking the root used to do: show what this unit is logging
        // right now.
        m_lastClickedIndex = index;
        m_lastRightClickedIndex = QModelIndex();
        if (m_currentPlaylistId == kInvalidPlaylistId) {
            return;
        }
        emit saveModelState();
        m_pPlaylistTableModel->selectPlaylist(m_currentPlaylistId);
        emit showTrackModel(m_pPlaylistTableModel);
        emit enableCoverArtDisplay(true);
        return;
    }

    int playlistId = playlistIdFromIndex(index);
    if (playlistId == kInvalidPlaylistId) {
        // may happen during initialization
        return;
    }
    m_lastClickedIndex = index;
    m_lastRightClickedIndex = QModelIndex();
    emit saveModelState();
    m_pPlaylistTableModel->selectPlaylist(playlistId);
    emit showTrackModel(m_pPlaylistTableModel);
    emit enableCoverArtDisplay(true);
}

void SetlogFeature::activatePlaylist(int playlistId) {
    // qDebug() << "SetlogFeature::activatePlaylist()" << playlistId;
    if (playlistId == kInvalidPlaylistId) {
        return;
    }
    QModelIndex index = indexFromPlaylistId(playlistId);
    VERIFY_OR_DEBUG_ASSERT(index.isValid()) {
        return;
    }
    m_shownMountRoot.clear();
    m_shownSessionName.clear();
    emit saveModelState();
    m_pPlaylistTableModel->selectPlaylist(playlistId);
    emit showTrackModel(m_pPlaylistTableModel);
    // Update sidebar selection only if this is a child, incl. current playlist.
    // indexFromPlaylistId() can't be used because, in case the root item was
    // selected, that would switch to the 'current' child.
    if (m_lastClickedIndex != m_pSidebarModel->getRootIndex()) {
        m_lastClickedIndex = index;
        m_lastRightClickedIndex = QModelIndex();
        emit featureSelect(this, index);
    }
    emit enableCoverArtDisplay(true);
}

QString SetlogFeature::getRootViewHtml() const {
    // Instead of the help text, the history shows the current playlist
    return QString();
}
