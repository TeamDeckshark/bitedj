#pragma once

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <Qt>
#include <algorithm>
#include <cmath>

/// Appearance and formatting of the "time remaining" watermark WOverview draws
/// on top of the track summary. Kept apart from the widget because it is pure
/// layout and painting with no widget state of its own, and woverview.cpp is
/// already long.
namespace TimeRemainingOverlay {

/// The digits are a watermark sitting behind the signal, not a label, so they
/// are translucent enough to read the waveform through.
constexpr float kDefaultOpacity = 0.55f;
/// Share of the waveform height the digits themselves fill.
constexpr float kDefaultScale = 0.8f;
/// ...but never wider than this share of the waveform, so a long "-12:34" on a
/// narrow view still has waveform visible either side of it.
constexpr float kMaxLengthProportion = 0.9f;
/// Distance between the text box and the edges of the waveform. Only reached
/// when the overlay is aligned to an edge rather than centred.
constexpr float kMargin = 4.f;
constexpr float kCornerRadius = 3.f;
constexpr float kMinPointSize = 7.f;
/// No filled backdrop by default: at this size a chip would blank out most of
/// the waveform. The dark outline below carries legibility instead. A skin can
/// still ask for one with <TimeRemainingBgColor>.
constexpr int kDefaultBackgroundAlpha = 0;
/// Outline thickness as a share of the font size, and its darkness.
constexpr double kOutlineWidthRatio = 0.09;
constexpr double kMinOutlineWidth = 2.0;
constexpr int kOutlineAlpha = 200;

/// Formats the remaining seconds as "-M:SS". Only whole seconds are shown: at
/// this size hundredths are unreadable on a dance floor, and the coarser text
/// also means the overlay is only re-rasterized once a second.
inline QString remainingTimeToString(double remainingSeconds) {
    // Round up so the display only reaches 0:00 when the track is actually
    // done, instead of sitting on a stale 0:00 while still audible.
    const int totalSeconds = static_cast<int>(std::ceil(remainingSeconds));
    const int minutes = totalSeconds / 60;
    const int seconds = totalSeconds - minutes * 60;

    return QString::asprintf("-%d:%02d", minutes, seconds);
}

/// Parses a skin <TimeRemainingAlign> value such as "bottom|left". Missing
/// horizontal or vertical flags fall back to the matching half of
/// defaultFlags.
inline Qt::Alignment decodeAlign(const QString& alignString, Qt::Alignment defaultFlags) {
    if (alignString.isEmpty()) {
        return defaultFlags;
    }
    const QString flags = alignString.toLower();

    Qt::Alignment hflag;
    Qt::Alignment vflag;
    if (flags.contains(QLatin1String("left"))) {
        hflag = Qt::AlignLeft;
    } else if (flags.contains(QLatin1String("right"))) {
        hflag = Qt::AlignRight;
    } else if (flags.contains(QLatin1String("hcenter")) ||
            flags.contains(QLatin1String("center"))) {
        hflag = Qt::AlignHCenter;
    }
    if (flags.contains(QLatin1String("top"))) {
        vflag = Qt::AlignTop;
    } else if (flags.contains(QLatin1String("bottom"))) {
        vflag = Qt::AlignBottom;
    } else if (flags.contains(QLatin1String("vcenter")) ||
            flags.contains(QLatin1String("center"))) {
        vflag = Qt::AlignVCenter;
    }
    if (!hflag) {
        hflag = defaultFlags & Qt::AlignHorizontal_Mask;
    }
    if (!vflag) {
        vflag = defaultFlags & Qt::AlignVertical_Mask;
    }
    return hflag | vflag;
}

/// A font sized to the waveform, plus the ink rectangle of the digits in it.
/// The font size is derived from the waveform height rather than fixed in
/// points, so the digits keep filling the same share of a waveform whatever
/// the view is resized to.
struct TextLayout {
    bool valid{false};
    QFont font;
    /// Ink extents of the text relative to a baseline at the origin. Ink, not
    /// line height: line height carries ascent/descent room that no glyph in
    /// "-0:00" reaches, which would leave the digits visibly short of the
    /// height that was asked for.
    QRectF ink;
    qreal penWidth{0.0};
    /// Room around the ink for the outline stroke.
    qreal padding{0.0};

    qreal boxWidth() const {
        return ink.width() + padding * 2;
    }
    qreal boxHeight() const {
        return ink.height() + padding * 2;
    }
    /// Where to put the text baseline so the ink lands inside the box.
    QPointF baseline() const {
        return QPointF(padding - ink.left(), padding - ink.top());
    }
};

/// Sizes `text` so its digits are `scale` of `breadth` tall, shrinking further
/// if that would run past `kMaxLengthProportion` of `length`.
inline TextLayout layoutFor(const QString& text, qreal breadth, qreal length, qreal scale) {
    TextLayout layout;
    if (text.isEmpty() || breadth <= 0.0 || length <= 0.0) {
        return layout;
    }

    // Measure once at a large nominal size and scale the result: font metrics
    // are close enough to linear in point size that one pass lands the height
    // within a pixel, and it avoids searching for a fit every second.
    constexpr qreal kNominalPointSize = 100.0;
    layout.font.setFamily(QStringLiteral("Open Sans"));
    layout.font.setBold(true);
    layout.font.setPointSizeF(kNominalPointSize);

    QFontMetricsF metrics{layout.font};
    const QRectF nominalInk = metrics.tightBoundingRect(text);
    if (nominalInk.height() <= 0.0 || nominalInk.width() <= 0.0) {
        return layout;
    }

    qreal pointSize = kNominalPointSize * (breadth * scale) / nominalInk.height();
    const qreal maxWidth = length * kMaxLengthProportion;
    const qreal widthAtThatSize = nominalInk.width() * pointSize / kNominalPointSize;
    if (widthAtThatSize > maxWidth) {
        pointSize *= maxWidth / widthAtThatSize;
    }
    pointSize = std::max<qreal>(kMinPointSize, pointSize);

    layout.font.setPointSizeF(pointSize);
    metrics = QFontMetricsF{layout.font};
    layout.ink = metrics.tightBoundingRect(text);
    if (layout.ink.height() <= 0.0 || layout.ink.width() <= 0.0) {
        return layout;
    }
    layout.penWidth = std::max(kMinOutlineWidth, pointSize * kOutlineWidthRatio);
    layout.padding = layout.penWidth / 2.0 + 1.0;
    layout.valid = true;
    return layout;
}

/// Draws the overlay with the top-left of its box at the painter's origin.
/// The caller sets the painter's opacity and translation.
inline void paintOverlay(QPainter* pPainter,
        const TextLayout& layout,
        const QString& text,
        const QColor& color,
        const QColor& backgroundColor) {
    pPainter->setRenderHint(QPainter::Antialiasing);

    if (backgroundColor.alpha() != 0) {
        pPainter->setPen(Qt::NoPen);
        pPainter->setBrush(backgroundColor);
        pPainter->drawRoundedRect(QRectF(0.0, 0.0, layout.boxWidth(), layout.boxHeight()),
                kCornerRadius,
                kCornerRadius);
    }

    QPainterPath path;
    path.addText(layout.baseline(), layout.font, text);

    // Stroke a dark outline first, then fill: over a bright waveform the
    // outline is what keeps translucent digits from washing out, and stroking
    // underneath means it never eats into the glyph shapes.
    QPen pen(QColor(0, 0, 0, kOutlineAlpha), layout.penWidth);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    pPainter->setPen(pen);
    pPainter->setBrush(Qt::NoBrush);
    pPainter->drawPath(path);

    pPainter->setPen(Qt::NoPen);
    pPainter->setBrush(color);
    pPainter->drawPath(path);
}

/// Top-left corner of a boxWidth x boxHeight overlay within a waveform of
/// `length` x `breadth`, honouring the alignment flags.
inline void alignedPosition(Qt::Alignment align,
        float length,
        float breadth,
        float boxWidth,
        float boxHeight,
        float* pX,
        float* pY) {
    if (align & Qt::AlignRight) {
        *pX = length - boxWidth - kMargin;
    } else if (align & Qt::AlignHCenter) {
        *pX = (length - boxWidth) / 2.f;
    } else {
        *pX = kMargin;
    }
    if (align & Qt::AlignBottom) {
        *pY = breadth - boxHeight - kMargin;
    } else if (align & Qt::AlignVCenter) {
        *pY = (breadth - boxHeight) / 2.f;
    } else {
        *pY = kMargin;
    }
}

} // namespace TimeRemainingOverlay
