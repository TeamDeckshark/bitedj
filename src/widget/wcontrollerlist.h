#pragma once

#include <QList>
#include <QPointF>
#include <QPushButton>
#include <QStringList>

#include "widget/wwidget.h"

class QDomNode;
class QMouseEvent;
class QScrollArea;
class QVBoxLayout;
class SkinContext;

// Bite DJ: renders the connected MIDI controllers reported by
// ControllerSettings as a vertical stack of tappable rows. Each row tap
// advances the controller to its next available mapping (cycling through
// Disabled at the wrap). Rebuilds on ControllerSettings::rowsChanged; the
// wrapper widget stays empty when the singleton hasn't been constructed
// (stock Mixxx fallback).
//
// More controllers than fit the panel are reached by scrolling: the rows live
// in a QScrollArea, which can be scrolled either by dragging its content or by
// dragging the scroll bar (same as WUsbList).
class WControllerList : public WWidget {
    Q_OBJECT
  public:
    explicit WControllerList(QWidget* parent = nullptr);
    ~WControllerList() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    // On the touchscreen the child QPushButtons never receive the tap:
    // WWidget::event synthesizes the touch into a QMouseEvent delivered to
    // *this* wrapper, not the child under the finger (and never to the scroll
    // bar either), so QPushButton::clicked never fires. The whole gesture is
    // classified here instead: a press that travels becomes a scroll, one that
    // doesn't becomes a row tap dispatched on release (same pattern as
    // WUsbList). The clicked() connection is kept for the desktop/mouse path
    // where a real event does reach the button.
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

  private slots:
    void onRowsChanged(const QStringList& rowLabels, const QList<bool>& rowActive);
    void onRowClicked();
    void onBusyChanged(bool busy);

  private:
    enum class DragState {
        // No press in flight.
        Idle,
        // Press seen, but it hasn't travelled far enough to be a scroll yet.
        Pending,
        // The finger is dragging the content.
        Scrolling,
        // The press landed on the scroll bar, which is driving the scroll.
        ScrollBar,
    };

    void rebuildRows(const QStringList& rowLabels, const QList<bool>& rowActive);
    void renderEmpty();
    void applyEnabledToRows();
    void handleRowTap(int index);
    // Index of the row under globalPos, or -1 when the point misses every row
    // or falls outside the viewport (i.e. lands on a scrolled-away row).
    int rowIndexAt(const QPoint& globalPos) const;
    // Replays a press/move/release onto the vertical scroll bar so it can be
    // dragged directly. Qt only routes these to the bar when they carry it as
    // their target: the events WWidget synthesizes from touches carry none, and
    // QScrollBar drops a move that doesn't say the button is still held.
    void forwardToScrollBar(QMouseEvent* pEvent);

    // Rows are laid out inside the scroll area's content widget, not directly
    // in this one, so they can extend past the bottom of the panel.
    QScrollArea* m_pScrollArea;
    QWidget* m_pContent;
    QVBoxLayout* m_pLayout;
    QList<QPushButton*> m_rows;
    QPushButton* m_pEmptyRow;
    bool m_busy;

    DragState m_dragState;
    // Where the press landed, in global coords — the rows move underneath us
    // while the content scrolls away.
    QPointF m_pressGlobalPos;
    qreal m_lastGlobalY;
    // Sub-pixel remainder of the movement not yet applied to the scroll bar.
    qreal m_remainingDy;
};
