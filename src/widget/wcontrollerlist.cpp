#include "widget/wcontrollerlist.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>

#include "moc_wcontrollerlist.cpp"
#include "notifications/notifications.h"
#include "preferences/controllersettings.h"
#include "skin/legacy/skincontext.h"
#include "util/math.h"

namespace {
const char* kRowObjectName = "ControllerRow";
const char* kScrollAreaObjectName = "ControllerScrollArea";
const char* kScrollContentObjectName = "ControllerScrollContent";
const char* kEmptyProperty = "empty";
const char* kActiveProperty = "active";
// Distance the finger has to travel vertically before the gesture counts as a
// scroll instead of a tap. Qt's drag distance is meant for a mouse and is
// easily exceeded by the jitter of a fingertip (same threshold as WUsbList and
// TouchScrollFilter, which do this for the drive list and the library views).
constexpr int kMinDragStartDistancePx = 12;
// Roughly a third of a row per wheel notch / scroll bar step.
constexpr int kScrollSingleStepPx = 24;

int dragStartDistance() {
    return math_max(QApplication::startDragDistance(), kMinDragStartDistancePx);
}
} // namespace

WControllerList::WControllerList(QWidget* parent)
        : WWidget(parent),
          m_pScrollArea(new QScrollArea(this)),
          m_pContent(new QWidget(m_pScrollArea)),
          m_pLayout(new QVBoxLayout(m_pContent)),
          m_pEmptyRow(nullptr),
          m_busy(false),
          m_dragState(DragState::Idle),
          m_lastGlobalY(0),
          m_remainingDy(0) {
    setAttribute(Qt::WA_StyledBackground, true);
    // The mouse events WWidget synthesizes from touches carry no held button,
    // and Qt drops a button-less move unless the widget tracks the mouse — so
    // without this a finger drag would never be seen as one.
    setMouseTracking(true);

    auto* pOuterLayout = new QVBoxLayout(this);
    pOuterLayout->setContentsMargins(0, 0, 0, 0);
    pOuterLayout->setSpacing(0);
    pOuterLayout->addWidget(m_pScrollArea);

    m_pScrollArea->setObjectName(kScrollAreaObjectName);
    m_pScrollArea->setFrameShape(QFrame::NoFrame);
    m_pScrollArea->setWidgetResizable(true);
    m_pScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_pScrollArea->setFocusPolicy(Qt::NoFocus);
    // The skin paints the panel behind us; the viewport must not cover it.
    m_pScrollArea->viewport()->setAutoFillBackground(false);
    m_pScrollArea->verticalScrollBar()->setSingleStep(kScrollSingleStepPx);

    m_pContent->setObjectName(kScrollContentObjectName);
    m_pContent->setAutoFillBackground(false);
    m_pScrollArea->setWidget(m_pContent);

    m_pLayout->setContentsMargins(0, 0, 0, 0);
    m_pLayout->setSpacing(6);
    // Keep rows pinned to the top instead of spreading down the panel. Rows are
    // inserted ahead of this stretch, which stays the layout's last item.
    m_pLayout->addStretch(1);

    ControllerSettings* pSettings = ControllerSettings::tryInstance();
    if (pSettings) {
        connect(pSettings,
                &ControllerSettings::rowsChanged,
                this,
                &WControllerList::onRowsChanged);
        rebuildRows(pSettings->rowLabels(), pSettings->rowActiveStates());
    } else {
        renderEmpty();
    }
    // Subscribe to the global busy state so rows grey out and refuse clicks
    // while a rescan or applyMapping is in flight. Initial value is read
    // from the singleton so we don't miss a transition that happened
    // between Notifications construction and our subscription.
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        m_busy = pNotifications->isBusy();
        connect(pNotifications,
                &Notifications::busyChanged,
                this,
                &WControllerList::onBusyChanged);
    }
    applyEnabledToRows();
}

WControllerList::~WControllerList() = default;

void WControllerList::setup(
        const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by ControllerSettings.
}

void WControllerList::onRowsChanged(
        const QStringList& rowLabels, const QList<bool>& rowActive) {
    rebuildRows(rowLabels, rowActive);
}

void WControllerList::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(e);
        return;
    }

    // Map via global coords so the hit-test is correct regardless of which
    // widget the synthesized event's local position referenced, and regardless
    // of how far the content is scrolled.
    QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
    if (pScrollBar->isVisible() &&
            pScrollBar->rect().contains(
                    pScrollBar->mapFromGlobal(e->globalPosition().toPoint()))) {
        m_dragState = DragState::ScrollBar;
        forwardToScrollBar(e);
        e->accept();
        return;
    }

    // Hold the press back until we know whether this is a tap or a drag; the
    // mapping change is dispatched on release.
    m_dragState = DragState::Pending;
    m_pressGlobalPos = e->globalPosition();
    m_lastGlobalY = m_pressGlobalPos.y();
    m_remainingDy = 0;
    e->accept();
}

void WControllerList::mouseMoveEvent(QMouseEvent* e) {
    if (m_dragState == DragState::Idle) {
        WWidget::mouseMoveEvent(e);
        return;
    }
    if (m_dragState == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }

    const qreal globalY = e->globalPosition().y();
    if (m_dragState == DragState::Pending) {
        if (std::abs(globalY - m_pressGlobalPos.y()) < dragStartDistance()) {
            // Might still become a tap, keep swallowing.
            e->accept();
            return;
        }
        m_dragState = DragState::Scrolling;
        // m_lastGlobalY is still the press position, so the content catches up
        // with the finger in this first step and stays pinned to it.
    }

    // Scroll bar values are integers, carry the remainder over to the next
    // move so slow drags don't get lost in rounding.
    m_remainingDy += m_lastGlobalY - globalY;
    const int scrollBy = static_cast<int>(m_remainingDy);
    if (scrollBy != 0) {
        m_remainingDy -= scrollBy;
        QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
        pScrollBar->setValue(pScrollBar->value() + scrollBy);
    }
    m_lastGlobalY = globalY;
    e->accept();
}

void WControllerList::mouseReleaseEvent(QMouseEvent* e) {
    const DragState state = m_dragState;
    m_dragState = DragState::Idle;

    if (state == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }
    if (state == DragState::Pending) {
        // The finger never moved: a tap after all.
        const int index = rowIndexAt(e->globalPosition().toPoint());
        if (index >= 0) {
            handleRowTap(index);
            e->accept();
            return;
        }
    }
    WWidget::mouseReleaseEvent(e);
}

int WControllerList::rowIndexAt(const QPoint& globalPos) const {
    // Rows scrolled out of sight still have a geometry, but it is clipped away
    // by the viewport, so only taps landing inside the viewport can hit one.
    const QWidget* pViewport = m_pScrollArea->viewport();
    if (!pViewport->rect().contains(pViewport->mapFromGlobal(globalPos))) {
        return -1;
    }
    for (int i = 0; i < m_rows.size(); ++i) {
        const QPushButton* pRow = m_rows.at(i);
        if (pRow->rect().contains(pRow->mapFromGlobal(globalPos))) {
            return i;
        }
    }
    return -1;
}

void WControllerList::forwardToScrollBar(QMouseEvent* pEvent) {
    QScrollBar* pScrollBar = m_pScrollArea->verticalScrollBar();
    const QPointF localPos = pScrollBar->mapFromGlobal(pEvent->globalPosition().toPoint());
    const bool isRelease = pEvent->type() == QEvent::MouseButtonRelease;
    const bool isMove = pEvent->type() == QEvent::MouseMove;
    const QPointingDevice* pDevice = pEvent->pointingDevice()
            ? pEvent->pointingDevice()
            : QPointingDevice::primaryPointingDevice();
    QMouseEvent forwarded(pEvent->type(),
            localPos,
            localPos,
            pEvent->globalPosition(),
            isMove ? Qt::NoButton : Qt::LeftButton,
            isRelease ? Qt::NoButton : Qt::LeftButton,
            pEvent->modifiers(),
            pDevice);
    QCoreApplication::sendEvent(pScrollBar, &forwarded);
}

void WControllerList::onRowClicked() {
    // Desktop/mouse path: a real QMouseEvent reaches the child button directly
    // and emits clicked(). (On the touchscreen this never fires; the press and
    // release handlers above do the dispatching instead.)
    handleRowTap(m_rows.indexOf(qobject_cast<QPushButton*>(sender())));
}

void WControllerList::handleRowTap(int index) {
    if (m_busy) {
        // Defensive: the rows are setEnabled(false) when busy so a tap
        // shouldn't reach here, but a queued event from before the
        // disable can still arrive. Drop it.
        return;
    }
    if (index < 0 || index >= m_rows.size()) {
        return;
    }
    ControllerSettings* pSettings = ControllerSettings::tryInstance();
    if (!pSettings) {
        return;
    }
    pSettings->toggleRow(index);
}

void WControllerList::onBusyChanged(bool busy) {
    m_busy = busy;
    applyEnabledToRows();
}

void WControllerList::applyEnabledToRows() {
    // The empty-state row is always disabled (placeholder). Real rows
    // flip with the busy state so they grey out via the [enabled="false"]
    // / :disabled QSS rules.
    for (QPushButton* pRow : std::as_const(m_rows)) {
        pRow->setEnabled(!m_busy);
    }
}

void WControllerList::rebuildRows(
        const QStringList& rowLabels, const QList<bool>& rowActive) {
    // Detach old rows from the widget tree immediately (setParent(nullptr)
    // removes them from layout AND from the parent's child list, so they
    // stop painting before this method returns), then deleteLater so Qt
    // can finish dispatching the click event the slot was triggered from
    // — synchronous `delete` from inside the button's own clicked handler
    // crashes when the queued mouse-release fires on the freed widget
    // (MixxxApplication::notify slow-event logger then segfaults trying
    // to print its objectName).
    for (QPushButton* pRow : std::as_const(m_rows)) {
        pRow->setParent(nullptr);
        pRow->deleteLater();
    }
    m_rows.clear();
    if (m_pEmptyRow) {
        m_pEmptyRow->setParent(nullptr);
        m_pEmptyRow->deleteLater();
        m_pEmptyRow = nullptr;
    }

    if (rowLabels.isEmpty()) {
        renderEmpty();
        return;
    }
    for (int i = 0; i < rowLabels.size(); ++i) {
        auto* pRow = new QPushButton(rowLabels.at(i), m_pContent);
        pRow->setObjectName(kRowObjectName);
        pRow->setFocusPolicy(Qt::NoFocus);
        // Active state drives the [active="true"] QSS rule so the row
        // showing the currently-applied mapping paints with the highlight
        // colour.
        pRow->setProperty(kActiveProperty,
                i < rowActive.size() && rowActive.at(i));
        connect(pRow,
                &QPushButton::clicked,
                this,
                &WControllerList::onRowClicked);
        // Ahead of the trailing stretch, which keeps the rows top-aligned.
        m_pLayout->insertWidget(m_pLayout->count() - 1, pRow);
        m_rows.append(pRow);
        style()->unpolish(pRow);
        style()->polish(pRow);
    }
    // New rows inherit the current busy state — important when a rescan's
    // refreshRows fires while still pending: the rebuilt rows must come
    // out disabled or a tap between rebuild and the watchdog/devicesChanged
    // clear would slip through.
    applyEnabledToRows();
}

void WControllerList::renderEmpty() {
    m_pEmptyRow = new QPushButton(tr("No MIDI controllers detected"), m_pContent);
    m_pEmptyRow->setObjectName(kRowObjectName);
    m_pEmptyRow->setProperty(kEmptyProperty, true);
    m_pEmptyRow->setEnabled(false);
    m_pLayout->insertWidget(m_pLayout->count() - 1, m_pEmptyRow);
    style()->unpolish(m_pEmptyRow);
    style()->polish(m_pEmptyRow);
}
