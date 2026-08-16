#pragma once

#include <QList>
#include <QPointF>
#include <QPushButton>
#include <QStringList>

#include "widget/wwidget.h"

class QDomNode;
class QHBoxLayout;
class QLabel;
class SkinContext;

// Bite DJ: the drive picker on the Samplers tab — one chip per mounted USB
// drive, the selected one highlighted. Tapping a chip makes that drive the one
// the sampler slots are filled from and saved to (SamplerDrive), which is the
// only thing that decides where a sample may come from.
//
// Built at runtime rather than declared in the skin because the choices are
// whatever is plugged in: it rebuilds on SamplerDrive::drivesChanged, and stays
// an empty placeholder when the singleton hasn't been constructed (stock Mixxx
// fallback), exactly like WUsbList.
//
// With the selected drive unplugged there is nothing to highlight, so the strip
// says which drive is missing instead — the grid is empty at that point and the
// DJ needs to know that plugging that stick back in is what refills it.
//
// The chips are plain QPushButtons for styling only. On the touchscreen they
// never see a mouse event of their own (WWidget::event synthesizes touch ->
// mouse to *this* widget), so the tap is hit-tested and dispatched here; the
// clicked() connection is only there for a real mouse on the desktop build.
class WSamplerDrive : public WWidget {
    Q_OBJECT
  public:
    explicit WSamplerDrive(QWidget* parent = nullptr);
    ~WSamplerDrive() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

  private slots:
    void onDrivesChanged(const QStringList& labels, int selectedIndex);
    void onChipClicked();

  private:
    void rebuild(const QStringList& labels, int selectedIndex);
    void renderEmpty();
    void clearChildren();
    // Fits each chip's label to the width it actually got. A volume name is as
    // long as the DJ made it and four sticks can be plugged in at once, so
    // without this the strip would push its last chip off the panel.
    void applyElidedLabels();
    int chipIndexAt(const QPoint& globalPos) const;

    QHBoxLayout* m_pLayout;
    QList<QPushButton*> m_chips;
    QStringList m_labels;
    QLabel* m_pPlaceholder;
    int m_selectedIndex;
    // Whether the press that would make a release a tap landed on us.
    bool m_pressed;
};
