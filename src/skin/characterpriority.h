#pragma once

#include <QAtomicPointer>
#include <QFont>
#include <QObject>
#include <QString>
#include <QStringList>
#include <memory>

#include "preferences/usersettings.h"

class ControlPushButton;

/// Bite DJ: CJK character priority — which regional glyph forms Han characters
/// are drawn with, and the font that draws them.
///
/// Two separate problems, one setting.
///
/// The first is coverage. The skin asks for `'Inter', sans-serif, 'Monospace'`
/// and the appliance image is minimal, so on a unit with no CJK font installed
/// every Japanese, Chinese or Korean track title is a row of tofu boxes. The
/// fix is to name a font that has those glyphs, in the skin's own fallback
/// list, rather than hoping fontconfig has something to offer.
///
/// The second is that naming *a* CJK font is not enough. Japanese, Simplified
/// Chinese, Traditional Chinese and Korean share most of their code points but
/// not their preferred glyph shapes, and a single pan-CJK font has to pick one
/// region per code point. Unicode carries no clue which one a given track title
/// wants — the text is identical — so the regional variant is a *setting*, the
/// same way Pioneer/AlphaTheta players expose CHARACTER PRIORITY. Noto Sans CJK
/// ships one family per region (JP/SC/TC/HK/KR) over a shared design, so the
/// choice is a family name and nothing else changes.
///
/// `Auto` derives the region from the UI locale (`[Config],Locale`, falling
/// back to the system locale), which is the right answer for a DJ whose UI and
/// collection speak the same language. It is deliberately overridable: an
/// English UI with a Japanese collection is a normal setup, and `Auto` would
/// otherwise silently render it with Chinese glyph forms.
///
/// The font is *proportional* Noto Sans CJK, not the Mono cut. Fixed-width CJK
/// wastes roughly half the width of every Latin letter, digit and space in a
/// mixed title, and the library rows are the narrowest thing on a 480px panel.
/// The skin's own fixed-width values — BPM, time, tempo — do not come from a
/// monospaced *family* anyway; they use OpenType tabular figures (`WLabel`'s
/// `TabularNumbers`), which keeps digits aligned without costing the width.
///
/// Applied by splicing the family into the skin's `font-family` declarations as
/// the sheet is parsed (`mapStyleSheet()`, called from
/// `LegacySkinParser::getStyleFromNode()`), directly behind the first family
/// named there: Latin keeps coming from Inter, Han comes from Noto. A Qt
/// stylesheet's family list *is* the font's fallback chain — it survives into
/// `QFont::families()` and outranks any `setFont()` a widget does for itself,
/// which is what puts it on the library table too. Because that happens at
/// parse time, a change reboots the skin view rather than repainting in place,
/// same as daylight mode — see `priorityChanged`.
///
/// Soft contract with stock Mixxx, like the rest of the fork's singletons:
/// every static no-ops when the instance has not been constructed, so the skin
/// still parses under a test harness or an unpatched binary.
class CharacterPriority : public QObject {
    Q_OBJECT
  public:
    /// The setting as the DJ picks it. Values are the states the skin's
    /// tap-to-cycle button walks and `[BiteDJ],character_priority` persists;
    /// keep them stable, and keep them contiguous from zero — the button cycles
    /// modulo the state count.
    enum class Priority {
        Auto = 0,
        Japanese = 1,
        SimplifiedChinese = 2,
        TraditionalChinese = 3,
        Korean = 4,
    };

    /// The setting once `Auto` has been resolved against the UI locale. Not the
    /// same set: Hong Kong has its own Noto cut and its own locale, but it is
    /// not offered as a choice — `Auto` is the only way to reach it, exactly as
    /// the players this mirrors do it.
    enum class Region {
        Japan,
        SimplifiedChina,
        TraditionalChina,
        HongKong,
        Korea,
    };

    explicit CharacterPriority(UserSettingsPointer pConfig);
    ~CharacterPriority() override;

    /// Null when the singleton has not been constructed. Callers then leave
    /// fonts alone.
    static CharacterPriority* tryInstance();

    /// The CJK family to draw Han characters with, or empty when no CJK font is
    /// installed on this unit (in which case nothing is spliced anywhere and
    /// the system falls back however it likes).
    static QString fontFamily();

    /// `styleSheet` with `fontFamily()` spliced into each of its `font-family`
    /// declarations, behind the family that declaration names first.
    static QString mapStyleSheet(const QString& styleSheet);

    // --- Pure, instance-free pieces. Public so they can be tested without a
    // --- settings file, a font directory or a skin.

    /// The concrete regional variant `priority` selects. `uiLocale` is a Qt
    /// locale name (`ja`, `zh_TW`, `en_US`, …) and is only consulted for
    /// `Auto`; an empty or non-CJK locale resolves to Japan — see the .cpp.
    static Region resolve(Priority priority, const QString& uiLocale);

    /// Font families that draw `region`'s glyph forms, best first. Every unit
    /// in the field is expected to have the first one (Debian's
    /// `fonts-noto-cjk`); the rest cover the other ways the same typeface gets
    /// packaged.
    static QStringList familyCandidates(Region region);

    /// `familyCandidates()` for `region` followed by every other region's, so a
    /// unit that has *a* CJK font shows glyphs rather than tofu even when it is
    /// not the one that would have been preferred. Wrong glyph forms are a
    /// typographic complaint; boxes are an unreadable track list.
    static QStringList familyPreference(Region region);

    /// Splices `family` into every `font-family` declaration in `styleSheet`,
    /// immediately after the first family each one names. Returns `styleSheet`
    /// unchanged for an empty `family`, and is idempotent: a declaration that
    /// already names `family` is left alone.
    static QString insertFallbackFamily(const QString& styleSheet, const QString& family);

    /// `font` with `family` appended right behind its primary family, for text
    /// that no stylesheet reaches.
    static QFont withFallbackFamily(const QFont& font, const QString& family);

  signals:
    /// Emitted after the new value is persisted. MixxxMainWindow reboots the
    /// skin view on this: the family is spliced in as the skin is parsed, so
    /// there is nothing to re-apply in place. It debounces first — the control
    /// is a cycle button, so reaching KR from AUTO emits this four times and
    /// only the last one is worth a rebuild.
    void priorityChanged();

  private slots:
    void onPriorityChanged(double value);

  private:
    /// Re-resolves m_family from m_priority and the UI locale, and pushes it
    /// onto the application font.
    void updateFamily();

    /// The UI locale to resolve `Auto` against: the configured one, or the
    /// system's when Mixxx is set to follow it.
    QString uiLocale() const;

    static QAtomicPointer<CharacterPriority> s_pInstance;

    UserSettingsPointer m_pConfig;
    std::unique_ptr<ControlPushButton> m_pCoPriority;
    Priority m_priority;
    /// Resolved family name, or empty when this unit has no CJK font at all.
    QString m_family;
    /// The application font as it was before we ever touched it, so repeated
    /// changes splice onto the original rather than onto our own output.
    QFont m_baseApplicationFont;
};
