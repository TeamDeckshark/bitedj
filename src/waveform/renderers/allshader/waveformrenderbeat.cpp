#include "waveform/renderers/allshader/waveformrenderbeat.h"

#include <QDomNode>
#include <algorithm>

#include "skin/legacy/skincontext.h"
#include "track/beats.h"
#include "track/track.h"
#include "waveform/renderers/allshader/matrixforwidgetgeometry.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "widget/wskincolor.h"

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

namespace allshader {

WaveformRenderBeat::WaveformRenderBeat(WaveformWidgetRenderer* waveformWidget,
        ::WaveformRendererAbstract::PositionSource type)
        : WaveformRenderer(waveformWidget),
          m_isSlipRenderer(type == ::WaveformRendererAbstract::Slip) {
}

void WaveformRenderBeat::initializeGL() {
    WaveformRenderer::initializeGL();
    m_shader.init();
}

void WaveformRenderBeat::setup(const QDomNode& node, const SkinContext& context) {
    m_color = QColor(context.selectString(node, "BeatColor"));
    m_color = WSkinColor::getCorrectColor(m_color).toRgb();
    QColor downbeatColor(context.selectString(node, "BeatDownbeatColor"));
    if (downbeatColor.isValid()) {
        m_downbeatColor = WSkinColor::getCorrectColor(downbeatColor).toRgb();
    } else {
        m_downbeatColor = QColor();
    }
}

void WaveformRenderBeat::paintGL() {
    TrackPointer trackInfo = m_waveformRenderer->getTrackInfo();

    if (!trackInfo || (m_isSlipRenderer && !m_waveformRenderer->isSlipActive())) {
        return;
    }

    auto positionType = m_isSlipRenderer ? ::WaveformRendererAbstract::Slip
                                         : ::WaveformRendererAbstract::Play;

    mixxx::BeatsPointer trackBeats = trackInfo->getBeats();
    if (!trackBeats) {
        return;
    }

    int alpha = m_waveformRenderer->getBeatGridAlpha();
    if (alpha == 0) {
        return;
    }

    const bool paintDownbeats = m_downbeatColor.isValid();

    const float devicePixelRatio = m_waveformRenderer->getDevicePixelRatio();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_color.setAlphaF(alpha / 100.0f);
    if (paintDownbeats) {
        m_downbeatColor.setAlphaF(alpha / 100.0f);
    }

    const double trackSamples = m_waveformRenderer->getTrackSamples();
    if (trackSamples <= 0) {
        return;
    }

    const double firstDisplayedPosition =
            m_waveformRenderer->getFirstDisplayedPosition(positionType);
    const double lastDisplayedPosition =
            m_waveformRenderer->getLastDisplayedPosition(positionType);

    const auto startPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            firstDisplayedPosition * trackSamples);
    const auto endPosition = mixxx::audio::FramePos::fromEngineSamplePos(
            lastDisplayedPosition * trackSamples);

    if (!startPosition.isValid() || !endPosition.isValid()) {
        return;
    }

    const float rendererBreadth = m_waveformRenderer->getBreadth();

    const int numVerticesPerLine = 6; // 2 triangles

    // Count the number of beats in the range to reserve space in the m_vertices vector.
    // Note that we could also use
    //   int numBearsInRange = trackBeats->numBeatsInRange(startPosition, endPosition);
    // for this, but there have been reports of that method failing with a DEBUG_ASSERT.
    // The positions of the outermost visible beats are kept as well, to derive
    // the on-screen beat spacing the decimation below is based on.
    int numBeatsInRange = 0;
    double firstBeatPosition = 0.0;
    double lastBeatPosition = 0.0;
    for (auto it = trackBeats->iteratorFrom(startPosition);
            it != trackBeats->cend() && *it <= endPosition;
            ++it) {
        if (numBeatsInRange == 0) {
            firstBeatPosition = it->toEngineSamplePos();
        }
        lastBeatPosition = it->toEngineSamplePos();
        numBeatsInRange++;
    }

    // Only draw every `stride`th beat line, so that zooming out does not turn
    // the beat grid into a solid wall of lines covering the waveform.
    int stride = 1;
    float droppedOpacity = 0.f;
    if (numBeatsInRange > 1) {
        const double pixelsPerBeat =
                (m_waveformRenderer->transformSamplePositionInRendererWorld(
                         lastBeatPosition, positionType) -
                        m_waveformRenderer->transformSamplePositionInRendererWorld(
                                firstBeatPosition, positionType)) /
                (numBeatsInRange - 1);
        stride = beatLineStride(pixelsPerBeat);
        droppedOpacity = droppedBeatLineOpacity(pixelsPerBeat, stride);
    }
    // The lines halfway between the kept ones, drawn while fading out.
    const int fadingOffset = stride / 2;

    const int reserved = numBeatsInRange * numVerticesPerLine;
    m_vertices.clear();
    m_vertices.reserve(reserved);
    m_fadingVertices.clear();
    m_downbeatVertices.clear();
    m_fadingDownbeatVertices.clear();
    if (paintDownbeats) {
        m_downbeatVertices.reserve(reserved);
    }

    auto it = trackBeats->iteratorFrom(startPosition);

    // Anchor the bar count on the Beats object's downbeat reference for the
    // visible range. `downbeatAnchorAt` returns the most-recent detected drop
    // anchor at-or-before `startPosition`, so as the user scrubs past each
    // drop the bar count re-anchors. With no anchors, falls back to
    // `lastMarkerPosition` (the analyzer's natural beat-1). Iterator
    // subtraction keeps the count consistent across tempo markers.
    // The bar count is also what the decimation phase is keyed on, so that the
    // lines that survive zooming out stay put instead of shifting while
    // scrolling.
    const auto anchor = trackBeats->iteratorFrom(
            trackBeats->downbeatAnchorAt(startPosition));
    int beatIndex = it - anchor;

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
                m_waveformRenderer->transformSamplePositionInRendererWorld(
                        beatPosition, positionType);

        xBeatPoint = qRound(xBeatPoint * devicePixelRatio) / devicePixelRatio;

        const float x1 = static_cast<float>(xBeatPoint);
        const float x2 = x1 + 1.f;
        const float y2 = m_isSlipRenderer ? rendererBreadth / 2 : rendererBreadth;

        const bool isDownbeat = paintDownbeats &&
                positiveModulo(beatIndex, kBeatsPerBar) == 0;

        VertexData& target = isDownbeat
                ? (isFading ? m_fadingDownbeatVertices : m_downbeatVertices)
                : (isFading ? m_fadingVertices : m_vertices);
        target.addRectangle(x1, 0.f, x2, y2);
    }

    DEBUG_ASSERT(reserved >=
            m_vertices.size() + m_fadingVertices.size() +
                    m_downbeatVertices.size() + m_fadingDownbeatVertices.size());

    const int positionLocation = m_shader.positionLocation();
    const int matrixLocation = m_shader.matrixLocation();
    const int colorLocation = m_shader.colorLocation();

    m_shader.bind();
    m_shader.enableAttributeArray(positionLocation);

    const QMatrix4x4 matrix = matrixForWidgetGeometry(m_waveformRenderer, false);
    m_shader.setUniformValue(matrixLocation, matrix);

    const auto drawBeatLines = [&](const VertexData& vertices, QColor color, float opacity) {
        if (vertices.size() == 0) {
            return;
        }
        color.setAlphaF(color.alphaF() * opacity);
        m_shader.setAttributeArray(
                positionLocation, GL_FLOAT, vertices.constData(), 2);
        m_shader.setUniformValue(colorLocation, color);
        glDrawArrays(GL_TRIANGLES, 0, vertices.size());
    };

    drawBeatLines(m_vertices, m_color, 1.f);
    drawBeatLines(m_fadingVertices, m_color, droppedOpacity);

    if (paintDownbeats) {
        drawBeatLines(m_downbeatVertices, m_downbeatColor, 1.f);
        drawBeatLines(m_fadingDownbeatVertices, m_downbeatColor, droppedOpacity);
    }

    m_shader.disableAttributeArray(positionLocation);
    m_shader.release();
}

} // namespace allshader
