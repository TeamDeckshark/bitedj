#pragma once

#include <QRect>
#include <QString>

#include "util/parented_ptr.h"
#include "widget/wcuemenupopup.h"
#include "widget/wpushbutton.h"

class WHotcueButton : public WPushButton {
    Q_OBJECT
  public:
    WHotcueButton(const QString& group, QWidget* pParent);

    void setup(const QDomNode& node, const SkinContext& context) override;

    ConfigKey getLeftClickConfigKey() {
        return createConfigKey(QStringLiteral("activate"));
    }
    ConfigKey getClearConfigKey() {
        return createConfigKey(QStringLiteral("clear"));
    }

    Q_PROPERTY(bool light MEMBER m_bCueColorIsLight);
    Q_PROPERTY(bool dark MEMBER m_bCueColorIsDark);
    Q_PROPERTY(QString type MEMBER m_type);

  protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    bool event(QEvent* e) override;
    void restyleAndRepaint() override;

  private slots:
    void slotColorChanged(double color);
    void slotTypeChanged(double type);

  private:
    ConfigKey createConfigKey(const QString& name);
    void updateStyleSheet();
    /// The cue menu is only reachable by right click, which a touch-only
    /// device never produces. Building one popup per button would cost a
    /// pile of widgets on a skin that lays out a full bank of pads, so it is
    /// built on first use instead.
    WCueMenuPopup* cueMenuPopup();

    /// Whether the clear badge should be drawn and hit-tested right now: only
    /// when the skin asked for one and the slot actually holds a cue, since an
    /// empty slot has nothing to clear.
    bool clearBadgeVisible();
    /// The badge as painted, in widget coordinates.
    QRect clearBadgeRect();
    /// The badge's tap target, a little larger than the glyph. Deliberately
    /// still well under the 44px touch floor the pads themselves clear: this
    /// is a destructive corner action that must be aimed at, not something a
    /// thumb going for the pad can catch.
    QRect clearBadgeHitRect();
    void cancelClearBadgePress();

    const QString m_group;
    int m_hotcue;
    bool m_hoverCueColor;
    parented_ptr<ControlProxy> m_pCoColor;
    parented_ptr<ControlProxy> m_pCoType;
    parented_ptr<ControlProxy> m_pCoClear;
    UserSettingsPointer m_pConfig;
    parented_ptr<WCueMenuPopup> m_pCueMenuPopup;
    int m_cueColorDimThreshold;
    bool m_bCueColorDimmed;
    bool m_bCueColorIsLight;
    bool m_bCueColorIsDark;
    bool m_bClearBadge;
    bool m_bClearBadgePressed;
    QString m_type;
};
