#include "control/controlnotifier.h"

#include <pthread.h>

#include <algorithm>
#include <cerrno>

#include "control/control.h"

// static
ControlNotifier& ControlNotifier::instance() {
    static ControlNotifier s_instance;
    return s_instance;
}

ControlNotifier::ControlNotifier() {
    sem_init(&m_sem, 0, 0);
    // Inherits the creating thread's scheduling and affinity: the controls are
    // registered from the main thread, which runs on the housekeeping cores.
    m_thread = std::thread([this] { run(); });
    pthread_setname_np(m_thread.native_handle(), "ControlNotify");
}

ControlNotifier::~ControlNotifier() {
    m_stop.store(true, std::memory_order_release);
    wake();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    sem_destroy(&m_sem);
}

void ControlNotifier::add(const QSharedPointer<ControlDoublePrivate>& pControl) {
    if (!pControl) {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_controls.push_back(pControl.toWeakRef());
}

void ControlNotifier::run() {
    std::vector<QWeakPointer<ControlDoublePrivate>> controls;
    while (true) {
        if (sem_wait(&m_sem) != 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (m_stop.load(std::memory_order_acquire)) {
            return;
        }
        // Collapse a burst of wakes into one pass: every pending flag is read
        // below anyway.
        while (sem_trywait(&m_sem) == 0) {
        }
        {
            const std::lock_guard<std::mutex> lock(m_mutex);
            m_controls.erase(std::remove_if(m_controls.begin(),
                                     m_controls.end(),
                                     [](const QWeakPointer<ControlDoublePrivate>& p) {
                                         return p.isNull();
                                     }),
                    m_controls.end());
            controls = m_controls;
        }
        // Emit without the lock held, so a receiver may register controls.
        for (const auto& pWeak : controls) {
            const QSharedPointer<ControlDoublePrivate> pControl = pWeak.toStrongRef();
            if (pControl) {
                pControl->emitDeferredNotify();
            }
        }
        controls.clear();
    }
}
