#pragma once

#include <QList>
#include <QPointF>
#include <QPushButton>
#include <QStringList>
#include <QTimer>

#include "widget/wwidget.h"

class QDomNode;
class QGridLayout;
class QLabel;
class QScrollArea;
class SkinContext;

// Bite DJ: renders the mounted USB drives reported by SystemSettings as a
// vertical stack of rows. Each row shows the drive label, a Record button and a
// dedicated Eject button. Eject is a two-tap action (safe-eject confirmation):
// the first tap arms the button (text -> "Confirm" + red [armed] style), the
// second tap within a few seconds ejects the drive (unload its tracks, then
// unmount).
// Rebuilds on SystemSettings::usbRowsChanged; the wrapper widget stays empty
// when the singleton hasn't been constructed (stock Mixxx fallback).
//
// Record is a single tap (nothing is destroyed by starting one) and starts
// recording the main output into a Recordings folder on that drive. The tapped
// button becomes "Stop Recording" and every other row's Record button is
// disabled, so only one drive can be recorded to at a time — enforced in
// SystemSettings, which owns the recording; this class only paints its state
// (from usbRecordingChanged, and from recordingRowIndex() on a rebuild).
//
// The Record column is wider while it says "Stop Recording" than at rest, so
// starting one takes width off the drive labels for as long as it runs. A fixed
// column would cost every row that width permanently, and at the 480px floor
// the labels have less of it to spare than one row does for the duration.
//
// More drives than fit the panel are reached by scrolling: the rows live in a
// QScrollArea, which can be scrolled either by dragging its content or by
// dragging the scroll bar.
class WUsbList : public WWidget {
    Q_OBJECT
  public:
    explicit WUsbList(QWidget* parent = nullptr);
    ~WUsbList() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    // On the touchscreen, WWidget::event() translates touches into synthesized
    // mouse events delivered to THIS widget (never to the rows or the scroll
    // bar, which are plain QWidgets — see wpushbutton.cpp, which is itself a
    // WWidget overriding mousePressEvent). So the whole gesture is handled
    // here: we classify it as a scroll or a tap and dispatch it ourselves.
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

  private slots:
    void onRowsChanged(const QStringList& labels);
    void onEjectClicked();
    void onRecordClicked();
    void onRecordingChanged(int recordingIndex);
    void onDisarmTimeout();

  private:
    enum class DragState {
        // No press to act on.
        Idle,
        // Press seen, gesture not classified yet: still either a tap or the
        // beginning of a content drag.
        Pending,
        // The content is following the finger.
        Scrolling,
        // The press landed on the scroll bar, which is driving the scroll.
        ScrollBar,
    };

    void rebuildRows(const QStringList& labels);
    void renderEmpty();
    void disarm();
    // Runs the arm-then-confirm eject flow for the row at index.
    void handleEjectTap(int index);
    // Starts recording onto the row's drive, or stops the recording running on
    // it. Cancels an armed Eject elsewhere in the list first — the tap moved on.
    void handleRecordTap(int index);
    // Paints the Record buttons for a recording on row `recordingIndex` (-1 for
    // none): that row's button reads "Stop Recording", every other row's is
    // disabled for as long as it runs.
    void applyRecordingState(int recordingIndex);
    // Index of the visible, enabled button under globalPos within `buttons`, or
    // -1 for none. A disabled button is not a target: WWidget synthesizes the
    // touch into a mouse event on the list, so nothing else would filter it out.
    int buttonIndexAt(const QList<QPushButton*>& buttons, const QPoint& globalPos) const;
    // Replays a press/move/release onto the vertical scroll bar so it can be
    // dragged with a finger. Buttons are forced to the left button because the
    // events WWidget synthesizes from touches carry none, and QScrollBar drops
    // moves that don't look like a held drag.
    void forwardToScrollBar(QMouseEvent* pEvent);

    // Rows are laid out inside the scroll area's content widget, not directly
    // in this widget, so the list can grow past the height the skin gives us.
    QScrollArea* m_pScrollArea;
    QWidget* m_pContent;
    // Grid: one row per drive, column 0 = name label, column 1 = Record button,
    // column 2 = Eject button. The fixed button columns can't be clipped
    // off-screen by a long label.
    QGridLayout* m_pLayout;
    // All row label+button widgets, kept for teardown on rebuild.
    QList<QWidget*> m_rowWidgets;
    // Eject buttons only, indexed parallel to the drive list for arm/eject.
    QList<QPushButton*> m_ejectButtons;
    // Record buttons, indexed parallel to the drive list as well.
    QList<QPushButton*> m_recordButtons;
    QStringList m_rowLabels;
    QLabel* m_pEmptyRow;
    int m_armedIndex;
    QTimer m_disarmTimer;

    DragState m_dragState;
    // Press and last seen positions in global coordinates, which stay valid
    // while the content underneath us scrolls away.
    QPointF m_pressGlobalPos;
    qreal m_lastGlobalY;
    // Sub-pixel remainder of the movement not yet applied to the scroll bar.
    qreal m_remainingDy;
};
