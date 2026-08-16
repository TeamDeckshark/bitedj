#include "widget/wusblist.h"

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>

#include "moc_wusblist.cpp"
#include "preferences/systemsettings.h"
#include "skin/legacy/skincontext.h"
#include "util/math.h"

namespace {
const char* kRowObjectName = "UsbRow";
const char* kRowLabelObjectName = "UsbRowLabel";
const char* kEjectButtonObjectName = "UsbEjectButton";
const char* kRecordButtonObjectName = "UsbRecordButton";
const char* kScrollAreaObjectName = "UsbScrollArea";
const char* kScrollContentObjectName = "UsbScrollContent";
const char* kEmptyProperty = "empty";
const char* kArmedProperty = "armed";
const char* kRecordingProperty = "recording";
// How long an armed eject button waits for the confirming second tap before
// reverting.
constexpr int kDisarmTimeoutMs = 3000;
// Distance the finger has to travel vertically before the gesture counts as a
// scroll instead of a tap. Qt's drag distance is meant for a mouse and is
// easily exceeded by the jitter of a fingertip (same threshold as
// TouchScrollFilter, which does this for the library views).
constexpr int kMinDragStartDistancePx = 12;
// Roughly a third of a row per wheel notch / scroll bar step.
constexpr int kScrollSingleStepPx = 24;

int dragStartDistance() {
    return math_max(QApplication::startDragDistance(), kMinDragStartDistancePx);
}

// Repolishes a widget so a property change picked up by the stylesheet
// ([armed], [recording], :disabled) is actually repainted.
void restyle(QStyle* pStyle, QWidget* pWidget) {
    pStyle->unpolish(pWidget);
    pStyle->polish(pWidget);
}
} // namespace

WUsbList::WUsbList(QWidget* parent)
        : WWidget(parent),
          m_pScrollArea(new QScrollArea(this)),
          m_pContent(new QWidget(m_pScrollArea)),
          m_pLayout(new QGridLayout(m_pContent)),
          m_pEmptyRow(nullptr),
          m_armedIndex(-1),
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
    m_pLayout->setHorizontalSpacing(6);
    m_pLayout->setVerticalSpacing(6);
    // The name column takes the slack; the Record and Eject button columns stay
    // at their own size so they are never squeezed to zero or pushed off-screen.
    m_pLayout->setColumnStretch(0, 1);
    m_pLayout->setColumnStretch(1, 0);
    m_pLayout->setColumnStretch(2, 0);
    // Keep rows pinned to the top instead of spreading down the panel.
    m_pLayout->setRowStretch(1000, 1);

    m_disarmTimer.setSingleShot(true);
    m_disarmTimer.setInterval(kDisarmTimeoutMs);
    connect(&m_disarmTimer, &QTimer::timeout, this, &WUsbList::onDisarmTimeout);

    SystemSettings* pSettings = SystemSettings::tryInstance();
    if (pSettings) {
        connect(pSettings,
                &SystemSettings::usbRowsChanged,
                this,
                &WUsbList::onRowsChanged);
        connect(pSettings,
                &SystemSettings::usbRecordingChanged,
                this,
                &WUsbList::onRecordingChanged);
        rebuildRows(pSettings->usbRowLabels());
    } else {
        renderEmpty();
    }
}

WUsbList::~WUsbList() = default;

void WUsbList::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by SystemSettings.
}

void WUsbList::onRowsChanged(const QStringList& labels) {
    rebuildRows(labels);
}

void WUsbList::mousePressEvent(QMouseEvent* e) {
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
    // eject is dispatched on release.
    m_dragState = DragState::Pending;
    m_pressGlobalPos = e->globalPosition();
    m_lastGlobalY = m_pressGlobalPos.y();
    m_remainingDy = 0;
    e->accept();
}

void WUsbList::mouseMoveEvent(QMouseEvent* e) {
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

void WUsbList::mouseReleaseEvent(QMouseEvent* e) {
    const DragState state = m_dragState;
    m_dragState = DragState::Idle;

    if (state == DragState::ScrollBar) {
        forwardToScrollBar(e);
        e->accept();
        return;
    }
    if (state == DragState::Pending) {
        // The finger never moved: a tap after all.
        const QPoint globalPos = e->globalPosition().toPoint();
        const int recordIndex = buttonIndexAt(m_recordButtons, globalPos);
        if (recordIndex >= 0) {
            handleRecordTap(recordIndex);
            e->accept();
            return;
        }
        const int index = buttonIndexAt(m_ejectButtons, globalPos);
        if (index >= 0) {
            handleEjectTap(index);
            e->accept();
            return;
        }
    }
    WWidget::mouseReleaseEvent(e);
}

int WUsbList::buttonIndexAt(const QList<QPushButton*>& buttons,
        const QPoint& globalPos) const {
    // Rows scrolled out of sight still have a geometry, but it is clipped away
    // by the viewport, so only taps landing inside the viewport can hit one.
    const QWidget* pViewport = m_pScrollArea->viewport();
    if (!pViewport->rect().contains(pViewport->mapFromGlobal(globalPos))) {
        return -1;
    }
    for (int i = 0; i < buttons.size(); ++i) {
        const QPushButton* pButton = buttons.at(i);
        if (!pButton->isEnabled()) {
            continue;
        }
        if (pButton->rect().contains(pButton->mapFromGlobal(globalPos))) {
            return i;
        }
    }
    return -1;
}

void WUsbList::forwardToScrollBar(QMouseEvent* pEvent) {
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

void WUsbList::onEjectClicked() {
    // Desktop/mouse path: a real QMouseEvent reaches the child button directly
    // and emits clicked(). (On the touchscreen this never fires; the press and
    // release handlers above do the dispatching instead.)
    const int index = m_ejectButtons.indexOf(qobject_cast<QPushButton*>(sender()));
    handleEjectTap(index);
}

void WUsbList::onRecordClicked() {
    // Desktop/mouse path, as onEjectClicked above.
    const int index = m_recordButtons.indexOf(qobject_cast<QPushButton*>(sender()));
    handleRecordTap(index);
}

void WUsbList::onRecordingChanged(int recordingIndex) {
    applyRecordingState(recordingIndex);
}

void WUsbList::handleRecordTap(int index) {
    if (index < 0 || index >= m_recordButtons.size()) {
        return;
    }
    // The finger has moved on to a different control, so a pending eject
    // confirmation elsewhere in the list is stale.
    disarm();
    if (SystemSettings* pSettings = SystemSettings::tryInstance()) {
        // Starting or stopping re-emits usbRecordingChanged, which repaints the
        // buttons — including this one — so nothing is set here.
        pSettings->toggleRecordRow(index);
    }
}

void WUsbList::applyRecordingState(int recordingIndex) {
    for (int i = 0; i < m_recordButtons.size(); ++i) {
        QPushButton* pButton = m_recordButtons.at(i);
        const bool isRecording = i == recordingIndex;
        pButton->setText(isRecording ? tr("Stop Recording") : tr("Record"));
        pButton->setProperty(kRecordingProperty, isRecording);
        // Only one drive at a time: while one is recording, its own button is
        // the only one left that does anything.
        pButton->setEnabled(recordingIndex < 0 || isRecording);
        restyle(style(), pButton);
    }
}

void WUsbList::handleEjectTap(int index) {
    if (index < 0 || index >= m_ejectButtons.size()) {
        return;
    }
    if (index == m_armedIndex) {
        // Second tap on the armed button: confirm the eject. SystemSettings
        // re-enumerates and emits usbRowsChanged, which rebuilds our rows
        // (and resets the armed state) — so disarm defensively first.
        disarm();
        if (SystemSettings* pSettings = SystemSettings::tryInstance()) {
            pSettings->ejectRow(index);
        }
        return;
    }
    // First tap: arm this button and revert any previously armed one.
    disarm();
    m_armedIndex = index;
    QPushButton* pButton = m_ejectButtons.at(index);
    pButton->setText(tr("Confirm"));
    pButton->setProperty(kArmedProperty, true);
    restyle(style(), pButton);
    m_disarmTimer.start();
}

void WUsbList::onDisarmTimeout() {
    disarm();
}

void WUsbList::disarm() {
    m_disarmTimer.stop();
    if (m_armedIndex >= 0 && m_armedIndex < m_ejectButtons.size()) {
        QPushButton* pButton = m_ejectButtons.at(m_armedIndex);
        pButton->setText(tr("Eject"));
        pButton->setProperty(kArmedProperty, false);
        restyle(style(), pButton);
    }
    m_armedIndex = -1;
}

void WUsbList::rebuildRows(const QStringList& labels) {
    m_disarmTimer.stop();
    m_armedIndex = -1;

    // Detach old row widgets immediately (so they stop painting) but deleteLater
    // so Qt can finish dispatching the click event that triggered the rebuild —
    // synchronous delete from inside the button's own clicked handler crashes
    // when the queued mouse-release fires on the freed widget.
    for (QWidget* pWidget : std::as_const(m_rowWidgets)) {
        pWidget->setParent(nullptr);
        pWidget->deleteLater();
    }
    m_rowWidgets.clear();
    m_ejectButtons.clear();
    m_recordButtons.clear();
    if (m_pEmptyRow) {
        m_pEmptyRow->setParent(nullptr);
        m_pEmptyRow->deleteLater();
        m_pEmptyRow = nullptr;
    }

    m_rowLabels = labels;
    if (labels.isEmpty()) {
        renderEmpty();
        return;
    }
    for (int i = 0; i < labels.size(); ++i) {
        auto* pLabel = new QLabel(labels.at(i), m_pContent);
        pLabel->setObjectName(kRowLabelObjectName);
        // Let the label shrink so it can never shove the button out of view.
        pLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_pLayout->addWidget(pLabel, i, 0);

        auto* pRecord = new QPushButton(tr("Record"), m_pContent);
        pRecord->setObjectName(kRecordButtonObjectName);
        pRecord->setFocusPolicy(Qt::NoFocus);
        pRecord->setProperty(kRecordingProperty, false);
        connect(pRecord, &QPushButton::clicked, this, &WUsbList::onRecordClicked);
        m_pLayout->addWidget(pRecord, i, 1);

        auto* pEject = new QPushButton(tr("Eject"), m_pContent);
        pEject->setObjectName(kEjectButtonObjectName);
        pEject->setFocusPolicy(Qt::NoFocus);
        pEject->setProperty(kArmedProperty, false);
        connect(pEject, &QPushButton::clicked, this, &WUsbList::onEjectClicked);
        m_pLayout->addWidget(pEject, i, 2);

        m_rowWidgets.append(pLabel);
        m_rowWidgets.append(pRecord);
        m_rowWidgets.append(pEject);
        m_recordButtons.append(pRecord);
        m_ejectButtons.append(pEject);
        restyle(style(), pLabel);
        restyle(style(), pRecord);
        restyle(style(), pEject);
    }

    // A recording survives the rebuild (a drive plugged in or pulled out does
    // not stop one on another drive), so the fresh buttons have to be given its
    // state rather than the resting one they were built with.
    if (SystemSettings* pSettings = SystemSettings::tryInstance()) {
        applyRecordingState(pSettings->recordingRowIndex());
    }
}

void WUsbList::renderEmpty() {
    m_pEmptyRow = new QLabel(tr("No USB drives detected"), m_pContent);
    m_pEmptyRow->setObjectName(kRowObjectName);
    m_pEmptyRow->setProperty(kEmptyProperty, true);
    m_pLayout->addWidget(m_pEmptyRow, 0, 0, 1, 3);
    restyle(style(), m_pEmptyRow);
}
