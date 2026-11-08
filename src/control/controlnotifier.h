#pragma once

#include <semaphore.h>

#include <QSharedPointer>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class ControlDoublePrivate;

/// Bite DJ: emits valueChanged() for controls that the audio engine updates,
/// so that the engine callback never has to.
///
/// Emitting a signal from the engine thread is not free when the receivers
/// live in other threads: every queued connection makes Qt lock its
/// signal-slot mutex and the receiving thread's post-event-list mutex. Any
/// thread posting to the GUI thread holds those mutexes for a moment. On
/// PREEMPT_RT a QMutex is a plain futex without priority inheritance, so when
/// such a thread is preempted on the busy housekeeping cores while holding one,
/// the FIFO 70 engine waits behind it. Field logs (20261106.1) caught the engine
/// blocked in QBasicMutex::lockInternal under EngineBuffer::postProcess
/// setting "playposition" for 10+ ms, long enough to underflow the 2-period
/// ALSA ring.
///
/// A control that opts in (ControlObject::enableDeferredNotify()) is set from
/// the engine with ControlObject::setDeferredNotify(): the value is stored
/// atomically and a pending flag is raised, and the engine's only other act is
/// a sem_post(). This thread wakes on that, and emits valueChanged() with the
/// control's current value and the original sender. Receivers that were queued
/// before stay queued (they live in neither thread). A receiver connected
/// directly now runs here rather than on the engine thread, so only controls
/// with no engine-side direct listeners may opt in.
class ControlNotifier {
  public:
    static ControlNotifier& instance();

    /// Registers a control for deferred notification. Takes a mutex, so it
    /// must not be called from the engine thread; call it where the control
    /// is constructed.
    void add(const QSharedPointer<ControlDoublePrivate>& pControl);

    /// Requests a drain. Lock-free and real-time safe: an atomic increment,
    /// and a futex wake (whose kernel-side lock does priority inheritance)
    /// only when the thread is waiting.
    void wake() noexcept {
        sem_post(&m_sem);
    }

    ~ControlNotifier();

  private:
    ControlNotifier();
    ControlNotifier(const ControlNotifier&) = delete;
    ControlNotifier& operator=(const ControlNotifier&) = delete;

    void run();

    sem_t m_sem;
    std::atomic<bool> m_stop{false};
    // Guards m_controls. Never taken by the engine thread.
    std::mutex m_mutex;
    std::vector<QWeakPointer<ControlDoublePrivate>> m_controls;
    std::thread m_thread;
};
