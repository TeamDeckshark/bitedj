#include <QPalette>
#include <QWindow>

#include "skin/highcontrast.h"
#include "waveform/sharedglcontext.h"
#include "widget/wglwidget.h"

WGLWidget::WGLWidget(QWidget* parent)
        : QGLWidget(parent, SharedGLContext::getWidget()) {
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    // Match the QOpenGLWindow path: the default palette Window role is
    // near-white, so any background fill before the first GL frame paints
    // reads as a white flash on a re-show (e.g. a tab switch). Force it black
    // so the pre-first-frame gap matches the surrounding skin.
    // Follows the waveform background, which daylight mode inverts to white.
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, HighContrast::mapColor(Qt::black));
    setPalette(palette);
    setAutoBufferSwap(false);
    // Not interested in repaint or update calls, as we draw from the vsync thread
    setUpdatesEnabled(false);
}

bool WGLWidget::isContextValid() const {
    // A QGLWidget should always have a context, but it is possible that
    // the context is not valid. for example, if the underlying hardware
    // does not support the format attributes that were requested.
    return context()->isValid();
}

bool WGLWidget::shouldRender() const {
    return isValid() && isVisible() && windowHandle() && windowHandle()->isExposed();
}

void WGLWidget::makeCurrentIfNeeded() {
    if (context() != QGLContext::currentContext()) {
        makeCurrent();
    }
}
