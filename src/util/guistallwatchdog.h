#pragma once

#include <QObject>
#include <QTimer>
#include <atomic>
#include <thread>

namespace mixxx {

/// Reports every stretch in which the main (GUI) thread's event loop stops
/// turning for a second or more, as warnings, so they reach mixxx.log at the
/// appliance's default --log-level warning and are flushed straight away.
///
/// Why: a field report of the UI freezing for seconds while loading a track
/// came with a mixxx.log that said nothing at all, because nothing on the
/// blocked path logs anything at warning level. Whatever blocks the GUI thread
/// next time names itself.
///
/// While the stall lasts, a watcher thread samples the GUI thread once a
/// second (more sparsely after the first few) and logs:
///  - its scheduler state and kernel wait channel (/proc/self/task/<tid>/stat
///    and wchan): "D" with an I/O wchan is a disk wait, "S" in futex_wait a
///    lock, "S" in hrtimer_nanosleep e.g. SQLite's busy handler;
///  - the syscall it is in, with the file its first argument names when that
///    is a file descriptor (so an fsync() says which file);
///  - its kernel stack, where /proc lets us read it (root);
///  - its user-space stack, taken by the GUI thread itself in a signal
///    handler. A thread in uninterruptible sleep only runs that handler once
///    it wakes, so that stack is then logged late, marked as such — it still
///    shows the call that blocked, because the handler runs on its way out.
///
/// Stack frames are printed as module+offset (plus a symbol name where the
/// dynamic symbol table has one). The offsets symbolize against the unstripped
/// binary of the same build, e.g.
///   aarch64-linux-gnu-addr2line -Cfipe build-release/mixxx 0x<offset - 1>
///
/// Linux only; a no-op elsewhere. Construct on the main thread right before
/// the event loop runs, destroy after it returns (shutdown is allowed to take
/// its time and is not reported).
class GuiStallWatchdog : public QObject {
  public:
    GuiStallWatchdog();
    ~GuiStallWatchdog() override;

  private:
    void run();

    QTimer m_heartbeat;
    std::atomic<bool> m_stop;
    std::thread m_thread;
};

} // namespace mixxx
