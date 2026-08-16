#include "util/pagecachelimiter.h"

#ifdef __LINUX__
#include <fcntl.h>
#endif

namespace mixxx {

void PageCacheLimiter::reset() {
    m_writtenBackTo = 0;
    m_droppedTo = 0;
}

PageCacheLimiter::Windows PageCacheLimiter::advance(qint64 endOffset) {
    Windows windows;
    // Also the guard against a file that has been seeked backwards (the wave
    // encoder patches its header before closing): nothing to roll on until the
    // content is a whole window past what we last queued.
    if (endOffset - m_writtenBackTo < kWindowBytes) {
        return windows;
    }
    windows.writeBack = Range{m_writtenBackTo, endOffset - m_writtenBackTo};
    // Dropped a round late, so the pages have had a full window's worth of
    // writing to reach the device. Empty on the first roll, when there is no
    // earlier window yet.
    windows.drop = Range{m_droppedTo, m_writtenBackTo - m_droppedTo};
    m_droppedTo = m_writtenBackTo;
    m_writtenBackTo = endOffset;
    return windows;
}

void PageCacheLimiter::onWritten(int fd, qint64 endOffset) {
    if (fd < 0) {
        return;
    }
    apply(fd, advance(endOffset));
}

void PageCacheLimiter::apply(int fd, const Windows& windows) {
#ifdef __LINUX__
    if (fd < 0) {
        return;
    }
    if (!windows.writeBack.isEmpty()) {
        // WRITE alone: queue the range and return. The WAIT_BEFORE/WAIT_AFTER
        // flags would park this thread on the device instead.
        static_cast<void>(sync_file_range(fd,
                static_cast<off_t>(windows.writeBack.offset),
                static_cast<off_t>(windows.writeBack.length),
                SYNC_FILE_RANGE_WRITE));
    }
    if (!windows.drop.isEmpty()) {
        static_cast<void>(posix_fadvise(fd,
                static_cast<off_t>(windows.drop.offset),
                static_cast<off_t>(windows.drop.length),
                POSIX_FADV_DONTNEED));
    }
#else
    Q_UNUSED(fd);
    Q_UNUSED(windows);
#endif
}

void PageCacheLimiter::dropAll(int fd) {
#ifdef __LINUX__
    if (fd < 0) {
        return;
    }
    // A zero length means "to the end of the file".
    static_cast<void>(posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED));
#else
    Q_UNUSED(fd);
#endif
}

} // namespace mixxx
