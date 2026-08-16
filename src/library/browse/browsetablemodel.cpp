#include "library/browse/browsetablemodel.h"

#include <QApplication>
#include <QColor>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QStringList>
#include <QStyle>
#include <QUrl>

#include "library/browse/browsetablemodel.h"
#include "library/browse/browsethread.h"
#include "library/dao/trackschema.h"
#include "library/playedtracks.h"
#include "library/tabledelegates/defaultdelegate.h"
#include "library/tabledelegates/previewbuttondelegate.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "moc_browsetablemodel.cpp"
#include "recording/recordingmanager.h"
#include "track/track.h"
#include "util/clipboard.h"
#include "widget/wlibrarytableview.h"

namespace {

// Colour used for a track whose file has gone missing.
// Matches WTrackTableView::kDefaultTrackMissingColor; the Browse model is a
// plain QStandardItemModel with no fs_deleted column, so it can't reuse the
// SQL models' styleable trackMissingColor and carries its own constant.
const QColor kMissingTrackColor = QColor(QStringLiteral("#ff0000"));

// Colour used for a track that has already been played this session. Same role
// as the SQL models' trackPlayedColor (see the Bite DJ skin's style.qss); like
// the missing colour above it has to be a constant here because this model has
// no styleable WTrackTableView property to read from.
const QColor kPlayedTrackColor = QColor(QStringLiteral("#4aa3e0"));

// Delegate that forces a flagged row's colour to render. The Bite DJ skin sets
// `#LibraryWrapper QTableView { color: ... }`, and Qt's style-sheet palette
// resolution clobbers the model's Qt::ForegroundRole with that colour — so a
// normal delegate would always draw white. For flagged rows we therefore paint
// the text ourselves; normal rows fall through to the base delegate and keep
// the skin's styling untouched.
class TintedTextDelegate : public DefaultDelegate {
  public:
    explicit TintedTextDelegate(QTableView* pTableView)
            : DefaultDelegate(pTableView) {
    }

    void paint(QPainter* painter,
            const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        const QVariant fgData = index.data(Qt::ForegroundRole);
        if (!fgData.canConvert<QColor>()) {
            // Not flagged: render normally (skin's QSS colour applies).
            DefaultDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QString text = opt.text;
        // Let the style draw the background/selection only, then overlay the
        // text in the flagged colour so the QSS `color` can't override it.
        opt.text.clear();
        QStyle* pStyle = opt.widget ? opt.widget->style() : QApplication::style();
        pStyle->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QRect textRect = pStyle->subElementRect(
                QStyle::SE_ItemViewItemText, &opt, opt.widget);
        int alignment = static_cast<int>(opt.displayAlignment);
        if (!(alignment & Qt::AlignVertical_Mask)) {
            alignment |= Qt::AlignVCenter;
        }
        const QString elided = opt.fontMetrics.elidedText(
                text, opt.textElideMode, textRect.width());
        painter->save();
        painter->setPen(fgData.value<QColor>());
        painter->drawText(textRect, alignment, elided);
        painter->restore();
    }
};

/// Helper to insert values into a QList with specific indices.
///
/// *For legacy code only - Do not use for new code!*
template<typename T>
void listAppendOrReplaceAt(QList<T>* pList, int index, const T& value) {
    VERIFY_OR_DEBUG_ASSERT(index <= pList->size()) {
        qWarning() << "listAppendOrReplaceAt: Padding list with"
                   << (index - pList->size()) << "default elements";
        while (index > pList->size()) {
            pList->append(T());
        }
    }
    VERIFY_OR_DEBUG_ASSERT(index == pList->size()) {
        pList->replace(index, value);
        return;
    }
    pList->append(value);
}

} // anonymous namespace

BrowseTableModel::BrowseTableModel(QObject* parent,
        TrackCollectionManager* pTrackCollectionManager,
        RecordingManager* pRecordingManager)
        : TrackModel(pTrackCollectionManager->internalCollection()->database(),
                  "mixxx.db.model.browse"),
          QStandardItemModel(parent),
          m_pTrackCollectionManager(pTrackCollectionManager),
          m_pRecordingManager(pRecordingManager),
          m_previewDeckGroup(PlayerManager::groupForPreviewDeck(0)) {
    QStringList headerLabels;
    /// The order of the columns appended here must exactly match the ordering
    /// of the enum that is used for indexing.
    listAppendOrReplaceAt(&headerLabels, COLUMN_PREVIEW, tr("Preview"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_FILENAME, tr("Filename"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_ARTIST, tr("Artist"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_TITLE, tr("Title"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_ALBUM, tr("Album"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_TRACK_NUMBER, tr("Track #"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_YEAR, tr("Year"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_GENRE, tr("Genre"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_COMPOSER, tr("Composer"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_COMMENT, tr("Comment"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_DURATION, tr("Time"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_BPM, tr("BPM"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_KEY, tr("Key"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_TYPE, tr("Type"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_BITRATE, tr("Bitrate"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_NATIVELOCATION, tr("Location"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_ALBUMARTIST, tr("Album Artist"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_GROUPING, tr("Grouping"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_FILE_MODIFIED_TIME, tr("File Modified"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_FILE_CREATION_TIME, tr("File Created"));
    listAppendOrReplaceAt(&headerLabels, COLUMN_REPLAYGAIN, tr("ReplayGain"));

    m_searchColumns = {
            COLUMN_FILENAME,
            COLUMN_ARTIST,
            COLUMN_ALBUM,
            COLUMN_TITLE,
            COLUMN_GENRE,
            COLUMN_COMPOSER,
            COLUMN_COMMENT,
            COLUMN_ALBUMARTIST,
            COLUMN_GROUPING};

    setDefaultSort(COLUMN_FILENAME, Qt::AscendingOrder);

    for (int i = 0; i < static_cast<int>(TrackModel::SortColumnId::IdMax); ++i) {
        m_columnIndexBySortColumnId[i] = -1;
    }
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Filename)] = COLUMN_FILENAME;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Artist)] = COLUMN_ARTIST;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Title)] = COLUMN_TITLE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Album)] = COLUMN_ALBUM;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::AlbumArtist)] =
            COLUMN_ALBUMARTIST;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Year)] = COLUMN_YEAR;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Genre)] = COLUMN_GENRE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Composer)] = COLUMN_COMPOSER;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] = COLUMN_GROUPING;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::TrackNumber)] =
            COLUMN_TRACK_NUMBER;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileType)] = COLUMN_TYPE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::NativeLocation)] =
            COLUMN_NATIVELOCATION;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Comment)] = COLUMN_COMMENT;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Duration)] = COLUMN_DURATION;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::BitRate)] = COLUMN_BITRATE;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Bpm)] = COLUMN_BPM;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::ReplayGain)] =
            COLUMN_REPLAYGAIN;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Key)] = COLUMN_KEY;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Preview)] = COLUMN_PREVIEW;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::Grouping)] = COLUMN_GROUPING;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileModifiedTime)] =
            COLUMN_FILE_MODIFIED_TIME;
    m_columnIndexBySortColumnId[static_cast<int>(
            TrackModel::SortColumnId::FileCreationTime)] =
            COLUMN_FILE_CREATION_TIME;

    m_sortColumnIdByColumnIndex.clear();
    for (int i = static_cast<int>(TrackModel::SortColumnId::IdMin);
            i < static_cast<int>(TrackModel::SortColumnId::IdMax);
            ++i) {
        TrackModel::SortColumnId sortColumn = static_cast<TrackModel::SortColumnId>(i);
        int columnIndex = m_columnIndexBySortColumnId[static_cast<int>(sortColumn)];
        if (columnIndex >= 0) {
            m_sortColumnIdByColumnIndex.insert(columnIndex, sortColumn);
        }
    }

    setHorizontalHeaderLabels(headerLabels);

    // Bite DJ fork: tag each column with its canonical trackschema name
    // under TrackModel::kHeaderNameRole so LibraryColumnControl can resolve
    // managed columns by name (the same contract BaseTrackTableModel honors
    // via setColumnHeader). Columns with no canonical equivalent
    // (Filename, File Modified, File Created) intentionally get no role
    // and remain unmanaged — they fall through to the resolver's default
    // unmanaged path and keep their natural pixel width.
    const auto setName = [this](int column, const QString& name) {
        setHeaderData(column, Qt::Horizontal, name, TrackModel::kHeaderNameRole);
    };
    setName(COLUMN_PREVIEW, LIBRARYTABLE_PREVIEW);
    setName(COLUMN_ARTIST, LIBRARYTABLE_ARTIST);
    setName(COLUMN_TITLE, LIBRARYTABLE_TITLE);
    setName(COLUMN_ALBUM, LIBRARYTABLE_ALBUM);
    setName(COLUMN_TRACK_NUMBER, LIBRARYTABLE_TRACKNUMBER);
    setName(COLUMN_YEAR, LIBRARYTABLE_YEAR);
    setName(COLUMN_GENRE, LIBRARYTABLE_GENRE);
    setName(COLUMN_COMPOSER, LIBRARYTABLE_COMPOSER);
    setName(COLUMN_COMMENT, LIBRARYTABLE_COMMENT);
    setName(COLUMN_DURATION, LIBRARYTABLE_DURATION);
    setName(COLUMN_BPM, LIBRARYTABLE_BPM);
    setName(COLUMN_KEY, LIBRARYTABLE_KEY);
    setName(COLUMN_TYPE, LIBRARYTABLE_FILETYPE);
    setName(COLUMN_BITRATE, LIBRARYTABLE_BITRATE);
    setName(COLUMN_NATIVELOCATION, TRACKLOCATIONSTABLE_LOCATION);
    setName(COLUMN_ALBUMARTIST, LIBRARYTABLE_ALBUMARTIST);
    setName(COLUMN_GROUPING, LIBRARYTABLE_GROUPING);
    setName(COLUMN_REPLAYGAIN, LIBRARYTABLE_REPLAYGAIN);

    // register the QList<T> as a metatype since we use QueuedConnection below
    qRegisterMetaType<QList<QList<QStandardItem*>>>(
            "QList< QList<QStandardItem*>>");
    qRegisterMetaType<BrowseTableModel*>("BrowseTableModel*");

    m_pBrowseThread = BrowseThread::getInstanceRef();
    connect(m_pBrowseThread.data(),
            &BrowseThread::clearModel,
            this,
            &BrowseTableModel::slotClear,
            Qt::QueuedConnection);

    connect(m_pBrowseThread.data(),
            &BrowseThread::rowsAppended,
            this,
            &BrowseTableModel::slotInsert,
            Qt::QueuedConnection);

    connect(&PlayerInfo::instance(),
            &PlayerInfo::trackChanged,
            this,
            &BrowseTableModel::trackChanged);
    // Bite DJ: repaint filenames when a track starts playing (or the session is
    // reset from Settings) so the 'played' colour follows.
    connect(&PlayedTracks::instance(),
            &PlayedTracks::playedTracksChanged,
            this,
            &BrowseTableModel::slotPlayedTracksChanged);
    trackChanged(m_previewDeckGroup,
            PlayerInfo::instance().getTrackInfo(m_previewDeckGroup),
            TrackPointer());
}

BrowseTableModel::~BrowseTableModel() {
}

int BrowseTableModel::columnIndexFromSortColumnId(TrackModel::SortColumnId column) const {
    if (column < TrackModel::SortColumnId::IdMin ||
            column >= TrackModel::SortColumnId::IdMax) {
        return -1;
    }

    return m_columnIndexBySortColumnId[static_cast<int>(column)];
}

TrackModel::SortColumnId BrowseTableModel::sortColumnIdFromColumnIndex(int index) const {
    return m_sortColumnIdByColumnIndex.value(index, TrackModel::SortColumnId::Invalid);
}

const QList<int>& BrowseTableModel::searchColumns() const {
    return m_searchColumns;
}

void BrowseTableModel::setPath(mixxx::FileAccess path) {
    VERIFY_OR_DEBUG_ASSERT(m_pBrowseThread) {
        return;
    }

    if (path.info().hasLocation() && path.info().isDir()) {
        m_currentDirectory = path.info().location();
        m_pBrowseThread->executePopulation(std::move(path), this);
    } else {
        m_currentDirectory = {};
        m_pBrowseThread->executePopulation({}, this);
    }
}

TrackPointer BrowseTableModel::getTrack(const QModelIndex& index) const {
    return getTrackByRef(TrackRef::fromFilePath(getTrackLocation(index)));
}

TrackPointer BrowseTableModel::getTrackByRef(const TrackRef& trackRef) const {
    if (m_pRecordingManager->getRecordingLocation() == trackRef.getLocation()) {
        QMessageBox::critical(nullptr,
                tr("Mixxx Library"),
                tr("Could not load the following file because it is in use by "
                   "Mixxx or another application.") +
                        "\n" + trackRef.getLocation());
        return TrackPointer();
    }
    // NOTE(uklotzde, 2015-12-08): Accessing tracks from the browse view
    // will implicitly add them to the library. Is this really what we
    // want here??
    // NOTE(rryan, 2015-12-27): This was intentional at the time since
    // some people use Browse instead of the library and we want to let
    // them edit the tracks in a way that persists across sessions
    // and we didn't want to edit the files on disk by default
    // unless the user opts in to that.
    return m_pTrackCollectionManager->getOrAddTrack(trackRef);
}

QString BrowseTableModel::getTrackLocation(const QModelIndex& index) const {
    int row = index.row();

    QModelIndex index2 = this->index(row, COLUMN_NATIVELOCATION);
    QString nativeLocation = data(index2).toString();
    QString location = QDir::fromNativeSeparators(nativeLocation);
    return location;
}

TrackId BrowseTableModel::getTrackId(const QModelIndex& index) const {
    TrackPointer pTrack = getTrack(index);
    if (pTrack) {
        return pTrack->getId();
    } else {
        qWarning()
                << "Track is not available in library"
                << getTrackUrl(index);
        return TrackId();
    }
}

QVariant BrowseTableModel::data(const QModelIndex& index, int role) const {
    // Paint the row red once a load has revealed the file to be missing, so the
    // DJ can see at a glance which entries are dead (e.g. left over after a USB
    // drive was pulled). Other roles are unaffected.
    // Tracks already played this session get the same treatment in blue, so a
    // DJ scrolling a USB folder can see what is already spent. Missing wins
    // over played: a dead file matters more than a played one.
    if (role == Qt::ForegroundRole && index.column() != COLUMN_PREVIEW) {
        const PlayedTracks& playedTracks = PlayedTracks::instance();
        if (!m_missingLocations.isEmpty() || !playedTracks.isEmpty()) {
            const QString location = getTrackLocation(index);
            if (m_missingLocations.contains(location)) {
                return QVariant::fromValue(kMissingTrackColor);
            }
            if (playedTracks.isPlayed(location)) {
                return QVariant::fromValue(kPlayedTrackColor);
            }
        }
    }
    return QStandardItemModel::data(index, role);
}

bool BrowseTableModel::verifyTrackFileExists(const QModelIndex& index) {
    const QString location = getTrackLocation(index);
    if (location.isEmpty()) {
        // Nothing to check against; let the normal load path handle it.
        return true;
    }

    // Already known missing: refuse immediately without touching the
    // filesystem. A stat() on a pulled USB mount can block, and this path runs
    // on every re-tap of a dead entry, so the fast rejection keeps the browse
    // view responsive.
    if (m_missingLocations.contains(location)) {
        return false;
    }

    if (QFileInfo::exists(location)) {
        // Not flagged (checked above) and present: nothing to do. Flags are
        // dropped when the directory listing is rebuilt (slotClear), so a
        // re-inserted drive recovers on the next navigation/refresh.
        return true;
    }

    // First time we've seen it missing: flag the row red, then allow this one
    // load to proceed. BaseTrackPlayerImpl::slotLoadTrack re-checks existence,
    // raises the single "could not be found" notification and keeps the deck's
    // current track. Subsequent taps are blocked above — no duplicate alert.
    m_missingLocations.insert(location);
    const int row = index.row();
    emit dataChanged(this->index(row, 0),
            this->index(row, NUM_COLUMNS - 1),
            {Qt::ForegroundRole});
    return true;
}

QUrl BrowseTableModel::getTrackUrl(const QModelIndex& index) const {
    const QString trackLocation = getTrackLocation(index);
    DEBUG_ASSERT(trackLocation.trimmed() == trackLocation);
    if (trackLocation.isEmpty()) {
        return {};
    }
    return QUrl::fromLocalFile(trackLocation);
}

CoverInfo BrowseTableModel::getCoverInfo(const QModelIndex& index) const {
    TrackPointer pTrack = getTrack(index);
    if (pTrack) {
        return CoverInfo(pTrack->getCoverInfo(), getTrackLocation(index));
    } else {
        qWarning()
                << "Track is not available in library"
                << getTrackUrl(index);
        return CoverInfo();
    }
}

const QVector<int> BrowseTableModel::getTrackRows(TrackId trackId) const {
    Q_UNUSED(trackId);
    // We can't implement this as it stands.
    return QVector<int>();
}

void BrowseTableModel::search(const QString&) {
}

const QString BrowseTableModel::currentSearch() const {
    return QString("");
}

bool BrowseTableModel::isColumnInternal(int) {
    return false;
}

bool BrowseTableModel::isColumnHiddenByDefault(int column) {
    if (column == COLUMN_FILENAME ||
            column == COLUMN_COMPOSER ||
            column == COLUMN_TRACK_NUMBER ||
            column == COLUMN_YEAR ||
            column == COLUMN_GROUPING ||
            column == COLUMN_NATIVELOCATION ||
            column == COLUMN_ALBUMARTIST ||
            column == COLUMN_FILE_CREATION_TIME ||
            column == COLUMN_REPLAYGAIN) {
        return true;
    }
    return false;
}

void BrowseTableModel::moveTrack(const QModelIndex&, const QModelIndex&) {
}

void BrowseTableModel::copyTracks(const QModelIndexList& indices) const {
    Clipboard::start();
    for (const QModelIndex& index : indices) {
        if (index.isValid()) {
            Clipboard::add(QUrl::fromLocalFile(getTrackLocation(index)));
        }
    }
    Clipboard::finish();

    // TODO Investigate if we can also implement cut and paste (via QFile
    // operations) so mixxx could manage files in the filesystem, rather than
    // having to go switch between mixxx and the system file browser.
}

void BrowseTableModel::removeTracks(const QModelIndexList&) {
}

QMimeData* BrowseTableModel::mimeData(const QModelIndexList& indexes) const {
    QMimeData* mimeData = new QMimeData();
    QList<QUrl> urls;

    // Ok, so the list of indexes we're given contains separates indexes for
    // each column, so even if only one row is selected, we'll have like 7
    // indexes.  We need to only count each row once:
    QList<int> rows;

    foreach (QModelIndex index, indexes) {
        if (index.isValid()) {
            if (!rows.contains(index.row())) {
                rows.push_back(index.row());
                QUrl url = getTrackUrl(index);
                if (!url.isValid()) {
                    qDebug() << "ERROR invalid url" << url;
                    continue;
                }
                urls.append(url);
            }
        }
    }
    mimeData->setUrls(urls);
    return mimeData;
}

void BrowseTableModel::slotPlayedTracksChanged() {
    const int rows = rowCount();
    if (rows <= 0) {
        return;
    }
    // We don't know which rows hold the affected location(s) without scanning
    // the whole listing, so signal the full range: the view only repaints what
    // is on screen, and this fires at most once per track change.
    emit dataChanged(index(0, 0),
            index(rows - 1, NUM_COLUMNS - 1),
            {Qt::ForegroundRole});
}

void BrowseTableModel::slotClear(BrowseTableModel* caller_object) {
    if (caller_object == this) {
        removeRows(0, rowCount());
        // The row indices these locations referred to are gone; re-discover
        // missing files lazily on the next load attempt in the new listing.
        m_missingLocations.clear();
    }
}

void BrowseTableModel::slotInsert(const QList<QList<QStandardItem*>>& rows,
        BrowseTableModel* caller_object) {
    // There exists more than one BrowseTableModel in Mixxx and we only want to
    // receive items this object has 'ordered' from the BrowseThread (singleton)
    if (caller_object == this) {
        emit saveModelState();
        //qDebug() << "BrowseTableModel::slotInsert";
        for (int i = 0; i < rows.size(); ++i) {
            appendRow(rows.at(i));
        }
        emit restoreModelState();
    }
}

TrackModel::Capabilities BrowseTableModel::getCapabilities() const {
    return Capability::AddToTrackSet |
            Capability::AddToAutoDJ |
            Capability::LoadToDeck |
            Capability::LoadToPreviewDeck |
            Capability::LoadToSampler |
            Capability::RemoveFromDisk |
            Capability::Sorting;
}

QString BrowseTableModel::modelKey(bool noSearch) const {
    // Searching is handled by the proxy model, so if there is an active search
    // modelkey is composed there, too.
    Q_UNUSED(noSearch);
    return QStringLiteral("browse:") + m_currentDirectory;
}

Qt::ItemFlags BrowseTableModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags defaultFlags = QAbstractItemModel::flags(index);

    // Enable dragging songs from this data model to elsewhere (like the
    // waveform widget to load a track into a Player).
    defaultFlags |= Qt::ItemIsDragEnabled;

    int column = index.column();

    switch (column) {
    case COLUMN_FILENAME:
    case COLUMN_BITRATE:
    case COLUMN_DURATION:
    case COLUMN_TYPE:
    case COLUMN_FILE_MODIFIED_TIME:
    case COLUMN_FILE_CREATION_TIME:
    case COLUMN_REPLAYGAIN:
        // read-only
        return defaultFlags;
    default:
        // editable
        return defaultFlags | Qt::ItemIsEditable;
    }
}

bool BrowseTableModel::setData(
        const QModelIndex& index,
        const QVariant& value,
        int role) {
    Q_UNUSED(role);

    QStandardItem* item = itemFromIndex(index);
    DEBUG_ASSERT(nullptr != item);

    TrackPointer pTrack(getTrack(index));
    if (!pTrack) {
        qWarning() << "BrowseTableModel::setData():"
                   << "Failed to resolve track"
                   << getTrackLocation(index);
        // restore previous item content
        item->setText(index.data().toString());
        item->setToolTip(item->text());
        return false;
    }

    // check if one the item were edited
    int col = index.column();
    switch (col) {
    case COLUMN_ARTIST:
        pTrack->setArtist(value.toString());
        break;
    case COLUMN_TITLE:
        pTrack->setTitle(value.toString());
        break;
    case COLUMN_ALBUM:
        pTrack->setAlbum(value.toString());
        break;
    case COLUMN_BPM:
        pTrack->trySetBpm(value.toDouble());
        break;
    case COLUMN_KEY:
        pTrack->setKeyText(value.toString());
        break;
    case COLUMN_TRACK_NUMBER:
        pTrack->setTrackNumber(value.toString());
        break;
    case COLUMN_COMMENT:
        pTrack->setComment(value.toString());
        break;
    case COLUMN_GENRE:
        m_pTrackCollectionManager->updateTrackGenre(pTrack.get(), value.toString());
        break;
    case COLUMN_COMPOSER:
        pTrack->setComposer(value.toString());
        break;
    case COLUMN_YEAR:
        pTrack->setYear(value.toString());
        break;
    case COLUMN_ALBUMARTIST:
        pTrack->setAlbumArtist(value.toString());
        break;
    case COLUMN_GROUPING:
        pTrack->setGrouping(value.toString());
        break;
    default:
        qWarning() << "BrowseTableModel::setData():"
                   << "No tagger column";
        // restore previous item context
        item->setText(index.data().toString());
        item->setToolTip(item->text());
        return false;
    }

    item->setText(value.toString());
    item->setToolTip(item->text());
    return true;
}

void BrowseTableModel::trackChanged(
        const QString& group, TrackPointer pNewTrack, TrackPointer pOldTrack) {
    Q_UNUSED(pOldTrack);
    if (group == m_previewDeckGroup) {
        for (int row = 0; row < rowCount(); ++row) {
            QModelIndex i = index(row, COLUMN_PREVIEW);
            if (i.data().toBool()) {
                QStandardItem* item = itemFromIndex(i);
                item->setText("0");
            }
        }
        if (pNewTrack) {
            QString trackLocation = pNewTrack->getLocation();
            for (int row = 0; row < rowCount(); ++row) {
                QModelIndex i = index(row, COLUMN_PREVIEW);
                QString location = getTrackLocation(i);
                if (location == trackLocation) {
                    QStandardItem* item = itemFromIndex(i);
                    item->setText("1");
                    break;
                }
            }
        }
    }
}

bool BrowseTableModel::isColumnSortable(int column) const {
    return COLUMN_PREVIEW != column;
}

QAbstractItemDelegate* BrowseTableModel::delegateForColumn(const int i, QObject* pParent) {
    WLibraryTableView* pTableView = qobject_cast<WLibraryTableView*>(pParent);
    DEBUG_ASSERT(pTableView);
    if (PlayerInfo::instance().numPreviewDecks() > 0 && i == COLUMN_PREVIEW) {
        return new PreviewButtonDelegate(pTableView, i);
    }
    // Custom delegate on every text column so a flagged row renders in its
    // missing/played colour despite the skin's QSS text colour, and so the
    // whole row is tinted like it is in the SQL-backed views. The Filename
    // column is hidden in the Bite DJ column layout, so tinting it alone would
    // be invisible here. See TintedTextDelegate.
    return new TintedTextDelegate(pTableView);
}

bool BrowseTableModel::updateTrackGenre(
        Track* pTrack,
        const QString& genre) const {
    return m_pTrackCollectionManager->updateTrackGenre(pTrack, genre);
}

#if defined(__EXTRA_METADATA__)
bool BrowseTableModel::updateTrackMood(
        Track* pTrack,
        const QString& mood) const {
    return m_pTrackCollectionManager->updateTrackMood(pTrack, mood);
}
#endif // __EXTRA_METADATA__

void BrowseTableModel::releaseBrowseThread() {
    // The shared browse thread is stopped in the destructor
    // if this is the last reference. All references must be reset before
    // the library is destructed.
    m_pBrowseThread.reset();
}
