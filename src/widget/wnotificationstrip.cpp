#include "widget/wnotificationstrip.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QString>
#include <QStyle>

#include "moc_wnotificationstrip.cpp"
#include "notifications/notifications.h"
#include "skin/legacy/skincontext.h"

WNotificationStrip::WNotificationStrip(QWidget* parent)
        : WWidget(parent),
          m_pLabel(new QLabel(this)) {
    // QSS-painted background/border requires WA_StyledBackground on a plain
    // QWidget subclass.
    setAttribute(Qt::WA_StyledBackground, true);

    auto* pLayout = new QHBoxLayout(this);
    pLayout->setContentsMargins(8, 0, 8, 0);
    pLayout->setSpacing(0);

    m_pLabel->setObjectName("NotificationLabel");
    m_pLabel->setAlignment(Qt::AlignCenter);
    m_pLabel->setWordWrap(false);
    pLayout->addWidget(m_pLabel);

    // Hidden by default so stock Mixxx (where the visibility CO never fires
    // and we never receive a messagePosted) doesn't paint an empty band.
    setVisible(false);
    applySeverity(static_cast<int>(Notifications::Severity::Info));

    Notifications* pNotifications = Notifications::tryInstance();
    if (pNotifications) {
        connect(pNotifications,
                &Notifications::messagePosted,
                this,
                &WNotificationStrip::onMessagePosted);
        connect(pNotifications,
                &Notifications::cleared,
                this,
                &WNotificationStrip::onCleared);
    }
    // No instance → soft-fall: the strip stays hidden and inert. The
    // <Connection>s in the skin XML similarly silently no-op when the
    // [Notifications],* COs don't exist.
}

WNotificationStrip::~WNotificationStrip() = default;

void WNotificationStrip::setup(const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration needed; severity styling lives in QSS and
    // text comes from the messagePosted signal.
}

void WNotificationStrip::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        WWidget::mousePressEvent(event);
        return;
    }
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        pNotifications->clear();
    }
    event->accept();
}

void WNotificationStrip::onMessagePosted(const QString& message, int severity) {
    m_message = message;
    updateLabelText();
    applySeverity(severity);
}

void WNotificationStrip::onCleared() {
    m_message.clear();
    m_pLabel->clear();
}

void WNotificationStrip::resizeEvent(QResizeEvent* event) {
    WWidget::resizeEvent(event);
    // The available width changed, so a message that previously fit (or
    // didn't) may need to be re-elided.
    updateLabelText();
}

void WNotificationStrip::updateLabelText() {
    if (m_message.isEmpty()) {
        m_pLabel->clear();
        return;
    }

    const int availableWidth = m_pLabel->contentsRect().width();
    const QFontMetrics metrics(m_pLabel->font());
    if (metrics.horizontalAdvance(m_message) <= availableWidth) {
        // Fits: keep it centered, the way short notifications look best.
        m_pLabel->setAlignment(Qt::AlignCenter);
        m_pLabel->setText(m_message);
        return;
    }

    // Overflows (typically a long file path): show the start of the message
    // — which carries the actual error — and elide the tail. Center
    // alignment would clip both ends and hide the descriptive beginning.
    m_pLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_pLabel->setText(metrics.elidedText(m_message, Qt::ElideRight, availableWidth));
}

void WNotificationStrip::applySeverity(int severity) {
    const char* name = "info";
    switch (static_cast<Notifications::Severity>(severity)) {
    case Notifications::Severity::Warning:
        name = "warning";
        break;
    case Notifications::Severity::Error:
        name = "error";
        break;
    case Notifications::Severity::Info:
    default:
        name = "info";
        break;
    }
    setProperty("severity", name);
    // Repolish so QSS rules guarded by [severity="..."] re-evaluate after a
    // dynamic property change.
    style()->unpolish(this);
    style()->polish(this);
}
