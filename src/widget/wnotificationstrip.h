#pragma once

#include <QLabel>
#include <QString>

#include "widget/wwidget.h"

class QDomNode;
class QResizeEvent;
class SkinContext;

// Bite DJ: renders the most-recent in-skin notification. Subscribes to
// Notifications::messagePosted to receive text + severity (CO transport for
// visibility/severity is handled separately via skin <Connection>s), sets a
// dynamic "severity" property so QSS can tint the band, and dismisses the
// notification on tap.
class WNotificationStrip : public WWidget {
    Q_OBJECT
  public:
    explicit WNotificationStrip(QWidget* parent = nullptr);
    ~WNotificationStrip() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private slots:
    void onMessagePosted(const QString& message, int severity);
    void onCleared();

  private:
    void applySeverity(int severity);
    // Renders m_message into the label, eliding the end and left-aligning
    // when it overflows so the start of the message stays visible.
    void updateLabelText();

    QLabel* m_pLabel;
    QString m_message;
};
