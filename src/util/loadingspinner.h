#pragma once

class QColor;
class QPainter;
class QRectF;

/// Bite DJ: the spinner shown while something is still being read off a USB
/// drive — in the sidebar in place of the item's icon, and in the library
/// breadcrumb after the name of what is opening. One turn is kFrameCount
/// frames, one frame per kFrameMillis.
namespace mixxx {
namespace loadingspinner {

constexpr int kFrameCount = 12;
constexpr int kFrameMillis = 80;

/// Draws frame `frame` of the spinner (a 270° arc, turning clockwise) into
/// `rect`.
void paint(QPainter* pPainter, const QRectF& rect, int frame, const QColor& color);

} // namespace loadingspinner
} // namespace mixxx
