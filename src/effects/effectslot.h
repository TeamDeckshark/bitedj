#pragma once

#include <QHash>
#include <QObject>
#include <QSharedPointer>
#include <QString>

#include "controllers/softtakeover.h"
#include "effects/backends/effectmanifest.h"
#include "engine/channelhandle.h"
#include "preferences/usersettings.h"
#include "util/class.h"

class EffectProcessor;
class EffectSlot;
class EffectState;
class EffectsManager;
class EngineEffect;
class EngineEffectChain;
class ControlProxy;
class EffectParameter;
class EffectKnobParameterSlot;
class ControlPotmeter;
class ControlPushButton;
class ControlEncoder;

typedef QMap<EffectParameterType, QList<EffectParameterPointer>> ParameterMap;

/// EffectSlot is a main thread class which creates EngineEffects and sends
/// updates to them in response to changes in ControlObjects. It owns the
/// ControlObjects for enabling/disabling the effect and the effect metaknob.
///
/// EffectSlot owns a list of EffectParameterSlotBases and EffectParameters.
/// The EffectParameters are the main thread representation of the state of an
/// EngineEffectParameter. EffectSlot creates and destroys EffectParameters
/// together with the EngineEffect as it loads/unloads EngineEffects. The
/// EffectParameterSlotBases own the ControlObjects for manipulating the
/// EffectParameters and showing them in skins. The EffectParameterSlotBases are
/// permanent for the lifetime the EffectSlot; they are not created or destroyed
/// when loading/unloading effects.
///
/// The separation of EffectParameters from EffectParameterSlotBases decouples
/// the parameters available from manipulation via ControlObjects from the
/// parameters of EngineEffects. This allows EffectSlot to arbitrarily hide and
/// rearrange EffectParameters by loading/unloading them from the
/// EffectParameterSlotBases without changing the audio processing in the engine.
/// This is useful to let the user customize which parameters are available for
/// controlling with the finite amount of knobs and buttons on their controller.
/// It is also useful for making external effects plugins such as LV2 plugins
/// work better with Mixxx because they often have lots of parameters with only
/// a few of them being useful in the context of Mixxx.
///
/// The state of an EffectSlot is loaded from an EffectPreset and a snapshot
/// of an EffectSlot's state can be serialized into a EffectPreset.
class EffectSlot : public QObject {
    Q_OBJECT
  public:
    EffectSlot(const QString& group,
            EffectsManager* pEffectsManager,
            EffectsMessengerPointer pEffectsMessenger,
            const unsigned int iEffectNumber,
            EffectChain* pChainSlot,
            EngineEffectChain* pEngineEffectChain);
    virtual ~EffectSlot();

    inline int getEffectSlotNumber() const {
        return m_iEffectNumber;
    }

    inline bool isLoaded() const {
        return m_pEngineEffect != nullptr;
    }

    const QString id() const {
        if (!isLoaded() || m_pManifest == nullptr) {
            return "";
        }
        return m_pManifest->id();
    }

    EffectBackendType backendType() const {
        if (!isLoaded() || m_pManifest == nullptr) {
            return EffectBackendType::BuiltIn;
        }
        return m_pManifest->backendType();
    }

    const ParameterMap getLoadedParameters() const {
        return m_loadedParameters;
    }

    const ParameterMap getHiddenParameters() const {
        return m_hiddenParameters;
    }

    /// Call with nullptr for pPreset to unload an effect
    void loadEffectFromPreset(
            const EffectPresetPointer pPreset);
    /// Call with nullptr for pManifest to unload an effect
    void loadEffectWithDefaults(
            const EffectManifestPointer pManifest);

    void hideParameter(EffectParameterPointer pParameter);
    void showParameter(EffectParameterPointer pParameter);
    void swapParameters(EffectParameterType type, int index1, int index2);

    void addEffectParameterSlot(EffectParameterType parameterType);
    EffectParameterSlotBasePointer getEffectParameterSlot(
            EffectParameterType parameterType, unsigned int slotNumber);

    double getMetaParameter() const;

    /// Ensures that Softtakover is bypassed for the following
    /// ChainParameterChange. Uses for testing only
    void syncSofttakeover();

    const QString& getGroup() const {
        return m_group;
    }

    void initalizeInputChannel(ChannelHandle inputChannel);

    EffectManifestPointer getManifest() const;

    unsigned int numParameters(EffectParameterType parameterType) const;

    void setEnabled(bool enabled);

    // Bite DJ fork: switch this slot to `pManifest`, preferring the
    // cached preset (if any) over the manifest's defaults, and snapshot
    // the outgoing effect into the cache. No-op for chains whose
    // remembersUserParameters() returns false (delegates to
    // loadEffectWithDefaults). Exposed publicly so widget call sites
    // (WEffectSelector, QML proxy) can route through the cache.
    void switchEffectRemembering(const EffectManifestPointer pManifest);

    /// Bite DJ fork: per-manifest cache of recently-edited effect state.
    /// Populated when the user switches away from an effect and consulted
    /// when an effect is re-selected, so BeatFX knob/metaknob values
    /// survive the round trip. Serialized via EffectPreset to effects.xml.
    const QHash<QString, EffectPresetPointer>& rememberedPresets() const {
        return m_rememberedPresets;
    }
    void setRememberedPresets(
            const QHash<QString, EffectPresetPointer>& presets) {
        m_rememberedPresets = presets;
    }

  public slots:
    void setMetaParameter(double v, bool force = false);

    void slotNextEffect(double v);
    void slotPrevEffect(double v);
    void slotLoadedEffectRequest(double value);
    void slotClear(double v);
    void slotEffectSelector(double v);
    void slotEffectMetaParameter(double v, bool force = false);

  signals:
    void effectChanged();
    void parametersChanged();

  private slots:
    void updateEngineState();
    void visibleEffectsListChanged();

    // Bite DJ fork additions: per-effect-slot quantize/triplet proxy
    // routing. The skin binds to a single `[group],quantize` /
    // `[group],triplet` ConfigKey; we forward writes to whichever loaded
    // button parameter has manifest id "quantize" / "triplet".
    void slotQuantizeChanged(double v);
    void slotTripletChanged(double v);
    void slotQuantizeParameterChanged(double v);
    void slotTripletParameterChanged(double v);

  private:
    QString debugString() const {
        return QString("EffectSlot(%1)").arg(m_group);
    }

    void addToEngine();
    void removeFromEngine();

    /// Call with nullptr for pManifest and pPreset to unload an effect
    void loadEffectInner(const EffectManifestPointer pManifest,
            EffectPresetPointer pPreset,
            bool adoptMetaknobFromPreset = false);

    void loadParameters();
    void unloadEffect();

    // Bite DJ fork: re-bind quantize/triplet proxies after an effect's
    // parameters have been loaded; clears them when unloaded.
    void rebindModifierProxies();
    void clearModifierProxies();

    const unsigned int m_iEffectNumber;
    QHash<EffectParameterType, unsigned int> m_iNumParameterSlots;
    const QString m_group;
    UserSettingsPointer m_pConfig;
    EffectsManager* m_pEffectsManager;
    EffectPresetManagerPointer m_pPresetManager;
    EffectsBackendManagerPointer m_pBackendManager;
    EffectsMessengerPointer m_pMessenger;
    VisibleEffectsListPointer m_pVisibleEffects;
    EffectManifestPointer m_pManifest;
    EffectChain* m_pChain;
    EngineEffectChain* m_pEngineEffectChain;
    EngineEffect* m_pEngineEffect;
    ParameterMap m_allParameters;
    ParameterMap m_loadedParameters;
    ParameterMap m_hiddenParameters;
    QMap<EffectParameterType, QList<EffectParameterSlotBasePointer>> m_parameterSlots;

    std::unique_ptr<ControlObject> m_pControlLoaded;
    // Apparently QHash doesn't work with std::unique_ptr
    QHash<EffectParameterType, QSharedPointer<ControlObject>> m_pControlNumParameters;
    QHash<EffectParameterType, QSharedPointer<ControlObject>> m_pControlNumParameterSlots;
    std::unique_ptr<ControlPushButton> m_pControlEnabled;
    std::unique_ptr<ControlObject> m_pControlNextEffect;
    std::unique_ptr<ControlObject> m_pControlPrevEffect;
    std::unique_ptr<ControlObject> m_pControlLoadedEffect;
    std::unique_ptr<ControlEncoder> m_pControlEffectSelector;
    std::unique_ptr<ControlObject> m_pControlClear;
    std::unique_ptr<ControlPotmeter> m_pControlMetaParameter;

    // Bite DJ fork additions: see slotQuantizeChanged docs above.
    std::unique_ptr<ControlPushButton> m_pControlQuantize;
    std::unique_ptr<ControlObject> m_pControlQuantizeLoaded;
    std::unique_ptr<ControlPushButton> m_pControlTriplet;
    std::unique_ptr<ControlObject> m_pControlTripletLoaded;
    std::unique_ptr<ControlProxy> m_pQuantizeParameterProxy;
    std::unique_ptr<ControlProxy> m_pTripletParameterProxy;

    // Bite DJ fork: see rememberedPresets() above.
    QHash<QString, EffectPresetPointer> m_rememberedPresets;

    SoftTakeover m_metaknobSoftTakeover;

    DISALLOW_COPY_AND_ASSIGN(EffectSlot);
};
