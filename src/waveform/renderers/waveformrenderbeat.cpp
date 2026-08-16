#include "waveform/renderers/waveformrenderbeat.h"

#include <QPainter>
#include <algorithm>

#include "track/beats.h"
#include "track/track.h"
#include "util/painterscope.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "widget/wskincolor.h"

class QPaintEvent;

namespace {
constexpr int kBeatsPerBar = 4;

// Minimum distance in pixels between two drawn beat lines. When zooming out,
// the grid is decimated by powers of two to keep at least this much space
// between lines, so the lines never outnumber the waveform they annotate.
constexpr double kMinBeatLineSpacingPx = 12.0;

// Safety net against degenerate (near zero) beat spacings.
constexpr int kMaxBeatLineStride = 1024;

int positiveModulo(int value, int modulus) {
    return ((value % modulus) + modulus) % modulus;
}

/// Number of beats between two drawn beat lines for the given on-screen beat
/// spacing. Always a power of two, so that the lines surviving the decimation
/// are the bar (and multi-bar) lines.
int beatLineStride(double pixelsPerBeat) {
    int stride = 1;
    while (stride < kMaxBeatLineStride &&
            pixelsPerBeat * stride < kMinBeatLineSpacingPx) {
        stride *= 2;
    }
    return stride;
}

/// Opacity of the lines halfway between the ones the current stride keeps,
/// i.e. the lines the next decimation step is about to drop. They fade out as
/// the spacing shrinks towards the decimation threshold, so that zooming thins
/// the grid gradually instead of making every other line pop away.
float droppedBeatLineOpacity(double pixelsPerBeat, int stride) {
    return static_cast<float>(std::clamp(
            pixelsPerBeat * stride / kMinBeatLineSpacingPx - 1.0, 0.0, 1.0));
}
} // namespace

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidgetRenderer)
        : WaveformRendererAbstract(waveformWidgetRenderer) {
    m_beats.resize(128);
    m_fadingBeats.resize(128);
    m_downbeats.resize(128);
    m_fadingDownbeats.resize(128);
}

WaveformRenderBeat::~WaveformRenderBeat() {
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& context) {
    m_beatColor = QColor(context.selectString(node, "BeatColor"));
    m_beatColor = WSkinColor::getCorrectColor(m_beatColor).toRgb();
    QColor downbeatColor(context.selectString(node, "BeatDownbeatColor"));
    if (downbeatColor.isValid()) {
        m_downbeatColor = WSkinColor::getCorrectColor(downbeatColor).toRgb();
    } else {
        m_downbeatColor = QColor();
    }
}

void WaveformRenderBeat::draw(QPainter* painter, QPaintEvent* /*event*/) {
    TrackPointer pTrackInfo = m_waveformRenderer->getTrackInfo();

    if (!pTrackInfo) {
        return;
    }

    mixxx::BeatsPointer trackBeats = pTrackInfo->getBeats();
    if (!trackBeats) {
        return;
    }

    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return;
    }

    const bool paintDownbeats = m_downbeatColor.isValid();

#ifdef MIXXX_USE_QOPENGL
    // Using alpha transparency with drawLines causes a graphical issue when
    // drawing with QPainter on the QOpenGLWindow: instead of individual lines
    // a large rectangle encompassing all beatlines is drawn.
    m_beatColor.setAlphaF(1.f);
    if (paintDownbeats) {
        m_downbeatColor.setAlphaF(1.f);
    }
#else
    m_beatColor.setAlphaF(alpha/100.0);
    if (paintDownbeats) {
        m_downbeatColor.setAlphaF(alpha/100.0);
    }
#endif

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0) {
        return;
    }

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition();
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition();

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);
    auto it = trackBeats->iteratorFrom(startPosition);

    // if no beat do not waste time saving/restoring painter
    if (it == trackBeats->cend() || *it > endPosition) {
        return;
    }

    // Measure the on-screen beat spacing over the visible range, which the
    // decimation below is based on.
    int numBeatsInRange = 0;
    double firstBeatPosition = 0.0;
    double lastBeatPosition = 0.0;
    for (auto countIt = it;
            countIt != trackBeats->cend() && *countIt <= endPosition;
            ++countIt) {
        if (numBeatsInRange == 0) {
            firstBeatPosition = countIt->toEngineSamplePos();
        }
        lastBeatPosition = countIt->toEngineSamplePos();
        numBeatsInRange++;
    }

    // Only draw every `stride`th beat line, so that zooming out does not turn
    // the beat grid into a solid wall of lines covering the waveform.
    int stride = 1;
    float droppedOpacity = 0.f;
    if (numBeatsInRange > 1) {
        const double pixelsPerBeat =
                (m_waveformRenderer->transformSamplePositionInRendererWorld(
                         lastBeatPosition) -
                        m_waveformRenderer->transformSamplePositionInRendererWorld(
                                firstBeatPosition)) /
                (numBeatsInRange - 1);
        stride = beatLineStride(pixelsPerBeat);
        droppedOpacity = droppedBeatLineOpacity(pixelsPerBeat, stride);
    }
#ifdef MIXXX_USE_QOPENGL
    // As above, alpha transparency can't be used here, so the lines being faded
    // out are simply dropped once they are more transparent than opaque.
    droppedOpacity = droppedOpacity < 0.5f ? 0.f : 1.f;
#endif
    // The lines halfway between the kept ones, drawn while fading out.
    const int fadingOffset = stride / 2;

    // Anchor the bar count on the Beats object's downbeat reference for the
    // visible range. `downbeatAnchorAt` returns the most-recent detected drop
    // anchor at-or-before `startPosition`, so as the user scrubs past each
    // drop the bar count re-anchors. With no anchors, falls back to
    // `lastMarkerPosition` (the analyzer's natural beat-1). Iterator
    // subtraction keeps the count consistent across tempo markers. The bar
    // count is also what the decimation phase is keyed on, so that the lines
    // that survive zooming out stay put instead of shifting while scrolling.
    const auto anchor = trackBeats->iteratorFrom(
            trackBeats->downbeatAnchorAt(startPosition));
    int beatIndex = it - anchor;

    PainterScope PainterScope(painter);

    painter->setRenderHint(QPainter::Antialiasing);

    const Qt::Orientation orientation = m_waveformRenderer->getOrientation();
    const float rendererWidth = m_waveformRenderer->getWidth();
    const float rendererHeight = m_waveformRenderer->getHeight();

    int beatCount = 0;
    int fadingBeatCount = 0;
    int downbeatCount = 0;
    int fadingDownbeatCount = 0;

    for (; it != trackBeats->cend() && *it <= endPosition; ++it, ++beatIndex) {
        const int strideOffset = positiveModulo(beatIndex, stride);
        if (strideOffset != 0 && strideOffset != fadingOffset) {
            // Decimated away at this zoom level.
            continue;
        }
        const bool isFading = strideOffset != 0;
        if (isFading && droppedOpacity <= 0.f) {
            continue;
        }

        double beatPosition = it->toEngineSamplePos();
        double xBeatPoint =
                m_waveformRenderer->transformSamplePositionInRendererWorld(beatPosition);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const bool isDownbeat = paintDownbeats &&
                positiveModulo(beatIndex, kBeatsPerBar) == 0;

        QVector<QLineF>& lines = isDownbeat
                ? (isFading ? m_fadingDownbeats : m_downbeats)
                : (isFading ? m_fadingBeats : m_beats);
        int& count = isDownbeat
                ? (isFading ? fadingDownbeatCount : downbeatCount)
                : (isFading ? fadingBeatCount : beatCount);

        // If we don't have enough space, double the size.
        if (count >= lines.size()) {
            lines.resize(lines.size() * 2);
        }

        if (orientation == Qt::Horizontal) {
            lines[count++].setLine(xBeatPoint, 0.0f, xBeatPoint, rendererHeight);
        } else {
            lines[count++].setLine(0.0f, xBeatPoint, rendererWidth, xBeatPoint);
        }
    }

    const auto drawBeatLines = [&](const QVector<QLineF>& lines,
                                       int count,
                                       QColor color,
                                       float opacity) {
        if (count == 0) {
            return;
        }
        color.setAlphaF(color.alphaF() * opacity);
        QPen pen(color);
        pen.setWidthF(std::max(1.0, scaleFactor()));
        painter->setPen(pen);
        // Make sure to use constData to prevent detaches!
        painter->drawLines(lines.constData(), count);
    };

    drawBeatLines(m_beats, beatCount, m_beatColor, 1.f);
    drawBeatLines(m_fadingBeats, fadingBeatCount, m_beatColor, droppedOpacity);

    if (paintDownbeats) {
        drawBeatLines(m_downbeats, downbeatCount, m_downbeatColor, 1.f);
        drawBeatLines(m_fadingDownbeats,
                fadingDownbeatCount,
                m_downbeatColor,
                droppedOpacity);
    }
}
