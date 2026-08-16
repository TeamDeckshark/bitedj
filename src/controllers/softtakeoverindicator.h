#pragma once

#include <QAtomicPointer>
#include <QObject>
#include <QTimer>
#include <atomic>

class ControlObject;

// Bite DJ: publishes a single shared "out-of-sync" indicator for whichever
// soft-takeover-armed control was most recently moved on the MIDI controller.
// Skin XML binds to [SoftTakeover],active and the signed-magnitude pair
// [SoftTakeover],offset_left / offset_right to render a directional arrow.
//
// Owned by ControllerManager so the COs have a deterministic lifetime that
// matches the rest of the controller subsystem (and is destroyed before the
// MixxxTest fixture's CO sweep in unit tests that don't construct one).
class SoftTakeoverIndicator : public QObject {
    Q_OBJECT
  public:
    explicit SoftTakeoverIndicator(QObject* parent = nullptr);
    ~SoftTakeoverIndicator() override;

    // Returns the live instance if one exists; nullptr otherwise (e.g. in
    // unit tests that exercise SoftTakeoverCtrl without ControllerManager).
    static SoftTakeoverIndicator* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    // Called from controller threads inside SoftTakeoverCtrl::ignore().
    // currentParameter is the software-side value, newParameter is the
    // hardware-side value, both already normalized to [0, 1].
    void publish(const ControlObject* pControl,
            double currentParameter,
            double newParameter,
            bool wasIgnored);

  private slots:
    void onValueUpdated();
    void onIdleTimeout();

  signals:
    void valueUpdated();

  private:
    void writeOffset(double offset);
    void clearOutputs();

    static constexpr int kIdleTimeoutMs = 1500;

    static QAtomicPointer<SoftTakeoverIndicator> s_pInstance;

    QTimer m_idleTimer;
    std::atomic<const ControlObject*> m_pActiveControl{nullptr};

    ControlObject* m_pCoActive;
    ControlObject* m_pCoOffsetLeft;
    ControlObject* m_pCoOffsetRight;
};
