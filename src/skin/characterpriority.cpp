#include "skin/characterpriority.h"

#include <QApplication>
#include <QFontDatabase>
#include <QLocale>
#include <QtDebug>
#include <algorithm>
#include <optional>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "moc_characterpriority.cpp"

namespace {
const QString kGroup = QStringLiteral("[BiteDJ]");
const QString kItem = QStringLiteral("character_priority");
constexpr double kDefault = 0.0; // Priority::Auto
// Auto plus the four regions the DJ can name.
constexpr int kPriorityCount = 5;

// Where the UI language lives. Empty means "follow the system", which is what
// the appliance ships as.
const QString kLocaleGroup = QStringLiteral("[Config]");
const QString kLocaleItem = QStringLiteral("Locale");

const QString kFontFamilyProperty = QStringLiteral("font-family");

CharacterPriority::Priority priorityFromValue(double value) {
    const int index = static_cast<int>(value);
    switch (index) {
    case 1:
        return CharacterPriority::Priority::Japanese;
    case 2:
        return CharacterPriority::Priority::SimplifiedChinese;
    case 3:
        return CharacterPriority::Priority::TraditionalChinese;
    case 4:
        return CharacterPriority::Priority::Korean;
    default:
        // Includes every value a hand-edited mixxx.cfg might hold: an
        // unrecognised priority is Auto, which always resolves to something.
        return CharacterPriority::Priority::Auto;
    }
}

/// The region `locale` writes in, or nullopt when it is not a CJK locale.
/// `locale` is a Qt locale name, so `ja`, `ja_JP`, `zh_Hant_TW`, `en_US`, …
std::optional<CharacterPriority::Region> regionForLocale(const QString& locale) {
    // QLocale spells the separator '_', translation file names use '-'; accept
    // both so this can be handed either.
    const QString name = QString(locale).replace(QLatin1Char('-'), QLatin1Char('_')).toLower();
    if (name.startsWith(QLatin1String("ja"))) {
        return CharacterPriority::Region::Japan;
    }
    if (name.startsWith(QLatin1String("ko"))) {
        return CharacterPriority::Region::Korea;
    }
    if (!name.startsWith(QLatin1String("zh"))) {
        return std::nullopt;
    }
    // Chinese splits by script, and the script is usually implied by the
    // territory rather than spelled out: Taiwan and Macau write traditional,
    // the mainland and Singapore simplified, Hong Kong writes traditional with
    // its own glyph preferences and its own Noto cut.
    if (name.contains(QLatin1String("hk"))) {
        return CharacterPriority::Region::HongKong;
    }
    if (name.contains(QLatin1String("hant")) || name.contains(QLatin1String("tw")) ||
            name.contains(QLatin1String("mo"))) {
        return CharacterPriority::Region::TraditionalChina;
    }
    // Plain `zh`, zh_CN, zh_SG, zh_Hans_*.
    return CharacterPriority::Region::SimplifiedChina;
}

/// The Noto/Source Han suffix naming `region`'s cut of the typeface.
QString regionSuffix(CharacterPriority::Region region) {
    switch (region) {
    case CharacterPriority::Region::Japan:
        return QStringLiteral("JP");
    case CharacterPriority::Region::SimplifiedChina:
        return QStringLiteral("SC");
    case CharacterPriority::Region::TraditionalChina:
        return QStringLiteral("TC");
    case CharacterPriority::Region::HongKong:
        return QStringLiteral("HK");
    case CharacterPriority::Region::Korea:
        return QStringLiteral("KR");
    }
    return QStringLiteral("JP");
}

/// Index of the first `,` in a declaration value that separates families,
/// i.e. one that is not inside a quoted family name. -1 when the value names a
/// single family.
int firstFamilySeparator(const QString& value) {
    QChar quote;
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            }
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            continue;
        }
        if (c == QLatin1Char(',')) {
            return i;
        }
    }
    return -1;
}

/// `value` — the text of a font-family declaration, without the property name,
/// the colon or the terminator — with `family` spliced in behind the family it
/// names first.
QString spliceFamily(const QString& value, const QString& family) {
    if (value.contains(family, Qt::CaseInsensitive)) {
        return value;
    }
    const QString quoted = QStringLiteral(", '%1'").arg(family);
    const int separator = firstFamilySeparator(value);
    if (separator >= 0) {
        return value.left(separator) + quoted + value.mid(separator);
    }
    // Single family: splice behind it but ahead of whatever whitespace trails
    // it, so the declaration keeps the layout the skin author gave it.
    int end = value.size();
    while (end > 0 && value.at(end - 1).isSpace()) {
        --end;
    }
    return value.left(end) + quoted + value.mid(end);
}

/// Whether `c` can be part of a CSS identifier — used to tell the `font-family`
/// property from the tail of a longer word.
bool isIdentChar(QChar c) {
    return c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_');
}
} // anonymous namespace

// static
QAtomicPointer<CharacterPriority> CharacterPriority::s_pInstance{nullptr};

CharacterPriority::CharacterPriority(UserSettingsPointer pConfig)
        : m_pConfig(pConfig),
          m_priority(Priority::Auto),
          m_baseApplicationFont(QApplication::font()) {
    // Created here rather than in SystemSettings, and constructed by
    // CoreServices before the skin is parsed, for the same reason HighContrast
    // is: the skin parser has to be able to ask for the family while it is
    // building widgets.
    const ConfigKey key(kGroup, kItem);
    const double value = m_pConfig->getValue(key, kDefault);
    m_priority = priorityFromValue(value);
    // A five-state ControlPushButton in TOGGLE mode rather than a plain
    // ControlObject: the settings row is one button that cycles
    // AUTO -> JP -> SC -> TC -> KR, and WPushButton only cycles when the CO it
    // is bound to says it is a toggle. Same pattern, same reason, as
    // LibraryColumnControl's size CO next to it on that page.
    m_pCoPriority = std::make_unique<ControlPushButton>(key);
    m_pCoPriority->setButtonMode(ControlPushButton::TOGGLE);
    m_pCoPriority->setStates(kPriorityCount);
    m_pCoPriority->set(static_cast<double>(m_priority));
    connect(m_pCoPriority.get(),
            &ControlObject::valueChanged,
            this,
            &CharacterPriority::onPriorityChanged);

    updateFamily();

    s_pInstance.storeRelease(this);
}

CharacterPriority::~CharacterPriority() {
    s_pInstance.storeRelease(nullptr);
}

// static
CharacterPriority* CharacterPriority::tryInstance() {
    return s_pInstance.loadAcquire();
}

// static
QString CharacterPriority::fontFamily() {
    CharacterPriority* pInstance = tryInstance();
    return pInstance ? pInstance->m_family : QString();
}

// static
QString CharacterPriority::mapStyleSheet(const QString& styleSheet) {
    return insertFallbackFamily(styleSheet, fontFamily());
}

// static
CharacterPriority::Region CharacterPriority::resolve(
        Priority priority, const QString& uiLocale) {
    switch (priority) {
    case Priority::Japanese:
        return Region::Japan;
    case Priority::SimplifiedChinese:
        return Region::SimplifiedChina;
    case Priority::TraditionalChinese:
        return Region::TraditionalChina;
    case Priority::Korean:
        return Region::Korea;
    case Priority::Auto:
        break;
    }
    // A non-CJK UI — which is what an appliance sold in English-speaking
    // markets runs — says nothing about the collection, so Auto has to guess.
    // Japan is the guess: it is what fontconfig already resolves `Monospace` to
    // under en_US.UTF-8 on the image, so Auto changes coverage without also
    // changing the glyph forms a unit was showing before this setting existed,
    // and a DJ whose collection disagrees has four explicit choices to make it
    // say so.
    return regionForLocale(uiLocale).value_or(Region::Japan);
}

// static
QStringList CharacterPriority::familyCandidates(Region region) {
    const QString suffix = regionSuffix(region);
    QStringList candidates;
    // Debian's fonts-noto-cjk, one .ttc holding every region as its own family.
    // This is the package the appliance image installs and what a unit in the
    // field will match on.
    candidates << QStringLiteral("Noto Sans CJK %1").arg(suffix);
    // Google's per-language downloads, packaged one region per file.
    candidates << QStringLiteral("Noto Sans %1").arg(suffix);
    // Same typeface under Adobe's name, which is what some distributions ship.
    candidates << QStringLiteral("Source Han Sans %1").arg(suffix);
    // Last resort within the region: the monospaced cut costs width in mixed
    // CJK/Latin text, but it is the right glyph forms, which is the thing this
    // setting exists to get right.
    candidates << QStringLiteral("Noto Sans Mono CJK %1").arg(suffix);
    return candidates;
}

// static
QStringList CharacterPriority::familyPreference(Region region) {
    QStringList preference = familyCandidates(region);
    // Hong Kong is a traditional-script variant, so its nearest neighbour is
    // Taiwan's cut rather than whatever the enum happens to list next.
    const QList<Region> fallbackOrder = region == Region::HongKong
            ? QList<Region>{Region::TraditionalChina,
                      Region::Japan,
                      Region::SimplifiedChina,
                      Region::Korea}
            : QList<Region>{Region::Japan,
                      Region::SimplifiedChina,
                      Region::TraditionalChina,
                      Region::HongKong,
                      Region::Korea};
    for (const Region other : fallbackOrder) {
        for (const QString& candidate : familyCandidates(other)) {
            if (!preference.contains(candidate)) {
                preference << candidate;
            }
        }
    }
    return preference;
}

// static
QString CharacterPriority::insertFallbackFamily(
        const QString& styleSheet, const QString& family) {
    if (family.isEmpty()) {
        return styleSheet;
    }
    // A family name carrying a quote would break out of the one we wrap it in.
    // No real font is named this way; refusing is still cheaper than emitting a
    // stylesheet Qt will fail to parse for every rule after it.
    if (family.contains(QLatin1Char('\'')) || family.contains(QLatin1Char('"'))) {
        qWarning() << "CharacterPriority: refusing to splice quoted font family" << family;
        return styleSheet;
    }

    QString out;
    out.reserve(styleSheet.size() + 64);

    const int n = styleSheet.size();
    int i = 0;
    // The last character that could have ended a declaration. A `font-family`
    // is a property only where a property can start — after `{` or `;`, never
    // in a selector such as `WLabel[font-family="x"]`. Comments do not move it,
    // so `{ /* note */ font-family: …` still counts as the start of one.
    QChar lastSignificant = QLatin1Char('{');

    while (i < n) {
        const QChar c = styleSheet.at(i);

        // Comments and quoted strings pass through untouched, so a
        // `font-family` written inside either is never mistaken for a
        // declaration.
        if (c == QLatin1Char('/') && i + 1 < n && styleSheet.at(i + 1) == QLatin1Char('*')) {
            const int end = styleSheet.indexOf(QLatin1String("*/"), i + 2);
            const int stop = end < 0 ? n : end + 2;
            out.append(styleSheet.mid(i, stop - i));
            i = stop;
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            const int end = styleSheet.indexOf(c, i + 1);
            const int stop = end < 0 ? n : end + 1;
            out.append(styleSheet.mid(i, stop - i));
            i = stop;
            continue;
        }

        const bool atPropertyStart = lastSignificant == QLatin1Char('{') ||
                lastSignificant == QLatin1Char(';');
        if (atPropertyStart &&
                QStringView(styleSheet).mid(i).startsWith(
                        kFontFamilyProperty, Qt::CaseInsensitive)) {
            int cursor = i + static_cast<int>(kFontFamilyProperty.size());
            // `font-family-ish` is a different property, whatever it means.
            const bool isWholeProperty =
                    cursor >= n || !isIdentChar(styleSheet.at(cursor));
            while (cursor < n && styleSheet.at(cursor).isSpace()) {
                ++cursor;
            }
            if (isWholeProperty && cursor < n && styleSheet.at(cursor) == QLatin1Char(':')) {
                // The value runs to the terminator; a font-family value holds
                // no braces or nested declarations, only families.
                int end = cursor + 1;
                QChar quote;
                while (end < n) {
                    const QChar v = styleSheet.at(end);
                    if (!quote.isNull()) {
                        if (v == quote) {
                            quote = QChar();
                        }
                    } else if (v == QLatin1Char('\'') || v == QLatin1Char('"')) {
                        quote = v;
                    } else if (v == QLatin1Char(';') || v == QLatin1Char('}')) {
                        break;
                    }
                    ++end;
                }
                out.append(styleSheet.mid(i, cursor + 1 - i));
                out.append(spliceFamily(
                        styleSheet.mid(cursor + 1, end - (cursor + 1)), family));
                lastSignificant = QLatin1Char(';');
                i = end;
                continue;
            }
        }

        if (!c.isSpace()) {
            lastSignificant = c;
        }
        out.append(c);
        ++i;
    }

    return out;
}

// static
QFont CharacterPriority::withFallbackFamily(const QFont& font, const QString& family) {
    if (family.isEmpty()) {
        return font;
    }
    QStringList families = font.families();
    if (families.isEmpty()) {
        families << font.family();
    }
    if (families.contains(family, Qt::CaseInsensitive)) {
        return font;
    }
    // Behind the primary family, ahead of everything else: the widget keeps the
    // Latin face it asked for and only the glyphs that face does not have come
    // from the CJK font.
    families.insert(std::min(1, static_cast<int>(families.size())), family);
    QFont out = font;
    out.setFamilies(families);
    return out;
}

QString CharacterPriority::uiLocale() const {
    const QString configured =
            m_pConfig->getValueString(ConfigKey(kLocaleGroup, kLocaleItem));
    return configured.isEmpty() ? QLocale::system().name() : configured;
}

void CharacterPriority::updateFamily() {
    const QStringList installed = QFontDatabase::families();
    const QStringList preference = familyPreference(resolve(m_priority, uiLocale()));
    m_family.clear();
    for (const QString& candidate : preference) {
        if (installed.contains(candidate, Qt::CaseInsensitive)) {
            m_family = candidate;
            break;
        }
    }
    if (m_family.isEmpty()) {
        // Not fatal and not worth a notification: a unit with no CJK font
        // installed renders exactly as it did before this setting existed.
        qWarning() << "CharacterPriority: no CJK font installed, tried" << preference;
    } else {
        qDebug() << "CharacterPriority: drawing CJK text with" << m_family;
    }

    // The skin's stylesheet covers everything the DJ normally sees, but not the
    // error dialogs and native menus that appear when something has already
    // gone wrong — those take the application font. Left entirely alone when
    // there is no family to add: setting the application font explicitly is not
    // free, it stops the platform theme's own font from propagating, and a unit
    // with no CJK font is supposed to render exactly as it did before.
    if (!m_family.isEmpty()) {
        QApplication::setFont(withFallbackFamily(m_baseApplicationFont, m_family));
    }
}

void CharacterPriority::onPriorityChanged(double value) {
    const Priority priority = priorityFromValue(value);
    if (priority == m_priority) {
        return;
    }
    m_priority = priority;
    // Persist and flush immediately, same reason as daylight mode and the
    // screen rotation: an appliance gets hard-powered-off, and the exit-time
    // save may never run.
    m_pConfig->setValue(ConfigKey(kGroup, kItem), static_cast<double>(m_priority));
    m_pConfig->save();
    updateFamily();
    emit priorityChanged();
}
