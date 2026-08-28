#include "library/basetracktablemodel.h"

#include <QFileInfo>
#include <QMimeData>

#include "control/controlproxy.h"
#include "library/dao/trackschema.h"
#include "library/library_prefs.h"
#include "library/playedtracks.h"
#include "library/starrating.h"
#include "library/tabledelegates/bpmdelegate.h"
#include "library/tabledelegates/checkboxdelegate.h"
#include "library/tabledelegates/colordelegate.h"
#include "library/tabledelegates/defaultdelegate.h"
#include "library/tabledelegates/locationdelegate.h"
#include "library/tabledelegates/multilineeditdelegate.h"
#include "library/tabledelegates/previewbuttondelegate.h"
#include "library/tabledelegates/stardelegate.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_basetracktablemodel.cpp"
#include "track/track.h"
#include "util/assert.h"
#include "util/clipboard.h"
#include "util/datetime.h"
#include "util/db/sqlite.h"
#include "util/logger.h"
#include "widget/wlibrary.h"
#include "widget/wtracktableview.h"

namespace {

const mixxx::Logger kLogger("BaseTrackTableModel");

constexpr int kReplayGainPrecision = 2;

inline QSqlDatabase cloneDatabase(
        const QSqlDatabase& prototype) {
    const auto connectionName =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto cloned = QSqlDatabase::cloneDatabase(
            prototype,
            connectionName);
    DEBUG_ASSERT(cloned.isValid());
    if (prototype.isOpen() && !cloned.open()) {
        kLogger.warning()
                << "Failed to open cloned database connection"
                << cloned
                << cloned.lastError();
    }
    return cloned;
}

QSqlDatabase cloneDatabase(
        TrackCollectionManager* pTrackCollectionManager) {
    VERIFY_OR_DEBUG_ASSERT(pTrackCollectionManager &&
            pTrackCollectionManager->internalCollection()) {
        return QSqlDatabase();
    }
    return cloneDatabase(
            pTrackCollectionManager->internalCollection()->database());
}

} // anonymous namespace

// static
constexpr int BaseTrackTableModel::kBpmColumnPrecisionDefault;
constexpr int BaseTrackTableModel::kBpmColumnPrecisionMinimum;
constexpr int BaseTrackTableModel::kBpmColumnPrecisionMaximum;

int BaseTrackTableModel::s_bpmColumnPrecision =
        kBpmColumnPrecisionDefault;

// static
void BaseTrackTableModel::setBpmColumnPrecision(int precision) {
    VERIFY_OR_DEBUG_ASSERT(precision >= BaseTrackTableModel::kBpmColumnPrecisionMinimum) {
        precision = BaseTrackTableModel::kBpmColumnPrecisionMinimum;
    }
    VERIFY_OR_DEBUG_ASSERT(precision <= BaseTrackTableModel::kBpmColumnPrecisionMaximum) {
        precision = BaseTrackTableModel::kBpmColumnPrecisionMaximum;
    }
    s_bpmColumnPrecision = precision;
}

bool BaseTrackTableModel::s_bApplyPlayedTrackColor =
        kApplyPlayedTrackColorDefault;

void BaseTrackTableModel::setApplyPlayedTrackColor(bool apply) {
    s_bApplyPlayedTrackColor = apply;
}

BaseTrackTableModel::BaseTrackTableModel(
        QObject* parent,
        TrackCollectionManager* pTrackCollectionManager,
        const char* settingsNamespace)
        : QAbstractTableModel(parent),
          TrackModel(cloneDatabase(pTrackCollectionManager), settingsNamespace),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_previewDeckGroup(PlayerManager::groupForPreviewDeck(0)),
          m_backgroundColorOpacity(WLibrary::kDefaultTrackTableBackgroundColorOpacity),
          m_trackPlayedColor(QColor(WTrackTableView::kDefaultTrackPlayedColor)),
          m_trackMissingColor(QColor(WTrackTableView::kDefaultTrackMissingColor)) {
    connect(&pTrackCollectionManager->internalCollection()->getTrackDAO(),
            &TrackDAO::tracksRemoved,
            this,
            &BaseTrackTableModel::slotTracksRemoved);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &BaseTrackTableModel::slotTrackChanged);
    // Bite DJ: repaint when a track starts playing (or the session is reset
    // from Settings) so its row picks up / drops the 'played' colour.
    connect(&PlayedTracks::instance(),
            &PlayedTracks::playedTracksChanged,
            this,
            &BaseTrackTableModel::slotPlayedTracksChanged);
    // Bite DJ: no CoverArtCache::coverFound connection — cover art is never
    // shown in the library here, so there's nothing to repaint when one loads.
    // Bite DJ: repaint the Key column when [Library],key_notation changes
    // at runtime (skin toggles Lancelot/Traditional). m_columnCache here only
    // holds this model's table columns (e.g. ID/PREVIEW/COVERART for the
    // library), not Key — so we listen to the CO directly via our own proxy
    // and use the virtual fieldIndex(), which routes through m_trackSource's
    // ColumnCache. By the time data() runs, KeyUtils::s_notation has already
    // been refreshed by Library::slotKeyNotationChanged.
    auto* pKeyNotationProxy = new ControlProxy(
            mixxx::library::prefs::kKeyNotationConfigKey, this);
    pKeyNotationProxy->connectValueChanged(this, [this](double) {
        const int keyCol = fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY);
        const int rows = rowCount();
        if (keyCol < 0 || rows <= 0) {
            return;
        }
        emit dataChanged(index(0, keyCol), index(rows - 1, keyCol));
    });
}

void BaseTrackTableModel::initTableColumnsAndHeaderProperties(
        const QStringList& tableColumns) {
    m_columnCache.setColumns(tableColumns);

    // Reset the column headers.
    m_columnHeaders.clear();

    VERIFY_OR_DEBUG_ASSERT(tableColumns.size() > 0) {
        return;
    }

    m_columnHeaders.resize(endFieldIndex());

    // Init the mapping of all columns, even for internal columns that are
    // hidden/invisible. Otherwise mapColumn() would not return a valid result
    // for those columns.
    for (int column = 0; column < ColumnCache::NUM_COLUMNS; ++column) {
        setHeaderProperties(static_cast<ColumnCache::Column>(column));
    }

    emit headerDataChanged(Qt::Horizontal, 0, tableColumns.size() - 1);
}

void BaseTrackTableModel::setHeaderProperties(ColumnCache::Column column) {
    // fieldIndex() is a virtual function that returns indexes from
    // this->m_columnCache and BaseTrackCache::m_columnCache, which are the
    // track meta data columns
    int section = fieldIndex(column);
    if (section < 0) {
        // Skipping header properties for unsupported column
        return;
    }
    VERIFY_OR_DEBUG_ASSERT(section < m_columnHeaders.size()) {
        return;
    }
    m_columnHeaders[section].column = column;
    m_columnHeaders[section].header[TrackModel::kHeaderNameRole] = m_columnCache.columnName(column);
    m_columnHeaders[section].header[Qt::DisplayRole] = m_columnCache.columnTitle(column);
    m_columnHeaders[section].header[TrackModel::kHeaderWidthRole] =
            m_columnCache.columnDefaultWidth(column);
}

bool BaseTrackTableModel::setHeaderData(
        int section,
        Qt::Orientation orientation,
        const QVariant& value,
        int role) {
    VERIFY_OR_DEBUG_ASSERT(section >= 0) {
        return false;
    }
    VERIFY_OR_DEBUG_ASSERT(section < m_columnHeaders.size()) {
        return false;
    }
    if (orientation != Qt::Horizontal) {
        // We only care about horizontal headers.
        return false;
    }
    m_columnHeaders[section].header[role] = value;
    emit headerDataChanged(orientation, section, section);
    return true;
}

QVariant BaseTrackTableModel::headerData(
        int section,
        Qt::Orientation orientation,
        int role) const {
    if (orientation == Qt::Horizontal) {
        switch (role) {
        case Qt::DisplayRole: {
            QVariant headerValue =
                    m_columnHeaders.value(section).header.value(role);
            if (!headerValue.isValid()) {
                // Try EditRole if DisplayRole wasn't present
                headerValue = m_columnHeaders.value(section).header.value(Qt::EditRole);
            }
            if (headerValue.isValid()) {
                return headerValue;
            } else {
                return QVariant(section).toString();
            }
        }
        case TrackModel::kHeaderWidthRole: {
            QVariant widthValue = m_columnHeaders.value(section).header.value(role);
            if (widthValue.isValid()) {
                return widthValue;
            }
            return ColumnCache::defaultColumnWidth();
        }
        case TrackModel::kHeaderNameRole: {
            return m_columnHeaders.value(section).header.value(role);
        }
        case Qt::ToolTipRole: {
            QVariant tooltip = m_columnHeaders.value(section).header.value(role);
            if (tooltip.isValid()) {
                return tooltip;
            }
            break;
        }
        default:
            break;
        }
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

int BaseTrackTableModel::columnCount(const QModelIndex& parent) const {
    VERIFY_OR_DEBUG_ASSERT(!parent.isValid()) {
        return 0;
    }
    return m_columnHeaders.size();
}

void BaseTrackTableModel::cutTracks(const QModelIndexList& indices) {
    copyTracks(indices);
    removeTracks(indices);
}

void BaseTrackTableModel::copyTracks(const QModelIndexList& indices) const {
    Clipboard::start();
    for (const QModelIndex& index : indices) {
        if (index.isValid()) {
            Clipboard::add(QUrl::fromLocalFile(getTrackLocation(index)));
        }
    }
    Clipboard::finish();
}

QList<int> BaseTrackTableModel::pasteTracks(const QModelIndex& insertionIndex) {
    // Don't paste into locked playlists and crates or into into History
    if (isLocked() || !hasCapabilities(TrackModel::Capability::ReceiveDrops)) {
        return QList<int>{};
    }

    int insertionPos = 0;
    const QList<QUrl> urls = Clipboard::urls();
    const QList<TrackId> trackIds = m_pTrackCollectionManager->resolveTrackIdsFromUrls(urls, true);
    if (!trackIds.isEmpty()) {
        addTracksWithTrackIds(insertionIndex, trackIds, &insertionPos);
    }

    QList<int> rows;
    for (const auto& trackId : trackIds) {
        const auto trackRows = getTrackRows(trackId);
        for (int trackRow : trackRows) {
            if (insertionPos == 0) {
                rows.append(trackRow);
            } else {
                int pos = getFieldVariant(index(trackRow, 0),
                        ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_POSITION)
                                  .toInt();
                // trackRows includes all instances in the table of the pasted
                // tracks. We only want to select the ones we just inserted
                if (pos >= insertionPos && pos < insertionPos + trackIds.size()) {
                    rows.append(trackRow);
                }
            }
        }
    }
    return rows;
}

bool BaseTrackTableModel::isColumnHiddenByDefault(
        int column) {
    return column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ALBUMARTIST) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE) ||
            // Bite DJ: cover art is never shown in the library (see delegateForColumn)
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CHANNELS) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMPOSER) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_GROUPING) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PLAYED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_SAMPLERATE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_YEAR);
}

QAbstractItemDelegate* BaseTrackTableModel::delegateForColumn(
        const int index, QObject* pParent) {
    auto* pTableView = qobject_cast<WTrackTableView*>(pParent);
    VERIFY_OR_DEBUG_ASSERT(pTableView) {
        return nullptr;
    }
    m_backgroundColorOpacity = pTableView->getBackgroundColorOpacity();
    // This is the color used for the text of played tracks.
    // data() uses this to compose the ForegroundRole QBrush if 'played' is checked.
    m_trackPlayedColor = pTableView->getTrackPlayedColor();
    connect(pTableView,
            &WTrackTableView::trackPlayedColorChanged,
            this,
            [this](QColor col) {
                m_trackPlayedColor = col;
            });
    // Same for the 'missing' color
    m_trackMissingColor = pTableView->getTrackMissingColor();
    connect(pTableView,
            &WTrackTableView::trackMissingColorChanged,
            this,
            [this](QColor col) {
                m_trackMissingColor = col;
            });
    if (index == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_RATING)) {
        return new StarDelegate(pTableView);
    } else if (index == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM)) {
        return new BPMDelegate(pTableView);
    } else if (index == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED)) {
        return new CheckboxDelegate(pTableView, QStringLiteral("LibraryPlayedCheckbox"));
    } else if (PlayerInfo::instance().numPreviewDecks() > 0 &&
            index == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW)) {
        return new PreviewButtonDelegate(pTableView, index);
    } else if (index == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COMMENT)) {
        return new MultiLineEditDelegate(pTableView);
    } else if (index == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION)) {
        return new LocationDelegate(pTableView);
    } else if (index == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COLOR)) {
        return new ColorDelegate(pTableView);
    }
    // Bite DJ: the cover-art column is intentionally not handled here. We never
    // show cover art in the library on this appliance, so it gets no
    // CoverArtDelegate — that delegate lazy-loads covers from disk as rows scroll
    // into view, which on a (possibly pulled) USB drive means slow, repeated
    // TagLib reads and a stutter. The column is hidden by default
    // (isColumnHiddenByDefault) and falls through to the plain DefaultDelegate.
    return new DefaultDelegate(pTableView);
}

bool BaseTrackTableModel::verifyTrackFileExists(const QModelIndex& index) {
    const QString location = getTrackLocation(index);
    if (location.isEmpty()) {
        // Nothing to check against; let the normal load path handle it.
        return true;
    }
    // Already known missing: refuse immediately without touching the
    // filesystem. A stat() on a pulled USB mount can block, and this runs on
    // every re-tap of a dead entry. The flag is cleared on the next model
    // repopulation (clearMissingTrackFlags() from select()), so a re-inserted
    // drive / rescan re-enables loading.
    if (m_missingTrackLocations.contains(location)) {
        return false;
    }
    if (QFileInfo::exists(location)) {
        return true;
    }
    // First time we've seen it missing: flag the row in the 'missing' colour,
    // then allow this one load to proceed. BaseTrackPlayerImpl::slotLoadTrack
    // re-checks existence, raises the single "could not be found" notification
    // and keeps the deck's current track. Subsequent taps are blocked above.
    m_missingTrackLocations.insert(location);
    const int row = index.row();
    // Repaint the whole row so the 'missing' colour covers all visible columns.
    emit dataChanged(index.sibling(row, 0),
            index.sibling(row, columnCount() - 1),
            {Qt::ForegroundRole});
    return true;
}

void BaseTrackTableModel::clearMissingTrackFlags() {
    // Called when the model is repopulated (e.g. a rescan after a USB was
    // re-inserted). Drop the in-memory missing flags so verifyTrackFileExists()
    // re-checks the filesystem on the next load attempt instead of refusing
    // outright. The repaint is implicit in the surrounding model reset.
    m_missingTrackLocations.clear();
}

QVariant BaseTrackTableModel::data(
        const QModelIndex& index,
        int role) const {
    if (!index.isValid()) {
        return QVariant();
    }

    if (role == Qt::BackgroundRole) {
        const auto rgbColorValue = rawSiblingValue(
                index,
                ColumnCache::COLUMN_LIBRARYTABLE_COLOR);
        const auto rgbColor = mixxx::RgbColor::fromQVariant(rgbColorValue);
        if (!rgbColor) {
            return QVariant();
        }
        auto bgColor = mixxx::RgbColor::toQColor(rgbColor);
        DEBUG_ASSERT(bgColor.isValid());
        DEBUG_ASSERT(m_backgroundColorOpacity >= 0.0);
        DEBUG_ASSERT(m_backgroundColorOpacity <= 1.0);
        bgColor.setAlphaF(static_cast<float>(m_backgroundColorOpacity));
        return QBrush(bgColor);
    } else if (role == Qt::ForegroundRole) {
        // Custom text color for missing tracks
        // Visible in playlists, crates and Missing feature.
        // Check this first so played, missing tracks (unlikely case, but possible)
        // get the 'missing' color.
        // Note: this is not helpful in Tracks -> Missing, so override it with
        // the regular track color (WTrackTableView { color: #xxx; }) like this:
        // #DlgMissing WTrackTableView { qproperty-trackMissingColor: #xxx; }

        // Tracks discovered missing at load time (USB pulled, DB's fs_deleted
        // not yet updated by a rescan). Checked before the fs_deleted column so
        // a freshly-pulled drive colours immediately. getTrackLocation() reads
        // the location column straight from the row cache — it must NOT be
        // getTrackId(), which the external models override to import the track.
        // See verifyTrackFileExists().
        if (!m_missingTrackLocations.isEmpty()) {
            const QString location = getTrackLocation(index);
            if (!location.isEmpty() && m_missingTrackLocations.contains(location)) {
                return QVariant::fromValue(m_trackMissingColor);
            }
        }
        auto missingRaw = rawSiblingValue(
                index,
                ColumnCache::COLUMN_TRACKLOCATIONSTABLE_FSDELETED);
        if (!missingRaw.isNull() &&
                missingRaw.canConvert<bool>() &&
                missingRaw.toBool()) {
            return QVariant::fromValue(m_trackMissingColor);
        }
        // Bite DJ: custom text color for tracks played in *this* session.
        // Stock reads the library's `played` column here; we consult the
        // session registry instead so the colour also works in the Rekordbox /
        // Serato views (no such column) and so a power-yanked appliance doesn't
        // boot with the whole of last night's set still flagged. Same
        // location-keyed lookup as the missing-file branch above, and skipped
        // outright until something has actually played. See PlayedTracks.
        if (s_bApplyPlayedTrackColor) {
            const PlayedTracks& playedTracks = PlayedTracks::instance();
            if (!playedTracks.isEmpty() &&
                    playedTracks.isPlayed(getTrackLocation(index))) {
                // TODO Maybe adjust color for bright track colors?
                // Here or in DefaultDelegate
                return QVariant::fromValue(m_trackPlayedColor);
            }
        }
    }

    // Return the preferred (default) width of the Color column.
    // This works around inconsistencies when the width is determined by
    // color values. See https://github.com/mixxxdj/mixxx/issues/12850
    if (role == Qt::SizeHintRole) {
        const auto field = mapColumn(index.column());
        if (field == ColumnCache::COLUMN_LIBRARYTABLE_COLOR) {
            return QSize(ColumnCache::defaultColumnWidth() / 2, 0);
        }
    }

    // Only retrieve a value for supported roles
    if (role != Qt::DisplayRole &&
            role != Qt::EditRole &&
            role != Qt::CheckStateRole &&
            role != Qt::ToolTipRole &&
            role != kDataExportRole &&
            role != Qt::TextAlignmentRole) {
        return QVariant();
    }

    return roleValue(index, rawValue(index), role);
}

QVariant BaseTrackTableModel::rawSiblingValue(
        const QModelIndex& index,
        ColumnCache::Column siblingField) const {
    VERIFY_OR_DEBUG_ASSERT(index.isValid()) {
        return QVariant();
    }
    VERIFY_OR_DEBUG_ASSERT(siblingField != ColumnCache::COLUMN_LIBRARYTABLE_INVALID) {
        return QVariant();
    }
    const int siblingColumn = fieldIndex(siblingField);
    if (siblingColumn < 0) {
        // Unsupported or unknown column/field
        // FIXME: This should never happen but it does. But why??
        return QVariant();
    }
    const QModelIndex siblingIndex = index.sibling(index.row(), siblingColumn);
    return rawValue(siblingIndex);
}

bool BaseTrackTableModel::setData(
        const QModelIndex& index,
        const QVariant& value,
        int role) {
    const int column = index.column();
    if (role == Qt::CheckStateRole) {
        const auto field = mapColumn(index.column());
        if (field == ColumnCache::COLUMN_LIBRARYTABLE_INVALID) {
            return false;
        }
        const auto checked = value.toInt() > 0;
        switch (field) {
        case ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED: {
            // Override sets to TIMESPLAYED and redirect them to PLAYED
            QModelIndex playedIndex = index.sibling(
                    index.row(),
                    fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PLAYED));
            return setData(playedIndex, checked, Qt::EditRole);
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_BPM: {
            QModelIndex bpmLockedIndex = index.sibling(
                    index.row(),
                    fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK));
            return setData(bpmLockedIndex, checked, Qt::EditRole);
        }
        default:
            return false;
        }
    }

    TrackPointer pTrack = getTrack(index);
    if (!pTrack) {
        return false;
    }

    // Do not save the track here. Changing the track dirties it and the caching
    // system will automatically save the track once it is unloaded from
    // memory. rryan 10/2010
    return setTrackValueForColumn(pTrack, column, value, role);
}

QVariant BaseTrackTableModel::roleValue(
        const QModelIndex& index,
        QVariant&& rawValue,
        int role) const {
    const auto field = mapColumn(index.column());
    if (field == ColumnCache::COLUMN_LIBRARYTABLE_INVALID) {
        return std::move(rawValue);
    }
    switch (role) {
    case Qt::ToolTipRole:
    case kDataExportRole:
        switch (field) {
        case ColumnCache::COLUMN_LIBRARYTABLE_COLOR:
            return mixxx::RgbColor::toQString(mixxx::RgbColor::fromQVariant(rawValue));
        // Bite DJ: no cover-art tooltip — cover art is never shown in the library.
        case ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW:
            return QVariant();
        case ColumnCache::COLUMN_LIBRARYTABLE_RATING:
        case ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED:
            return std::move(rawValue);
        default:
            // Same value as for Qt::DisplayRole (see below)
            break;
        }
        [[fallthrough]];
    // NOTE: for export we need to fall through to Qt::DisplayRole,
    // so do not add any other role cases here, or the export
    // will be empty
    case Qt::DisplayRole:
        switch (field) {
        case ColumnCache::COLUMN_LIBRARYTABLE_DURATION: {
            if (rawValue.isNull()) {
                return QVariant();
            }
            double durationInSeconds;
            if (rawValue.canConvert<mixxx::Duration>()) {
                const auto duration = rawValue.value<mixxx::Duration>();
                VERIFY_OR_DEBUG_ASSERT(duration >= mixxx::Duration::empty()) {
                    return QVariant();
                }
                durationInSeconds = duration.toDoubleSeconds();
            } else {
                VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<double>()) {
                    return QVariant();
                }
                bool ok;
                durationInSeconds = rawValue.toDouble(&ok);
                VERIFY_OR_DEBUG_ASSERT(ok && durationInSeconds >= 0) {
                    return QVariant();
                }
            }
            return mixxx::Duration::formatTime(
                    durationInSeconds,
                    mixxx::Duration::Precision::SECONDS);
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_RATING: {
            if (rawValue.isNull()) {
                return QVariant();
            }
            VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<int>()) {
                return QVariant();
            }
            bool ok;
            const auto starCount = rawValue.toInt(&ok);
            VERIFY_OR_DEBUG_ASSERT(ok && starCount >= StarRating::kMinStarCount) {
                return QVariant();
            }
            return QVariant::fromValue(StarRating(starCount));
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED: {
            if (rawValue.isNull()) {
                return QVariant();
            }
            VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<int>()) {
                return QVariant();
            }
            bool ok;
            const auto timesPlayed = rawValue.toInt(&ok);
            VERIFY_OR_DEBUG_ASSERT(ok && timesPlayed >= 0) {
                return QVariant();
            }
            return QString::number(timesPlayed);
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED:
        case ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED: {
            VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<QDateTime>()) {
                return QVariant();
            }
            // TODO: This is a hot code path, executed very often while library scrolling,
            // and localDateTimeFromUtc is time consuming, probably because,
            // we pass around QDateTime with a wrong time zone set
            QDateTime dt = mixxx::localDateTimeFromUtc(rawValue.toDateTime());
            if (role == Qt::ToolTipRole || role == kDataExportRole) {
                // localized text date: "Wednesday, May 20, 1998 03:40:13 AM CEST"
                return dt;
            }
            if (field == ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED) {
                // Timestamp column in history feature:
                // Use localized date/time format without text: "5/20/98 03:40 AM"
                return mixxx::displayLocalDateTime(dt);
            }
            // For Date Added, use just the date: "5/20/98"
            return dt.date();
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT: {
            QDateTime lastPlayedAt;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (rawValue.metaType().id() == QMetaType::QString) {
#else
            if (rawValue.type() == QVariant::String) {
#endif
                // column value
                lastPlayedAt = mixxx::sqlite::readGeneratedTimestamp(rawValue);
            } else {
                // cached in memory (local time)
                lastPlayedAt = rawValue.toDateTime().toUTC();
            }
            if (!lastPlayedAt.isValid()) {
                return QVariant();
            }
            DEBUG_ASSERT(lastPlayedAt.timeSpec() == Qt::UTC);
            // TODO: This is a hot code path, executed very often while library scrolling,
            // and localDateTimeFromUtc is time consuming, probably because,
            // we pass around QDateTime with a wrong time zone set
            QDateTime dt = mixxx::localDateTimeFromUtc(lastPlayedAt);
            if (role == Qt::ToolTipRole || role == kDataExportRole) {
                return dt;
            }
            return dt.date();
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_BPM: {
            mixxx::Bpm bpm;
            if (!rawValue.isNull()) {
                if (rawValue.canConvert<mixxx::Bpm>()) {
                    bpm = rawValue.value<mixxx::Bpm>();
                } else {
                    VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<double>()) {
                        return QVariant();
                    }
                    bool ok;
                    const auto bpmValue = rawValue.toDouble(&ok);
                    VERIFY_OR_DEBUG_ASSERT(ok) {
                        return QVariant();
                    }
                    bpm = mixxx::Bpm(bpmValue);
                }
            }
            if (bpm.isValid()) {
                if (role == Qt::ToolTipRole || role == kDataExportRole) {
                    return QString::number(bpm.value(), 'f', 4);
                } else {
                    // Use the locale here to make the display and editor consistent.
                    // Custom precision, set in DlgPrefLibrary.
                    return QLocale().toString(bpm.value(), 'f', s_bpmColumnPrecision);
                }
            } else {
                return QChar('-');
            }
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_BITRATE: {
            if (rawValue.isNull()) {
                return QVariant();
            }
            if (rawValue.canConvert<mixxx::audio::Bitrate>()) {
                // return value as is
                return std::move(rawValue);
            } else {
                VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<int>()) {
                    return QVariant();
                }
                bool ok;
                const auto bitrateValue = rawValue.toInt(&ok);
                VERIFY_OR_DEBUG_ASSERT(ok) {
                    return QVariant();
                }
                if (mixxx::audio::Bitrate(bitrateValue).isValid()) {
                    // return value as is
                    return std::move(rawValue);
                } else {
                    // clear invalid values
                    return QVariant();
                }
            }
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_KEY: {
            const auto keyId = rawSiblingValue(
                    index, ColumnCache::COLUMN_LIBRARYTABLE_KEY_ID);
            // Bite DJ: stock keyFromKeyTextAndIdFields returns the raw text
            // verbatim when key_id is INVALID — which is the case for every
            // track imported from Rekordbox/Serato/etc., since those carriers
            // store only the text key (e.g. "Dbm") and not Mixxx's chromatic
            // id. Result: the user's notation setting silently has no effect
            // on those rows. Parse the text first and, if it resolves to a
            // chromatic key, render it through s_notation so the column tracks
            // the [Library],key_notation toggle.
            if (rawValue.metaType().id() == QMetaType::QString) {
                const auto parsed =
                        KeyUtils::guessKeyFromText(rawValue.toString());
                if (parsed != mixxx::track::io::key::INVALID) {
                    return QVariant{KeyUtils::keyToString(parsed)};
                }
            }
            return KeyUtils::keyFromKeyTextAndIdFields(rawValue, keyId);
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN: {
            if (rawValue.isNull()) {
                return QVariant();
            }
            double rgRatio;
            if (rawValue.canConvert<mixxx::ReplayGain>()) {
                rgRatio = rawValue.value<mixxx::ReplayGain>().getRatio();
            } else {
                VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<double>()) {
                    return QVariant();
                }
                bool ok;
                rgRatio = rawValue.toDouble(&ok);
                VERIFY_OR_DEBUG_ASSERT(ok) {
                    return QVariant();
                }
            }
            if (role == Qt::ToolTipRole || role == kDataExportRole) {
                return mixxx::ReplayGain::ratioToString(rgRatio);
            } else {
                return mixxx::ReplayGain::ratioToString(rgRatio, kReplayGainPrecision);
            }
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_CHANNELS:
            // Not yet supported
            DEBUG_ASSERT(rawValue.isNull());
            break;
        case ColumnCache::COLUMN_LIBRARYTABLE_SAMPLERATE:
            // Not yet supported
            DEBUG_ASSERT(rawValue.isNull());
            break;
        case ColumnCache::COLUMN_LIBRARYTABLE_URL:
            // Not yet supported
            DEBUG_ASSERT(rawValue.isNull());
            break;
        default:
            // Otherwise, just use the column value
            break;
        }
        break;
    case Qt::EditRole:
        switch (field) {
        case ColumnCache::COLUMN_LIBRARYTABLE_BPM: {
            bool ok;
            const auto bpmValue = rawValue.toDouble(&ok);
            if (!ok) {
                return mixxx::Bpm::kValueUndefined;
            }
            return mixxx::Bpm{bpmValue}.valueOr(mixxx::Bpm::kValueUndefined);
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED:
            return index.sibling(
                                index.row(),
                                fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PLAYED))
                    .data()
                    .toBool();
        case ColumnCache::COLUMN_LIBRARYTABLE_RATING:
            VERIFY_OR_DEBUG_ASSERT(rawValue.canConvert<int>()) {
                return QVariant();
            }
            return QVariant::fromValue(StarRating(rawValue.toInt()));
        default:
            // Otherwise, just use the column value
            break;
        }
        break;
    case Qt::CheckStateRole: {
        QVariant boolValue;
        switch (field) {
        case ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW:
            boolValue = rawValue;
            break;
        case ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED:
            boolValue = rawSiblingValue(
                    index,
                    ColumnCache::COLUMN_LIBRARYTABLE_PLAYED);
            break;
        case ColumnCache::COLUMN_LIBRARYTABLE_BPM:
            // The BPM-lock checkbox is intentionally not shown in the BPM
            // column to reduce UI clutter.
            return QVariant();
        default:
            // No check state supported
            return QVariant();
        }
        // Flags in the database are stored as integers that are
        // convertible to bool.
        if (!boolValue.isNull() && boolValue.canConvert<bool>()) {
            return boolValue.toBool() ? Qt::Checked : Qt::Unchecked;
        } else {
            // Undecidable
            return Qt::PartiallyChecked;
        }
    }
    // Right align BPM, duration and bitrate so big/small values can easily be
    // spotted by length (number of digits)
    case Qt::TextAlignmentRole: {
        switch (field) {
        case ColumnCache::COLUMN_LIBRARYTABLE_BPM:
        case ColumnCache::COLUMN_LIBRARYTABLE_DURATION:
        case ColumnCache::COLUMN_LIBRARYTABLE_BITRATE:
        case ColumnCache::COLUMN_LIBRARYTABLE_TRACKNUMBER: {
            // We need to cast to int due to a bug similar to
            // https://bugreports.qt.io/browse/QTBUG-67582
            return static_cast<int>(Qt::AlignVCenter | Qt::AlignRight);
        }
        case ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED:
        case ColumnCache::COLUMN_PLAYLISTTRACKSTABLE_DATETIMEADDED:
        case ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT: {
            return static_cast<int>(Qt::AlignVCenter | Qt::AlignHCenter);
        }
        default:
            return QVariant(); // default AlignLeft for all other columns
        }
    }
    default:
        DEBUG_ASSERT(!"unexpected role");
        break;
    }
    return std::move(rawValue);
}

bool BaseTrackTableModel::isBpmLocked(
        const QModelIndex& index) const {
    return getFieldVariant(index, ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK).toBool();
}

Qt::ItemFlags BaseTrackTableModel::defaultItemFlags(
        const QModelIndex& index) const {
    if (index.isValid()) {
        return QAbstractItemModel::flags(index) |
                // Enable dragging songs from this data model to elsewhere
                // like the waveform widget to load a track into a Player
                Qt::ItemIsDragEnabled;
    } else {
        return Qt::ItemIsEnabled;
    }
}

Qt::ItemFlags BaseTrackTableModel::readOnlyFlags(
        const QModelIndex& index) const {
    return defaultItemFlags(index);
}

Qt::ItemFlags BaseTrackTableModel::readWriteFlags(
        const QModelIndex& index) const {
    if (!index.isValid()) {
        return defaultItemFlags(index);
    }

    const int column = index.column();
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BITRATE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CHANNELS) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COLOR) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DATETIMEADDED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_LAST_PLAYED_AT) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_DURATION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_FILETYPE) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_LOCATION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_REPLAYGAIN) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_SAMPLERATE)) {
        return readOnlyFlags(index);
    }

    Qt::ItemFlags itemFlags = defaultItemFlags(index);
    if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PLAYED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_TIMESPLAYED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK)) {
        // Checkable cells
        itemFlags |= Qt::ItemIsUserCheckable;
    } else if (column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM)) {
        // The BPM-lock checkbox is not shown in the BPM column, so the cell is
        // not user-checkable. Allow editing of BPM only if not locked.
        if (!isBpmLocked(index)) {
            itemFlags |= Qt::ItemIsEditable;
        }
    } else {
        // Cells are editable by default
        itemFlags |= Qt::ItemIsEditable;
    }
    return itemFlags;
}

Qt::ItemFlags BaseTrackTableModel::flags(
        const QModelIndex& index) const {
    return readWriteFlags(index);
}

QList<QUrl> BaseTrackTableModel::collectUrls(
        const QModelIndexList& indexes) const {
    QList<QUrl> urls;
    urls.reserve(indexes.size());
    // The list of indexes we're given contains separates indexes for each
    // column, so even if only one row is selected, we'll have columnCount()
    // indices.  We need to only count each row once:
    QSet<int> visitedRows;
    for (const auto& index : indexes) {
        if (visitedRows.contains(index.row())) {
            continue;
        }
        visitedRows.insert(index.row());
        QUrl url = getTrackUrl(index);
        if (url.isValid()) {
            urls.append(url);
        }
    }
    return urls;
}

QMimeData* BaseTrackTableModel::mimeData(
        const QModelIndexList& indexes) const {
    const auto urls = collectUrls(indexes);
    if (urls.isEmpty()) {
        return nullptr;
    } else {
        QMimeData* mimeData = new QMimeData();
        mimeData->setUrls(urls);
        return mimeData;
    }
}

void BaseTrackTableModel::slotTrackChanged(
        const QString& group,
        TrackPointer pNewTrack,
        TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    if (group == m_previewDeckGroup) {
        // If there was a previously loaded track, refresh its rows so the
        // preview state will update.
        if (m_previewDeckTrackId.isValid()) {
            const int numColumns = columnCount();
            const auto rows = getTrackRows(m_previewDeckTrackId);
            m_previewDeckTrackId = TrackId(); // invalidate
            for (int row : rows) {
                QModelIndex topLeft = index(row, 0);
                QModelIndex bottomRight = index(row, numColumns);
                emit dataChanged(topLeft, bottomRight);
            }
        }
        m_previewDeckTrackId = doGetTrackId(pNewTrack);
    }
}

void BaseTrackTableModel::slotPlayedTracksChanged() {
    const int rows = rowCount();
    const int columns = columnCount();
    if (rows <= 0 || columns <= 0) {
        return;
    }
    // We don't know which rows hold the affected location(s) without scanning
    // the whole model, so signal the full range: the view only repaints what is
    // on screen, and this fires at most once per track change.
    emit dataChanged(index(0, 0),
            index(rows - 1, columns - 1),
            {Qt::ForegroundRole});
}

void BaseTrackTableModel::slotRefreshAllRows() {
    select();
}

void BaseTrackTableModel::slotTracksRemoved(const QSet<TrackId>& trackIds) {
    removeTrackRows(trackIds);
}

void BaseTrackTableModel::emitDataChangedForMultipleRowsInColumn(
        const QList<int>& rows,
        int column,
        const QVector<int>& roles) {
    DEBUG_ASSERT(column >= 0);
    DEBUG_ASSERT(column < columnCount());
    int beginRow = -1;
    int endRow = -1;
    for (const int row : rows) {
        DEBUG_ASSERT(row >= rows.first());
        DEBUG_ASSERT(row <= rows.last());
        DEBUG_ASSERT(row >= 0);
        if (row >= rowCount()) {
            // The number of rows might have changed since the signal
            // has been emitted. This case seems to occur after switching
            // to a different view with less rows.
            continue;
        }
        if (beginRow < 0) {
            // Start the first stride
            DEBUG_ASSERT(beginRow == endRow);
            DEBUG_ASSERT(row == rows.first());
            beginRow = row;
            endRow = row + 1;
        } else if (row == endRow) {
            // Continue the current stride
            ++endRow;
        } else {
            // Finish the current stride...
            DEBUG_ASSERT(beginRow >= rows.first());
            DEBUG_ASSERT(beginRow < endRow);
            DEBUG_ASSERT(endRow - 1 <= rows.last());
            QModelIndex topLeft = index(beginRow, column);
            QModelIndex bottomRight = index(endRow - 1, column);
            emit dataChanged(topLeft, bottomRight, roles);
            // ...before starting the next stride
            // Rows are expected to be sorted in ascending order
            // without duplicates!
            DEBUG_ASSERT(row >= endRow);
            beginRow = row;
            endRow = row + 1;
        }
    }
    if (beginRow < endRow) {
        // Finish the final stride
        DEBUG_ASSERT(beginRow >= rows.first());
        DEBUG_ASSERT(endRow - 1 <= rows.last());
        QModelIndex topLeft = index(beginRow, column);
        QModelIndex bottomRight = index(endRow - 1, column);
        emit dataChanged(topLeft, bottomRight, roles);
    }
}

TrackPointer BaseTrackTableModel::getTrackByRef(
        const TrackRef& trackRef) const {
    return m_pTrackCollectionManager->getTrackByRef(trackRef);
}

TrackId BaseTrackTableModel::doGetTrackId(
        const TrackPointer& pTrack) const {
    return pTrack ? pTrack->getId() : TrackId();
}

bool BaseTrackTableModel::updateTrackGenre(
        Track* pTrack,
        const QString& genre) const {
    return m_pTrackCollectionManager->updateTrackGenre(pTrack, genre);
}

#if defined(__EXTRA_METADATA__)
bool BaseTrackTableModel::updateTrackMood(
        Track* pTrack,
        const QString& mood) const {
    return m_pTrackCollectionManager->updateTrackMood(pTrack, mood);
}
#endif // __EXTRA_METADATA__

QVariant BaseTrackTableModel::getFieldVariant(
        const QModelIndex& index, ColumnCache::Column column) const {
    return index.sibling(index.row(), fieldIndex(column)).data();
}

QVariant BaseTrackTableModel::getFieldVariant(
        const QModelIndex& index, const QString& fieldName) const {
    return index.sibling(index.row(), fieldIndex(fieldName)).data();
}

QString BaseTrackTableModel::getFieldString(
        const QModelIndex& index, ColumnCache::Column column) const {
    return getFieldVariant(index, column).toString();
}
