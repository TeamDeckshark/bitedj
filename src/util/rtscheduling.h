#pragma once

namespace mixxx {

// SCHED_FIFO priorities for the app's latency-critical threads. These must
// stay below the audio/USB IRQ threads. The hardware interrupt handling
// must always be able to preempt us, the audio callback must preempt
// everything else in the app, and controller input must preempt the main
// (GUI) thread, which in turn sits above everything left on SCHED_OTHER
// (analysis, library workers).
constexpr int kRtPrioAudioEngine = 70;
// The two hops between the audio callback and the disk. CachingReaderWorker is
// the only EngineWorker there is, so every chunk the decks consume travels
// callback -> EngineWorkerScheduler::runWorkers() -> the worker's decode, and
// both hops used to be SCHED_OTHER on the housekeeping cores where the GUI
// thread below could preempt them: waveform rendering delaying the fetch of
// samples that are about to be played is precisely backwards. They sit above
// controller input too, because without sample data there is nothing to
// scratch, and both block on I/O or a wait condition constantly, so they yield
// the CPU rather than holding it the way a GUI repaint does.
//
// The scheduler gets its own rung above the worker for a specific reason:
// SCHED_FIFO only preempts *strictly* lower priorities, so at equal priority a
// wake-up for the next chunk would have to wait out the decode of the current
// one on the same core. The scheduler does nothing but walk the worker list
// and wake, so letting it cut in costs a few microseconds.
constexpr int kRtPrioEngineWorkerScheduler = 62;
constexpr int kRtPrioTrackReader = 60;
constexpr int kRtPrioControllerInput = 50;
constexpr int kRtPrioMainThread = 49;

/// Best-effort promotion of the calling thread to the real-time SCHED_FIFO
/// policy on Linux. QThread priorities are ignored by the default Linux
/// scheduler (SCHED_OTHER applies no priorities between threads), so
/// latency-critical threads must use an RT policy to reliably preempt GUI
/// rendering. Requires CAP_SYS_NICE or an rtprio rlimit; returns false and
/// logs a warning when the promotion is not permitted. No-op returning false
/// on non-Linux platforms.
bool promoteCurrentThreadToRealtime(int priority, const char* threadName);

/// Best-effort demotion of the calling thread to the weakest scheduling class
/// Linux has: SCHED_IDLE, which is only given the CPU when no SCHED_OTHER (let
/// alone SCHED_FIFO) thread on the machine wants it. The counterpart of the
/// promotion above, for threads doing work the latency-critical threads handed
/// off precisely so they would not have to do it — encoding and writing a
/// recording, which must never be in a position to take a slice from the audio
/// callback no matter how long the device it writes to takes.
///
/// It also settles the thread's *I/O* priority: the block layer derives the
/// I/O class from the scheduling class, so a SCHED_IDLE thread's writes queue
/// behind everything else instead of in front of a deck reading its track.
///
/// Lowering a thread's own class needs no privileges. Falls back to nice 19
/// (still SCHED_OTHER, so only a weighting) if the policy change is refused,
/// and returns false when neither works. No-op returning false on non-Linux.
bool demoteCurrentThreadToIdle(const char* threadName);

/// Best-effort demotion of the calling thread to *background* work: nice 19,
/// the weakest SCHED_OTHER weighting Linux has (roughly a sixty-eighth of the
/// CPU share a nice-0 thread gets), under SCHED_BATCH, which additionally
/// tells the scheduler the thread is non-interactive so it never preempts
/// anything on wakeup.
///
/// This is for long CPU-bound jobs that must yield to whatever is feeding the
/// audio device. Track analysis is the case it exists for: an analyzer worker
/// decodes and scans a whole file flat out, and it shares the housekeeping
/// cores with CachingReaderWorker and the engine worker pool — the threads
/// that hand the audio callback its next chunk, and which are SCHED_OTHER at
/// nice 0, because QThread::HighPriority maps to nothing under SCHED_OTHER
/// (see the caution in main()). At equal weight, analyzers saturating those
/// cores delay the reads past the buffer deadline and the decks glitch.
///
/// Deliberately *not* demoteCurrentThreadToIdle(): an analyzer holds the
/// global track cache and database locks that the reader threads also take,
/// and SCHED_IDLE only runs when nothing else on the machine wants the CPU,
/// which makes a lock holder easy to leave sitting behind the SCHED_FIFO GUI
/// thread — a priority inversion on the exact path this was meant to protect.
/// nice 19 stays inside normal CFS fairness, so the thread always makes
/// forward progress and always releases the lock. EngineSideChain can afford
/// SCHED_IDLE because it shares no lock with the audio path.
///
/// Also drops the thread to the weakest best-effort I/O priority, so the
/// analyzer reading a whole track off a USB stick does not queue in front of
/// a deck reading the track that is playing. Honoured by BFQ and ignored by
/// mq-deadline/none, hence best-effort like everything else here.
///
/// Lowering a thread's own class and nice value needs no privileges. Falls
/// back to plain SCHED_OTHER at nice 19 when SCHED_BATCH is refused, and
/// returns false only if the renice fails too. No-op returning false on
/// non-Linux platforms.
bool demoteCurrentThreadToBackground(const char* threadName);

/// Pin the calling thread to the single CPU named (as a decimal core number)
/// by the environment variable envVar, if it is set.
bool pinCurrentThreadToCpuFromEnv(const char* envVar, const char* threadName);

/// Best-effort mlockall() so no page of the process can be reclaimed once it
/// has been touched (MCL_ONFAULT: pages are locked on first fault instead of
/// being faulted in upfront, which keeps a Qt application from wiring
/// hundreds of MB of library text and caches it never uses — that matters on
/// a 1GB Pi). Without this, memory pressure from track analysis or waveform
/// caching can evict executable pages that the audio callback then major-
/// faults back in, blowing the buffer deadline. Requires a memlock rlimit
bool lockAllMemory();

} // namespace mixxx
