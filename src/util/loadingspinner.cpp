#include "util/loadingspinner.h"

#include <QColor>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <algorithm>

namespace mixxx {
namespace loadingspinner {

void paint(QPainter* pPainter, const QRectF& rect, int frame, const QColor& color) {
    const qreal side = std::min(rect.width(), rect.height());
    const qreal penWidth = std::max<qreal>(1.5, side / 7);
    // Square, centred in `rect`, and inset by half the pen so the round caps
    // stay inside it.
    const QRectF arc(rect.center().x() - side / 2 + penWidth / 2,
            rect.center().y() - side / 2 + penWidth / 2,
            side - penWidth,
            side - penWidth);
    pPainter->save();
    pPainter->setRenderHint(QPainter::Antialiasing);
    pPainter->setPen(QPen(color, penWidth, Qt::SolidLine, Qt::RoundCap));
    pPainter->setBrush(Qt::NoBrush);
    // Qt angles are in 1/16 degree, counter-clockwise: negate the step so it
    // turns clockwise.
    const int startAngle = -(frame % kFrameCount) * (360 / kFrameCount) * 16;
    pPainter->drawArc(arc, startAngle, 270 * 16);
    pPainter->restore();
}

} // namespace loadingspinner
} // namespace mixxx
