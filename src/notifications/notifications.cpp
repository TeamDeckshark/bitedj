#include "notifications/notifications.h"

#include <QCoreApplication>
#include <QEvent>

#include "control/controlobject.h"
#include "moc_notifications.cpp"

namespace {
constexpr const char* kGroup = "[Notifications]";
}

QAtomicPointer<Notifications> Notifications::s_pInstance = nullptr;

Notifications::Notifications()
        : m_pCoVisible(new ControlObject(ConfigKey(kGroup, "visible"))),
          m_pCoSeverity(new ControlObject(ConfigKey(kGroup, "severity"))),
          m_pCoBusy(new ControlObject(ConfigKey(kGroup, "busy"))),
          m_busy(false),
          m_suppressingInput(false),
          m_swallowedPress(false) {
    m_pCoVisible->setReadOnly();
    m_pCoSeverity->setReadOnly();
    m_pCoBusy->setReadOnly();

    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleTimeoutMs);
    connect(&m_idleTimer,
            &QTimer::timeout,
            this,
            &Notifications::onIdleTimeout);

    m_inputGraceTimer.setSingleShot(true);
    m_inputGraceTimer.setInterval(kInputGraceMs);
    connect(&m_inputGraceTimer,
            &QTimer::timeout,
            this,
            &Notifications::onInputGraceTimeout);

    s_pInstance.storeRelease(this);
}

Notifications::~Notifications() {
    s_pInstance.storeRelease(nullptr);
    if (m_suppressingInput) {
        QCoreApplication::instance()->removeEventFilter(this);
    }
    delete m_pCoVisible;
    delete m_pCoSeverity;
    delete m_pCoBusy;
}

void Notifications::publish(const QString& message, Severity severity) {
    // Last-writer-wins: a fresh publish replaces the on-screen message and
    // resets the auto-clear countdown. Severity must be set before visible
    // flips on so the strip's QSS picks the right tint on the same frame.
    m_pCoSeverity->forceSet(static_cast<double>(severity));
    m_pCoVisible->forceSet(1.0);
    emit messagePosted(message, static_cast<int>(severity));
    m_idleTimer.start();
}

void Notifications::publishSticky(const QString& message, Severity severity) {
    // Same write order as publish, but skip the idle timer so the message
    // sits there until clear() or a fresh publish() replaces it. Any
    // previously-armed timer from an earlier publish() is cancelled so
    // it doesn't auto-clear our sticky message.
    m_idleTimer.stop();
    m_pCoSeverity->forceSet(static_cast<double>(severity));
    m_pCoVisible->forceSet(1.0);
    emit messagePosted(message, static_cast<int>(severity));
}

void Notifications::clear() {
    m_idleTimer.stop();
    m_pCoVisible->forceSet(0.0);
    m_pCoSeverity->forceSet(0.0);
    emit cleared();
}

void Notifications::setBusy(bool busy) {
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    if (busy) {
        m_inputGraceTimer.stop();
        if (!m_suppressingInput) {
            m_suppressingInput = true;
            m_swallowedPress = false;
            QCoreApplication::instance()->installEventFilter(this);
        }
    } else {
        // Keep suppressing until the grace window elapses so taps that were
        // queued while the GUI thread was blocked (delivered on the first
        // loop iterations after we return) get discarded, not replayed.
        m_inputGraceTimer.start();
    }
    m_pCoBusy->forceSet(busy ? 1.0 : 0.0);
    emit busyChanged(busy);
}

void Notifications::onInputGraceTimeout() {
    if (m_busy || !m_suppressingInput) {
        return;
    }
    m_suppressingInput = false;
    m_swallowedPress = false;
    QCoreApplication::instance()->removeEventFilter(this);
}

bool Notifications::eventFilter(QObject* pObj, QEvent* pEvent) {
    // Filter at the QWindow level: swallowing the window-system delivery
    // stops the widget-level event (and Qt's touch→mouse synthesis) from
    // ever being created. Widget-level re-deliveries of events that slipped
    // through are left alone.
    if (!pObj->isWindowType()) {
        return false;
    }
    switch (pEvent->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::TouchBegin:
        m_swallowedPress = true;
        pEvent->accept();
        return true;
    case QEvent::Wheel:
        pEvent->accept();
        return true;
    case QEvent::MouseMove:
    case QEvent::TouchUpdate:
        // Only swallow when part of a swallowed gesture; a drag whose press
        // was delivered before suppression started stays coherent.
        if (m_swallowedPress) {
            pEvent->accept();
            return true;
        }
        return false;
    case QEvent::MouseButtonRelease:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        if (m_swallowedPress) {
            m_swallowedPress = false;
            pEvent->accept();
            return true;
        }
        // Unpaired release: its press was delivered before suppression
        // started (e.g. the very tap that raised the busy state), so let
        // the gesture finish normally.
        return false;
    default:
        return false;
    }
}

void Notifications::onIdleTimeout() {
    clear();
}
