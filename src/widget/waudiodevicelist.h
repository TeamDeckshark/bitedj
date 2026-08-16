#pragma once

#include <QList>
#include <QPushButton>
#include <QStringList>

#include "widget/wwidget.h"

class QDomNode;
class QMouseEvent;
class QVBoxLayout;
class SkinContext;

// Bite DJ: renders the three output buses reported by AudioDeviceSettings
// (Master, Booth, Headphones) as a vertical stack of tappable rows. Each row
// shows the bus's current (device + channel-pair) assignment, or "None"; a tap
// cycles that bus to the next option. Rebuilds on
// AudioDeviceSettings::busesChanged; the wrapper widget renders the empty-state
// row when the singleton hasn't been constructed (stock Mixxx fallback).
class WAudioDeviceList : public WWidget {
    Q_OBJECT
  public:
    explicit WAudioDeviceList(QWidget* parent = nullptr);
    ~WAudioDeviceList() override;

    void setup(const QDomNode& node, const SkinContext& context);

  protected:
    // On the touchscreen the child QPushButtons never receive the tap:
    // WWidget::event synthesizes the touch into a QMouseEvent delivered to
    // *this* wrapper, not the child under the finger, so QPushButton::clicked
    // never fires. Hit-test the tap against the row geometries here instead
    // (same pattern as WUsbList). The clicked() connection is kept for the
    // desktop/mouse path where a real event does reach the button.
    void mousePressEvent(QMouseEvent* e) override;

  private slots:
    void onBusesChanged(const QStringList& busLabels, const QList<bool>& assigned);
    void onRowClicked();
    void onBusyChanged(bool busy);

  private:
    void rebuildRows(const QStringList& busLabels, const QList<bool>& assigned);
    void renderEmpty();
    void handleRowTap(int index);
    void applyEnabledToRows();

    QVBoxLayout* m_pLayout;
    QList<QPushButton*> m_rows;
    QPushButton* m_pEmptyRow;
    // Mirrors the global Notifications busy state so rows grey out and refuse
    // taps while an audio-config apply is in flight (device close/reopen takes
    // several seconds). Same pattern as WControllerList.
    bool m_busy;
};
