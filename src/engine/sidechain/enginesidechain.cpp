// This class provides a way to do audio processing that does not need
// to be executed in real-time. For example, broadcast encoding
// and recording encoding can be done here. This class uses double-buffering
// to increase the amount of time the CPU has to do whatever work needs to
// be done, and that work is executed in a separate thread. (Threading
// allows the next buffer to be filled while processing a buffer that's is
// already full.)

#include "engine/sidechain/enginesidechain.h"

#include <QtDebug>

#include "engine/engine.h"
#include "engine/sidechain/sidechainworker.h"
#include "moc_enginesidechain.cpp"
#include "util/counter.h"
#include "util/event.h"
#include "util/rtscheduling.h"
#include "util/sample.h"
#include "util/trace.h"

#define SIDECHAIN_BUFFER_SIZE 65536

namespace {
// How long the thread sleeps before looking at the FIFO again. Nothing wakes
// it early (see run()), so this is the whole of its scheduling: the FIFO holds
// about 0.74 s of stereo audio at 44.1 kHz, which leaves an order of magnitude
// of headroom over this interval before a sample could be dropped, while
// costing ten wake-ups a second on an appliance that is doing nothing.
constexpr unsigned long kDrainIntervalMs = 100;
} // namespace

EngineSideChain::EngineSideChain(
        UserSettingsPointer pConfig,
        CSAMPLE* sidechainMix)
        : m_pConfig(pConfig),
          m_bStopThread(false),
          m_sampleFifo(SIDECHAIN_BUFFER_SIZE),
          m_pWorkBuffer(SampleUtil::alloc(SIDECHAIN_BUFFER_SIZE)),
          m_pSidechainMix(sidechainMix) {
    // Bite DJ: the lowest priority Qt offers, not the HighPriority upstream
    // uses (see issue #7272). Nothing this thread does has a deadline — it
    // exists so that the audio callback does not have to encode and write a
    // recording — and on this appliance it writes to a USB stick that can stall
    // for as long as it likes, so it must never be able to take CPU from the
    // engine. run() goes further and puts it in SCHED_IDLE, which is what
    // actually enforces that on Linux; this is here because it must not be
    // InheritPriority, which would hand a thread doing blocking file I/O the
    // SCHED_FIFO 49 policy of the thread that started it.
    start(QThread::IdlePriority);
}

EngineSideChain::~EngineSideChain() {
    m_waitLock.lock();
    m_bStopThread = true;
    m_waitForSamples.wakeAll();
    m_waitLock.unlock();

    // Wait until the thread has finished.
    wait();

    MMutexLocker locker(&m_workerLock);
    while (!m_workers.empty()) {
        SideChainWorker* pWorker = m_workers.takeLast();
        pWorker->shutdown();
        delete pWorker;
    }
    locker.unlock();

    SampleUtil::free(m_pWorkBuffer);
}

void EngineSideChain::addSideChainWorker(SideChainWorker* pWorker) {
    MMutexLocker locker(&m_workerLock);
    m_workers.append(pWorker);
}

void EngineSideChain::receiveBuffer(const AudioInput& input,
        const CSAMPLE* pBuffer,
        unsigned int iFrames) {
    VERIFY_OR_DEBUG_ASSERT(input.getType() == AudioPathType::RecordBroadcast) {
        qDebug() << "WARNING: AudioInput type is not RECORD_BROADCAST. Ignoring incoming buffer.";
        return;
    }
    // Just copy the received samples form the sound card input to the
    // engine. After processing we get it back via writeSamples()
    SampleUtil::copy(m_pSidechainMix, pBuffer, iFrames * mixxx::kEngineChannelCount);
}

void EngineSideChain::writeSamples(const CSAMPLE* pBuffer, int iFrames) {
    Trace sidechain("EngineSideChain::writeSamples");
    // TODO: remove assumption of stereo buffer
    const int numSamples = iFrames * mixxx::kEngineChannelCount;
    const int numSamplesWritten = m_sampleFifo.write(pBuffer, numSamples);

    if (numSamplesWritten != numSamples) {
        // Dropped, never blocked: this runs in the audio callback, and a
        // recording missing a buffer is always better than the engine missing
        // its deadline. This FIFO is also the hard ceiling on what the sidechain
        // can hold in memory — SIDECHAIN_BUFFER_SIZE samples, 256 KiB, fixed at
        // construction — no matter how far behind the encoder or the device it
        // writes to has fallen.
        Counter("EngineSideChain::writeSamples buffer overrun").increment();
    }

    // Deliberately no wake-up from here: the sidechain polls instead (see
    // run()). QWaitCondition::wakeAll() takes the condition's own mutex, which
    // the sidechain thread holds for a moment either side of its sleep — from
    // here that is a lock the real-time callback can block on, held by a
    // SCHED_IDLE thread that may not be scheduled again for a while. The
    // callback's entire interaction with the sidechain is the wait-free FIFO
    // write above.
}

void EngineSideChain::run() {
    // the id of this thread, for debugging purposes //XXX copypasta (should
    // factor this out somehow), -kousu 2/2009
    unsigned static id = 0;
    QThread::currentThread()->setObjectName(QString("EngineSideChain %1").arg(++id));
    static const QString tag("EngineSideChain");
    Event::start(tag);

    // Everything below this line is work the audio callback handed off so that
    // it would not have to do it: encoding, writing the recording to a USB
    // stick, broadcasting. None of it has a deadline, and all of it can block
    // on a device for an unbounded time, so it runs in the weakest scheduling
    // class there is — only ever on a CPU nothing else wants. That also puts
    // its writes in the idle I/O class, behind the reads a deck is doing from
    // whatever stick its track came from.
    mixxx::demoteCurrentThreadToIdle("EngineSideChain");

    while (!m_bStopThread) {
        // Sleep until there is plausibly something to do. Timed rather than
        // woken by the callback: see writeSamples(). The destructor still wakes
        // us so that shutdown does not wait out the interval.
        m_waitLock.lock();

        Event::end(tag);
        m_waitForSamples.wait(&m_waitLock, kDrainIntervalMs);
        m_waitLock.unlock();
        Event::start(tag);

        // Take a copy of the worker list instead of holding the lock across
        // process(): that call encodes and writes to disk, and on a stalled USB
        // stick it can sit in an uninterruptible write for as long as the device
        // takes. Whoever wanted this lock meanwhile — the GUI thread registering
        // a worker — would be stuck behind that write, i.e. a frozen UI. Workers
        // are only ever removed by the destructor, which joins this thread
        // before touching the list, so the copy cannot outlive its entries.
        QList<SideChainWorker*> workers;
        {
            MMutexLocker locker(&m_workerLock);
            workers = m_workers;
        }

        int samples_read;
        while ((samples_read = m_sampleFifo.read(m_pWorkBuffer,
                                                 SIDECHAIN_BUFFER_SIZE))) {
            Trace process("EngineSideChain::process");
            for (SideChainWorker* pWorker : std::as_const(workers)) {
                pWorker->process(m_pWorkBuffer, samples_read);
            }
        }

        // Check to see if we're supposed to exit/stop this thread.
        if (m_bStopThread) {
            return;
        }
    }
}
