#include "util/guistallwatchdog.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtDebug>
#include <chrono>

#include "util/rtscheduling.h"

#ifdef __LINUX__
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace mixxx {

namespace {

#ifdef __LINUX__
constexpr int kHeartbeatMs = 200;
constexpr int kPollMs = 100;
// Shorter freezes than this are ordinary on a Pi (a skin page switch, a big
// library query) and not worth a line each.
constexpr qint64 kStallMs = 1000;
// One sample a second for the first few, then one every ten: enough to see
// whether the thread moves on, without flooding the log during a real hang.
constexpr qint64 kSampleEveryMs = 1000;
constexpr int kDenseSamples = 5;
constexpr qint64 kSparseSampleEveryMs = 10000;
// How long a sample waits for the GUI thread to answer the stack request
// before reporting it as not answering (uninterruptible sleep, most likely).
constexpr int kStackWaitMs = 50;
constexpr int kKernelStackLines = 12;

std::atomic<qint64> s_lastBeatMs{0};

qint64 nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

constexpr int kMaxFrames = 48;

pid_t s_mainTid = 0;
pthread_t s_mainThread;

// Written by the signal handler on the GUI thread, read by the watcher once
// s_frameCount says the handler is done. Only one request is ever in flight.
void* s_frames[kMaxFrames];
std::atomic<int> s_frameCount{-1};

int stackSignal() {
    return SIGRTMIN + 3;
}

void handleStackSignal(int) {
    const int savedErrno = errno;
    const int count = backtrace(s_frames, kMaxFrames);
    s_frameCount.store(count, std::memory_order_release);
    errno = savedErrno;
}

QByteArray readProcFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll().trimmed();
}

QString taskPath(const char* entry) {
    return QStringLiteral("/proc/self/task/%1/%2").arg(s_mainTid).arg(QLatin1String(entry));
}

struct SyscallName {
    long nr;
    const char* name;
    bool takesFd; // arg0 is a file descriptor
};

// The calls a blocked GUI thread is likely to sit in. Numbers differ between
// architectures (the Pi is aarch64), hence names; any other call is logged by
// number only.
const SyscallName kSyscallNames[] = {
        {SYS_read, "read", true},
        {SYS_write, "write", true},
        {SYS_pread64, "pread64", true},
        {SYS_pwrite64, "pwrite64", true},
        {SYS_readv, "readv", true},
        {SYS_writev, "writev", true},
        {SYS_fsync, "fsync", true},
        {SYS_fdatasync, "fdatasync", true},
        {SYS_ftruncate, "ftruncate", true},
        {SYS_fcntl, "fcntl", true},
        {SYS_flock, "flock", true},
        {SYS_ioctl, "ioctl", true},
        {SYS_getdents64, "getdents64", true},
        {SYS_openat, "openat", false},
        {SYS_unlinkat, "unlinkat", false},
        {SYS_futex, "futex", false},
        {SYS_nanosleep, "nanosleep", false},
        {SYS_clock_nanosleep, "clock_nanosleep", false},
        {SYS_ppoll, "ppoll", false},
#ifdef SYS_poll
        {SYS_poll, "poll", false},
#endif
#ifdef SYS_umount2
        {SYS_umount2, "umount2", false},
#endif
};

/// "state=D wchan=folio_wait_bit_common syscall=fsync (fd 23 -> /media/X/a.sqlite)"
QString describeThreadState() {
    QString description;
    // The state is the field after the parenthesised command name, which may
    // itself contain spaces and parentheses.
    const QByteArray stat = readProcFile(taskPath("stat"));
    const int commEnd = stat.lastIndexOf(')');
    if (commEnd >= 0 && commEnd + 2 < stat.size()) {
        description += QStringLiteral("state=") + QChar::fromLatin1(stat.at(commEnd + 2));
    }
    const QByteArray wchan = readProcFile(taskPath("wchan"));
    if (!wchan.isEmpty() && wchan != "0") {
        description += QStringLiteral(" wchan=") + QString::fromLatin1(wchan);
    }
    // "<nr> <arg0> ... <sp> <pc>", or "running", or "-1 <sp> <pc>" outside a
    // syscall.
    const QList<QByteArray> syscall = readProcFile(taskPath("syscall")).split(' ');
    bool ok = false;
    const long nr = syscall.value(0).toLong(&ok);
    if (ok && nr >= 0) {
        const SyscallName* pKnown = nullptr;
        for (const SyscallName& known : kSyscallNames) {
            if (known.nr == nr) {
                pKnown = &known;
                break;
            }
        }
        description += QStringLiteral(" syscall=") +
                (pKnown ? QString::fromLatin1(pKnown->name) : QString::number(nr));
        // Name the file a read, write, fsync, ... is waiting on.
        const qulonglong arg0 = syscall.value(1).toULongLong(&ok, 16);
        if (pKnown && pKnown->takesFd && ok) {
            // Not QFileInfo::symLinkTarget(), which turns "pipe:[123]" into a
            // path under /proc.
            const QByteArray link = QStringLiteral("/proc/self/fd/%1").arg(arg0).toLatin1();
            char target[512];
            const ssize_t length = readlink(link.constData(), target, sizeof(target));
            if (length > 0) {
                description += QStringLiteral(" (fd %1 -> %2)")
                                       .arg(arg0)
                                       .arg(QString::fromLocal8Bit(target, length));
            }
        }
    }
    return description;
}

QStringList kernelStack() {
    QStringList lines;
    const QList<QByteArray> stack = readProcFile(taskPath("stack")).split('\n');
    for (const QByteArray& line : stack) {
        if (line.isEmpty()) {
            continue;
        }
        lines << QString::fromLatin1(line);
        if (lines.size() == kKernelStackLines) {
            break;
        }
    }
    return lines;
}

QStringList formatFrames(int count) {
    QStringList lines;
    for (int i = 0; i < count; ++i) {
        const void* pc = s_frames[i];
        Dl_info info{};
        if (dladdr(pc, &info) == 0 || !info.dli_fname) {
            lines << QStringLiteral("#%1 %2").arg(i).arg(
                    reinterpret_cast<quintptr>(pc), 0, 16);
            continue;
        }
        const quintptr offset = reinterpret_cast<quintptr>(pc) -
                reinterpret_cast<quintptr>(info.dli_fbase);
        QString line = QStringLiteral("#%1 %2+0x%3")
                               .arg(i)
                               .arg(QFileInfo(QString::fromLocal8Bit(info.dli_fname))
                                               .fileName())
                               .arg(offset, 0, 16);
        if (info.dli_sname) {
            line += QStringLiteral(" (%1+0x%2)")
                            .arg(QString::fromLatin1(info.dli_sname))
                            .arg(reinterpret_cast<quintptr>(pc) -
                                            reinterpret_cast<quintptr>(info.dli_saddr),
                                    0,
                                    16);
        }
        lines << line;
    }
    return lines;
}

void logStack(const char* what, qint64 stalledMs, const QStringList& frames) {
    qWarning().noquote() << "GuiStallWatchdog:" << what << "after" << stalledMs
                         << "ms:\n  " + frames.join(QStringLiteral("\n  "));
}
#endif

} // anonymous namespace

GuiStallWatchdog::GuiStallWatchdog()
        : m_stop(false) {
#ifdef __LINUX__
    s_mainTid = static_cast<pid_t>(syscall(SYS_gettid));
    s_mainThread = pthread_self();

    struct sigaction action{};
    action.sa_handler = handleStackSignal;
    sigemptyset(&action.sa_mask);
    // A sample interrupts whatever the GUI thread is waiting in; restart what
    // can be restarted. The rest (poll, nanosleep) return EINTR, which the
    // event loop and SQLite's busy handler both treat as a short wait.
    action.sa_flags = SA_RESTART;
    if (sigaction(stackSignal(), &action, nullptr) != 0) {
        qWarning() << "GuiStallWatchdog: cannot install the stack signal handler";
    }
    // The first backtrace() loads libgcc's unwinder, which allocates; do that
    // here rather than inside the handler.
    void* frame;
    backtrace(&frame, 1);

    s_lastBeatMs.store(nowMs());
    m_heartbeat.setInterval(kHeartbeatMs);
    connect(&m_heartbeat, &QTimer::timeout, this, [] {
        s_lastBeatMs.store(nowMs(), std::memory_order_relaxed);
    });
    m_heartbeat.start();

    m_thread = std::thread([this] { run(); });
#endif
}

GuiStallWatchdog::~GuiStallWatchdog() {
    m_stop.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void GuiStallWatchdog::run() {
#ifdef __LINUX__
    // Spawned from the SCHED_FIFO main thread, so it inherited that class
    // (see main()). Watching needs none of it.
    setCurrentThreadRealtimeQuietly(false, 0);
    pthread_setname_np(pthread_self(), "GuiStallWatch");

    qint64 stallStartMs = 0; // the last heartbeat before the stall; 0 = none
    qint64 nextSampleMs = 0;
    int samples = 0;
    QStringList lastKernelStack;
    bool stackPending = false;
    qint64 stackRequestedMs = 0;

    while (!m_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        const qint64 now = nowMs();
        const qint64 lastBeat = s_lastBeatMs.load(std::memory_order_relaxed);

        if (stackPending) {
            const int count = s_frameCount.load(std::memory_order_acquire);
            if (count >= 0) {
                stackPending = false;
                logStack("late GUI thread stack (taken when it woke)",
                        now - stackRequestedMs,
                        formatFrames(count));
            }
        }

        if (stallStartMs != 0 && lastBeat > stallStartMs) {
            qWarning() << "GuiStallWatchdog: GUI thread was blocked for"
                       << (lastBeat - stallStartMs - kHeartbeatMs) << "ms";
            stallStartMs = 0;
            continue;
        }
        if (now - lastBeat < kStallMs + kHeartbeatMs) {
            continue;
        }
        if (stallStartMs == 0) {
            stallStartMs = lastBeat;
            nextSampleMs = now;
            samples = 0;
            lastKernelStack.clear();
        }
        if (now < nextSampleMs) {
            continue;
        }
        ++samples;
        nextSampleMs = now +
                (samples < kDenseSamples ? kSampleEveryMs : kSparseSampleEveryMs);

        const qint64 stalledMs = now - stallStartMs - kHeartbeatMs;
        qWarning().noquote() << "GuiStallWatchdog: GUI thread blocked for"
                             << stalledMs << "ms," << describeThreadState();
        const QStringList kernel = kernelStack();
        if (!kernel.isEmpty() && kernel != lastKernelStack) {
            logStack("GUI thread kernel stack", stalledMs, kernel);
            lastKernelStack = kernel;
        }

        // One user-space stack per stall, at its first sample: the signal
        // cuts short whatever sleep the GUI thread is in (see the
        // constructor), so asking again every second would shorten the very
        // stall being watched. The samples after it are read from /proc only.
        if (samples > 1 || stackPending) {
            continue;
        }
        s_frameCount.store(-1, std::memory_order_relaxed);
        stackRequestedMs = now;
        if (pthread_kill(s_mainThread, stackSignal()) != 0) {
            continue;
        }
        stackPending = true;
        for (int waited = 0; waited < kStackWaitMs; waited += 5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            const int count = s_frameCount.load(std::memory_order_acquire);
            if (count >= 0) {
                stackPending = false;
                logStack("GUI thread stack", stalledMs, formatFrames(count));
                break;
            }
        }
        if (stackPending) {
            qWarning() << "GuiStallWatchdog: GUI thread did not answer the stack "
                          "request (uninterruptible sleep?); logging it when it does";
        }
    }
#endif
}

} // namespace mixxx
