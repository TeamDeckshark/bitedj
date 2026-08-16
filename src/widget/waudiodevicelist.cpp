#include "widget/waudiodevicelist.h"

#include <QMouseEvent>
#include <QStyle>
#include <QVBoxLayout>

#include "moc_waudiodevicelist.cpp"
#include "notifications/notifications.h"
#include "preferences/audiodevicesettings.h"
#include "skin/legacy/skincontext.h"

namespace {
const char* kRowObjectName = "AudioDeviceRow";
const char* kAssignedProperty = "assigned";
const char* kEmptyProperty = "empty";
} // namespace

WAudioDeviceList::WAudioDeviceList(QWidget* parent)
        : WWidget(parent),
          m_pLayout(new QVBoxLayout(this)),
          m_pEmptyRow(nullptr),
          m_busy(false) {
    setAttribute(Qt::WA_StyledBackground, true);
    m_pLayout->setContentsMargins(0, 0, 0, 0);
    m_pLayout->setSpacing(6);

    AudioDeviceSettings* pSettings = AudioDeviceSettings::tryInstance();
    if (pSettings) {
        connect(pSettings,
                &AudioDeviceSettings::busesChanged,
                this,
                &WAudioDeviceList::onBusesChanged);
        rebuildRows(pSettings->busLabels(), pSettings->busAssigned());
    } else {
        // Stock Mixxx fallback: render the empty-state row so the panel
        // doesn't look broken when the singleton is absent.
        renderEmpty();
    }
    // Subscribe to the global busy state so rows grey out and refuse taps while
    // an audio-config apply is in flight. Seed from the singleton so we don't
    // miss a transition between Notifications construction and this subscribe.
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        m_busy = pNotifications->isBusy();
        connect(pNotifications,
                &Notifications::busyChanged,
                this,
                &WAudioDeviceList::onBusyChanged);
    }
    applyEnabledToRows();
}

WAudioDeviceList::~WAudioDeviceList() = default;

void WAudioDeviceList::setup(
        const QDomNode& /*node*/, const SkinContext& /*context*/) {
    // No XML-side configuration; everything is driven by AudioDeviceSettings.
}

void WAudioDeviceList::onBusesChanged(
        const QStringList& busLabels, const QList<bool>& assigned) {
    rebuildRows(busLabels, assigned);
}

void WAudioDeviceList::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        // Map via global coords so the hit-test is correct regardless of which
        // widget the synthesized event's local position referenced. The rows
        // are direct children, so geometry() is in our coordinates.
        const QPoint pos = mapFromGlobal(e->globalPosition().toPoint());
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i)->geometry().contains(pos)) {
                handleRowTap(i);
                e->accept();
                return;
            }
        }
    }
    WWidget::mousePressEvent(e);
}

void WAudioDeviceList::onRowClicked() {
    // Desktop/mouse path: a real QMouseEvent reaches the child button directly
    // and emits clicked(). (On the touchscreen this never fires; mousePressEvent
    // above handles it instead.)
    handleRowTap(m_rows.indexOf(qobject_cast<QPushButton*>(sender())));
}

void WAudioDeviceList::onBusyChanged(bool busy) {
    m_busy = busy;
    applyEnabledToRows();
}

void WAudioDeviceList::applyEnabledToRows() {
    // Real rows flip with the busy state so they grey out via the
    // :disabled QSS rule. The empty-state placeholder stays disabled.
    for (QPushButton* pRow : std::as_const(m_rows)) {
        pRow->setEnabled(!m_busy);
    }
}

void WAudioDeviceList::handleRowTap(int index) {
    if (m_busy) {
        // Defensive: rows are disabled while busy, but a queued tap from just
        // before the disable could still arrive. Drop it so we don't stack a
        // second (blocking) apply on top of the one in flight.
        return;
    }
    if (index < 0 || index >= m_rows.size()) {
        return;
    }
    AudioDeviceSettings* pSettings = AudioDeviceSettings::tryInstance();
    if (!pSettings) {
        return;
    }
    pSettings->cycleBus(index);
}

void WAudioDeviceList::rebuildRows(
        const QStringList& busLabels, const QList<bool>& assigned) {
    // Detach from the parent immediately (so the old rows stop painting
    // before this method returns) then deleteLater. Synchronous delete
    // from inside the button's own clicked handler crashes when Qt
    // delivers the queued mouse-release to the freed widget.
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

    if (busLabels.isEmpty()) {
        renderEmpty();
        return;
    }
    for (int i = 0; i < busLabels.size(); ++i) {
        const bool isAssigned = i < assigned.size() && assigned.at(i);
        auto* pRow = new QPushButton(busLabels.at(i), this);
        pRow->setObjectName(kRowObjectName);
        pRow->setProperty(kAssignedProperty, isAssigned);
        pRow->setFocusPolicy(Qt::NoFocus);
        connect(pRow,
                &QPushButton::clicked,
                this,
                &WAudioDeviceList::onRowClicked);
        m_pLayout->addWidget(pRow);
        m_rows.append(pRow);
    }
    // Repolish so [assigned="false"] QSS rules re-evaluate.
    for (QPushButton* pRow : std::as_const(m_rows)) {
        style()->unpolish(pRow);
        style()->polish(pRow);
    }
    // Freshly-built rows inherit the current busy state (a rebuild can land
    // mid-apply via the refreshDeviceList → busesChanged emitted before
    // applyBusConfig clears busy).
    applyEnabledToRows();
}

void WAudioDeviceList::renderEmpty() {
    m_pEmptyRow = new QPushButton(tr("No audio devices detected"), this);
    m_pEmptyRow->setObjectName(kRowObjectName);
    m_pEmptyRow->setProperty(kEmptyProperty, true);
    m_pEmptyRow->setEnabled(false);
    m_pLayout->addWidget(m_pEmptyRow);
    style()->unpolish(m_pEmptyRow);
    style()->polish(m_pEmptyRow);
}
