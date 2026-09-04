#include "widget/wtracktableviewheader.h"

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QScopedValueRollback>
#include <QWidgetAction>

#include "library/librarycolumncontrol.h"
#include "library/trackmodel.h"
#include "moc_wtracktableviewheader.cpp"
#include "util/math.h"
#include "util/parented_ptr.h"
#include "widget/wmenucheckbox.h"

#define WTTVH_MINIMUM_SECTION_SIZE 20

HeaderViewState::HeaderViewState(const WTrackTableViewHeader& headers) {
    QAbstractItemModel* model = headers.model();
    for (int vi = 0; vi < headers.count(); ++vi) {
        int li = headers.logicalIndex(vi);
        mixxx::library::HeaderViewState::HeaderState* header_state =
                m_view_state.add_header_state();
        header_state->set_hidden(headers.isSectionHidden(li));
        // Unfortunately sectionSize() is always 0 for hidden columns. Though,
        // QHeaderView keeps track of hidden sizes internally, and we do the same.
        int size = headers.sectionSize(li);
        if (headers.isSectionHidden(li) && size == 0) {
            size = headers.getWidthOfHiddenColumn(li);
            if (size == 0) {
                // Indicates a hidden column we didn't enable yet, hence didn't
                // store its previous width when hiding it.
                // Let's get the default width from the track model.
                // If this also returns 0 for some reason, we'll reset to minimum
                // width when restoring the header.
                auto* pTrackModel = headers.model();
                DEBUG_ASSERT(pTrackModel);
                size = pTrackModel->headerData(
                                          li,
                                          headers.orientation(),
                                          TrackModel::kHeaderWidthRole)
                               .toInt();
            }
        }
        header_state->set_size(size);
        header_state->set_logical_index(li);
        header_state->set_visual_index(vi);
        const QString column_name = model->headerData(
                                                 li, Qt::Horizontal, TrackModel::kHeaderNameRole)
                                            .toString();
        // If there was some sort of error getting the column id,
        // we have to skip this one. (Happens with non-displayed columns)
        if (column_name.isEmpty()) {
            continue;
        }
        header_state->set_column_name(column_name.toStdString());
    }
    m_view_state.set_sort_indicator_shown(headers.isSortIndicatorShown());
    if (m_view_state.sort_indicator_shown()) {
        m_view_state.set_sort_indicator_section(headers.sortIndicatorSection());
        m_view_state.set_sort_order(
                static_cast<int>(headers.sortIndicatorOrder()));
    }
}

HeaderViewState::HeaderViewState(const QString& base64serialized) {
    // First decode the array from Base64, then initialize the protobuf from it.
    QByteArray array = QByteArray::fromBase64(base64serialized.toLatin1());
    if (!m_view_state.ParseFromArray(array.constData(), array.size())) {
        qWarning() << "Could not parse m_view_state from QByteArray of size "
                   << array.size();
        return;
    }
}

QString HeaderViewState::saveState() const {
    // Serialize the proto to a byte array, then encode the array as Base64.
#if GOOGLE_PROTOBUF_VERSION >= 3001000
    int size = static_cast<int>(m_view_state.ByteSizeLong());
#else
    int size = m_view_state.ByteSize();
#endif
    QByteArray array(size, '\0');
    if(m_view_state.SerializeToArray(array.data(), size)) {
        return QString(array.toBase64());
    } else {
        qWarning() << "Could not serialize m_view_state to QByteArray of size "
                   << array.size();
        return "";
    }
}

void HeaderViewState::restoreState(WTrackTableViewHeader* pHeaders) {
    const int max_columns =
            math_min(pHeaders->count(), m_view_state.header_state_size());

    typedef QMap<QString, mixxx::library::HeaderViewState::HeaderState*> state_map;
    state_map map;
    for (int i = 0; i < m_view_state.header_state_size(); ++i) {
        map[QString::fromStdString(m_view_state.header_state(i).column_name())] =
                m_view_state.mutable_header_state(i);
    }

    // First set all sections to be hidden and update logical indexes.
    for (int li = 0; li < pHeaders->count(); ++li) {
        pHeaders->setSectionHidden(li, true);
        auto it = map.find(pHeaders->model()->headerData(
                                                    li, Qt::Horizontal, TrackModel::kHeaderNameRole)
                        .toString());
        if (it != map.end()) {
            it.value()->set_logical_index(li);
        }
    }

    // Now restore
    for (int vi = 0; vi < max_columns; ++vi) {
        const mixxx::library::HeaderViewState::HeaderState& header =
                m_view_state.header_state(vi);
        const int li = header.logical_index();
        pHeaders->setSectionHidden(li, header.hidden());
        // If the stored size is 0 or less than the minimum column width,
        // we use the latter. This might happen if  WTTVH_MINIMUM_SECTION_SIZE
        // has been increased by us or the header state from database was corrupted.
        // Note: setting the size works even if the column is hidden. Size is stored
        // by QHeaderView internally and is applied once the column is shown.
        int size = math_max(header.size(), WTTVH_MINIMUM_SECTION_SIZE);
        pHeaders->resizeSection(li, size);
        pHeaders->moveSection(pHeaders->visualIndex(li), vi);
    }
    if (m_view_state.sort_indicator_shown()) {
        pHeaders->setSortIndicator(
                m_view_state.sort_indicator_section(),
                static_cast<Qt::SortOrder>(m_view_state.sort_order()));
    }
}

WTrackTableViewHeader::WTrackTableViewHeader(Qt::Orientation orientation,
        QWidget* pParent)
        : QHeaderView(orientation, pParent),
          m_menu(tr("Show or hide columns."), this),
          m_restoringHeaderState(false) {
    if (auto* pColumnControl = LibraryColumnControl::tryInstance()) {
        pColumnControl->registerHeader(this);
        // The appliance may be powered off without the normal widget
        // destruction path, so persist a completed Qt header drag at once.
        connect(this,
                &QHeaderView::sectionMoved,
                this,
                &WTrackTableViewHeader::slotSaveColumnOrder);
        // Whenever Qt re-initializes this header's sections (model reset,
        // column count change — the moments hidden-section state can be
        // dropped), re-assert the managed layout. The appliance has no
        // header context menu and a fixed window, so nothing else would
        // ever repair a stray unhidden column.
        connect(this,
                &QHeaderView::sectionCountChanged,
                this,
                &WTrackTableViewHeader::slotReapplyColumnControl);
    }
}

WTrackTableViewHeader::~WTrackTableViewHeader() {
    if (auto* pColumnControl = LibraryColumnControl::tryInstance()) {
        pColumnControl->unregisterHeader(this);
    }
}

void WTrackTableViewHeader::contextMenuEvent(QContextMenuEvent* pEvent) {
    // Bite DJ fork: column visibility is owned by LibraryColumnControl
    // via mixxx.cfg + the in-skin settings page. The legacy right-click
    // menu would be a confusing second source of truth (and unreachable
    // on a touch-only device anyway), so swallow the event without
    // showing it.
    if (LibraryColumnControl::tryInstance()) {
        pEvent->accept();
        return;
    }
    pEvent->accept();
    m_menu.popup(pEvent->globalPos());
}

void WTrackTableViewHeader::resizeEvent(QResizeEvent* pEvent) {
    QHeaderView::resizeEvent(pEvent);
    // Re-distribute column widths proportionally when the header's available
    // width changes (e.g. parent table viewport resizes). No-op against
    // stock Mixxx where LibraryColumnControl is never instantiated.
    if (auto* pColumnControl = LibraryColumnControl::tryInstance()) {
        pColumnControl->applyTo(this);
    }
}

void WTrackTableViewHeader::setModel(QAbstractItemModel* pModel) {
    TrackModel* pOldTrackModel = getTrackModel();

    if (dynamic_cast<QAbstractItemModel*>(pOldTrackModel) == pModel) {
        // If the models are the same, do nothing but the redundant call.
        QHeaderView::setModel(pModel);
        return;
    }

    // Won't happen in practice since the WTrackTableView new's a new
    // WTrackTableViewHeader each time a new TrackModel is loaded.
    // if (pOldTrackModel) {
    //     saveHeaderState();
    // }

    // First clear all the context menu actions for the old model.
    clearActions();

    // Now set the header view to show the new model
    QHeaderView::setModel(pModel);

    // Now build actions for the new TrackModel
    TrackModel* pTrackModel = dynamic_cast<TrackModel*>(pModel);

    if (!pTrackModel) {
        return;
    }

    // Restore saved header state to get sizes, column positioning, etc. back.
    m_hiddenColumnSizes.clear();
    restoreHeaderState();

    // Here we can override values to prevent restoring corrupt values from database
    setSectionsMovable(true);

    // Setting true in the next line causes Bug #925619 at least with Qt 4.6.1
    setCascadingSectionResizes(false);

    setMinimumSectionSize(WTTVH_MINIMUM_SECTION_SIZE);

    // Bite DJ fork: skip building the visibility-toggle menu entirely
    // when LibraryColumnControl is active. The menu is suppressed in
    // contextMenuEvent above, so any work to build it would be wasted —
    // and the legacy "all columns hidden" safety net at the bottom is
    // already provided by LibraryColumnControl::slotVisibilityChanged.
    if (LibraryColumnControl::tryInstance()) {
        // Re-assert the managed layout after every repopulation of the model
        // (BaseSqlTableModel::select() runs on rescans, search changes and
        // playlist switches that reuse this model, and signals rows — not a
        // reset). If anything drops the header's hidden-section state along
        // the way, internal columns (e.g. the external playlists' untitled
        // track_id — a bare column of numeric ids) and managed-hidden columns
        // (e.g. the skinless grey preview button column) would reappear with
        // no user-reachable way to hide them again.
        connect(pModel,
                &QAbstractItemModel::rowsInserted,
                this,
                &WTrackTableViewHeader::slotReapplyColumnControl,
                Qt::UniqueConnection);
        connect(pModel,
                &QAbstractItemModel::modelReset,
                this,
                &WTrackTableViewHeader::slotReapplyColumnControl,
                Qt::UniqueConnection);
        return;
    }

    // Create a checkbox for each column.
    // We want to keep the menu open after un/ticking a box because that allows
    // to toggle multiple columns in one go, i.e. without having to open the
    // menu again and again. This does not work with regular QActions so we
    // create QCheckboxes inside QWidgetAction.
    // * toggle a box with mouse click or Space on a selected box (via keyboard,
    //   not just hovered by mouse pointer)
    // * toggle and close by pressing Return on a selected box
    int columns = pModel->columnCount();
    for (int i = 0; i < columns; ++i) {
        if (pTrackModel->isColumnInternal(i)) {
            continue;
        }

        const QString title = pModel->headerData(i, orientation()).toString();

        // Custom QCheckBox with fixed hover behavior
        auto pCheckBox = make_parented<WMenuCheckBox>(title, &m_menu);
        // Keep a map of checkboxes and columns
        m_columnCheckBoxes.insert(i, pCheckBox.get());
        connect(pCheckBox.get(),
                &QCheckBox::toggled,
                this,
                [this, i] {
                    showOrHideColumn(i);
                });
        // If Mixxx starts the first time or the header states have been cleared
        // due to database schema evolution we gonna hide all columns that may
        // contain a potential large number of NULL values.  Here we uncheck
        // the items that are hidden by default (e.g., key column).
        if (!hasPersistedHeaderState() && pTrackModel->isColumnHiddenByDefault(i)) {
            pCheckBox->setChecked(false);
        } else {
            pCheckBox->setChecked(!isSectionHidden(i));
        }

        auto pAction = make_parented<QWidgetAction>(this);
        pAction->setDefaultWidget(pCheckBox.get());
        // Pressing Return triggers the action but that would not toggle the
        // checkbox, we need to do this ourselves while the menu is being closed.
        connect(pAction,
                &QAction::triggered,
                this,
                [pCheckBox{pCheckBox.get()}] {
                    pCheckBox->toggle();
                });
        m_menu.addAction(pAction);

    }

    // Safety check against someone getting stuck with all columns hidden
    // (produces an empty library table). Just re-show them all.
    if (hiddenCount() == columns) {
        for (int i = 0; i < columns; ++i) {
            showSection(i);
        }
    }
}

void WTrackTableViewHeader::saveHeaderState() {
    TrackModel* pTrackModel = getTrackModel();
    if (!pTrackModel) {
        return;
    }
    // Keep the upstream format for column order. BiteDJ re-applies its
    // managed visibility and weighted widths after restoring this state.
    // Convert the QByteArray to a Base64 string and save it.
    HeaderViewState view_state(*this);
    pTrackModel->setModelSetting("header_state_pb", view_state.saveState());
    //qDebug() << "Saving old header state:" << result << headerState;
}

void WTrackTableViewHeader::restoreHeaderState() {
    TrackModel* pTrackModel = getTrackModel();

    if (!pTrackModel) {
        return;
    }
    const QScopedValueRollback restoringGuard(m_restoringHeaderState, true);

    const QString headerStateString = pTrackModel->getModelSetting("header_state_pb");
    if (headerStateString.isNull()) {
        loadDefaultHeaderState();
    } else {
        // Load the previous header state (stored as serialized protobuf).
        // Decode it and restore it.
        //qDebug() << "Restoring header state from proto" << headerStateString;
        HeaderViewState view_state(headerStateString);
        if (!view_state.healthy()) {
            loadDefaultHeaderState();
        } else {
            view_state.restoreState(this);
        }
    }
    // Restore order first, then let BiteDJ's config override the saved
    // pixel widths and visibility. This does not move any sections.
    slotReapplyColumnControl();
}

void WTrackTableViewHeader::slotSaveColumnOrder() {
    if (!m_restoringHeaderState) {
        saveHeaderState();
    }
}

void WTrackTableViewHeader::loadDefaultHeaderState() {
    // TODO: isColumnHiddenByDefault logic probably belongs here now.
    QAbstractItemModel* pModel = model();
    for (int i = 0; i < count(); ++i) {
        int header_size = pModel->headerData(
                                        i, orientation(), TrackModel::kHeaderWidthRole)
                                  .toInt();
        if (header_size > 0) {
            resizeSection(i, header_size);
        }
    }
}

bool WTrackTableViewHeader::hasPersistedHeaderState() {
    TrackModel* pTrackModel = getTrackModel();
    if (!pTrackModel) {
        return false;
    }
    // Bite DJ fork: LibraryColumnControl always has authoritative state
    // in mixxx.cfg, so report "persisted" to short-circuit the first-run
    // hidden-by-default loop in WTrackTableView::setTrackTableModel —
    // restoreHeaderState already handled hidden-by-default columns above.
    if (LibraryColumnControl::tryInstance()) {
        return true;
    }
    const QString headerStateString = pTrackModel->getModelSetting("header_state_pb");
    return !headerStateString.isNull();
}

void WTrackTableViewHeader::slotReapplyColumnControl() {
    auto* pColumnControl = LibraryColumnControl::tryInstance();
    TrackModel* pTrackModel = getTrackModel();
    if (!pColumnControl || !pTrackModel) {
        return;
    }
    // After restoring the header state or repopulating the model, pre-hide the
    // hidden-by-default columns we don't manage, then let the control apply
    // visibility, internal-column hiding and flex widths.
    for (int i = 0; i < count(); ++i) {
        if (pTrackModel->isColumnHiddenByDefault(i)) {
            setSectionHidden(i, true);
        }
    }
    pColumnControl->applyTo(this);
}

void WTrackTableViewHeader::clearActions() {
    // The QActions are parented to the menu, so clearing deletes them. Since
    // they are deleted we don't have to disconnect their signals from the
    // mapper.
    m_columnCheckBoxes.clear();
    m_menu.clear();
}

void WTrackTableViewHeader::showOrHideColumn(int column) {
    auto it = m_columnCheckBoxes.constFind(column);
    if (it == m_columnCheckBoxes.constEnd()) {
        qWarning() << "WTrackTableViewHeader got invalid column" << column;
        return;
    }
    QCheckBox* pCheckBox = it.value();
    if (pCheckBox->isChecked()) {
        showSection(column);
        VERIFY_OR_DEBUG_ASSERT(sectionSize(column) >= WTTVH_MINIMUM_SECTION_SIZE) {
            resizeSection(column, WTTVH_MINIMUM_SECTION_SIZE);
        }
        m_hiddenColumnSizes.remove(column);
    } else {
        // If the user hides every column, the table will disappear. This guards
        // against that. Note: hiddenCount reflects number of checked QActions,
        // so size - hiddenCount will be zero the moment they uncheck the last
        // section.
        if (m_columnCheckBoxes.size() - hiddenCount() > 0) {
            m_hiddenColumnSizes.insert(column, sectionSize(column));
            hideSection(column);
        } else {
            // Otherwise, ignore the request and re-check this QAction.
            pCheckBox->setChecked(true);
        }
    }
}

int WTrackTableViewHeader::getWidthOfHiddenColumn(int column) const {
    const auto& it = m_hiddenColumnSizes.find(column);
    if (it != m_hiddenColumnSizes.constEnd()) {
        return it.value();
    }
    return 0;
}

int WTrackTableViewHeader::hiddenCount() {
    int count = 0;
    for (const auto& pCheckBox : std::as_const(m_columnCheckBoxes)) {
        if (!pCheckBox->isChecked()) {
            count += 1;
        }
    }
    return count;
}

TrackModel* WTrackTableViewHeader::getTrackModel() {
    TrackModel* pTrackModel = dynamic_cast<TrackModel*>(model());
    return pTrackModel;
}
