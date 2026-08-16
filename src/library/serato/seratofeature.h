#pragma once
// seratofeature.h
// Created 2020-01-31 by Jan Holthuis
//
// This feature reads tracks and crates from removable Serato Libraries,
// either in the Music directory or on removable devices (USB drives, etc),
// by parsing the contents of the _Serato_ directory on each device.
//
// Most of the groundwork for this has been done here:
//
//      https://github.com/Holzhaus/serato-tags
//      https://github.com/Holzhaus/serato-tags/blob/main/scripts/database_v2.py

#include <QFuture>
#include <QFutureWatcher>
#include <QTimer>
#include <memory>
#include <vector>

#include "library/baseexternallibraryfeature.h"
#include "util/parented_ptr.h"

class SeratoPlaylistModel;
class BaseTrackCache;

class SeratoFeature : public BaseExternalLibraryFeature {
    Q_OBJECT
  public:
    SeratoFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~SeratoFeature() override;

    QVariant title() override;
    static bool isSupported();
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;

    // Bite DJ: hidden until the background poll (or a foreground scan)
    // finds a mounted Serato database; hides again when the last one
    // disappears. See requestSidebarVisibility.
    bool isSidebarVisibleByDefault() const override {
        return false;
    }

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void refreshLibraryModels();
    void onSeratoDatabasesFound();
    void onTracksFound();
    // Bite DJ: connected to Library::mountEjected. Removes the sidebar
    // database whose mount path matches `mountPoint` the instant the drive
    // is unmounted, so a dead volume can't be tapped in the window before
    // the background poll would have culled it.
    void ejectDevice(const QString& mountPoint);

  private slots:
    void htmlLinkClicked(const QUrl& link);
    void onBackgroundPollTick();
    void onBackgroundSeratoDatabasesFound();
    void onBackgroundTracksFound();

  private:
    QString formatRootViewHtml() const;
    std::unique_ptr<BaseSqlTableModel> createPlaylistModelForPlaylist(
            const QVariant& data) override;

    void mergeFoundDatabasesIntoSidebar(
            std::vector<std::unique_ptr<TreeItem>> foundDatabases,
            bool isBackgroundScan);
    void pumpBackgroundParseQueue();
    TreeItem* findDatabaseByLabel(const QString& label) const;

    // A database found on disk but deliberately withheld from the sidebar, see
    // m_stagedDatabases.
    struct StagedDatabase {
        std::unique_ptr<TreeItem> pItem;
        // Identifies the physical drive this volume sits on. Volumes sharing a
        // key are shown together, once the last of them has been parsed.
        QString driveKey;
        bool parsed = false;
    };
    QString driveKeyOfDatabase(const TreeItem* pDatabase) const;
    StagedDatabase* findStagedDatabase(const QString& label);
    QStringList stagedDatabaseLabels() const;
    std::unique_ptr<TreeItem> takeStagedDatabase(const QString& label);
    void dropStagedDatabase(const QString& label);
    // Moves every staged database whose drive is fully parsed into the sidebar.
    void promoteCompletedDrives();

    parented_ptr<TreeItemModel> m_pSidebarModel;
    SeratoPlaylistModel* m_pSeratoPlaylistModel;

    QFutureWatcher<QList<TreeItem*>> m_databasesFutureWatcher;
    QFuture<QList<TreeItem*>> m_databasesFuture;
    QFutureWatcher<QString> m_tracksFutureWatcher;
    QFuture<QString> m_tracksFuture;
    QString m_title;

    // Background polling: surfaces newly-inserted USB drives with Serato
    // databases (the sidebar entry is hidden while none are present, so a
    // user tap can't trigger the first scan) and parses them so they gain
    // crate children before they are shown. Mirrors RekordboxFeature.
    QTimer m_bgPollTimer;
    QFutureWatcher<QList<TreeItem*>> m_bgDatabasesFutureWatcher;
    QFuture<QList<TreeItem*>> m_bgDatabasesFuture;
    QFutureWatcher<QString> m_bgTracksFutureWatcher;
    QFuture<QString> m_bgTracksFuture;
    // Databases found on disk but deliberately withheld from the sidebar
    // until they have been parsed, so a device row never appears before the
    // crates it is supposed to expand into. Parsed front-first; the items
    // move into the sidebar model in promoteCompletedDrives(), a whole drive
    // at a time so the volumes of a multi-partition stick appear together.
    std::vector<StagedDatabase> m_stagedDatabases;
    bool m_bgParseInFlight = false;
    // Label of the staged database the in-flight parse is writing into.
    QString m_bgParseLabel;
    // Set when that database is unmounted mid-parse: the item can't be freed
    // while the worker thread writes into it, so it is discarded once the
    // future completes rather than being inserted into the sidebar.
    bool m_bgParseAbandoned = false;
    // A single empty background enumeration is often a transient hiccup right
    // after a (re)mount. Require several consecutive empty scans before
    // tearing down a database entry, so it isn't needlessly re-parsed when it
    // reappears.
    int m_bgConsecutiveEmptyScans = 0;

    QSharedPointer<BaseTrackCache> m_trackSource;
};
