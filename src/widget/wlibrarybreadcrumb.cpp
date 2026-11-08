#include "widget/wlibrarybreadcrumb.h"

#include <QFontMetrics>
#include <QPainter>
#include <algorithm>

#include "moc_wlibrarybreadcrumb.cpp"
#include "util/loadingspinner.h"

WLibraryBreadcrumb::WLibraryBreadcrumb(QWidget* pParent)
        : WLabel(pParent),
          m_frame(0),
          m_loading(false) {
    m_animationTimer.setInterval(mixxx::loadingspinner::kFrameMillis);
    connect(&m_animationTimer, &QTimer::timeout, this, [this] {
        m_frame = (m_frame + 1) % mixxx::loadingspinner::kFrameCount;
        update();
    });
}

void WLibraryBreadcrumb::setLoading(bool loading) {
    if (loading == m_loading) {
        return;
    }
    m_loading = loading;
    if (m_loading) {
        m_frame = 0;
        m_animationTimer.start();
    } else {
        m_animationTimer.stop();
    }
    update();
}

void WLibraryBreadcrumb::paintEvent(QPaintEvent* pEvent) {
    WLabel::paintEvent(pEvent);
    if (!m_loading) {
        return;
    }
    // Right after the name, as tall as its capitals, in the colour it is
    // drawn in. A name too long for that leaves the spinner at the right edge.
    const QRect contents = contentsRect();
    const QFontMetrics metrics(font());
    const int side = std::max(8, static_cast<int>(metrics.height() * 0.8));
    const int gap = side / 2;
    // QLabel's own rule for a negative indent: half an 'x' when it has a frame.
    const int textIndent = indent() >= 0
            ? indent()
            : (frameWidth() > 0 ? metrics.horizontalAdvance(QLatin1Char('x')) / 2 : 0);
    const int textEnd = contents.left() + margin() + textIndent +
            metrics.horizontalAdvance(QLabel::text());
    const int left = std::min(textEnd + gap, contents.right() - side);
    const QRectF rect(left, contents.center().y() - side / 2.0, side, side);
    QPainter painter(this);
    mixxx::loadingspinner::paint(
            &painter, rect, m_frame, palette().color(foregroundRole()));
}
