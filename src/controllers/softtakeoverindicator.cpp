#include "controllers/softtakeoverindicator.h"

#include <algorithm>
#include <cmath>

#include "control/controlobject.h"
#include "moc_softtakeoverindicator.cpp"
#include "preferences/usersettings.h"

namespace {
constexpr const char* kGroup = "[SoftTakeover]";
// Hide the arrow once the hardware is within this absolute distance of the
// software value (parameters are normalized to [0, 1]). Larger than the
// soft-takeover ignore threshold (~3/128) so a near-tolerance reject doesn't
// flash the indicator for a single frame.
constexpr double kVisibilityThreshold = 0.02;
}

QAtomicPointer<SoftTakeoverIndicator> SoftTakeoverIndicator::s_pInstance = nullptr;

SoftTakeoverIndicator::SoftTakeoverIndicator(QObject* parent)
        : QObject(parent),
          m_pCoActive(new ControlObject(ConfigKey(kGroup, "active"))),
          m_pCoOffsetLeft(new ControlObject(ConfigKey(kGroup, "offset_left"))),
          m_pCoOffsetRight(new ControlObject(ConfigKey(kGroup, "offset_right"))) {
    m_pCoActive->setReadOnly();
    m_pCoOffsetLeft->setReadOnly();
    m_pCoOffsetRight->setReadOnly();

    m_idleTimer.setSingleShot(true);
    m_idleTimer.setInterval(kIdleTimeoutMs);
    connect(&m_idleTimer,
            &QTimer::timeout,
            this,
            &SoftTakeoverIndicator::onIdleTimeout);

    // publish() may run on a controller thread; bounce the timer restart back
    // to this object's owning thread (the GUI thread).
    connect(this,
            &SoftTakeoverIndicator::valueUpdated,
            this,
            &SoftTakeoverIndicator::onValueUpdated,
            Qt::QueuedConnection);

    s_pInstance.storeRelease(this);
}

SoftTakeoverIndicator::~SoftTakeoverIndicator() {
    s_pInstance.storeRelease(nullptr);
    delete m_pCoActive;
    delete m_pCoOffsetLeft;
    delete m_pCoOffsetRight;
}

void SoftTakeoverIndicator::publish(const ControlObject* pControl,
        double currentParameter,
        double newParameter,
        bool wasIgnored) {
    const double offset = newParameter - currentParameter;
    const bool aboveThreshold = std::fabs(offset) > kVisibilityThreshold;
    if (!wasIgnored || !aboveThreshold) {
        // Either the value was taken over, or the hardware is close enough
        // that surfacing an arrow would just flash for a frame. If this
        // control is the one currently shown, clear the indicator. Otherwise
        // leave whichever other control is active alone.
        if (m_pActiveControl.load(std::memory_order_relaxed) == pControl) {
            m_pActiveControl.store(nullptr, std::memory_order_relaxed);
            clearOutputs();
        }
        return;
    }

    m_pActiveControl.store(pControl, std::memory_order_relaxed);
    // Sign convention per UI spec: hardware above software → arrow right (+),
    // hardware below software → arrow left (−).
    writeOffset(offset);
    emit valueUpdated();
}

void SoftTakeoverIndicator::writeOffset(double offset) {
    const double magnitude = std::clamp(std::fabs(offset), 0.0, 1.0);
    m_pCoOffsetLeft->forceSet(offset < 0.0 ? magnitude : 0.0);
    m_pCoOffsetRight->forceSet(offset > 0.0 ? magnitude : 0.0);
    m_pCoActive->forceSet(magnitude > 0.0 ? 1.0 : 0.0);
}

void SoftTakeoverIndicator::clearOutputs() {
    m_pCoActive->forceSet(0.0);
    m_pCoOffsetLeft->forceSet(0.0);
    m_pCoOffsetRight->forceSet(0.0);
}

void SoftTakeoverIndicator::onValueUpdated() {
    // Restart the idle countdown on every fresh out-of-sync sample.
    m_idleTimer.start();
}

void SoftTakeoverIndicator::onIdleTimeout() {
    m_pActiveControl.store(nullptr, std::memory_order_relaxed);
    clearOutputs();
}
