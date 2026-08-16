#pragma once

#include <QObject>
#include <QPointF>
#include <Qt>

class QAbstractScrollArea;
class QEvent;
class QMouseEvent;
class QPointingDevice;

/// Scrolls a scroll area by dragging its content, which is how a touch screen
/// is expected to browse long lists.
///
/// The filter watches the viewport and holds the press back until it knows
/// what the gesture is:
/// * moved far enough vertically: the content follows the finger and the view
///   never sees the press, so neither an item drag nor a selection rubber band
///   can start
/// * released without moving: the press is replayed just before the release,
///   which leaves taps (selection, editing, double click) untouched
class TouchScrollFilter : public QObject {
    Q_OBJECT

  public:
    /// Enables drag scrolling for pScrollArea. The filter is owned by the
    /// scroll area. Item views are switched to per-pixel scrolling, which drag
    /// scrolling requires to follow the finger.
    static void install(QAbstractScrollArea* pScrollArea);

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;

  private:
    enum class State {
        /// No press to act on
        Idle,
        /// Press held back, gesture not classified yet
        Pending,
        /// Content is following the finger
        Scrolling,
    };

    explicit TouchScrollFilter(QAbstractScrollArea* pScrollArea);

    bool handleMousePress(QMouseEvent* pEvent);
    bool handleMouseMove(QMouseEvent* pEvent);
    bool handleMouseRelease(QMouseEvent* pEvent);
    void replayPress();

    QAbstractScrollArea* const m_pScrollArea;

    State m_state;
    /// Guards the press we send ourselves against being filtered again
    bool m_replayingPress;

    QPointF m_pressPos;
    QPointF m_pressScenePos;
    QPointF m_pressGlobalPos;
    Qt::KeyboardModifiers m_pressModifiers;
    const QPointingDevice* m_pPressDevice;

    QPointF m_lastPos;
    /// Sub-pixel remainder of the movement not yet applied to the scroll bar
    qreal m_remainingDy;
};
