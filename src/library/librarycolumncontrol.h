#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

#include "preferences/usersettings.h"

class ControlObject;
class ControlPushButton;
class WTrackTableViewHeader;

// Global, in-skin-driven override of library column visibility *and* relative
// width. Mirrors what the upstream right-click "Show or hide columns" menu
// plus drag-resize does, but the source of truth is mixxx.cfg under
// [Library]/ColumnVisible_<Name> + [Library]/ColumnWeight_<Name>, and the
// trigger is the [Library],column_visible_<name> + column_weight_<name>
// ControlObjects set by the skin.
//
// WTrackTableViewHeader saves/restores column order using the upstream
// per-model header_state_pb. This class applies visibility and weighted
// widths after that restore, overriding the protobuf's layout dimensions.
//
// Width semantics: each visible column has an integer weight (1..4). On
// every apply pass the visible weights are summed and each column is sized
// proportionally to fill the header's current width. Hiding a column
// re-distributes its share to the remaining visible columns automatically.
class LibraryColumnControl : public QObject {
    Q_OBJECT
  public:
    explicit LibraryColumnControl(UserSettingsPointer pConfig,
            QObject* parent = nullptr);
    ~LibraryColumnControl() override;

    // Atomic accessor used by WTrackTableViewHeader so widget/ doesn't have
    // to depend on library/ structurally. Returns nullptr in unit-test builds
    // where Library is never instantiated.
    static LibraryColumnControl* tryInstance();

    // Apply current visibility + width state to a header. Safe to call
    // repeatedly. No-op if pHeader has no model or zero width.
    void applyTo(WTrackTableViewHeader* pHeader);

    // Track live headers so CO changes can re-apply across all open views.
    void registerHeader(WTrackTableViewHeader* pHeader);
    void unregisterHeader(WTrackTableViewHeader* pHeader);

  private slots:
    void slotVisibilityChanged(double v);
    void slotWeightChanged(double v);
    void slotSizeChanged(double v);

  private:
    struct ManagedColumn {
        QString name;            // canonical lowercase, e.g. "title"
        QString visCfgKey;       // "ColumnVisible_Title"
        QString weightCfgKey;    // "ColumnWeight_Title"
        bool defaultVisible;
        int defaultWeight;
        // ControlPushButton (TOGGLE mode, 2 states) so a skin <PushButton>
        // bound to this CO actually toggles 0↔1 on press. A plain
        // ControlObject would leave WPushButton in PUSH mode where it only
        // emits 1.0 on press / 0.0 on release, but EMIT_ON_PRESS gates the
        // release-side write so the value gets stuck at 1.0.
        std::unique_ptr<ControlPushButton> pVisibleCO;
        std::unique_ptr<ControlObject> pWeightCO;
        // The two above, folded into the one control the settings page shows:
        // 0 = hidden, 1..4 = visible at that weight. A ControlPushButton in
        // TOGGLE mode with five states, so a skin <PushButton> bound to it
        // cycles OFF -> XS -> S -> M -> L -> OFF on each tap (WPushButton takes
        // its cycling behaviour from the CO's button mode) and reads its label
        // straight off the state.
        //
        // It exists because the page has to fit 480x420: a label plus a 56px
        // toggle plus a 160px four-segment strip is ~300px per column, and two
        // of those do not fit in 480, let alone the three columns the 240px
        // panel height forces. One 56px button per row does.
        //
        // Not the source of truth — the pair above still is, and still owns the
        // cfg keys. This mirrors them in both directions, so a value written
        // from anywhere (mixxx.cfg, a controller mapping, the hide-the-last-
        // column refusal below) still lands on the button.
        std::unique_ptr<ControlPushButton> pSizeCO;
    };

    void applyAllToHeader(WTrackTableViewHeader* pHeader);
    // The bodies of the two slots above, callable directly. A ControlObject
    // does not emit valueChanged for its own set(), so the size CO cannot make
    // a column visible by writing the visibility CO and expecting that slot to
    // do the cfg write — it has to come in here.
    void applyVisibility(ManagedColumn& col, bool wantVisible);
    void applyWeight(ManagedColumn& col, int weight);
    // Pushes `col`'s current visible+weight onto its size CO. No-op while a
    // size-CO change is the thing driving those two.
    void syncSizeCO(const ManagedColumn& col);
    int findLogicalIndexForColumn(WTrackTableViewHeader* pHeader,
            const QString& name);
    int countVisibleManaged() const;
    static int clampWeight(int w);

    // Guards the size CO <-> visible/weight CO mirroring against re-entering
    // itself: slotSizeChanged writes the pair, each of which would otherwise
    // write the size CO back from a half-updated state.
    bool m_syncingSize = false;

    const UserSettingsPointer m_pConfig;
    std::vector<ManagedColumn> m_columns;
    QList<WTrackTableViewHeader*> m_headers;
};
