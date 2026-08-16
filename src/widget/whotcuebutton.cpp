#include "widget/whotcuebutton.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <algorithm>

#include "mixer/playerinfo.h"
#include "moc_whotcuebutton.cpp"
#include "track/track.h"
#include "widget/controlwidgetconnection.h"

namespace {
constexpr int kDefaultDimBrightThreshold = 127;

// Clear badge geometry, in unscaled skin pixels.
constexpr int kClearBadgeSize = 16;
constexpr int kClearBadgeMargin = 3;
// How far past the glyph the tap target reaches on each side.
constexpr int kClearBadgeTouchSlop = 5;

// The badge is only ever drawn on top of the cue's own colour, which may be
// anything from black to white, so it carries its own disc rather than trusting
// the pad underneath. The fill is opaque, not alpha-blended: over the default
// hotcue orange a translucent black reads as brown and takes the red down with
// it, and the whole point of the disc is that the contrast is the same whatever
// colour the DJ gave the cue. The rim is what keeps it visible on a cue that is
// itself near-black.
//
// Painted here rather than declared in QSS, so neither daylight-mode hook sees
// them - which is right: the colour they sit on is itself deliberately never
// inverted, so a badge that flipped with the mode would be the one thing on the
// pad fighting its background.
const QColor kClearBadgeBack(0x14, 0x14, 0x14);
const QColor kClearBadgeBackPressed(0x3a, 0x3a, 0x3a);
const QColor kClearBadgeRim(0xff, 0xff, 0xff, 60);
const QColor kClearBadgeGlyph(0xff, 0x51, 0x47);
const QColor kClearBadgeGlyphPressed(0xff, 0x8f, 0x84);
} // namespace

WHotcueButton::WHotcueButton(const QString& group, QWidget* pParent)
        : WPushButton(pParent),
          m_group(group),
          m_hotcue(Cue::kNoHotCue),
          m_hoverCueColor(false),
          m_pCoColor(nullptr),
          m_cueColorDimThreshold(kDefaultDimBrightThreshold),
          m_bCueColorDimmed(false),
          m_bCueColorIsLight(false),
          m_bCueColorIsDark(false),
          m_bClearBadge(false),
          m_bClearBadgePressed(false) {
}

void WHotcueButton::setup(const QDomNode& node, const SkinContext& context) {
    // Setup parent class.
    WPushButton::setup(node, context);

    bool ok;
    int hotcue = context.selectInt(node, QStringLiteral("Hotcue"), &ok);
    if (ok && hotcue > 0) {
        m_hotcue = hotcue - 1;
    } else {
        SKIN_WARNING(node,
                context,
                QStringLiteral("Hotcue index '%1' invalid")
                        .arg(context.selectString(node, QStringLiteral("Hotcue"))));
    }

    bool okay;
    m_cueColorDimThreshold = context.selectInt(node, QStringLiteral("DimBrightThreshold"), &okay);
    if (!okay) {
        m_cueColorDimThreshold = kDefaultDimBrightThreshold;
    }

    m_hoverCueColor = context.selectBool(node, QStringLiteral("Hover"), false);

    m_pConfig = context.getConfig();

    setFocusPolicy(Qt::NoFocus);

    m_pCoColor = make_parented<ControlProxy>(
            createConfigKey(QStringLiteral("color")),
            this,
            ControlFlag::NoAssertIfMissing);
    m_pCoColor->connectValueChanged(this, &WHotcueButton::slotColorChanged);
    slotColorChanged(m_pCoColor->get());

    m_pCoType = make_parented<ControlProxy>(
            createConfigKey(QStringLiteral("type")),
            this,
            ControlFlag::NoAssertIfMissing);
    m_pCoType->connectValueChanged(this, &WHotcueButton::slotTypeChanged);
    slotTypeChanged(m_pCoType->get());

    // Opt-in corner badge that clears the slot. Right-click is the only stock
    // way to delete a cue and a touch-only device never produces one, so a pad
    // grid otherwise has no way to free a slot at all.
    m_bClearBadge = context.selectBool(node, QStringLiteral("ClearBadge"), false);
    if (m_bClearBadge) {
        m_pCoClear = make_parented<ControlProxy>(
                getClearConfigKey(),
                this,
                ControlFlag::NoAssertIfMissing);
    }

    auto* pLeftConnection = new ControlParameterWidgetConnection(
            this,
            getLeftClickConfigKey(), // "activate"
            nullptr,
            ControlParameterWidgetConnection::DIR_FROM_WIDGET,
            ControlParameterWidgetConnection::EMIT_ON_PRESS_AND_RELEASE);
    addLeftConnection(pLeftConnection);

    auto* pDisplayConnection = new ControlParameterWidgetConnection(
            this,
            createConfigKey(QStringLiteral("status")),
            nullptr,
            ControlParameterWidgetConnection::DIR_TO_WIDGET,
            ControlParameterWidgetConnection::EMIT_NEVER);
    addConnection(pDisplayConnection);
    setDisplayConnection(pDisplayConnection);

    QDomNode con = context.selectNode(node, QStringLiteral("Connection"));
    if (!con.isNull()) {
        SKIN_WARNING(node, context, QStringLiteral("Additional Connections are not allowed"));
    }
}

bool WHotcueButton::clearBadgeVisible() {
    if (!m_bClearBadge || !readDisplayValue()) {
        return false;
    }
    // Don't crowd out the label on a pad too small to carry both.
    const QRect badge = clearBadgeRect();
    return width() >= badge.width() * 3 && height() >= badge.height() * 2;
}

QRect WHotcueButton::clearBadgeRect() {
    const int size = static_cast<int>(kClearBadgeSize * scaleFactor());
    const int margin = static_cast<int>(kClearBadgeMargin * scaleFactor());
    return QRect(width() - size - margin, margin, size, size);
}

QRect WHotcueButton::clearBadgeHitRect() {
    const int slop = static_cast<int>(kClearBadgeTouchSlop * scaleFactor());
    return clearBadgeRect().adjusted(-slop, -slop, slop, slop).intersected(rect());
}

void WHotcueButton::cancelClearBadgePress() {
    if (m_bClearBadgePressed) {
        m_bClearBadgePressed = false;
        update();
    }
}

void WHotcueButton::paintEvent(QPaintEvent* e) {
    WPushButton::paintEvent(e);

    if (!clearBadgeVisible()) {
        return;
    }

    const QRectF badge(clearBadgeRect());
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    p.setPen(QPen(kClearBadgeRim, 1.0));
    p.setBrush(m_bClearBadgePressed ? kClearBadgeBackPressed : kClearBadgeBack);
    p.drawEllipse(badge.adjusted(0.5, 0.5, -0.5, -0.5));

    const qreal inset = badge.width() * 0.3;
    const QRectF glyph = badge.adjusted(inset, inset, -inset, -inset);
    QPen pen(m_bClearBadgePressed ? kClearBadgeGlyphPressed : kClearBadgeGlyph);
    pen.setWidthF(std::max(1.5, badge.width() / 9.0));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.drawLine(glyph.topLeft(), glyph.bottomRight());
    p.drawLine(glyph.topRight(), glyph.bottomLeft());
}

void WHotcueButton::mousePressEvent(QMouseEvent* e) {
    const bool rightClick = e->button() == Qt::RightButton;
    if (rightClick) {
        if (isPressed()) {
            // Discard right clicks when already left clicked.
            // Otherwise the pop up menu receives the release event and the
            // button stucks in the pressed stage.
            return;
        }
        if (readDisplayValue()) {
            // hot cue is set
            TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(m_group);
            if (!pTrack) {
                return;
            }

            CuePointer pHotCue;
            const QList<CuePointer> cueList = pTrack->getCuePoints();
            for (const auto& pCue : cueList) {
                if (pCue->getHotCue() == m_hotcue) {
                    pHotCue = pCue;
                    break;
                }
            }
            if (!pHotCue) {
                return;
            }
            if (e->modifiers().testFlag(Qt::ShiftModifier)) {
                pTrack->removeCue(pHotCue);
                return;
            }
            WCueMenuPopup* pPopup = cueMenuPopup();
            pPopup->setTrackCueGroup(pTrack, pHotCue, m_group);
            // use the bottom left corner as starting point for popup
            pPopup->popup(mapToGlobal(QPoint(0, height())));
        }
        return;
    }

    if (e->button() == Qt::LeftButton && clearBadgeVisible() &&
            clearBadgeHitRect().contains(e->pos())) {
        // The badge is its own tap target inside the pad. Swallow the press so
        // the cue doesn't fire underneath it, and hold the action until the
        // release: a finger that lands on the badge by mistake can slide off
        // to cancel, which matters for something that deletes a cue mid-set.
        m_bClearBadgePressed = true;
        update();
        return;
    }

    // Pass all other press events to the base class.
    WPushButton::mousePressEvent(e);
}

void WHotcueButton::mouseReleaseEvent(QMouseEvent* e) {
    const bool rightClick = e->button() == Qt::RightButton;
    if (rightClick) {
        // Don't handle stray release events
        return;
    }

    if (m_bClearBadgePressed) {
        m_bClearBadgePressed = false;
        const bool onBadge = clearBadgeVisible() &&
                clearBadgeHitRect().contains(e->pos());
        update();
        if (onBadge && m_pCoClear) {
            // hotcue_N_clear is a push button, edge triggered on > 0.
            m_pCoClear->set(1.0);
            m_pCoClear->set(0.0);
        }
        // Never fall through to the base class: it would emit the activate
        // release for a press it never saw.
        return;
    }

    WPushButton::mouseReleaseEvent(e);
}

bool WHotcueButton::event(QEvent* e) {
    // WPushButton unsticks its own pressed state on these; the badge's is
    // separate and would otherwise stay lit with no release coming.
    if (e->type() == QEvent::Leave || e->type() == QEvent::WindowDeactivate) {
        cancelClearBadgePress();
    }
    return WPushButton::event(e);
}

WCueMenuPopup* WHotcueButton::cueMenuPopup() {
    if (!m_pCueMenuPopup) {
        m_pCueMenuPopup = make_parented<WCueMenuPopup>(m_pConfig, this);
        ColorPaletteSettings colorPaletteSettings(m_pConfig);
        m_pCueMenuPopup->setColorPalette(
                colorPaletteSettings.getHotcueColorPalette());
    }
    return m_pCueMenuPopup;
}

ConfigKey WHotcueButton::createConfigKey(const QString& name) {
    ConfigKey key;
    key.group = m_group;
    // Add one to hotcue so that we don't have a hotcue_0
    key.item = QStringLiteral("hotcue_") + QString::number(m_hotcue + 1) + QChar('_') + name;
    return key;
}

void WHotcueButton::slotColorChanged(double color) {
    VERIFY_OR_DEBUG_ASSERT(color >= 0 && color <= 0xFFFFFF) {
        return;
    }
    QColor cueColor = QColor::fromRgb(static_cast<QRgb>(color));
    m_bCueColorDimmed = Color::isDimColorCustom(cueColor, m_cueColorDimThreshold);

    QString style =
            QStringLiteral(
                    "WWidget[displayValue=\"1\"], "
                    "WWidget[displayValue=\"2\"] { background-color: ") +
            cueColor.name() +
            QStringLiteral("; }");

    if (m_hoverCueColor) {
        style +=
                QStringLiteral(
                        "WWidget[displayValue=\"1\"]:hover, "
                        "WWidget[displayValue=\"2\"]:hover { background-color: ") +
                cueColor.lighter(m_bCueColorDimmed ? 120 : 80).name() +
                QStringLiteral("; }");
    }

    setStyleSheet(style);
    restyleAndRepaint();
}

void WHotcueButton::slotTypeChanged(double type) {
    switch (static_cast<mixxx::CueType>(static_cast<int>(type))) {
    case mixxx::CueType::Invalid:
        m_type = QLatin1String("");
        break;
    case mixxx::CueType::HotCue:
        m_type = QStringLiteral("hotcue");
        break;
    case mixxx::CueType::MainCue:
        m_type = QStringLiteral("maincue");
        break;
    case mixxx::CueType::Beat:
        m_type = QStringLiteral("beat");
        break;
    case mixxx::CueType::Loop:
        m_type = QStringLiteral("loop");
        break;
    case mixxx::CueType::Jump:
        m_type = QStringLiteral("jump");
        break;
    case mixxx::CueType::Intro:
        m_type = QStringLiteral("intro");
        break;
    case mixxx::CueType::Outro:
        m_type = QStringLiteral("outro");
        break;
    case mixxx::CueType::N60dBSound:
        m_type = QStringLiteral("n60dbsound");
        break;
    default:
        DEBUG_ASSERT(!"Unknown cue type!");
        m_type = QLatin1String("");
    }
    restyleAndRepaint();
}

void WHotcueButton::restyleAndRepaint() {
    if (readDisplayValue()) {
        // Adjust properties for Qss file
        m_bCueColorIsLight = !m_bCueColorDimmed;
        m_bCueColorIsDark = m_bCueColorDimmed;
    } else {
        // We are now at the background set by qss.
        // Since we don't know the color reset both
        m_bCueColorIsLight = false;
        m_bCueColorIsDark = false;
    }
    WPushButton::restyleAndRepaint();
}
