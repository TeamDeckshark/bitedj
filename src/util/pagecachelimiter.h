#pragma once

#include <QtGlobal>

namespace mixxx {

/// Keeps the kernel page cache a file being appended to leaves behind bounded.
///
/// Written to naively, a long recording ends up in the page cache in its
/// entirety, dirty for as long as the device it is going to takes to catch up.
/// On a 1 GiB appliance recording onto a USB stick that is a problem twice
/// over: the cached pages are memory nothing else can have, and once the
/// system-wide dirty limit is reached, *any* thread that dirties a page — the
/// GUI, a library scan, an analyzer — is throttled in balance_dirty_pages()
/// until the slowest device in the box has written its backlog out. The
/// recording thread is not the only one that pays for a slow stick.
///
/// Feeding every write through onWritten() caps what the recording holds at
/// two windows: the one just completed is handed to the kernel to write back,
/// and the one before it — long since on the device — is dropped from the
/// cache. Both calls are asynchronous by design: SYNC_FILE_RANGE_WRITE only
/// queues the writeback, and POSIX_FADV_DONTNEED drops the clean pages and
/// skips the rest rather than waiting on them, so neither blocks on the device
/// even when it has stalled.
///
/// Everything here is best effort. A kernel or filesystem that refuses either
/// call just leaves the cache growing the way it did before, which is why no
/// error is reported: there is nothing a caller could usefully do about it.
///
/// Linux-only; every entry point compiles to a no-op elsewhere.
class PageCacheLimiter {
  public:
    /// How much is written between two rounds of writeback, and so also the
    /// granularity of the cache footprint: at most two of these are resident.
    static constexpr qint64 kWindowBytes = 1 << 20; // 1 MiB

    /// A byte range of the file, [offset, offset + length).
    struct Range {
        qint64 offset = 0;
        qint64 length = 0;

        bool isEmpty() const {
            return length <= 0;
        }
    };

    /// The ranges one advance() acts on: `writeBack` has just been filled and
    /// is queued for the device, `drop` was queued a round earlier and is
    /// released from the cache. Either can be empty.
    struct Windows {
        Range writeBack;
        Range drop;

        bool isEmpty() const {
            return writeBack.isEmpty() && drop.isEmpty();
        }
    };

    /// Forgets the file written so far. Call when a new file is opened — a
    /// recording that splits reuses the limiter for the next part.
    void reset();

    /// Rolls the windows on for a file whose content now ends at `endOffset`,
    /// and applies the result to `fd`. Cheap to call after every write: all it
    /// does until a window has filled is one comparison.
    void onWritten(int fd, qint64 endOffset);

    /// The window bookkeeping behind onWritten(), without the syscalls.
    Windows advance(qint64 endOffset);

    /// Issues the two calls `windows` describes on `fd`.
    static void apply(int fd, const Windows& windows);

    /// Releases the whole file from the cache. For the end of a recording,
    /// where the last window would otherwise stay resident with nothing left
    /// to push it out.
    static void dropAll(int fd);

  private:
    // Everything below this offset has been handed to the kernel for
    // writeback; everything below m_droppedTo has been released as well.
    qint64 m_writtenBackTo = 0;
    qint64 m_droppedTo = 0;
};

} // namespace mixxx
