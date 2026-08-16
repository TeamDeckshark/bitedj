// Tests for the Bite DJ controller list: a press and drag scrolls the rows so
// more controllers than fit the panel can be reached, and the scroll bar can be
// grabbed directly.
//
// Everything goes through the mouse events WWidget::event() synthesizes from
// touches (delivered to the list itself, never to its children), which is the
// only way the rows are reachable on the appliance's touchscreen.
//
// Row taps are not covered here: they dispatch straight into the
// ControllerSettings singleton, which needs a live ControllerManager. What is
// covered is the half of the gesture handling that owns the scrolling.
#include "widget/wcontrollerlist.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLayout>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionSlider>
#include <memory>

#include "control/controlpushbutton.h"
#include "test/mixxxtest.h"

namespace {

constexpr int kRowCount = 40;
constexpr int kListWidth = 400;
constexpr int kListHeight = 120;

// Well above the drag start distance
constexpr int kDragDistance = 60;

class WControllerListTest : public MixxxTest {
  protected:
    void SetUp() override {
        // Every WWidget holds a proxy on this one.
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));
        m_pList = std::make_unique<WControllerList>();

        m_pList->resize(kListWidth, kListHeight);
        m_pList->show();
        QCoreApplication::processEvents();

        setRows(kRowCount);
    }

    // The rows normally arrive from ControllerSettings, which enumerates the
    // connected MIDI devices. The singleton isn't up here, so drive the slot it
    // is connected to directly.
    void setRows(int count) {
        QStringList labels;
        QList<bool> active;
        for (int i = 0; i < count; ++i) {
            labels.append(QStringLiteral("CONTROLLER%1 — DISABLED").arg(i));
            active.append(false);
        }
        QMetaObject::invokeMethod(m_pList.get(),
                "onRowsChanged",
                Qt::DirectConnection,
                Q_ARG(QStringList, labels),
                Q_ARG(QList<bool>, active));
        layOut();
    }

    void layOut() {
        QCoreApplication::sendPostedEvents();
        m_pList->layout()->activate();
        QCoreApplication::processEvents();
    }

    // Mimics WWidget::event()'s touch translation: the event is delivered to
    // the list itself and carries no held buttons.
    void sendTouchAsMouse(QEvent::Type type, const QPoint& globalPos) {
        const QPointF pos = m_pList->mapFromGlobal(globalPos);
        QMouseEvent event(type,
                pos,
                pos,
                QPointF(globalPos),
                Qt::LeftButton,
                Qt::NoButton,
                Qt::NoModifier,
                QPointingDevice::primaryPointingDevice());
        QCoreApplication::sendEvent(m_pList.get(), &event);
    }

    void press(const QPoint& globalPos) {
        sendTouchAsMouse(QEvent::MouseButtonPress, globalPos);
    }

    void moveTo(const QPoint& globalPos) {
        sendTouchAsMouse(QEvent::MouseMove, globalPos);
    }

    void release(const QPoint& globalPos) {
        sendTouchAsMouse(QEvent::MouseButtonRelease, globalPos);
    }

    void dragBy(const QPoint& globalPos, int dy) {
        press(globalPos);
        moveTo(globalPos + QPoint(0, dy / 2));
        moveTo(globalPos + QPoint(0, dy));
        release(globalPos + QPoint(0, dy));
    }

    QScrollArea* scrollArea() const {
        return m_pList->findChild<QScrollArea*>(
                QStringLiteral("ControllerScrollArea"));
    }

    QScrollBar* scrollBar() const {
        return scrollArea()->verticalScrollBar();
    }

    int scrollPosition() const {
        return scrollBar()->value();
    }

    // Where the scroll bar's handle currently sits, in its own coordinates.
    QRect handleRect() const {
        QScrollBar* pScrollBar = scrollBar();
        QStyleOptionSlider option;
        option.initFrom(pScrollBar);
        option.orientation = Qt::Vertical;
        option.minimum = pScrollBar->minimum();
        option.maximum = pScrollBar->maximum();
        option.sliderPosition = pScrollBar->sliderPosition();
        option.sliderValue = pScrollBar->value();
        option.singleStep = pScrollBar->singleStep();
        option.pageStep = pScrollBar->pageStep();
        option.subControls = QStyle::SC_All;
        return pScrollBar->style()->subControlRect(QStyle::CC_ScrollBar,
                &option,
                QStyle::SC_ScrollBarSlider,
                pScrollBar);
    }

    QList<QPushButton*> rows() const {
        return m_pList->findChildren<QPushButton*>(
                QStringLiteral("ControllerRow"));
    }

    // Centre of a row in global coordinates, wherever it currently sits.
    QPoint rowCenter(int index) const {
        QPushButton* pRow = rows().at(index);
        return pRow->mapToGlobal(pRow->rect().center());
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<WControllerList> m_pList;
};

TEST_F(WControllerListTest, MoreRowsThanFitAreScrollable) {
    // The rows must not stretch the list past the height the skin gave it;
    // the ones that don't fit are reached by scrolling instead.
    EXPECT_EQ(kListHeight, m_pList->height());
    EXPECT_GT(scrollBar()->maximum(), 0);
    EXPECT_TRUE(scrollBar()->isVisible());
}

TEST_F(WControllerListTest, FewRowsNeedNoScrollBar) {
    setRows(1);

    EXPECT_EQ(0, scrollBar()->maximum());
    EXPECT_FALSE(scrollBar()->isVisible());
}

TEST_F(WControllerListTest, RowsStayTopAligned) {
    setRows(1);

    // A single row keeps its own height at the top of the panel rather than
    // being stretched or centred down it.
    ASSERT_EQ(1, rows().size());
    QPushButton* pRow = rows().at(0);
    EXPECT_EQ(0, pRow->mapTo(m_pList.get(), QPoint(0, 0)).y());
    EXPECT_LT(pRow->height(), kListHeight);
}

TEST_F(WControllerListTest, DragUpScrollsDown) {
    dragBy(rowCenter(0), -kDragDistance);

    // The content sticks to the finger, so the list scrolls by the whole
    // distance including the part travelled before the gesture was known to
    // be a scroll.
    EXPECT_EQ(kDragDistance, scrollPosition());
}

TEST_F(WControllerListTest, DragDownScrollsUp) {
    scrollBar()->setValue(scrollBar()->maximum());
    const int start = scrollPosition();

    dragBy(rowCenter(kRowCount - 1), kDragDistance);

    EXPECT_EQ(start - kDragDistance, scrollPosition());
}

TEST_F(WControllerListTest, DraggingTheScrollBarScrolls) {
    QScrollBar* pScrollBar = scrollBar();
    // The scroll bar is a plain child widget, so it never sees a touch either;
    // grabbing its handle has to work through the list's own handlers.
    const QPoint handlePos = pScrollBar->mapToGlobal(handleRect().center());

    press(handlePos);
    moveTo(handlePos + QPoint(0, 20));
    release(handlePos + QPoint(0, 20));

    // Dragging the bar down scrolls the list down, and much further than the
    // 20 px of travel, since the bar is a fraction of the content's height.
    EXPECT_GT(scrollPosition(), 20);
}

} // anonymous namespace
