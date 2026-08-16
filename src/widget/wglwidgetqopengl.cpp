#include <QPalette>
#include <QResizeEvent>

#include "skin/highcontrast.h"
#include "widget/openglwindow.h"
#include "widget/tooltipqopengl.h"
#include "widget/wglwidget.h"

WGLWidget::WGLWidget(QWidget* pParent)
        : QWidget(pParent),
          m_pOpenGLWindow(nullptr),
          m_pContainerWidget(nullptr),
          m_pTrackDropTarget(nullptr) {
    // When the widget is resized or moved, the QOpenGLWindow visibly resizes
    // or moves before the widgets do. This can be solved by calling
    //   setAttribute(Qt::WA_PaintOnScreen);
    // here, but this comes with a clear performance penalty and drop in
    // frame rate.
}

WGLWidget::~WGLWidget() {
    ToolTipQOpenGL::singleton().stop();
    if (m_pOpenGLWindow) {
        m_pOpenGLWindow->widgetDestroyed();
    }
}

QPaintDevice* WGLWidget::paintDevice() {
    makeCurrentIfNeeded();
    return m_pOpenGLWindow;
}

void WGLWidget::setTrackDropTarget(TrackDropTarget* pTarget) {
    m_pTrackDropTarget = pTarget;
}

TrackDropTarget* WGLWidget::trackDropTarget() const {
    return m_pTrackDropTarget;
}

void WGLWidget::showEvent(QShowEvent* event) {
    if (!m_pOpenGLWindow) {
        m_pOpenGLWindow = new OpenGLWindow(this);
        m_pContainerWidget = createWindowContainer(m_pOpenGLWindow, this);
        m_pContainerWidget->resize(size());
        m_pContainerWidget->show();
        // The container's autofilled background is what shows for the frames
        // between the embedded GL window being (re-)mapped and its first
        // swapped buffer — e.g. every time a tab switch re-shows a waveform.
        // The default palette Window role is near-white, which reads as a
        // white flash before the first GL frame paints. Fill it with black so
        // the gap matches the surrounding skin instead of flashing white.
        // In daylight mode the waveform's own background is inverted to white,
        // so the gap has to follow it or the flash just changes colour.
        QPalette palette = m_pContainerWidget->palette();
        palette.setColor(QPalette::Window, HighContrast::mapColor(Qt::black));
        m_pContainerWidget->setPalette(palette);
        m_pContainerWidget->setAutoFillBackground(true);
    }
    QWidget::showEvent(event);
}

void WGLWidget::resizeEvent(QResizeEvent* event) {
    if (m_pContainerWidget) {
        m_pContainerWidget->resize(event->size());
    }
    QWidget::resizeEvent(event);
}

bool WGLWidget::isContextValid() const {
    return m_pOpenGLWindow && m_pOpenGLWindow->context() && m_pOpenGLWindow->context()->isValid();
}

void WGLWidget::makeCurrentIfNeeded() {
    if (m_pOpenGLWindow && m_pOpenGLWindow->context() != QOpenGLContext::currentContext()) {
        m_pOpenGLWindow->makeCurrent();
    }
}

void WGLWidget::doneCurrent() {
    if (m_pOpenGLWindow) {
        m_pOpenGLWindow->doneCurrent();
    }
}

void WGLWidget::paintGL() {
    // to be implemented in derived widgets if needed
}

void WGLWidget::initializeGL() {
    // to be implemented in derived widgets if needed
}

void WGLWidget::resizeGL(int w, int h) {
    Q_UNUSED(w);
    Q_UNUSED(h);
    // to be implemented in derived widgets if needed
}

void WGLWidget::swapBuffers() {
    if (shouldRender()) {
        m_pOpenGLWindow->context()->swapBuffers(m_pOpenGLWindow->context()->surface());
    }
}

bool WGLWidget::shouldRender() const {
    return m_pOpenGLWindow && m_pOpenGLWindow->isExposed();
}

QOpenGLWindow* WGLWidget::getOpenGLWindow() const {
    return m_pOpenGLWindow;
}
