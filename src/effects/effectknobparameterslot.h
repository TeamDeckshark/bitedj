#pragma once

#include <QObject>
#include <QString>
#include <QVariant>
#include <optional>

#include "effects/effectparameterslotbase.h"
#include "util/class.h"

class ControlPushButton;
class ControlEffectKnob;
class SoftTakeover;

/// Refer to EffectParameterSlotBase for documentation
class EffectKnobParameterSlot : public EffectParameterSlotBase {
    Q_OBJECT
  public:
    EffectKnobParameterSlot(const QString& group, const unsigned int iParameterSlotNumber);
    virtual ~EffectKnobParameterSlot();

    static QString formatItemPrefix(const unsigned int iParameterSlotNumber) {
        return QString("parameter%1").arg(iParameterSlotNumber + 1);
    }

    void loadParameter(EffectParameterPointer pEffectParameter) override;

    double getValueParameter() const;

    void onEffectMetaParameterChanged(double parameter, bool force = false) override;

    // Syncs the Super button with the parameter, that the following
    // super button change will be passed to the effect parameter
    // used during test
    void syncSofttakeover() override;

    // Clear the currently loaded effect
    void clear() override;

    void setParameter(double value) override;

  private slots:
    // Solely for handling control changes
    void slotLinkTypeChanging(double v);
    void slotLinkInverseChanged(double v);

    // Bite DJ fork additions: raw-value alias mirror.
    void slotKnobValueMirror(double v);
    void slotValueAliasFromSkin(double v);

  private:
    QString debugString() const {
        return QString("EffectKnobParameterSlot(%1,%2)").arg(m_group).arg(m_iParameterSlotNumber);
    }

    SoftTakeover* m_pMetaknobSoftTakeover;

    // Control exposed to the rest of Mixxx
    ControlEffectKnob* m_pControlValue;
    ControlPushButton* m_pControlLinkType;
    ControlPushButton* m_pControlLinkInverse;

    // Bite DJ fork additions: expose manifest metadata so the skin can
    // pick the right widget per parameter (bucket selector for Beats,
    // ms slider for Time, plain slider otherwise) and render real units.
    ControlObject* m_pControlUnits;
    ControlObject* m_pControlMin;
    ControlObject* m_pControlMax;
    ControlObject* m_pControlDefault;

    // Bite DJ fork addition: raw-value alias of m_pControlValue. The
    // existing parameterN CO is normalized [0,1] (parameter, not value),
    // so connections from skin XML can't write specific raw values for
    // log-scaled parameters like Tremolo's `rate` [0.25, 8]. This alias
    // is a plain ControlObject (no behaviour) so its parameter equals
    // its value, and a connection's <IsEqual>X</IsEqual> writes raw
    // value X to the underlying knob (clamped by behaviour). Used by
    // the BeatFX bucket picker.
    ControlObject* m_pControlValueAlias;
    bool m_bMirroringValueAlias;

    DISALLOW_COPY_AND_ASSIGN(EffectKnobParameterSlot);
};
