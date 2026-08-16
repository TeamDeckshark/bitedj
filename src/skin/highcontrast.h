#pragma once

#include <QAtomicPointer>
#include <QColor>
#include <QObject>
#include <QString>
#include <memory>

#include "preferences/usersettings.h"

class ControlObject;

/// Bite DJ: high-contrast ("daylight") mode.
///
/// The skin is a dark theme, which is what a booth wants and what a screen in
/// direct sun is worst at: an LCD's blacks wash out to grey outdoors, so a
/// light-on-dark UI loses most of its contrast exactly when it is needed. This
/// flips the whole skin to dark-on-light without maintaining a second theme —
/// every colour is derived from the one the skin already declares.
///
/// Colours invert in HSL space: lightness flips, hue and saturation ride
/// through untouched. A plain RGB inversion would turn the danger red cyan and
/// the playing-deck orange blue, and this skin's state colours are required to
/// mirror the hardware LEDs.
///
/// Three parse-time hooks cover the skin between them:
///   - `mapStyleSheet()` from `LegacySkinParser::getStyleFromNode()` — every
///     QSS sheet the skin declares, plus the SVG icons its `url()`s name.
///   - `ImgHighContrast` installed by `ColorSchemeParser` — every colour read
///     from skin XML (`WSkinColor::getCorrectColor`: waveform signal/beat/mark
///     colours, `WOverview`, knobs, labels) and every image loaded through
///     `WPixmapStore`/`WImageStore` (knob.svg, the effect meters).
///   - `mapColor()` for the handful of colours hard-coded in C++.
/// Because those all run while the skin is being built, a toggle reboots the
/// skin view rather than trying to repaint in place — see `enabledChanged`.
class HighContrast : public QObject {
    Q_OBJECT
  public:
    explicit HighContrast(UserSettingsPointer pConfig);
    ~HighContrast() override;

    /// Null when the singleton has not been constructed (e.g. stock Mixxx, or
    /// a skin parsed by a test harness). Callers then leave colours alone.
    static HighContrast* tryInstance();

    static bool isEnabled();

    /// The colour to actually paint: inverted while the mode is on, otherwise
    /// `color` unchanged. For colours that are hard-coded in C++ rather than
    /// read from the skin.
    static QColor mapColor(const QColor& color);

    /// The stylesheet to actually apply: inverted while the mode is on.
    static QString mapStyleSheet(const QString& styleSheet);

    /// Inverts every colour literal in a Qt stylesheet, leaving selectors,
    /// geometry and text alone. SVG icons referenced by url() are inverted the
    /// same way into iconCacheDir and the url rewritten to point at the copy;
    /// an empty iconCacheDir leaves url()s untouched. A `/* no-invert */`
    /// comment exempts the rule block that follows it — for rules whose colour
    /// is paired against something this transform cannot reach, such as the
    /// cue-pad label that has to stay legible against the cue's own colour.
    /// Pure — public for tests.
    static QString invertStyleSheet(const QString& styleSheet,
            const QString& iconCacheDir = QString());

    /// Flips a colour's HSL lightness, preserving hue, saturation and alpha.
    static QColor invertColor(const QColor& color);

  signals:
    /// Emitted after the new value is persisted. MixxxMainWindow reboots the
    /// skin view on this: the inversion happens as the skin is parsed, so
    /// there is nothing to re-apply in place.
    void enabledChanged(bool enabled);

  private slots:
    void onEnabledChanged(double value);

  private:
    QString iconCacheDir() const;

    static QAtomicPointer<HighContrast> s_pInstance;

    UserSettingsPointer m_pConfig;
    std::unique_ptr<ControlObject> m_pCoEnabled;
    bool m_enabled;
};
