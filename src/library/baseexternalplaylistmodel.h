#pragma once

#include <QHash>
#include <QObject>
#include <QString>

#include "library/basesqltablemodel.h"
#include "library/trackmodel.h"

class QModelIndex;

class BaseExternalPlaylistModel : public BaseSqlTableModel {
    Q_OBJECT
  public:
    BaseExternalPlaylistModel(QObject* pParent, TrackCollectionManager* pTrackCollectionManager,
                              const char* settingsNamespace, const QString& playlistsTable,
                              const QString& playlistTracksTable, QSharedPointer<BaseTrackCache> trackSource);

    ~BaseExternalPlaylistModel() override;

    void setPlaylist(const QString& path_name);
    void setPlaylistById(int playlistId);

    /// Bite DJ: record where the playlist that is about to be shown physically
    /// lives. Features backed by removable media (Rekordbox, Serato) set this
    /// alongside setPlaylist() so the library can close the view when that
    /// drive is ejected. Any path under the device works, see
    /// TrackModel::backingLocation().
    void setBackingLocation(const QString& location) {
        m_backingLocation = location;
    }

    TrackPointer getTrack(const QModelIndex& index) const override;
    TrackId getTrackId(const QModelIndex& index) const override;
    QString backingLocation() const override {
        return m_backingLocation;
    }
    bool isColumnInternal(int column) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    Capabilities getCapabilities() const override;
    QString modelKey(bool noSearch) const override;

  protected:
    void updateTrackIdLookup() override;

  private:
    TrackId doGetTrackId(const TrackPointer& pTrack) const override;

    QString m_playlistsTable;
    QString m_playlistTracksTable;
    QString m_backingLocation;
    QSharedPointer<BaseTrackCache> m_trackSource;
    int m_currentPlaylistId;
    QHash<int, QString> m_searchTexts;
    QHash<QString, TrackId> m_trackIdsByLocation;
};
