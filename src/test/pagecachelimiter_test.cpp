// Tests the window bookkeeping that keeps a recording's page cache footprint
// flat: what the limiter hands to the kernel for writeback and what it releases
// afterwards. The syscalls those ranges turn into are not observable from a
// test — whether a page is resident is the kernel's business — so what is
// covered here is the arithmetic that bounds them.
#include "util/pagecachelimiter.h"

#include <gtest/gtest.h>

namespace {

using mixxx::PageCacheLimiter;

constexpr qint64 kWindow = PageCacheLimiter::kWindowBytes;

class PageCacheLimiterTest : public ::testing::Test {
  protected:
    PageCacheLimiter m_limiter;
};

TEST_F(PageCacheLimiterTest, nothingUntilAWindowHasFilled) {
    EXPECT_TRUE(m_limiter.advance(0).isEmpty());
    EXPECT_TRUE(m_limiter.advance(4096).isEmpty());
    EXPECT_TRUE(m_limiter.advance(kWindow - 1).isEmpty());
}

TEST_F(PageCacheLimiterTest, firstWindowIsWrittenBackButNothingIsDroppedYet) {
    const PageCacheLimiter::Windows windows = m_limiter.advance(kWindow);
    EXPECT_EQ(0, windows.writeBack.offset);
    EXPECT_EQ(kWindow, windows.writeBack.length);
    // Nothing has been queued long enough to be worth dropping.
    EXPECT_TRUE(windows.drop.isEmpty());
}

TEST_F(PageCacheLimiterTest, eachWindowIsDroppedOneRoundAfterItIsWrittenBack) {
    m_limiter.advance(kWindow);

    PageCacheLimiter::Windows windows = m_limiter.advance(2 * kWindow);
    EXPECT_EQ(kWindow, windows.writeBack.offset);
    EXPECT_EQ(kWindow, windows.writeBack.length);
    EXPECT_EQ(0, windows.drop.offset);
    EXPECT_EQ(kWindow, windows.drop.length);

    windows = m_limiter.advance(3 * kWindow);
    EXPECT_EQ(2 * kWindow, windows.writeBack.offset);
    EXPECT_EQ(kWindow, windows.writeBack.length);
    EXPECT_EQ(kWindow, windows.drop.offset);
    EXPECT_EQ(kWindow, windows.drop.length);
}

TEST_F(PageCacheLimiterTest, windowsCoverTheFileWithoutGapsOrOverlap) {
    // Writes do not land on window boundaries — an encoder emits whatever size
    // frame it has — so a roll carries the overshoot with it rather than
    // leaving it behind for the next one.
    qint64 offset = 0;
    qint64 writtenBackTo = 0;
    qint64 droppedTo = 0;
    for (int i = 0; i < 100; ++i) {
        offset += kWindow / 3 + 17;
        const PageCacheLimiter::Windows windows = m_limiter.advance(offset);
        if (windows.isEmpty()) {
            continue;
        }
        EXPECT_EQ(writtenBackTo, windows.writeBack.offset);
        writtenBackTo = windows.writeBack.offset + windows.writeBack.length;
        if (!windows.drop.isEmpty()) {
            EXPECT_EQ(droppedTo, windows.drop.offset);
            droppedTo = windows.drop.offset + windows.drop.length;
        }
        // The two windows are what stays resident: never more than what has
        // been written back but not yet dropped, plus the current partial one.
        EXPECT_LE(writtenBackTo - droppedTo, 2 * kWindow);
    }
    // Everything but the tail below a full window has been handed over.
    EXPECT_LT(offset - writtenBackTo, kWindow);
}

TEST_F(PageCacheLimiterTest, aBackwardsSeekRollsNothing) {
    m_limiter.advance(4 * kWindow);
    // The wave encoder patches its header before closing, which puts the
    // stream position back at the start of the file.
    EXPECT_TRUE(m_limiter.advance(0).isEmpty());
    EXPECT_TRUE(m_limiter.advance(44).isEmpty());
    // ... and the rolling picks up where it left off once past the tail again.
    const PageCacheLimiter::Windows windows = m_limiter.advance(5 * kWindow);
    EXPECT_EQ(4 * kWindow, windows.writeBack.offset);
    EXPECT_EQ(kWindow, windows.writeBack.length);
}

TEST_F(PageCacheLimiterTest, resetStartsTheNextFileFromScratch) {
    m_limiter.advance(10 * kWindow);
    m_limiter.advance(20 * kWindow);
    // A recording that splits opens a new file and reuses the limiter.
    m_limiter.reset();

    EXPECT_TRUE(m_limiter.advance(kWindow - 1).isEmpty());
    const PageCacheLimiter::Windows windows = m_limiter.advance(kWindow);
    EXPECT_EQ(0, windows.writeBack.offset);
    EXPECT_EQ(kWindow, windows.writeBack.length);
    EXPECT_TRUE(windows.drop.isEmpty());
}

TEST_F(PageCacheLimiterTest, aClosedFileIsNotTouched) {
    // openFile() failed, or the file was closed under us: the fd is -1 and the
    // syscalls must not be attempted with it.
    m_limiter.onWritten(-1, 4 * kWindow);
    PageCacheLimiter::apply(-1, PageCacheLimiter::Windows{});
    PageCacheLimiter::dropAll(-1);
    // The window state is untouched, so the first real write still starts at 0.
    const PageCacheLimiter::Windows windows = m_limiter.advance(kWindow);
    EXPECT_EQ(0, windows.writeBack.offset);
}

} // namespace
