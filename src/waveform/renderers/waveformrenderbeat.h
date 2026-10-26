#pragma once

#include <QColor>
#include <QPainterPath>

#include "skin/legacy/skincontext.h"
#include "util/class.h"
#include "waveform/renderers/waveformrendererabstract.h"

class WaveformRenderBeat : public WaveformRendererAbstract {
  public:
    explicit WaveformRenderBeat(WaveformWidgetRenderer* waveformWidgetRenderer);
    virtual ~WaveformRenderBeat();

    virtual void setup(const QDomNode& node, const SkinContext& context);
    virtual void draw(QPainter* painter, QPaintEvent* event);

  private:
    void addDownbeatArrows(QPainterPath& arrows,
            float position,
            Qt::Orientation orientation,
            float breadth) const;

    QColor m_beatColor;
    QColor m_downbeatColor;
    QVector<QLineF> m_beats;
    QVector<QLineF> m_fadingBeats;
    QVector<QLineF> m_downbeats;
    QVector<QLineF> m_fadingDownbeats;
    QPainterPath m_downbeatArrows;
    QPainterPath m_fadingDownbeatArrows;

    DISALLOW_COPY_AND_ASSIGN(WaveformRenderBeat);
};
