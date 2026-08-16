// Tests for the Bite DJ USB drive list: a press and drag scrolls the rows so
// more drives than fit the panel can be reached, while a press that doesn't
// move still arms the Eject button underneath it. The per-row Record button is
// covered here for the state it paints and for being untappable while another
// drive is recording; what a tap then does to the recording itself belongs to
// SystemSettings, which owns it and is not up in this harness.
//
// Everything goes through the mouse events WWidget::event() synthesizes from
// touches (delivered to the list itself, never to its children), which is the
// only way the rows are reachable on the appliance's touchscreen.
#include "widget/wusblist.h"

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

class WUsbListTest : public MixxxTest {
  protected:
    void SetUp() override {
        // Every WWidget holds a proxy on this one.
        m_pTouchShift = std::make_unique<ControlPushButton>(
                ConfigKey("[Controls]", "touch_shift"));
        m_pList = std::make_unique<WUsbList>();

        m_pList->resize(kListWidth, kListHeight);
        m_pList->show();
        QCoreApplication::processEvents();

        QStringList labels;
        for (int i = 0; i < kRowCount; ++i) {
            labels.append(QStringLiteral("DRIVE%1").arg(i));
        }
        // The rows normally arrive from SystemSettings, which enumerates real
        // mount points. The singleton isn't up here, so drive the slot it is
        // connected to directly.
        QMetaObject::invokeMethod(m_pList.get(),
                "onRowsChanged",
                Qt::DirectConnection,
                Q_ARG(QStringList, labels));
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

    void tap(const QPoint& globalPos) {
        press(globalPos);
        release(globalPos);
    }

    void dragBy(const QPoint& globalPos, int dy) {
        press(globalPos);
        moveTo(globalPos + QPoint(0, dy / 2));
        moveTo(globalPos + QPoint(0, dy));
        release(globalPos + QPoint(0, dy));
    }

    QScrollArea* scrollArea() const {
        return m_pList->findChild<QScrollArea*>(QStringLiteral("UsbScrollArea"));
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

    QList<QPushButton*> ejectButtons() const {
        return m_pList->findChildren<QPushButton*>(
                QStringLiteral("UsbEjectButton"));
    }

    // Centre of an Eject button in global coordinates, wherever the row
    // currently sits.
    QPoint ejectButtonCenter(int index) const {
        QPushButton* pButton = ejectButtons().at(index);
        return pButton->mapToGlobal(pButton->rect().center());
    }

    bool isArmed(int index) const {
        return ejectButtons().at(index)->property("armed").toBool();
    }

    int armedCount() const {
        int count = 0;
        for (QPushButton* pButton : ejectButtons()) {
            if (pButton->property("armed").toBool()) {
                count++;
            }
        }
        return count;
    }

    QList<QPushButton*> recordButtons() const {
        return m_pList->findChildren<QPushButton*>(
                QStringLiteral("UsbRecordButton"));
    }

    QPoint recordButtonCenter(int index) const {
        QPushButton* pButton = recordButtons().at(index);
        return pButton->mapToGlobal(pButton->rect().center());
    }

    // Paints the list as SystemSettings would when a recording starts on `index`
    // (-1 for none) — the signal the real one emits carries just that.
    void setRecordingRow(int index) {
        QMetaObject::invokeMethod(m_pList.get(),
                "onRecordingChanged",
                Qt::DirectConnection,
                Q_ARG(int, index));
        layOut();
    }

    bool isRecording(int index) const {
        return recordButtons().at(index)->property("recording").toBool();
    }

    int enabledRecordButtonCount() const {
        int count = 0;
        for (QPushButton* pButton : recordButtons()) {
            if (pButton->isEnabled()) {
                count++;
            }
        }
        return count;
    }

    std::unique_ptr<ControlPushButton> m_pTouchShift;
    std::unique_ptr<WUsbList> m_pList;
};

TEST_F(WUsbListTest, MoreRowsThanFitAreScrollable) {
    // The rows must not stretch the list past the height the skin gave it;
    // the ones that don't fit are reached by scrolling instead.
    EXPECT_EQ(kListHeight, m_pList->height());
    EXPECT_GT(scrollBar()->maximum(), 0);
    EXPECT_TRUE(scrollBar()->isVisible());
}

TEST_F(WUsbListTest, FewRowsNeedNoScrollBar) {
    QMetaObject::invokeMethod(m_pList.get(),
            "onRowsChanged",
            Qt::DirectConnection,
            Q_ARG(QStringList, QStringList{QStringLiteral("DRIVE0")}));
    layOut();

    EXPECT_EQ(0, scrollBar()->maximum());
    EXPECT_FALSE(scrollBar()->isVisible());
}

TEST_F(WUsbListTest, DragUpScrollsDown) {
    dragBy(ejectButtonCenter(0), -kDragDistance);

    // The content sticks to the finger, so the list scrolls by the whole
    // distance including the part travelled before the gesture was known to
    // be a scroll.
    EXPECT_EQ(kDragDistance, scrollPosition());
}

TEST_F(WUsbListTest, DragDownScrollsUp) {
    scrollBar()->setValue(scrollBar()->maximum());
    const int start = scrollPosition();

    dragBy(ejectButtonCenter(kRowCount - 1), kDragDistance);

    EXPECT_EQ(start - kDragDistance, scrollPosition());
}

TEST_F(WUsbListTest, DragOverEjectButtonDoesNotArmIt) {
    dragBy(ejectButtonCenter(0), -kDragDistance);

    EXPECT_EQ(0, armedCount());
}

TEST_F(WUsbListTest, TapArmsEjectButton) {
    tap(ejectButtonCenter(0));

    EXPECT_TRUE(isArmed(0));
    EXPECT_EQ(1, armedCount());
}

TEST_F(WUsbListTest, TapBesideEjectButtonArmsNothing) {
    // The drive name label takes the rest of the row and is not actionable.
    tap(m_pList->mapToGlobal(QPoint(4, 4)));

    EXPECT_EQ(0, armedCount());
}

TEST_F(WUsbListTest, TapArmsTheRowScrolledUnderTheFinger) {
    scrollBar()->setValue(scrollBar()->maximum());
    layOut();

    // Whichever row the last one is now next to: tapping its button must arm
    // that row, not the one that used to be at those coordinates.
    const int lastRow = kRowCount - 1;
    tap(ejectButtonCenter(lastRow));

    EXPECT_TRUE(isArmed(lastRow));
    EXPECT_EQ(1, armedCount());
}

TEST_F(WUsbListTest, TapOnRowScrolledOutOfSightArmsNothing) {
    const QPoint firstRowCenter = ejectButtonCenter(0);
    scrollBar()->setValue(scrollBar()->maximum());
    layOut();

    // The first row has been scrolled above the viewport, so a tap at the
    // coordinates its button was pushed to must not reach it.
    ASSERT_LT(ejectButtonCenter(0).y(), m_pList->mapToGlobal(QPoint(0, 0)).y());
    tap(firstRowCenter);

    EXPECT_EQ(0, armedCount());
}

TEST_F(WUsbListTest, EveryRowHasARecordButton) {
    ASSERT_EQ(kRowCount, recordButtons().size());
    EXPECT_EQ(kRowCount, enabledRecordButtonCount());
    for (QPushButton* pButton : recordButtons()) {
        EXPECT_FALSE(pButton->property("recording").toBool());
    }
}

TEST_F(WUsbListTest, RecordingRowIsTheOnlyOneLeftTappable) {
    setRecordingRow(1);

    // Only one drive can be recorded to, so every other row's button is out of
    // action for the duration — the recording row's own is how it is stopped.
    EXPECT_TRUE(isRecording(1));
    EXPECT_EQ(1, enabledRecordButtonCount());
    EXPECT_TRUE(recordButtons().at(1)->isEnabled());
    EXPECT_FALSE(recordButtons().at(0)->isEnabled());
    EXPECT_EQ(QStringLiteral("Stop Recording"), recordButtons().at(1)->text());
    EXPECT_EQ(QStringLiteral("Record"), recordButtons().at(0)->text());
}

TEST_F(WUsbListTest, EveryRowIsTappableAgainOnceNothingRecords) {
    setRecordingRow(1);
    setRecordingRow(-1);

    EXPECT_FALSE(isRecording(1));
    EXPECT_EQ(kRowCount, enabledRecordButtonCount());
    EXPECT_EQ(QStringLiteral("Record"), recordButtons().at(1)->text());
}

TEST_F(WUsbListTest, TapOnRecordButtonCancelsAnArmedEject) {
    tap(ejectButtonCenter(0));
    ASSERT_TRUE(isArmed(0));

    // The finger moved on to a different control, so the pending eject
    // confirmation is stale and must not be left one tap from ejecting.
    tap(recordButtonCenter(0));

    EXPECT_EQ(0, armedCount());
}

TEST_F(WUsbListTest, TapOnDisabledRecordButtonDoesNothing) {
    setRecordingRow(1);
    tap(ejectButtonCenter(0));
    ASSERT_TRUE(isArmed(0));

    // Row 0's Record button is disabled while row 1 records. The list
    // hit-tests the synthesized touch itself, so nothing but that hit-test
    // stops the tap — if it landed, it would have cancelled the armed eject.
    tap(recordButtonCenter(0));

    EXPECT_EQ(1, armedCount());
}

TEST_F(WUsbListTest, DragOverRecordButtonIsAScrollNotATap) {
    tap(ejectButtonCenter(0));
    ASSERT_TRUE(isArmed(0));

    dragBy(recordButtonCenter(0), -kDragDistance);

    // Scrolled, and the gesture was not delivered to the button it started on
    // (which would have cancelled the arm).
    EXPECT_EQ(kDragDistance, scrollPosition());
    EXPECT_TRUE(isArmed(0));
}

TEST_F(WUsbListTest, DraggingTheScrollBarScrolls) {
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
    EXPECT_EQ(0, armedCount());
}

} // anonymous namespace
