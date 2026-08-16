#include "util/rtscheduling.h"

#include <QtDebug>

#ifdef __LINUX__
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#endif

namespace mixxx {

#if defined(__LINUX__) && defined(SCHED_IDLE)
namespace {
// Weakest SCHED_OTHER weighting, used only when SCHED_IDLE is unavailable.
constexpr int kNiceLowest = 19;
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
