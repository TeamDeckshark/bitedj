#include "effects/effectknobparameterslot.h"

#include "control/controleffectknob.h"
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "controllers/softtakeover.h"
#include "effects/effectparameter.h"
#include "moc_effectknobparameterslot.cpp"

EffectKnobParameterSlot::EffectKnobParameterSlot(
        const QString& group, const unsigned int iParameterSlotNumber)
        : EffectParameterSlotBase(
                  group, iParameterSlotNumber, EffectParameterType::Knob) {
    QString itemPrefix = formatItemPrefix(iParameterSlotNumber);

    m_pControlValue = new ControlEffectKnob(
            ConfigKey(m_group, itemPrefix));
    connect(m_pControlValue,
            &ControlObject::valueChanged,
            this,
            &EffectKnobParameterSlot::slotValueChanged);

    m_pControlLoaded = new ControlObject(
            ConfigKey(m_group, itemPrefix + QString("_loaded")));
    m_pControlLoaded->setReadOnly();

    m_pControlType = new ControlObject(
            ConfigKey(m_group, itemPrefix + QString("_type")));
    m_pControlType->setReadOnly();

    m_pControlLinkType = new ControlPushButton(
            ConfigKey(m_group, itemPrefix + QString("_link_type")));
    m_pControlLinkType->setButtonMode(ControlPushButton::TOGGLE);
    m_pControlLinkType->setStates(
            static_cast<int>(EffectManifestParameter::LinkType::NumLinkTypes));
    m_pControlLinkType->connectValueChangeRequest(
            this, &EffectKnobParameterSlot::slotLinkTypeChanging);

    m_pControlLinkInverse = new ControlPushButton(
            ConfigKey(m_group, itemPrefix + QString("_link_inverse")));
    m_pControlLinkInverse->setButtonMode(ControlPushButton::TOGGLE);
    connect(m_pControlLinkInverse,
            &ControlObject::valueChanged,
            this,
            &EffectKnobParameterSlot::slotLinkInverseChanged);

    // Bite DJ fork additions: read-only manifest metadata for skins.
    m_pControlUnits = new ControlObject(
            ConfigKey(m_group, itemPrefix + QString("_units")));
    m_pControlUnits->setReadOnly();
    m_pControlMin = new ControlObject(
            ConfigKey(m_group, itemPrefix + QString("_min")));
    m_pControlMin->setReadOnly();
    m_pControlMax = new ControlObject(
            ConfigKey(m_group, itemPrefix + QString("_max")));
    m_pControlMax->setReadOnly();
    m_pControlDefault = new ControlObject(
            ConfigKey(m_group, itemPrefix + QString("_default")));
    m_pControlDefault->setReadOnly();

    // Bite DJ fork addition: raw-value alias for the knob, mirrored
    // bidirectionally. Plain ControlObject (no behaviour) so writes
    // through a skin Connection's setParameter() land directly as raw
    // values; the alias->knob slot then forwards via the knob's set()
    // which the knob's behaviour clamps to the parameter's range.
    m_bMirroringValueAlias = false;
    m_pControlValueAlias = new ControlObject(
            ConfigKey(m_group, itemPrefix + QString("_value")));
    connect(m_pControlValue,
            &ControlObject::valueChanged,
            this,
            &EffectKnobParameterSlot::slotKnobValueMirror);
    connect(m_pControlValueAlias,
            &ControlObject::valueChanged,
            this,
            &EffectKnobParameterSlot::slotValueAliasFromSkin);

    m_pMetaknobSoftTakeover = new SoftTakeover();

    clear();
}

EffectKnobParameterSlot::~EffectKnobParameterSlot() {
    delete m_pControlValue;
    // m_pControlLoaded and m_pControlType are deleted by ~EffectParameterSlotBase
    delete m_pControlLinkType;
    delete m_pControlLinkInverse;
    delete m_pControlUnits;
    delete m_pControlMin;
    delete m_pControlMax;
    delete m_pControlDefault;
    delete m_pControlValueAlias;
    delete m_pMetaknobSoftTakeover;
}

void EffectKnobParameterSlot::loadParameter(EffectParameterPointer pEffectParameter) {
    clear();

    VERIFY_OR_DEBUG_ASSERT(pEffectParameter->manifest()->parameterType() ==
            EffectParameterType::Knob) {
        return;
    }

    m_pEffectParameter = pEffectParameter;

    if (m_pEffectParameter) {
        m_pManifestParameter = m_pEffectParameter->manifest();

        EffectManifestParameter::ValueScaler type = m_pManifestParameter->valueScaler();
        m_pControlValue->setBehaviour(type,
                m_pManifestParameter->getMinimum(),
                m_pManifestParameter->getMaximum());
        m_pControlValue->setDefaultValue(m_pManifestParameter->getDefault());
        m_pControlValue->set(m_pEffectParameter->getValue());
        // TODO(rryan) expose this from EffectParameter
        m_pControlType->forceSet(static_cast<double>(type));
        // Default loaded parameters to loaded and unlinked
        m_pControlLoaded->forceSet(1.0);

        // Bite DJ fork additions: publish manifest metadata.
        m_pControlUnits->forceSet(
                static_cast<double>(m_pManifestParameter->unitsHint()));
        m_pControlMin->forceSet(m_pManifestParameter->getMinimum());
        m_pControlMax->forceSet(m_pManifestParameter->getMaximum());
        m_pControlDefault->forceSet(m_pManifestParameter->getDefault());

        // Bite DJ fork addition: seed the raw-value alias from the
        // current knob value. forceSet skips behaviour and the
        // valueChanged signal, so we set m_bMirroringValueAlias to
        // suppress the alias->knob slot just in case forceSet still
        // emits.
        m_bMirroringValueAlias = true;
        m_pControlValueAlias->forceSet(m_pEffectParameter->getValue());
        m_bMirroringValueAlias = false;

        m_pControlLinkType->set(
                static_cast<double>(pEffectParameter->linkType()));
        m_pControlLinkInverse->set(
                static_cast<double>(pEffectParameter->linkInversion()));
    }

    emit updated();
}

void EffectKnobParameterSlot::clear() {
    if (m_pEffectParameter) {
        m_pEffectParameter = nullptr;
        m_pManifestParameter.clear();
    }

    m_pControlLoaded->forceSet(0.0);
    m_pControlValue->set(0.0);
    m_pControlValue->setDefaultValue(0.0);
    m_pControlType->forceSet(0.0);
    m_pControlLinkType->setAndConfirm(
            static_cast<double>(EffectManifestParameter::LinkType::None));
    m_pMetaknobSoftTakeover->setThreshold(SoftTakeover::kDefaultTakeoverThreshold);
    m_pControlLinkInverse->set(0.0);
    m_pControlUnits->forceSet(
            static_cast<double>(EffectManifestParameter::UnitsHint::Unknown));
    m_pControlMin->forceSet(0.0);
    m_pControlMax->forceSet(1.0);
    m_pControlDefault->forceSet(0.0);
    m_bMirroringValueAlias = true;
    m_pControlValueAlias->forceSet(0.0);
    m_bMirroringValueAlias = false;
    emit updated();
}

void EffectKnobParameterSlot::setParameter(double value) {
    m_pControlValue->setParameterFrom(value, this);
}

void EffectKnobParameterSlot::slotLinkTypeChanging(double v) {
    m_pMetaknobSoftTakeover->ignoreNext();
    EffectManifestParameter::LinkType newType =
            static_cast<EffectManifestParameter::LinkType>(
                    static_cast<int>(v));
    if (newType == EffectManifestParameter::LinkType::LinkedLeft ||
            newType == EffectManifestParameter::LinkType::LinkedRight ||
            newType == EffectManifestParameter::LinkType::LinkedLeftRight) {
        double neutral = m_pManifestParameter->neutralPointOnScale();
        if (neutral > 0.0 && neutral < 1.0) {
            // Knob is already a split knob, meaning it has a positive and
            // negative effect if it's twisted above the neutral point or
            // below the neutral point.
            // Toggle back to 0
            newType = EffectManifestParameter::LinkType::None;
        }
    }
    if (newType == EffectManifestParameter::LinkType::LinkedLeft ||
            newType == EffectManifestParameter::LinkType::LinkedRight) {
        m_pMetaknobSoftTakeover->setThreshold(
                SoftTakeover::kDefaultTakeoverThreshold * 2.0);
    } else {
        m_pMetaknobSoftTakeover->setThreshold(SoftTakeover::kDefaultTakeoverThreshold);
    }
    m_pControlLinkType->setAndConfirm(static_cast<double>(newType));
    m_pEffectParameter->setLinkType(newType);
}

void EffectKnobParameterSlot::slotLinkInverseChanged(double v) {
    Q_UNUSED(v);
    m_pMetaknobSoftTakeover->ignoreNext();
    m_pEffectParameter->setLinkInversion(
            static_cast<EffectManifestParameter::LinkInversion>(
                    static_cast<int>(v)));
}

void EffectKnobParameterSlot::onEffectMetaParameterChanged(double parameter, bool force) {
    m_dChainParameter = parameter;
    if (m_pEffectParameter != nullptr) {
        // Intermediate cast to integer is needed for VC++.
        EffectManifestParameter::LinkType type =
                static_cast<EffectManifestParameter::LinkType>(
                        static_cast<int>(m_pControlLinkType->get()));

        bool inverse = m_pControlLinkInverse->toBool();
        double neutral = m_pManifestParameter->neutralPointOnScale();

        switch (type) {
        case EffectManifestParameter::LinkType::Linked:
            if (parameter < 0.0 || parameter > 1.0) {
                return;
            }
            if (neutral > 0.0 && neutral < 1.0) {
                if (inverse) {
                    // the neutral position must stick where it is
                    neutral = 1.0 - neutral;
                }
                // Knob is already a split knob
                // Match to center position of meta knob
                if (parameter <= 0.5) {
                    parameter /= 0.5;
                    parameter *= neutral;
                } else {
                    parameter -= 0.5;
                    parameter /= 0.5;
                    parameter *= 1 - neutral;
                    parameter += neutral;
                }
            }
            break;
        case EffectManifestParameter::LinkType::LinkedLeft:
            if (parameter >= 0.5 && parameter <= 1.0) {
                parameter = 1;
            } else if (parameter >= 0.0 && parameter <= 0.5) {
                parameter *= 2;
            } else {
                return;
            }
            break;
        case EffectManifestParameter::LinkType::LinkedRight:
            if (parameter >= 0.5 && parameter <= 1.0) {
                parameter -= 0.5;
                parameter *= 2;
            } else if (parameter >= 0.0 && parameter < 0.5) {
                parameter = 0.0;
            } else {
                return;
            }
            break;
        case EffectManifestParameter::LinkType::LinkedLeftRight:
            if (parameter >= 0.5 && parameter <= 1.0) {
                parameter -= 0.5;
                parameter *= 2;
            } else if (parameter >= 0.0 && parameter < 0.5) {
                parameter *= 2;
                parameter = 1.0 - parameter;
            } else {
                return;
            }
            break;
        case EffectManifestParameter::LinkType::None:
        default:
            return;
        }

        if (inverse) {
            parameter = 1.0 - parameter;
        }

        //qDebug() << "onEffectMetaParameterChanged" << debugString() << parameter << "force?" << force;
        if (force) {
            m_pControlValue->setParameterFrom(parameter, nullptr);
            // This ensures that softtakover is in sync for following updates
            m_pMetaknobSoftTakeover->ignore(m_pControlValue, parameter);
        } else if (!m_pMetaknobSoftTakeover->ignore(m_pControlValue, parameter)) {
            m_pControlValue->setParameterFrom(parameter, nullptr);
        }
    }
}

void EffectKnobParameterSlot::syncSofttakeover() {
    double parameter = m_pControlValue->getParameter();
    m_pMetaknobSoftTakeover->ignore(m_pControlValue, parameter);
}

double EffectKnobParameterSlot::getValueParameter() const {
    return m_pControlValue->getParameter();
}

void EffectKnobParameterSlot::slotKnobValueMirror(double v) {
    if (m_bMirroringValueAlias) {
        return;
    }
    m_bMirroringValueAlias = true;
    m_pControlValueAlias->set(v);
    m_bMirroringValueAlias = false;
}

void EffectKnobParameterSlot::slotValueAliasFromSkin(double v) {
    if (m_bMirroringValueAlias) {
        return;
    }
    m_bMirroringValueAlias = true;
    // m_pControlValue is a ControlEffectKnob; its behaviour clamps v
    // to the parameter's manifest range before storing.
    m_pControlValue->set(v);
    // Reflect the actual (possibly clamped) knob value back into the
    // alias so the skin sees the truth — without this, an out-of-range
    // write (e.g. 8 on Echo's [0, 2] delay_time) would leave the alias
    // at 8 while the knob stored 2, and the bucket's highlight binding
    // would never match. forceSet bypasses behaviour and emits
    // valueChanged, which re-enters this slot — the guard above bails.
    double clamped = m_pControlValue->get();
    if (clamped != v) {
        m_pControlValueAlias->forceSet(clamped);
    }
    // Push to the audio engine ourselves: m_pControlValue->set(v)
    // passes the ControlObject as the sender, and
    // ControlObject::privateValueChanged filters out self-originated
    // changes, so the valueChanged → slotValueChanged →
    // EffectParameter::setValue → updateEngineState chain never fires
    // for alias writes. Other ControlProxy listeners (the on-screen
    // knob, the bucket highlights) still get the signal because their
    // sender pointer differs, so the UI looks right but the audio
    // thread keeps using the stale value.
    slotValueChanged(clamped);
    m_bMirroringValueAlias = false;
}
