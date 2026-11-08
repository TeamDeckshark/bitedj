#include "soundio/enginecallbackprofiler.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtDebug>
#include <array>
#include <atomic>
#include <thread>

#include "util/rtscheduling.h"

#ifdef __LINUX__
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace mixxx {

namespace EngineCallbackProfiler {

#ifdef __LINUX__
namespace {

// A callback is recorded when it started this much later than one period
// after the previous one ...
constexpr double kLateGapPeriods = 1.25;
// ... or ran for more than this share of a period ...
constexpr double kLongRunPeriods = 0.5;
// ... or PortAudio flagged an underflow, or it took a major fault.

constexpr int kRingSize = 128; // records between two drains (5 s)

// Watcher (debug mode only): poll interval, how long a callback has to have
// been running before the engine thread is sampled, and the second, later
// sample of the same callback if it is still running.
constexpr int kWatchPollUs = 1000;
constexpr int64_t kStallUs = 3000;
constexpr int64_t kSecondSampleUs = 10000;
// One user-space stack per this many ms: the signal that takes it cuts into
// the very callback that is already late.
constexpr int64_t kUserStackEveryMs = 30000;
constexpr int kUserStackWaitMs = 20;
constexpr int kKernelStackLines = 16;
constexpr int kMaxFrames = 48;
// Above every BiteDJ thread on the housekeeping cores, below the engine
// (kRtPrioAudioEngine = 70): it has to run while those cores are saturated,
// which is exactly when callbacks are late.
constexpr int kRtPrioWatcher = 69;

struct Usage {
    long minflt = 0;
    long majflt = 0;
    long nvcsw = 0;
    long nivcsw = 0;
};

Usage threadUsage() {
    struct rusage ru{};
    getrusage(RUSAGE_THREAD, &ru);
    Usage u;
    u.minflt = ru.ru_minflt;
    u.majflt = ru.ru_majflt;
    u.nvcsw = ru.ru_nvcsw;
    u.nivcsw = ru.ru_nivcsw;
    return u;
}

Usage operator-(const Usage& a, const Usage& b) {
    Usage d;
    d.minflt = a.minflt - b.minflt;
    d.majflt = a.majflt - b.majflt;
    d.nvcsw = a.nvcsw - b.nvcsw;
    d.nivcsw = a.nivcsw - b.nivcsw;
    return d;
}

int64_t monoNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

int64_t realNs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

struct Record {
    int64_t wallNs;
    int32_t periodUs;
    int32_t gapUs;
    int32_t queuedUs; // -1 = unknown
    int32_t ranUs;
    int32_t engineUs;
    int32_t outputUs;
    int32_t writeUs;
    Usage during;
    Usage before;
    bool underflow;
};

// Single producer (the engine thread), single consumer (the main thread).
std::array<Record, kRingSize> s_ring;
std::atomic<uint32_t> s_head{0}; // written by the engine thread
std::atomic<uint32_t> s_tail{0}; // written by the main thread

// Per drain window, written by the engine thread, read and reset by the main
// thread. A reset racing an update can lose one callback's worth; these are
// for orientation, the records are the evidence.
std::atomic<uint32_t> s_callbacks{0};
std::atomic<uint32_t> s_slow{0};
std::atomic<uint32_t> s_dropped{0};
std::atomic<int32_t> s_maxRanUs{0};
std::atomic<int32_t> s_maxGapUs{0};
std::atomic<int32_t> s_periodUs{0};
std::atomic<uint32_t> s_minflt{0};
std::atomic<uint32_t> s_majflt{0};
std::atomic<uint32_t> s_nvcsw{0};
std::atomic<uint32_t> s_nivcsw{0};

// Engine-thread-only state.
int64_t t_beginNs = 0;
int64_t t_prevBeginNs = 0;
int64_t t_engineNs = 0;
int64_t t_outputNs = 0;
double t_periodSecs = 0;
double t_queuedSecs = 0;
bool t_underflow = false;
Usage t_beginUsage;
Usage t_prevEndUsage;
bool t_havePrevEnd = false;

// Shared with the watcher.
std::atomic<pid_t> s_engineTid{0};
std::atomic<bool> s_haveEngineThread{false};
pthread_t s_engineThread;
std::atomic<int64_t> s_runningSinceNs{0}; // 0 = not in a callback
std::atomic<uint64_t> s_callbackSeq{0};

std::atomic<bool> s_stop{false};
std::thread s_watcher;
bool s_signalInstalled = false;

void* s_frames[kMaxFrames];
std::atomic<int> s_frameCount{-1};

int stackSignal() {
    return SIGRTMIN + 4;
}

void handleStackSignal(int) {
    const int savedErrno = errno;
    const int count = backtrace(s_frames, kMaxFrames);
    s_frameCount.store(count, std::memory_order_release);
    errno = savedErrno;
}

void storeMax(std::atomic<int32_t>* pMax, int32_t value) {
    if (value > pMax->load(std::memory_order_relaxed)) {
        pMax->store(value, std::memory_order_relaxed);
    }
}

void addRelaxed(std::atomic<uint32_t>* pCount, long value) {
    if (value > 0) {
        pCount->fetch_add(static_cast<uint32_t>(value), std::memory_order_relaxed);
    }
}

QString wallClock(int64_t wallNs) {
    const time_t secs = static_cast<time_t>(wallNs / 1000000000);
    struct tm local{};
    localtime_r(&secs, &local);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &local);
    return QStringLiteral("%1.%2")
            .arg(QLatin1String(buf))
            .arg(static_cast<int>((wallNs / 1000000) % 1000), 3, 10, QChar('0'));
}

QString ms(int32_t us) {
    return QString::number(us / 1000.0, 'f', 1);
}

QString usage(const Usage& u) {
    return QStringLiteral("faults %1/%2, switches %3/%4")
            .arg(u.minflt)
            .arg(u.majflt)
            .arg(u.nvcsw)
            .arg(u.nivcsw);
}

QByteArray readProcFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return file.readAll().trimmed();
}

QString taskPath(pid_t tid, const char* entry) {
    return QStringLiteral("/proc/self/task/%1/%2").arg(tid).arg(QLatin1String(entry));
}

/// "state=S wchan=futex_wait_queue syscall=98"
QString describeThread(pid_t tid) {
    QString description;
    const QByteArray stat = readProcFile(taskPath(tid, "stat"));
    const int commEnd = stat.lastIndexOf(')');
    if (commEnd >= 0 && commEnd + 2 < stat.size()) {
        description += QStringLiteral("state=") + QChar::fromLatin1(stat.at(commEnd + 2));
    }
    const QByteArray wchan = readProcFile(taskPath(tid, "wchan"));
    if (!wchan.isEmpty() && wchan != "0") {
        description += QStringLiteral(" wchan=") + QString::fromLatin1(wchan);
    }
    const QList<QByteArray> syscallFields = readProcFile(taskPath(tid, "syscall")).split(' ');
    bool ok = false;
    const long nr = syscallFields.value(0).toLong(&ok);
    if (ok && nr >= 0) {
        QString name;
        switch (nr) {
        case SYS_futex:
            name = QStringLiteral("futex");
            break;
        case SYS_ioctl:
            name = QStringLiteral("ioctl");
            break;
        case SYS_ppoll:
            name = QStringLiteral("ppoll");
            break;
        case SYS_read:
            name = QStringLiteral("read");
            break;
        case SYS_write:
            name = QStringLiteral("write");
            break;
        case SYS_clock_nanosleep:
            name = QStringLiteral("clock_nanosleep");
            break;
        case SYS_mmap:
            name = QStringLiteral("mmap");
            break;
        case SYS_munmap:
            name = QStringLiteral("munmap");
            break;
        case SYS_madvise:
            name = QStringLiteral("madvise");
            break;
        default:
            name = QString::number(nr);
        }
        description += QStringLiteral(" syscall=") + name;
    } else if (syscallFields.value(0) == "running") {
        description += QStringLiteral(" running");
    } else if (ok) {
        // "-1 <sp> <pc>": not in a syscall (user space, or a page fault).
        description += QStringLiteral(" not-in-syscall");
    }
    return description;
}

QStringList kernelStack(pid_t tid) {
    QStringList lines;
    const QList<QByteArray> stack = readProcFile(taskPath(tid, "stack")).split('\n');
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
            lines << QStringLiteral("#%1 %2").arg(i).arg(reinterpret_cast<quintptr>(pc), 0, 16);
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

void sleepUs(int us) {
    struct timespec ts{0, static_cast<long>(us) * 1000};
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
}

void watch() {
    pthread_setname_np(pthread_self(), "EngineStallWatch");
    if (!setCurrentThreadRealtimeQuietly(true, kRtPrioWatcher)) {
        qWarning() << "EngineCallbackProfiler: stall watcher could not get "
                      "SCHED_FIFO"
                   << kRtPrioWatcher
                   << "and may miss stalls while the housekeeping cores are busy";
    }

    uint64_t sampledSeq = 0;
    int samplesOfSeq = 0;
    int64_t lastUserStackNs = 0;

    while (!s_stop.load(std::memory_order_relaxed)) {
        sleepUs(kWatchPollUs);
        const int64_t since = s_runningSinceNs.load(std::memory_order_acquire);
        if (since == 0) {
            continue;
        }
        const int64_t now = monoNs();
        const int64_t runningUs = (now - since) / 1000;
        const uint64_t seq = s_callbackSeq.load(std::memory_order_acquire);
        if (seq != sampledSeq) {
            sampledSeq = seq;
            samplesOfSeq = 0;
        }
        if (samplesOfSeq == 0 ? runningUs < kStallUs
                              : (samplesOfSeq == 1 ? runningUs < kSecondSampleUs : true)) {
            continue;
        }
        ++samplesOfSeq;
        const pid_t tid = s_engineTid.load(std::memory_order_relaxed);
        if (tid == 0) {
            continue;
        }

        // Read /proc first, while the thread is still where it is stuck.
        const QString state = describeThread(tid);
        const QStringList kernel = kernelStack(tid);
        const QString wall = wallClock(realNs());

        int frameCount = -1;
        bool askedForStack = false;
        if (samplesOfSeq == 1 && s_haveEngineThread.load(std::memory_order_acquire) &&
                (lastUserStackNs == 0 ||
                        now - lastUserStackNs >= kUserStackEveryMs * 1000000)) {
            lastUserStackNs = now;
            s_frameCount.store(-1, std::memory_order_relaxed);
            if (pthread_kill(s_engineThread, stackSignal()) == 0) {
                askedForStack = true;
                for (int waited = 0; waited < kUserStackWaitMs; ++waited) {
                    sleepUs(1000);
                    frameCount = s_frameCount.load(std::memory_order_acquire);
                    if (frameCount >= 0) {
                        break;
                    }
                }
            }
        }

        // Still the same callback? Tells a stall that ended right after the
        // sample from one that is still going.
        const bool sameCallback = s_callbackSeq.load(std::memory_order_acquire) == seq &&
                s_runningSinceNs.load(std::memory_order_acquire) == since;
        qWarning().noquote() << "EngineCallbackProfiler: at" << wall
                             << "a callback had been running for" << ms(static_cast<int32_t>(runningUs))
                             << "ms:" << state
                             << (sameCallback ? "" : "(it has since returned)");
        if (!kernel.isEmpty()) {
            qWarning().noquote() << "EngineCallbackProfiler: engine kernel stack:\n  " +
                            kernel.join(QStringLiteral("\n  "));
        }
        if (frameCount >= 0) {
            qWarning().noquote() << "EngineCallbackProfiler: engine user stack:\n  " +
                            formatFrames(frameCount).join(QStringLiteral("\n  "));
        } else if (askedForStack) {
            qWarning() << "EngineCallbackProfiler: the engine thread did not take "
                          "its stack within"
                       << kUserStackWaitMs << "ms (uninterruptible sleep?)";
        }
    }
}

} // anonymous namespace

void start(bool watchStalls) {
    if (!watchStalls || s_watcher.joinable()) {
        return;
    }
    struct sigaction action{};
    action.sa_handler = handleStackSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(stackSignal(), &action, nullptr) != 0) {
        qWarning() << "EngineCallbackProfiler: cannot install the stack signal handler";
    } else {
        s_signalInstalled = true;
    }
    // Load libgcc's unwinder here: the first backtrace() allocates.
    void* frame;
    backtrace(&frame, 1);
    s_stop.store(false);
    s_watcher = std::thread(watch);
    qWarning() << "EngineCallbackProfiler: watching engine callbacks for stalls over"
               << kStallUs / 1000 << "ms (debug mode)";
}

void stop() {
    s_stop.store(true);
    if (s_watcher.joinable()) {
        s_watcher.join();
    }
}

void callbackBegin(double periodSecs, double queuedSecs, bool underflowFlagged) {
    t_beginNs = monoNs();
    t_beginUsage = threadUsage();
    t_periodSecs = periodSecs;
    t_queuedSecs = queuedSecs;
    t_underflow = underflowFlagged;
    t_engineNs = 0;
    t_outputNs = 0;
    if (s_engineTid.load(std::memory_order_relaxed) == 0) {
        s_engineThread = pthread_self();
        s_haveEngineThread.store(true, std::memory_order_release);
        s_engineTid.store(static_cast<pid_t>(syscall(SYS_gettid)), std::memory_order_relaxed);
    }
    s_callbackSeq.fetch_add(1, std::memory_order_release);
    s_runningSinceNs.store(t_beginNs, std::memory_order_release);
}

void markEngineDone() {
    t_engineNs = monoNs();
}

void markOutputDone() {
    t_outputNs = monoNs();
}

void callbackEnd() {
    const int64_t endNs = monoNs();
    s_runningSinceNs.store(0, std::memory_order_release);
    const Usage endUsage = threadUsage();

    const Usage during = endUsage - t_beginUsage;
    Usage before;
    if (t_havePrevEnd) {
        before = t_beginUsage - t_prevEndUsage;
    }
    t_prevEndUsage = endUsage;
    t_havePrevEnd = true;

    const int32_t periodUs = static_cast<int32_t>(t_periodSecs * 1e6);
    const int32_t gapUs = t_prevBeginNs
            ? static_cast<int32_t>((t_beginNs - t_prevBeginNs) / 1000)
            : 0;
    t_prevBeginNs = t_beginNs;
    const int32_t ranUs = static_cast<int32_t>((endNs - t_beginNs) / 1000);

    s_callbacks.fetch_add(1, std::memory_order_relaxed);
    s_periodUs.store(periodUs, std::memory_order_relaxed);
    storeMax(&s_maxRanUs, ranUs);
    storeMax(&s_maxGapUs, gapUs);
    addRelaxed(&s_minflt, during.minflt);
    addRelaxed(&s_majflt, during.majflt);
    addRelaxed(&s_nvcsw, during.nvcsw);
    addRelaxed(&s_nivcsw, during.nivcsw);

    const bool late = gapUs > kLateGapPeriods * periodUs;
    const bool longRun = ranUs > kLongRunPeriods * periodUs;
    if (!(late || longRun || t_underflow || during.majflt > 0)) {
        return;
    }
    s_slow.fetch_add(1, std::memory_order_relaxed);

    const uint32_t head = s_head.load(std::memory_order_relaxed);
    if (head - s_tail.load(std::memory_order_acquire) >= kRingSize) {
        s_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Record& r = s_ring[head % kRingSize];
    r.wallNs = realNs();
    r.periodUs = periodUs;
    r.gapUs = gapUs;
    r.queuedUs = t_queuedSecs > 0 ? static_cast<int32_t>(t_queuedSecs * 1e6) : -1;
    r.ranUs = ranUs;
    const int64_t engineNs = t_engineNs ? t_engineNs : endNs;
    const int64_t outputNs = t_outputNs ? t_outputNs : engineNs;
    r.engineUs = static_cast<int32_t>((engineNs - t_beginNs) / 1000);
    r.outputUs = static_cast<int32_t>((outputNs - engineNs) / 1000);
    r.writeUs = static_cast<int32_t>((endNs - outputNs) / 1000);
    r.during = during;
    r.before = before;
    r.underflow = t_underflow;
    s_head.store(head + 1, std::memory_order_release);
}

void drainAndLog() {
    const uint32_t callbacks = s_callbacks.exchange(0, std::memory_order_relaxed);
    const uint32_t slow = s_slow.exchange(0, std::memory_order_relaxed);
    const uint32_t dropped = s_dropped.exchange(0, std::memory_order_relaxed);
    const int32_t maxRanUs = s_maxRanUs.exchange(0, std::memory_order_relaxed);
    const int32_t maxGapUs = s_maxGapUs.exchange(0, std::memory_order_relaxed);
    const uint32_t minflt = s_minflt.exchange(0, std::memory_order_relaxed);
    const uint32_t majflt = s_majflt.exchange(0, std::memory_order_relaxed);
    const uint32_t nvcsw = s_nvcsw.exchange(0, std::memory_order_relaxed);
    const uint32_t nivcsw = s_nivcsw.exchange(0, std::memory_order_relaxed);

    const uint32_t head = s_head.load(std::memory_order_acquire);
    uint32_t tail = s_tail.load(std::memory_order_relaxed);
    for (; tail != head; ++tail) {
        const Record& r = s_ring[tail % kRingSize];
        QString queued = r.queuedUs >= 0
                ? ms(r.queuedUs) + QStringLiteral(" ms queued")
                : QStringLiteral("queued unknown");
        qWarning().noquote()
                << "EngineCallbackProfiler: slow callback at" << wallClock(r.wallNs)
                << (r.underflow ? "(underflow flagged):" : ":")
                << "gap" << ms(r.gapUs) << "ms (period" << ms(r.periodUs) << ")," << queued
                << "at entry, ran" << ms(r.ranUs) << "ms (engine" << ms(r.engineUs)
                << ", output" << ms(r.outputUs) << ", write" << ms(r.writeUs) << ");"
                << "during:" << usage(r.during) << "; before:" << usage(r.before);
    }
    s_tail.store(tail, std::memory_order_release);

    if (slow == 0 && dropped == 0 && majflt == 0) {
        return;
    }
    qWarning().noquote() << "EngineCallbackProfiler:" << callbacks << "callbacks," << slow
                         << "slow" << (dropped ? QStringLiteral("(%1 not recorded)").arg(dropped) : QString())
                         << "; longest ran" << ms(maxRanUs) << "ms, widest gap" << ms(maxGapUs)
                         << "ms, period" << ms(s_periodUs.load(std::memory_order_relaxed))
                         << "ms; in callbacks: faults" << minflt << "/" << majflt << ", switches"
                         << nvcsw << "/" << nivcsw;
}

#else // __LINUX__

void start(bool) {
}
void stop() {
}
void callbackBegin(double, double, bool) {
}
void markEngineDone() {
}
void markOutputDone() {
}
void callbackEnd() {
}
void drainAndLog() {
}

#endif // __LINUX__

} // namespace EngineCallbackProfiler

} // namespace mixxx
