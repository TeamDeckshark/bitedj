#include "widget/touchscrollfilter.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScrollBar>
#include <cmath>

#include "moc_touchscrollfilter.cpp"
#include "util/assert.h"
#include "util/math.h"

namespace {

/// Distance the finger has to travel vertically before the gesture counts as a
/// scroll instead of a tap. Qt's drag distance is meant for a mouse and is
/// easily exceeded by the jitter of a fingertip.
constexpr int kMinDragStartDistancePx = 12;

int dragStartDistance() {
    return math_max(QApplication::startDragDistance(), kMinDragStartDistancePx);
}

} // anonymous namespace

// static
void TouchScrollFilter::install(QAbstractScrollArea* pScrollArea) {
    VERIFY_OR_DEBUG_ASSERT(pScrollArea) {
        return;
    }
    auto* pItemView = qobject_cast<QAbstractItemView*>(pScrollArea);
    if (pItemView) {
        // Dragging the content by pixels only works if the view scrolls by
        // pixels, too. With per-item scrolling every pixel would jump a row.
        pItemView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    }
    // Owned by the scroll area
    new TouchScrollFilter(pScrollArea);
}

TouchScrollFilter::TouchScrollFilter(QAbstractScrollArea* pScrollArea)
        : QObject(pScrollArea),
          m_pScrollArea(pScrollArea),
          m_state(State::Idle),
          m_replayingPress(false),
          m_pressModifiers(Qt::NoModifier),
          m_pPressDevice(nullptr),
          m_remainingDy(0) {
    pScrollArea->viewport()->installEventFilter(this);
}

bool TouchScrollFilter::eventFilter(QObject* pWatched, QEvent* pEvent) {
    if (m_replayingPress || pWatched != m_pScrollArea->viewport()) {
        return false;
    }

    switch (pEvent->type()) {
    case QEvent::MouseButtonPress:
        return handleMousePress(static_cast<QMouseEvent*>(pEvent));
    case QEvent::MouseMove:
        return handleMouseMove(static_cast<QMouseEvent*>(pEvent));
    case QEvent::MouseButtonRelease:
        return handleMouseRelease(static_cast<QMouseEvent*>(pEvent));
    case QEvent::MouseButtonDblClick:
        // Qt sends this instead of the second press of a double tap, so there
        // is nothing to hold back.
        m_state = State::Idle;
        return false;
    default:
        return false;
    }
}

bool TouchScrollFilter::handleMousePress(QMouseEvent* pEvent) {
    if (pEvent->button() != Qt::LeftButton) {
        m_state = State::Idle;
        return false;
    }

    m_state = State::Pending;
    m_pressPos = pEvent->position();
    m_pressScenePos = pEvent->scenePosition();
    m_pressGlobalPos = pEvent->globalPosition();
    m_pressModifiers = pEvent->modifiers();
    m_pPressDevice = pEvent->pointingDevice();
    m_lastPos = m_pressPos;
    m_remainingDy = 0;

    // Hold the press back until we know this is a tap.
    pEvent->accept();
    return true;
}

bool TouchScrollFilter::handleMouseMove(QMouseEvent* pEvent) {
    if (m_state == State::Idle) {
        // Plain hover move, delegates rely on those.
        return false;
    }
    if (!pEvent->buttons().testFlag(Qt::LeftButton)) {
        // The release was delivered elsewhere, e.g. to a popup menu.
        m_state = State::Idle;
        return false;
    }

    const QPointF pos = pEvent->position();
    if (m_state == State::Pending) {
        if (std::abs(pos.y() - m_pressPos.y()) < dragStartDistance()) {
            // Might still become a tap, keep swallowing.
            pEvent->accept();
            return true;
        }
        m_state = State::Scrolling;
        // m_lastPos is still the press position, so the content catches up
        // with the finger in this first step and stays pinned to it.
    }

    // Scroll bar values are integers, carry the remainder over to the next
    // move so slow drags don't get lost in rounding.
    m_remainingDy += m_lastPos.y() - pos.y();
    const int scrollBy = static_cast<int>(m_remainingDy);
    if (scrollBy != 0) {
        m_remainingDy -= scrollBy;
        QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
        pScrollBar->setValue(pScrollBar->value() + scrollBy);
    }
    m_lastPos = pos;

    pEvent->accept();
    return true;
}

bool TouchScrollFilter::handleMouseRelease(QMouseEvent* pEvent) {
    const State state = m_state;
    m_state = State::Idle;

    if (state == State::Scrolling) {
        // The gesture was a scroll, the view must not act on it.
        pEvent->accept();
        return true;
    }
    if (state == State::Pending) {
        // A tap after all: give the view the press it never got. The release
        // we return to is delivered right after it.
        replayPress();
    }
    return false;
}

void TouchScrollFilter::replayPress() {
    const QPointingDevice* pDevice = m_pPressDevice
            ? m_pPressDevice
            : QPointingDevice::primaryPointingDevice();
    QMouseEvent pressEvent(QEvent::MouseButtonPress,
            m_pressPos,
            m_pressScenePos,
            m_pressGlobalPos,
            Qt::LeftButton,
            Qt::LeftButton,
            m_pressModifiers,
            pDevice);
    m_replayingPress = true;
    QCoreApplication::sendEvent(m_pScrollArea->viewport(), &pressEvent);
    m_replayingPress = false;
}
