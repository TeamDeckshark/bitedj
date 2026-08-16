#include "widget/wsofttakeoverindicator.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>

#include "control/controlproxy.h"
#include "moc_wsofttakeoverindicator.cpp"
#include "skin/legacy/skincontext.h"

namespace {
constexpr const char* kGroup = "[SoftTakeover]";
}

WSoftTakeoverIndicator::WSoftTakeoverIndicator(QWidget* parent)
        : WWidget(parent),
          m_pOffsetLeft(new ControlProxy(kGroup, "offset_left", this)),
          m_pOffsetRight(new ControlProxy(kGroup, "offset_right", this)) {
    m_pOffsetLeft->connectValueChanged(this, &WSoftTakeoverIndicator::onOffsetChanged);
    m_pOffsetRight->connectValueChanged(this, &WSoftTakeoverIndicator::onOffsetChanged);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

WSoftTakeoverIndicator::~WSoftTakeoverIndicator() = default;

void WSoftTakeoverIndicator::setup(const QDomNode& node, const SkinContext& context) {
    QString colorStr;
    if (context.hasNodeSelectString(node, "Color", &colorStr)) {
        QColor parsed(colorStr);
        if (parsed.isValid()) {
            m_color = parsed;
        }
    }
}

void WSoftTakeoverIndicator::onOffsetChanged() {
    update();
}

void WSoftTakeoverIndicator::paintEvent(QPaintEvent* /*event*/) {
    const double left = m_pOffsetLeft->get();
    const double right = m_pOffsetRight->get();
    if (left <= 0.0 && right <= 0.0) {
        return;
    }
    const bool pointsLeft = left > right;
    const double magnitude = pointsLeft ? left : right;
    if (magnitude <= 0.0) {
        return;
    }

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect();
    const double centerY = r.center().y();
    const double half = r.height() / 2.0;
    // Reserve a slim minimum so even a near-tolerance offset is visible; let
    // it grow to the full half-width as the gap widens.
    const double tipReach = (r.width() / 2.0) * std::clamp(magnitude, 0.05, 1.0);
    const double centerX = r.center().x();

    QPolygonF arrow;
    if (pointsLeft) {
        arrow << QPointF(centerX - tipReach, centerY)
              << QPointF(centerX, centerY - half)
              << QPointF(centerX, centerY + half);
    } else {
        arrow << QPointF(centerX + tipReach, centerY)
              << QPointF(centerX, centerY - half)
              << QPointF(centerX, centerY + half);
    }

    p.setPen(Qt::NoPen);
    p.setBrush(m_color);
    p.drawPolygon(arrow);
}
