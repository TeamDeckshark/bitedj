#include "library/librarycolumncontrol.h"

#include <QAbstractItemModel>
#include <QAtomicPointer>
#include <QHeaderView>
#include <algorithm>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "library/dao/trackschema.h"
#include "library/starrating.h"
#include "library/trackmodel.h"
#include "moc_librarycolumncontrol.cpp"
#include "widget/wtracktableviewheader.h"

namespace {
const QString kLibraryGroup = QStringLiteral("[Library]");
const QString kVisCOPrefix = QStringLiteral("column_visible_");
const QString kWeightCOPrefix = QStringLiteral("column_weight_");
const QString kVisCfgPrefix = QStringLiteral("ColumnVisible_");
const QString kWeightCfgPrefix = QStringLiteral("ColumnWeight_");

constexpr int kMinWeight = 1;
constexpr int kMaxWeight = 4;
constexpr int kMinSectionPx = 20;

// The rating column must never be squeezed below the width StarRating
// actually paints (5 stars x PaintingScaleFactor), or the last star(s) are
// clipped and the DJ can neither read nor set them. A little slack covers
// the delegate/cell padding around the polygon strip.
int minSectionPxForColumn(const QString& name) {
    if (name == LIBRARYTABLE_RATING) {
        static const int minRatingPx = StarRating().sizeHint().width() + 10;
        return minRatingPx;
    }
    return kMinSectionPx;
}

QAtomicPointer<LibraryColumnControl> s_pInstance;

struct ColumnSpec {
    const QString* pName; // canonical column name from trackschema.h
    bool defaultVisible;
    int defaultWeight;
};

// Every column the various TrackModel subclasses can expose gets a CO so
// mixxx.cfg is the single authority for visibility and weight. The 10
// curated entries at the top get UI in settings.xml; the rest default to
// invisible and exist only so an unmanaged column can't slip through with
// its natural pixel width and clobber the flex-weight layout. A power
// user can still surface any of them by flipping
// [Library],ColumnVisible_<name> in mixxx.cfg.
//
// Cfg-key suffix is the canonical lowercase column name (matches the CO
// item suffix and TrackModel::kHeaderNameRole), not CamelCase. Keeping a
// single source of truth for naming is worth the minor departure from
// the [Controls]/RateRangePercent precedent.
const ColumnSpec kSpecs[] = {
        // Curated default-visible columns (have UI in settings.xml).
        {&LIBRARYTABLE_TITLE, true, 4},
        {&LIBRARYTABLE_ARTIST, true, 4},
        {&LIBRARYTABLE_BPM, true, 1},
        {&LIBRARYTABLE_KEY, true, 1},
        {&LIBRARYTABLE_DURATION, true, 1},
        // Curated default-hidden columns (have UI in settings.xml).
        {&LIBRARYTABLE_GENRE, false, 2},
        {&LIBRARYTABLE_RATING, false, 1},
        {&LIBRARYTABLE_TIMESPLAYED, false, 1},
        {&LIBRARYTABLE_YEAR, false, 1},
        {&LIBRARYTABLE_COMMENT, false, 4},
        // Uncurated — every other potentially-visible column. No UI in
        // settings.xml; default invisible, weight 1. Listed by enum
        // order from ColumnCache::Column for review-friendliness.
        {&LIBRARYTABLE_ALBUM, false, 1},
        {&LIBRARYTABLE_ALBUMARTIST, false, 1},
        {&LIBRARYTABLE_COMPOSER, false, 1},
        {&LIBRARYTABLE_GROUPING, false, 1},
        {&LIBRARYTABLE_TRACKNUMBER, false, 1},
        {&LIBRARYTABLE_FILETYPE, false, 1},
        {&LIBRARYTABLE_BITRATE, false, 1},
        {&LIBRARYTABLE_REPLAYGAIN, false, 1},
        {&LIBRARYTABLE_SAMPLERATE, false, 1},
        {&LIBRARYTABLE_CHANNELS, false, 1},
        {&LIBRARYTABLE_DATETIMEADDED, false, 1},
        {&LIBRARYTABLE_LAST_PLAYED_AT, false, 1},
        {&LIBRARYTABLE_PREVIEW, false, 1},
        {&LIBRARYTABLE_COLOR, false, 1},
        {&LIBRARYTABLE_COVERART, false, 1},
        {&TRACKLOCATIONSTABLE_LOCATION, false, 1},
        {&PLAYLISTTRACKSTABLE_POSITION, false, 1},
        {&PLAYLISTTRACKSTABLE_DATETIMEADDED, false, 1},
};
} // namespace

LibraryColumnControl::LibraryColumnControl(
        UserSettingsPointer pConfig, QObject* parent)
        : QObject(parent),
          m_pConfig(pConfig) {
    m_columns.reserve(std::size(kSpecs));
    for (const auto& spec : kSpecs) {
        ManagedColumn col;
        col.name = *spec.pName;
        col.visCfgKey = kVisCfgPrefix + col.name;
        col.weightCfgKey = kWeightCfgPrefix + col.name;
        col.defaultVisible = spec.defaultVisible;
        col.defaultWeight = spec.defaultWeight;

        const bool visible = m_pConfig->getValue(
                ConfigKey(kLibraryGroup, col.visCfgKey),
                col.defaultVisible);
        const int weight = clampWeight(m_pConfig->getValue(
                ConfigKey(kLibraryGroup, col.weightCfgKey),
                col.defaultWeight));

        col.pVisibleCO = std::make_unique<ControlPushButton>(
                ConfigKey(kLibraryGroup, kVisCOPrefix + col.name));
        col.pVisibleCO->setButtonMode(ControlPushButton::TOGGLE);
        col.pVisibleCO->setStates(2);
        col.pVisibleCO->set(visible ? 1.0 : 0.0);
        connect(col.pVisibleCO.get(),
                &ControlObject::valueChanged,
                this,
                &LibraryColumnControl::slotVisibilityChanged);

        col.pWeightCO = std::make_unique<ControlObject>(
                ConfigKey(kLibraryGroup, kWeightCOPrefix + col.name));
        col.pWeightCO->set(static_cast<double>(weight));
        connect(col.pWeightCO.get(),
                &ControlObject::valueChanged,
                this,
                &LibraryColumnControl::slotWeightChanged);

        m_columns.push_back(std::move(col));
    }

    s_pInstance.storeRelease(this);
}

LibraryColumnControl::~LibraryColumnControl() {
    s_pInstance.storeRelease(nullptr);
}

LibraryColumnControl* LibraryColumnControl::tryInstance() {
    return s_pInstance.loadAcquire();
}

void LibraryColumnControl::registerHeader(WTrackTableViewHeader* pHeader) {
    if (pHeader && !m_headers.contains(pHeader)) {
        m_headers.append(pHeader);
    }
}

void LibraryColumnControl::unregisterHeader(WTrackTableViewHeader* pHeader) {
    m_headers.removeAll(pHeader);
}

void LibraryColumnControl::applyTo(WTrackTableViewHeader* pHeader) {
    applyAllToHeader(pHeader);
}

int LibraryColumnControl::clampWeight(int w) {
    return std::clamp(w, kMinWeight, kMaxWeight);
}

void LibraryColumnControl::slotVisibilityChanged(double v) {
    auto* pSender = qobject_cast<ControlObject*>(sender());
    if (!pSender) {
        return;
    }
    const QString senderKey = pSender->getKey().item;
    if (!senderKey.startsWith(kVisCOPrefix)) {
        return;
    }
    const QString name = senderKey.mid(kVisCOPrefix.size());

    auto it = std::find_if(m_columns.begin(), m_columns.end(),
            [&name](const ManagedColumn& c) { return c.name == name; });
    if (it == m_columns.end()) {
        return;
    }

    const bool wantVisible = v != 0.0;

    // Refuse to hide if this would leave zero managed columns visible
    // (the table would render blank). Mirrors the upstream safeguard in
    // WTrackTableViewHeader::showOrHideColumn. The CO has already been
    // updated to 0 by the time this slot fires, so countVisibleManaged()
    // reflects the post-toggle state.
    if (!wantVisible && countVisibleManaged() == 0) {
        // Snap the CO back. The set will re-fire valueChanged with v=1.0;
        // that re-entry takes the normal write-cfg-and-apply path,
        // idempotent against the current visible state.
        it->pVisibleCO->set(1.0);
        return;
    }

    m_pConfig->set(ConfigKey(kLibraryGroup, it->visCfgKey),
            ConfigValue{wantVisible ? 1 : 0});

    for (auto* pHeader : std::as_const(m_headers)) {
        applyAllToHeader(pHeader);
    }
}

void LibraryColumnControl::slotWeightChanged(double v) {
    auto* pSender = qobject_cast<ControlObject*>(sender());
    if (!pSender) {
        return;
    }
    const QString senderKey = pSender->getKey().item;
    if (!senderKey.startsWith(kWeightCOPrefix)) {
        return;
    }
    const QString name = senderKey.mid(kWeightCOPrefix.size());

    auto it = std::find_if(m_columns.begin(), m_columns.end(),
            [&name](const ManagedColumn& c) { return c.name == name; });
    if (it == m_columns.end()) {
        return;
    }

    const int clamped = clampWeight(static_cast<int>(v));
    if (static_cast<double>(clamped) != v) {
        it->pWeightCO->set(static_cast<double>(clamped));
        return;
    }

    m_pConfig->set(ConfigKey(kLibraryGroup, it->weightCfgKey),
            ConfigValue{clamped});

    for (auto* pHeader : std::as_const(m_headers)) {
        applyAllToHeader(pHeader);
    }
}

void LibraryColumnControl::applyAllToHeader(WTrackTableViewHeader* pHeader) {
    if (!pHeader || !pHeader->model()) {
        return;
    }

    // Resolve managed-column → logical-index up front, summing weights of
    // columns that will be visible.
    struct Resolved {
        const ManagedColumn* pCol;
        int li;
        bool visible;
        int weight;
    };
    std::vector<Resolved> resolved;
    resolved.reserve(m_columns.size());
    int totalWeight = 0;
    for (const auto& col : m_columns) {
        const int li = findLogicalIndexForColumn(pHeader, col.name);
        if (li < 0) {
            continue;
        }
        const bool visible = col.pVisibleCO->get() != 0.0;
        const int weight = clampWeight(static_cast<int>(col.pWeightCO->get()));
        resolved.push_back({&col, li, visible, weight});
        if (visible) {
            totalWeight += weight;
        }
    }

    // Apply visibility first so subsequent resizeSection calls operate on
    // the right set of visible logical indices.
    for (const auto& r : resolved) {
        pHeader->setSectionHidden(r.li, !r.visible);
    }

    // Enforce internal columns hidden on every pass, after the managed pass
    // so "internal" always wins (e.g. the preview column when there is no
    // preview deck). WTrackTableView only hides internal columns once, when
    // the model is first attached — if anything un-hides a section later
    // (a header reset, a stray restore), an internal column like the external
    // playlists' track_id would otherwise pop in as an untitled column of raw
    // ids with no toggle that can ever hide it again. Re-asserting here makes
    // the layout self-healing on the next apply (resize, CO change, reset).
    if (auto* pTrackModel = dynamic_cast<TrackModel*>(pHeader->model())) {
        for (int li = 0; li < pHeader->count(); ++li) {
            if (pTrackModel->isColumnInternal(li)) {
                pHeader->setSectionHidden(li, true);
            }
        }
    }

    if (totalWeight <= 0) {
        return;
    }

    // Distribute the header's available width proportionally across visible
    // managed columns. Width is read from the header itself — when the
    // table viewport resizes, our resizeEvent override re-runs this pass.
    const int availableWidth = pHeader->width();
    if (availableWidth <= 0) {
        return;
    }

    // Compute integer pixel widths proportional to weight, with the last
    // visible column absorbing any rounding remainder so widths sum exactly
    // to availableWidth.
    int assignedSoFar = 0;
    int lastVisibleIdxInResolved = -1;
    for (size_t i = 0; i < resolved.size(); ++i) {
        if (resolved[i].visible) {
            lastVisibleIdxInResolved = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < resolved.size(); ++i) {
        const auto& r = resolved[i];
        if (!r.visible) {
            continue;
        }
        int px;
        if (static_cast<int>(i) == lastVisibleIdxInResolved) {
            px = availableWidth - assignedSoFar;
        } else {
            px = (availableWidth * r.weight) / totalWeight;
        }
        const int minPx = minSectionPxForColumn(r.pCol->name);
        if (px < minPx) {
            px = minPx;
        }
        if (static_cast<int>(i) != lastVisibleIdxInResolved) {
            assignedSoFar += px;
        }
        pHeader->resizeSection(r.li, px);
    }
}

int LibraryColumnControl::findLogicalIndexForColumn(
        WTrackTableViewHeader* pHeader, const QString& name) {
    QAbstractItemModel* pModel = pHeader->model();
    if (!pModel) {
        return -1;
    }
    for (int li = 0; li < pHeader->count(); ++li) {
        const QString colName = pModel->headerData(
                                                li,
                                                Qt::Horizontal,
                                                TrackModel::kHeaderNameRole)
                                        .toString();
        if (colName == name) {
            return li;
        }
    }
    return -1;
}

int LibraryColumnControl::countVisibleManaged() const {
    int n = 0;
    for (const auto& col : m_columns) {
        if (col.pVisibleCO->get() != 0.0) {
            ++n;
        }
    }
    return n;
}
