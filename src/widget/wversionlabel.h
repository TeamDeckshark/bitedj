#pragma once

#include "widget/wlabel.h"

class QDomNode;
class SkinContext;

class WVersionLabel : public WLabel {
    Q_OBJECT
  public:
    explicit WVersionLabel(QWidget* pParent = nullptr);

    void setup(const QDomNode& node, const SkinContext& context) override;
};
