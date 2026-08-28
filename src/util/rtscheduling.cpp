#include "util/rtscheduling.h"

#include <QtDebug>

#ifdef __LINUX__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#endif

namespace mixxx {

#ifdef __LINUX__
namespace {
// Weakest SCHED_OTHER weighting: the fallback when SCHED_IDLE is unavailable,
// and the actual mechanism behind demoteCurrentThreadToBackground().
constexpr int kNiceLowest = 19;

// ioprio_set(2) has no glibc wrapper. IOPRIO_WHO_PROCESS with who == 0 is the
// calling *thread*, exactly as it is for setpriority(PRIO_PROCESS, 0, ...);
// class 2 is best-effort and 7 its weakest level.
constexpr int kIoprioWhoProcess = 1;
constexpr int kIoprioClassShift = 13;
constexpr int kIoprioClassBestEffort = 2;
constexpr int kIoprioBestEffortLowest = 7;

void demoteCurrentThreadIoPriority(const char* threadName) {
    constexpr int ioprio =
            (kIoprioClassBestEffort << kIoprioClassShift) | kIoprioBestEffortLowest;
    if (syscall(SYS_ioprio_set, kIoprioWhoProcess, 0, ioprio) != 0) {
        // Not fatal: the I/O scheduler in use may simply not have priorities.
        qWarning() << "Failed to lower I/O priority of thread" << threadName
                   << ":" << strerror(errno);
    }
}
} // namespace
#endif

bool promoteCurrentThreadToRealtime(int priority, const char* threadName) {
#ifdef __LINUX__
    sched_param param{};
    param.sched_priority = priority;
    const int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (err == 0) {
        qInfo() << "Thread" << threadName
                << "scheduled with SCHED_FIFO priority" << priority;
        return true;
    }
    qWarning() << "Failed to set SCHED_FIFO priority" << priority
               << "for thread" << threadName << ":" << strerror(err)
               << "- check rtprio rlimit / CAP_SYS_NICE";
    return false;
#else
    Q_UNUSED(priority);
    Q_UNUSED(threadName);
    return false;
#endif
}

bool demoteCurrentThreadToIdle(const char* threadName) {
#if defined(__LINUX__) && defined(SCHED_IDLE)
    sched_param param{};
    // SCHED_IDLE has no priority levels; the class itself is the demotion.
    param.sched_priority = 0;
    const int err = pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
    if (err == 0) {
        qInfo() << "Thread" << threadName << "scheduled SCHED_IDLE";
        return true;
    }
    qWarning() << "Failed to set SCHED_IDLE for thread" << threadName << ":"
               << strerror(err) << "- falling back to nice 19";
    // PRIO_PROCESS with who == 0 is the calling *thread* on Linux, not the
    // process: nice values are per-thread there.
    if (setpriority(PRIO_PROCESS, 0, kNiceLowest) == 0) {
        qInfo() << "Thread" << threadName << "reniced to" << kNiceLowest;
        return true;
    }
    qWarning() << "Failed to renice thread" << threadName << ":" << strerror(errno);
    return false;
#else
    Q_UNUSED(threadName);
    return false;
#endif
}

bool demoteCurrentThreadToBackground(const char* threadName) {
#ifdef __LINUX__
    bool batched = false;
#ifdef SCHED_BATCH
    sched_param param{};
    // SCHED_BATCH has no priority levels of its own; nice is the weighting.
    param.sched_priority = 0;
    const int err = pthread_setschedparam(pthread_self(), SCHED_BATCH, &param);
    if (err == 0) {
        batched = true;
    } else {
        qWarning() << "Failed to set SCHED_BATCH for thread" << threadName << ":"
                   << strerror(err) << "- staying on SCHED_OTHER";
    }
#endif
    // The nice value is what actually keeps this thread out of the audio
    // path's way, and it survives the policy change either way, so it is set
    // second and regardless of whether SCHED_BATCH took. PRIO_PROCESS with
    // who == 0 is the calling thread on Linux; nice values are per-thread.
    const bool reniced = setpriority(PRIO_PROCESS, 0, kNiceLowest) == 0;
    if (!reniced) {
        qWarning() << "Failed to renice thread" << threadName << "to"
                   << kNiceLowest << ":" << strerror(errno);
    }
    if (!batched && !reniced) {
        return false;
    }
    qInfo() << "Thread" << threadName << "scheduled as background work"
            << (batched ? "(SCHED_BATCH," : "(SCHED_OTHER,") << "nice"
            << (reniced ? kNiceLowest : getpriority(PRIO_PROCESS, 0)) << ")";
    demoteCurrentThreadIoPriority(threadName);
    return true;
#else
    Q_UNUSED(threadName);
    return false;
#endif
}

bool pinCurrentThreadToCpuFromEnv(const char* envVar, const char* threadName) {
#ifdef __LINUX__
    // Plain getenv/strtol, no QByteArray: this runs inside the first audio
    // callback and must not allocate.
    const char* value = std::getenv(envVar);
    if (value == nullptr || *value == '\0') {
        return false;
    }
    char* end = nullptr;
    const long cpu = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || cpu < 0 || cpu >= CPU_SETSIZE) {
        qWarning() << "Ignoring invalid CPU number in" << envVar << ":" << value;
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(cpu), &set);
    const int err = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (err == 0) {
        qInfo() << "Thread" << threadName << "pinned to CPU" << cpu
                << "(" << envVar << ")";
        return true;
    }
    qWarning() << "Failed to pin thread" << threadName << "to CPU" << cpu
               << ":" << strerror(err);
    return false;
#else
    Q_UNUSED(envVar);
    Q_UNUSED(threadName);
    return false;
#endif
}

bool lockAllMemory() {
#ifdef __LINUX__
    if (mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) == 0) {
        qInfo() << "Locked process memory (mlockall, on-fault)";
        return true;
    }
    qWarning() << "mlockall failed:" << strerror(errno)
               << "- check memlock rlimit; audio threads may major-fault";
    return false;
#else
    return false;
#endif
}

} // namespace mixxx
