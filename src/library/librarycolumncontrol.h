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
// The per-model header_state_pb (which the upstream menu writes) is unreachable
// from the touch UI. This class wins over the protobuf for visibility and
// width — column order and sort indicator still come from the protobuf
// restore path in WTrackTableViewHeader, so those upstream behaviors are
// preserved.
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
    };

    void applyAllToHeader(WTrackTableViewHeader* pHeader);
    int findLogicalIndexForColumn(WTrackTableViewHeader* pHeader,
            const QString& name);
    int countVisibleManaged() const;
    static int clampWeight(int w);

    const UserSettingsPointer m_pConfig;
    std::vector<ManagedColumn> m_columns;
    QList<WTrackTableViewHeader*> m_headers;
};
