#pragma once

#include <QFont>
#include <QList>
#include <QObject>
#include <QPointer>

#include "analyzer/trackanalysisscheduler.h"
#include "library/library_decl.h"
#ifdef __ENGINEPRIME__
#include "library/trackset/crate/crateid.h"
#endif
#include "preferences/usersettings.h"
#include "track/track_decl.h"
#include "util/db/dbconnectionpool.h"
#include "util/parented_ptr.h"

class AnalysisFeature;
class BrowseFeature;
class ControlObject;
class ControlPushButton;
class CrateFeature;
class LibraryColumnControl;
class LibraryControl;
class LibraryFeature;
class LibraryTableModel;
class KeyboardEventFilter;
class MixxxLibraryFeature;
class PlayerManager;
class PlaylistFeature;
class RecordingManager;
class SidebarModel;
class TrackCollectionManager;
class WSearchLineEdit;
class WLibrarySidebar;
class WLibrary;
class QAbstractItemModel;

#ifdef __ENGINEPRIME__
namespace mixxx {
class LibraryExporter;
} // namespace mixxx
#endif

// A Library class is a container for all the model-side aspects of the library.
// A library widget can be attached to the Library object by calling bindLibraryWidget.
class Library: public QObject {
    Q_OBJECT

  public:
    Library(QObject* parent,
            UserSettingsPointer pConfig,
            mixxx::DbConnectionPoolPtr pDbConnectionPool,
            TrackCollectionManager* pTrackCollectionManager,
            PlayerManager* pPlayerManager,
            RecordingManager* pRecordingManager);
    ~Library() override;

    void stopPendingTasks();

    const mixxx::DbConnectionPoolPtr& dbConnectionPool() const {
        return m_pDbConnectionPool;
    }

    TrackCollectionManager* trackCollectionManager() const;

    TrackAnalysisScheduler::Pointer createTrackAnalysisScheduler(
            int numWorkerThreads,
            AnalyzerModeFlags modeFlags) const;

    void bindSearchboxWidget(WSearchLineEdit* pSearchboxWidget);
    void bindSidebarWidget(WLibrarySidebar* sidebarWidget);
    void bindLibraryWidget(WLibrary* libraryWidget,
                    KeyboardEventFilter* pKeyboard);

    void addFeature(LibraryFeature* feature);

    /// Needed for exposing models to QML
    LibraryTableModel* trackTableModel() const;

    bool isTrackIdInCurrentLibraryView(const TrackId& trackId);

    int getTrackTableRowHeight() const {
        return m_iTrackTableRowHeight;
    }

    const QFont& getTrackTableFont() const {
        return m_trackTableFont;
    }

    bool selectedClickEnabled() const {
        return m_editMetadataSelectedClick;
    }

    //static Library* buildDefaultLibrary();

    static const int kDefaultRowHeightPx;

    void setFont(const QFont& font);
    void setRowHeight(int rowHeight);
    void setEditMetadataSelectedClick(bool enable);

    /// Triggers a new search in the internal track collection
    /// and shows the results by switching the view.
    void searchTracksInCollection(const QString& query);

    bool requestAddDir(const QString& directory);
    bool requestRemoveDir(const QString& directory, LibraryRemovalType removalType);
    bool requestRelocateDir(const QString& previousDirectory, const QString& newDirectory);

#ifdef __ENGINEPRIME__
    std::unique_ptr<mixxx::LibraryExporter> makeLibraryExporter(QWidget* parent);
#endif

  public slots:
    void slotShowTrackModel(QAbstractItemModel* model);
    void slotSwitchToView(const QString& view);
    void slotLoadTrack(TrackPointer pTrack);
    void slotLoadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play);
    void slotLoadLocationToPlayer(const QString& location, const QString& group, bool play);
    void slotRefreshLibraryModels();
    void slotCreatePlaylist();
    void slotCreateCrate();
    void onSkinLoadFinished();
    void slotSaveCurrentViewState() const;
    void slotRestoreCurrentViewState() const;

  signals:
    void showTrackModel(QAbstractItemModel* model, bool restoreState = true);
    void switchToView(const QString& view);
    void sidebarLeafItemActivated(const QString& title);
    void loadTrack(TrackPointer pTrack);
    void loadTrackToPlayer(TrackPointer pTrack, const QString& group, bool play = false);
    void restoreSearch(const QString&);
    void search(const QString& text);
    void disableSearch();
    void pasteFromSidebar();
    // emit this signal to enable/disable the cover art widget
    void enableCoverArtDisplay(bool);
    void selectTrack(const TrackId&);
    void trackSelected(TrackPointer pTrack);
    void analyzeTracks(const QList<AnalyzerScheduledTrack>& tracks);
#ifdef __ENGINEPRIME__
    void exportLibrary();
    void exportCrate(CrateId crateId);
#endif
    void saveModelState();
    void restoreModelState();

    void setTrackTableFont(const QFont& font);
    void setTrackTableRowHeight(int rowHeight);
    void setSelectedClick(bool enable);

    void onTrackAnalyzerProgress(TrackId trackId, AnalyzerProgress analyzerProgress);

    // Bite DJ: forwarded from SystemSettings::mountEjected (wired in
    // CoreServices). Features backed by removable media (Rekordbox) connect to
    // this to drop a device the instant its filesystem is unmounted, rather
    // than waiting on their own background poll.
    void mountEjected(const QString& mountPoint);

    // Bite DJ: the metadata overrides stored on the drives have been wiped
    // (Settings -> General -> Clear -> Meta). Features that mirror a drive's
    // own library into a table of their own connect to this to put the ratings
    // they scanned back to what the drive exported.
    void metaOverridesCleared();

  private slots:
      /// Bite DJ: close the track view when the drive it is reading from goes
      /// away, and hand focus back to the sidebar browser. Connected to this
      /// object's own mountEjected signal, before the features connect to it.
      void slotMountEjected(const QString& mountPoint);
      void onPlayerManagerTrackAnalyzerProgress(TrackId trackId, AnalyzerProgress analyzerProgress);
      void onPlayerManagerTrackAnalyzerIdle();
      void slotKeyNotationChanged(double value);
      void slotClearCachedWaveforms(double value);
      /// Bite DJ: forget which tracks have been played this session, clearing
      /// the 'played' tint from every library view. Bound to
      /// [Library],reset_played_tracks (Settings -> General -> Played).
      void slotResetPlayedTracks(double value);
      /// Bite DJ: delete the cues this unit stored on every connected USB
      /// drive, handing the tracks back to whatever rekordbox exported for
      /// them, and take those cues off the tracks that are still loaded so the
      /// decks stop showing them. Bound to [Library],clear_cue_overrides
      /// (Settings -> General -> Clear -> Cues).
      void slotClearCueOverrides(double value);
      /// Bite DJ: delete the track metadata (star ratings) this unit stored on
      /// every connected USB drive, handing the tracks back to the ratings
      /// their source library exported, and put those ratings back on the
      /// tracks that are still loaded. Bound to [Library],clear_meta_overrides
      /// (Settings -> General -> Clear -> Meta).
      void slotClearMetaOverrides(double value);

  private:
    const UserSettingsPointer m_pConfig;

    // The Mixxx database connection pool
    const mixxx::DbConnectionPoolPtr m_pDbConnectionPool;

    const QPointer<TrackCollectionManager> m_pTrackCollectionManager;

    parented_ptr<SidebarModel> m_pSidebarModel;
    parented_ptr<LibraryControl> m_pLibraryControl;
    parented_ptr<LibraryColumnControl> m_pLibraryColumnControl;

    QList<LibraryFeature*> m_features;
    const static QString m_sTrackViewName;
    const static QString m_sAutoDJViewName;
    WLibrary* m_pLibraryWidget;
    MixxxLibraryFeature* m_pMixxxLibraryFeature;
    PlaylistFeature* m_pPlaylistFeature;
    CrateFeature* m_pCrateFeature;
    AnalysisFeature* m_pAnalysisFeature;
    BrowseFeature* m_pBrowseFeature;
    QFont m_trackTableFont;
    int m_iTrackTableRowHeight;
    bool m_editMetadataSelectedClick;
    QScopedPointer<ControlObject> m_pKeyNotation;
    QScopedPointer<ControlPushButton> m_pClearCachedWaveforms;
    QScopedPointer<ControlPushButton> m_pResetPlayedTracks;
    QScopedPointer<ControlPushButton> m_pClearCueOverrides;
    QScopedPointer<ControlPushButton> m_pClearMetaOverrides;
};
