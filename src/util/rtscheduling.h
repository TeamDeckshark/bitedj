#pragma once

namespace mixxx {

// SCHED_FIFO priorities for the app's latency-critical threads. These must
// stay below the audio/USB IRQ threads. The hardware interrupt handling
// must always be able to preempt us, the audio callback must preempt
// everything else in the app, and controller input must preempt the main
// (GUI) thread, which in turn sits above everything left on SCHED_OTHER
// (analysis, library workers).
constexpr int kRtPrioAudioEngine = 70;
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
