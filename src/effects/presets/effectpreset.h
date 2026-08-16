#pragma once
#include <QDomElement>
#include <QHash>

#include "effects/defs.h"
#include "effects/presets/effectparameterpreset.h"

class EffectSlot;

/// EffectPreset is a read-only snapshot of the state of an effect that can be
/// serialized to/deserialized from XML. It is used by EffectChainPreset to
/// save/load chain presets. It is also used by EffectPresetManager to save custom
/// defaults for each effect.
class EffectPreset {
  public:
    EffectPreset();
    EffectPreset(const QDomElement& element);
    EffectPreset(const EffectSlotPointer pEffectSlot);
    /// Bite DJ fork: raw-pointer overload used to snapshot an EffectSlot
    /// from inside its own member functions (e.g. when caching the outgoing
    /// effect during a switch). `includeRememberedPresets` controls whether
    /// the slot's per-manifest cache is copied into this snapshot — only
    /// the outermost serialization snapshot wants that, never a cache entry.
    EffectPreset(const EffectSlot* pEffectSlot,
            bool includeRememberedPresets);
    EffectPreset(const EffectManifestPointer pManifest);

    const QDomElement toXml(QDomDocument* doc) const;

    const QString& id() const {
        return m_id;
    }

    bool isEmpty() const {
        return m_effectParameterPresets.size() == 0;
    }

    EffectBackendType backendType() const {
        return m_backendType;
    }

    double metaParameter() const {
        return m_dMetaParameter;
    }

    const QList<EffectParameterPreset>& getParameterPresets() const {
        return m_effectParameterPresets;
    }

    /// Bite DJ fork: presets the EffectSlot was carrying for previously-
    /// visited effects, keyed by manifest id. Lets the BeatFX picker
    /// restore knob/metaknob state when the user returns to an effect.
    const QHash<QString, EffectPresetPointer>& rememberedPresets() const {
        return m_rememberedPresets;
    }

    /// updates all of the parameters of `this` with the parameters
    /// of `preset`.
    /// The operation is not symmetric:
    /// Parameters which are present on `preset` but not on `this` will
    /// not be added to `this`
    /// Parameters present on `this` but not `preset` will keep their previous
    /// settings
    void updateParametersFrom(const EffectPreset& preset);

  private:
    QString m_id;
    EffectBackendType m_backendType;
    double m_dMetaParameter;

    QList<EffectParameterPreset> m_effectParameterPresets;

    // Bite DJ fork: per-manifest cache (see rememberedPresets()).
    QHash<QString, EffectPresetPointer> m_rememberedPresets;
};
