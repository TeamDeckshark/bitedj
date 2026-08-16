// Tests for the Bite DJ touch scrolling of list views: a press and drag moves
// the content instead of dragging items out of the view, while a press that
// doesn't move still reaches the view as an ordinary click.
#include "widget/touchscrollfilter.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QHeaderView>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTableView>

namespace {

constexpr int kRowCount = 200;
constexpr int kRowHeight = 20;
constexpr int kViewHeight = 100;

// Well above the drag start distance
constexpr int kDragDistance = 60;

void sendMouse(QWidget* pWidget,
        QEvent::Type type,
        const QPointF& pos,
        Qt::MouseButton button,
        Qt::MouseButtons buttons) {
    QMouseEvent event(type,
            pos,
            pos,
            pWidget->mapToGlobal(pos),
            button,
            buttons,
            Qt::NoModifier,
            QPointingDevice::primaryPointingDevice());
    QCoreApplication::sendEvent(pWidget, &event);
}

class TouchScrollFilterTest : public testing::Test {
  protected:
    void SetUp() override {
        m_model.setRowCount(kRowCount);
        m_model.setColumnCount(1);
        for (int row = 0; row < kRowCount; row++) {
            m_model.setItem(row, 0, new QStandardItem(QString::number(row)));
        }

        m_view.setModel(&m_model);
        m_view.setSelectionBehavior(QAbstractItemView::SelectRows);
        m_view.verticalHeader()->setDefaultSectionSize(kRowHeight);
        m_view.verticalHeader()->hide();
        m_view.horizontalHeader()->hide();
        m_view.resize(200, kViewHeight);
        m_view.show();
        QCoreApplication::processEvents();

        TouchScrollFilter::install(&m_view);
    }

    void press(int y) {
        sendMouse(m_view.viewport(),
                QEvent::MouseButtonPress,
                QPointF(10, y),
                Qt::LeftButton,
                Qt::LeftButton);
    }

    void moveTo(int y) {
        sendMouse(m_view.viewport(),
                QEvent::MouseMove,
                QPointF(10, y),
                Qt::NoButton,
                Qt::LeftButton);
    }

    void release(int y) {
        sendMouse(m_view.viewport(),
                QEvent::MouseButtonRelease,
                QPointF(10, y),
                Qt::LeftButton,
                Qt::NoButton);
    }

    int scrollPosition() const {
        return m_view.verticalScrollBar()->value();
    }

    QModelIndexList selectedRows() const {
        return m_view.selectionModel()->selectedRows();
    }

    QStandardItemModel m_model;
    QTableView m_view;
};

TEST_F(TouchScrollFilterTest, ItemViewsScrollPerPixel) {
    // Dragging the content by pixels needs a view that scrolls by pixels.
    EXPECT_EQ(QAbstractItemView::ScrollPerPixel, m_view.verticalScrollMode());
}

TEST_F(TouchScrollFilterTest, DragUpScrollsDown) {
    press(80);
    moveTo(80 - kDragDistance / 2);
    moveTo(80 - kDragDistance);
    release(80 - kDragDistance);

    // The content sticks to the finger, so the view scrolls by the whole
    // distance including the part travelled before the gesture was known to
    // be a scroll.
    EXPECT_EQ(kDragDistance, scrollPosition());
}

TEST_F(TouchScrollFilterTest, DragDownScrollsUp) {
    m_view.verticalScrollBar()->setValue(500);

    press(20);
    moveTo(20 + kDragDistance / 2);
    moveTo(20 + kDragDistance);
    release(20 + kDragDistance);

    EXPECT_EQ(500 - kDragDistance, scrollPosition());
}

TEST_F(TouchScrollFilterTest, DragDoesNotSelect) {
    press(80);
    moveTo(80 - kDragDistance);
    release(80 - kDragDistance);

    EXPECT_TRUE(selectedRows().isEmpty());
}

TEST_F(TouchScrollFilterTest, TapSelectsRowUnderFinger) {
    const int row = 3;
    press(row * kRowHeight + kRowHeight / 2);
    release(row * kRowHeight + kRowHeight / 2);

    ASSERT_EQ(1, selectedRows().size());
    EXPECT_EQ(row, selectedRows().first().row());
    EXPECT_EQ(0, scrollPosition());
}

TEST_F(TouchScrollFilterTest, JitterBelowDragDistanceStaysATap) {
    const int row = 3;
    const int y = row * kRowHeight + kRowHeight / 2;
    press(y);
    moveTo(y - 2);
    moveTo(y + 1);
    release(y + 1);

    ASSERT_EQ(1, selectedRows().size());
    EXPECT_EQ(row, selectedRows().first().row());
    EXPECT_EQ(0, scrollPosition());
}

TEST_F(TouchScrollFilterTest, SlowDragAccumulatesSubPixelSteps) {
    press(80);
    // Cross the drag start distance, then creep upwards in half pixels.
    moveTo(80 - kDragDistance);
    const int scrolled = scrollPosition();
    for (int i = 1; i <= 10; i++) {
        sendMouse(m_view.viewport(),
                QEvent::MouseMove,
                QPointF(10, 80 - kDragDistance - i * 0.5),
                Qt::NoButton,
                Qt::LeftButton);
    }
    release(80 - kDragDistance - 5);

    EXPECT_EQ(scrolled + 5, scrollPosition());
}

TEST_F(TouchScrollFilterTest, TapAfterScrollStillSelects) {
    press(80);
    moveTo(80 - kDragDistance);
    release(80 - kDragDistance);
    ASSERT_TRUE(selectedRows().isEmpty());

    const int y = 30;
    press(y);
    release(y);

    ASSERT_EQ(1, selectedRows().size());
    EXPECT_EQ((scrollPosition() + y) / kRowHeight, selectedRows().first().row());
}

TEST_F(TouchScrollFilterTest, RightPressIsNotHeldBack) {
    // Only the left button scrolls, the right one has to reach the view so
    // that the context menu keeps working.
    const int y = 30;
    sendMouse(m_view.viewport(),
            QEvent::MouseButtonPress,
            QPointF(10, y),
            Qt::RightButton,
            Qt::RightButton);
    sendMouse(m_view.viewport(),
            QEvent::MouseMove,
            QPointF(10, y - kDragDistance),
            Qt::NoButton,
            Qt::RightButton);

    EXPECT_EQ(0, scrollPosition());
}

} // anonymous namespace
