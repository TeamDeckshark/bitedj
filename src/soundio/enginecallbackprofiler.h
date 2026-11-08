#pragma once

#include <cstdint>

namespace mixxx {

/// Times every audio callback of the clock-reference sound device and logs,
/// at warning level, the ones that put the output at risk, so that an
/// underrun in mixxx.log comes with what the engine thread was doing.
///
/// Why: a 20261105.1 debug capture had the output underrun (PortAudio
/// paOutputUnderflow, code 6) 21 times, each a second or two after a track
/// load while track analysis kept cores 0-1 at 70-94 % busy, and the engine,
/// alone on core 3, at 19-28 %. A callback that is late with its own core
/// idle is either waiting for something (a lock, a page fault, I/O), or is
/// woken late. The numbers below tell those apart:
///
///  - gap: time since the previous callback started; anything beyond one
///    period is time the ALSA ring drained without being refilled.
///  - queued: audio already queued for the DAC when the callback started
///    (PortAudio's outputBufferDacTime - currentTime). Near zero means the
///    callback started too late, whatever it then did.
///  - ran, and its stages (engine = SoundManager::readProcess() +
///    onDeviceOutputCallback(), i.e. EngineMixer::process(); output = the
///    copy into PortAudio's buffer; write = writeProcess() and the rest).
///  - faults (minor/major) and context switches (voluntary/involuntary) of
///    the engine thread over the callback, from getrusage(RUSAGE_THREAD). A
///    voluntary switch inside a callback means it blocked: a futex (lock), a
///    page fault waiting on I/O, a sleep. An involuntary one means something
///    of higher priority took the core. The same counts between the previous
///    callback's end and this one's start are PortAudio's own part of the
///    cycle (its poll and ALSA mmap calls), printed as "before".
///
/// In debug mode (--log-level debug) a watcher thread also catches a callback
/// in the act: once one has been running for kStallUs it reads the engine
/// thread's state, wait channel, syscall and kernel stack from /proc, and (at
/// most once every kUserStackEveryMs) has the engine thread take its own
/// user-space stack in a signal handler, which names the lock or call it is
/// stuck in. Frames are module+offset, for addr2line against the unstripped
/// build, as with GuiStallWatchdog.
///
/// The callback side is lock-free and allocation-free: two clock reads and
/// two getrusage() calls per callback, and a record pushed onto a
/// single-producer ring when the callback was slow. The ring is drained and
/// logged from the main thread by drainAndLog(), every 5 s, next to the
/// underrun count.
///
/// Linux only; the calls are no-ops elsewhere. One device at a time (the
/// clock reference) may call the callback hooks.
namespace EngineCallbackProfiler {

/// Main thread, once: starts the debug-mode watcher if enabled.
void start(bool watchStalls);
/// Main thread: stops the watcher. Safe to call without start().
void stop();

/// Engine thread, first thing in the callback. queuedSecs is
/// outputBufferDacTime - currentTime from PortAudio (<= 0 if unknown).
/// periodSecs is framesPerBuffer / sample rate. underflowFlagged is whether
/// PortAudio flagged an output underflow for this callback.
void callbackBegin(double periodSecs, double queuedSecs, bool underflowFlagged);
/// Engine thread: the engine has produced this callback's audio.
void markEngineDone();
/// Engine thread: the audio has been copied into PortAudio's buffer.
void markOutputDone();
/// Engine thread, last thing in the callback.
void callbackEnd();

/// Main thread: logs the slow callbacks recorded since the last call.
void drainAndLog();

} // namespace EngineCallbackProfiler

} // namespace mixxx
