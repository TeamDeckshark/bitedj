#include "engine/engineworkerscheduler.h"

#include "engine/engineworker.h"
#include "moc_engineworkerscheduler.cpp"
#include "util/compatibility/qmutex.h"
#include "util/event.h"
#include "util/rtscheduling.h"

namespace {
// Upper bound on how long the scheduler sleeps without a runWorkers() wake-up.
// Only reached when the audio callback is not running (see run()), so it costs
// nothing in normal operation; short enough that a track loaded with the sound
// device down still appears promptly rather than hanging until it comes back.
constexpr unsigned long kFallbackWakeIntervalMs = 50;
} // namespace

EngineWorkerScheduler::EngineWorkerScheduler(QObject* pParent)
        : m_bWakeScheduler(false),
          m_bQuit(false) {
    Q_UNUSED(pParent);
}

EngineWorkerScheduler::~EngineWorkerScheduler() {
    {
        // tell run method to terminate
        const auto lock = lockMutex(&m_mutex);
        m_bQuit = true;
        m_waitCondition.wakeAll();
    }
    // wait for thread to terminate
    wait();
}

void EngineWorkerScheduler::workerReady() {
    m_bWakeScheduler.store(true);
}

void EngineWorkerScheduler::addWorker(EngineWorker* pWorker) {
    DEBUG_ASSERT(pWorker);
    const auto lock = lockMutex(&m_mutex);
    m_workers.push_back(pWorker);
}

void EngineWorkerScheduler::runWorkers() {
    // Wake the scheduler if we have written a worker-ready message to the
    // scheduler. This is called from the callback thread, so we use an
    // atomic and not a mutex.
    if (m_bWakeScheduler.exchange(false)) {
        m_waitCondition.wakeAll();
    }
}

void EngineWorkerScheduler::run() {
    static const QString tag("EngineWorkerScheduler");

    // The wake-up hop between the audio callback and CachingReaderWorker, the
    // only EngineWorker there is. All it does per callback is walk the worker
    // list and wake whoever is ready, but it was doing that on SCHED_OTHER at
    // nice 0 (EngineMixer passes QThread::HighPriority to start(), which means
    // nothing under SCHED_OTHER), so a busy GUI thread on the same core could
    // sit in front of it and delay every chunk fetch behind it. Runs one rung
    // above the worker it wakes so a new request never queues behind the
    // decode of the previous one; see util/rtscheduling.h.
    mixxx::promoteCurrentThreadToRealtime(
            mixxx::kRtPrioEngineWorkerScheduler, "EngineWorkerScheduler");

    bool quit = false;
    while (!quit) {
        Event::start(tag);
        {
            const auto lock = lockMutex(&m_mutex);
            for(const auto& pWorker: m_workers) {
                pWorker->wakeIfReady();
            }
        }
        Event::end(tag);
        {
            const auto lock = lockMutex(&m_mutex);
            if (!m_bQuit) {
                // Wait for the next runWorkers() call, but never indefinitely.
                //
                // runWorkers() is called from exactly one place —
                // EngineMixer::process(), i.e. the audio callback — while
                // workReady() only sets the m_bWakeScheduler atomic and does
                // not touch this condition. So with no audio device open
                // (device unplugged, or its ALSA card gone after a USB
                // re-enumeration) there is no callback, nothing ever wakes
                // this thread, and wakeIfReady() is never reached: every
                // EngineWorker stays blocked on its own semaphore forever.
                //
                // The visible cost of that was CachingReaderWorker never
                // picking up a queued newTrack(), so a track loaded while the
                // sound device was down never finished loading — no waveform
                // (it is the reader that emits trackLoaded), and no analysis
                // either, since PlayerManager schedules that off
                // BaseTrackPlayer::newTrackLoaded. The whole load path runs on
                // the worker thread, so a wake-up is the only thing that was
                // missing; with this fallback the load completes and the deck
                // populates normally, it just cannot play until audio is back.
                //
                // While the engine is running this timeout is never reached —
                // runWorkers() wakes us first — so the hot path is unchanged.
                m_waitCondition.wait(&m_mutex, kFallbackWakeIntervalMs);
            }
            // copy mutex protected var to local
            quit = m_bQuit;
        }
    }
}
