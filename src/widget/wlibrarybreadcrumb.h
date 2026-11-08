#pragma once

#include <QTimer>

#include "widget/wlabel.h"

/// Bite DJ: the library top bar's name of what is open (the last sidebar leaf
/// tapped). While that item is still being read off its drive it shows the
/// sidebar's loading spinner right after the name: tapping a leaf hides the
/// sidebar, so this is the only place left that can say the view is not
/// complete yet.
class WLibraryBreadcrumb : public WLabel {
    Q_OBJECT
  public:
    explicit WLibraryBreadcrumb(QWidget* pParent = nullptr);

  public slots:
    void setLoading(bool loading);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    QTimer m_animationTimer;
    int m_frame;
    bool m_loading;
};
