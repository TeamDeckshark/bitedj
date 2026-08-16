#pragma once

#include <QColor>

#include "widget/wwidget.h"

class ControlProxy;
class QDomNode;
class SkinContext;

// Bite DJ: paints a single-arrow overlay indicating how out-of-sync the
// most-recently-moved soft-takeover-armed control is. Reads the
// [SoftTakeover],offset_left / offset_right pair populated by
// SoftTakeoverIndicator.
class WSoftTakeoverIndicator : public WWidget {
    Q_OBJECT
  public:
    explicit WSoftTakeoverIndicator(QWidget* parent = nullptr);
    ~WSoftTakeoverIndicator() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private slots:
    void onOffsetChanged();

  private:
    ControlProxy* m_pOffsetLeft;
    ControlProxy* m_pOffsetRight;

    QColor m_color{0xff, 0xa5, 0x00}; // amber default
};
