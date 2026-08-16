#pragma once

#include <QJSValue>
#include <QObject>

#include "controllers/softtakeover.h"
#include "util/alphabetafilter.h"
#include "util/performancetimer.h"
#include "util/runtimeloggingcategory.h"

class ControllerScriptEngineLegacy;
class ControlObjectScript;
class ScriptConnection;
class ConfigKey;

/// ControllerScriptInterfaceLegacy is the legacy API for controller scripts to interact
/// with Mixxx. It is inserted into the JS environment as the "engine" object.
class ControllerScriptInterfaceLegacy : public QObject {
    Q_OBJECT
  public:
    ControllerScriptInterfaceLegacy(ControllerScriptEngineLegacy* m_pEngine,
            const RuntimeLoggingCategory& logger);

    virtual ~ControllerScriptInterfaceLegacy();

    Q_INVOKABLE QJSValue getSetting(const QString& name);
    Q_INVOKABLE double getValue(const QString& group, const QString& name);
    Q_INVOKABLE void setValue(const QString& group, const QString& name, double newValue);
    Q_INVOKABLE double getParameter(const QString& group, const QString& name);
    Q_INVOKABLE void setParameter(const QString& group, const QString& name, double newValue);
    Q_INVOKABLE double getParameterForValue(
            const QString& group, const QString& name, double value);
    Q_INVOKABLE void reset(const QString& group, const QString& name);
    Q_INVOKABLE double getDefaultValue(const QString& group, const QString& name);
    Q_INVOKABLE double getDefaultParameter(const QString& group, const QString& name);
    Q_INVOKABLE QJSValue makeConnection(const QString& group,
            const QString& name,
            const QJSValue& callback);
    Q_INVOKABLE QJSValue makeUnbufferedConnection(const QString& group,
            const QString& name,
            const QJSValue& callback);
    // DEPRECATED: Use makeConnection instead.
    Q_INVOKABLE QJSValue connectControl(const QString& group,
            const QString& name,
            const QJSValue& passedCallback,
            bool disconnect = false);
    // Called indirectly by the objects returned by connectControl
    Q_INVOKABLE void trigger(const QString& group, const QString& name);
    // DEPRECATED: Use console.log instead.
    Q_INVOKABLE void log(const QString& message);
    Q_INVOKABLE int beginTimer(int interval, QJSValue scriptCode, bool oneShot = false);
    Q_INVOKABLE void stopTimer(int timerId);
    Q_INVOKABLE void scratchEnable(int deck,
            int intervalsPerRev,
            double rpm,
            double alpha,
            double beta,
            bool ramp = true);
    Q_INVOKABLE void scratchTick(int deck, int interval);
    Q_INVOKABLE void scratchDisable(int deck, bool ramp = true);
    Q_INVOKABLE bool isScratching(int deck);
    Q_INVOKABLE void softTakeover(const QString& group, const QString& name, bool set);
    Q_INVOKABLE void softTakeoverIgnoreNextValue(const QString& group, const QString& name);
    Q_INVOKABLE void brake(const int deck,
            bool activate,
            double factor = 1.0,
            const double rate = 1.0);
    Q_INVOKABLE void spinback(const int deck,
            bool activate,
            double factor = 1.8,
            const double rate = -10.0);
    Q_INVOKABLE void softStart(const int deck, bool activate, double factor = 1.0);

    bool removeScriptConnection(const ScriptConnection& conn);
    /// Execute a ScriptConnection's JS callback
    void triggerScriptConnection(const ScriptConnection& conn);

    /// Handler for timers that scripts set.
    virtual void timerEvent(QTimerEvent* event);

  private:
    QJSValue makeConnectionInternal(const QString& group,
            const QString& name,
            const QJSValue& callback,
            bool skipSuperseded = false);
    QHash<ConfigKey, ControlObjectScript*> m_controlCache;
    ControlObjectScript* getControlObjectScript(const QString& group, const QString& name);

    SoftTakeoverCtrl m_st;

    struct TimerInfo {
        QJSValue callback;
        bool oneShot;
    };
    QHash<int, TimerInfo> m_timers;

    QVarLengthArray<int> m_intervalAccumulator;
    QVarLengthArray<mixxx::Duration> m_lastMovement;
    QVarLengthArray<double> m_dx, m_rampTo, m_rampFactor;
    QVarLengthArray<bool> m_ramp, m_brakeActive, m_spinbackActive, m_softStartActive;
    // Bite DJ vinyl brake: while m_vinylBrakeActive, scratchProcess ignores the
    // alpha-beta filter and coasts the scratch rate towards the deck's own rate
    // (0 while it is stopped), re-read on every tick so that starting or
    // stopping the deck mid-run-out changes where the platter is headed. It
    // closes the distance left at m_vinylBrakeDrag per second of that distance
    // plus a constant m_vinylBrakeFriction per second - so the platter sheds
    // speed fastest while it is turning fastest, yet still arrives. Stepped by
    // the time m_vinylBrakeTick has measured since the last tick rather than by
    // a tick count, because the 1ms scratch timer coalesces under load. Its own
    // monotonic clock rather than mixxx::Time, which tests freeze.
    // See startVinylBrake().
    QVarLengthArray<double> m_vinylBrakeRate, m_vinylBrakeDrag, m_vinylBrakeFriction;
    QVarLengthArray<PerformanceTimer> m_vinylBrakeTick;
    QVarLengthArray<bool> m_vinylBrakeActive;
    QVarLengthArray<AlphaBetaFilter*> m_scratchFilters;
    QHash<int, int> m_scratchTimers;
    /// Applies the accumulated movement to the track speed
    void scratchProcess(int timerId);
    /// Bite DJ: arms the configurable vinyl brake for a jog wheel that was just
    /// released, so the platter coasts to the deck's rate instead of snapping
    /// there.
    /// Returns false (leaving the deck to the stock ramp) when the brake is
    /// switched off or the wheel was barely moving.
    bool startVinylBrake(int deck, const QString& group);
    /// Bite DJ: one vinyl-brake tick. Returns true when it owned this tick, in
    /// which case scratchProcess must not run the alpha-beta ramp as well.
    bool vinylBrakeProcess(int deck, int timerId, const QString& group);
    /// Bite DJ: lets go of a coasting platter, whether it arrived at its target
    /// or something else (a cue press) took the deck off the scratch first.
    void endVinylBrake(int deck, int timerId);
    void stopScratchTimer(int timerId);
    bool isDeckPlaying(const QString& group);
    void stopDeck(const QString& group);
    bool isTrackLoaded(const QString& group);
    double getDeckRate(const QString& group);

    ControllerScriptEngineLegacy* m_pScriptEngineLegacy;
    const RuntimeLoggingCategory m_logger;
};
