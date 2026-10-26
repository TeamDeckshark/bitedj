#include "mixer/crossfaderassignlock.h"

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "engine/channels/enginechannel.h"
#include "engine/enginexfader.h"
#include "mixer/playermanager.h"
#include "moc_crossfaderassignlock.cpp"
#include "notifications/notifications.h"
#include "soundio/soundmanager.h"

namespace {

const QString kAppGroup = QStringLiteral("[App]");
const QString kSoundManagerGroup = QStringLiteral("[SoundManager]");
const QString kMasterGroup = QStringLiteral("[Master]");
const QString kXfaderProfileGroup = QString::fromLatin1(EngineXfader::kXfaderConfigKey);

} // namespace

CrossfaderAssignLock::CrossfaderAssignLock() {
    m_pCoEnabled = std::make_unique<ControlObject>(
            ConfigKey(QStringLiteral("[BiteDJ]"), QStringLiteral("xfader_assign_enabled")));
    m_pCoEnabled->setReadOnly();

    m_pSoundStatus = watch(kSoundManagerGroup, QStringLiteral("status"));
    m_pCrossfader = watch(kMasterGroup, QStringLiteral("crossfader"));
    m_pXfaderCurve = watch(kXfaderProfileGroup, QStringLiteral("xFaderCurve"));
    m_pXfaderCalibration = watch(kXfaderProfileGroup, QStringLiteral("xFaderCalibration"));
    m_pXfaderMode = watch(kXfaderProfileGroup, QStringLiteral("xFaderMode"));
    m_pXfaderReverse = watch(kXfaderProfileGroup, QStringLiteral("xFaderReverse"));

    // Decks are created before this object (CoreServices adds them well before
    // the settings objects); num_decks only covers a deck added later.
    m_pNumDecks = new ControlProxy(kAppGroup, QStringLiteral("num_decks"), this);
    m_pNumDecks->connectValueChanged(this, [this](double) { addNewDecks(); });
    addNewDecks();
}

CrossfaderAssignLock::~CrossfaderAssignLock() = default;

ControlProxy* CrossfaderAssignLock::watch(const QString& group, const QString& item) {
    // Play and the deck controls change on the engine thread; the proxies queue
    // those onto the GUI thread like every other CO subscription in the skin.
    auto* pProxy = new ControlProxy(group, item, this);
    pProxy->connectValueChanged(this, &CrossfaderAssignLock::updateEnabled);
    return pProxy;
}

void CrossfaderAssignLock::addNewDecks() {
    const int numDecks = static_cast<int>(m_pNumDecks->get());
    for (int i = static_cast<int>(m_decks.size()); i < numDecks; ++i) {
        const QString group = PlayerManager::groupForDeck(i);
        ControlObject* pOrientation = ControlObject::getControl(
                ConfigKey(group, QStringLiteral("orientation")));
        if (!pOrientation) {
            // Not created yet; retried on the next num_decks change.
            break;
        }
        // The only request handler on the CO: EngineChannel sets orientation
        // through plain set(), so its own orientation_left/right/center push
        // buttons land here too.
        pOrientation->connectValueChangeRequest(this,
                [this, pOrientation](double value) {
                    onOrientationChangeRequest(pOrientation, value);
                });
        m_decks.append(DeckWatch{
                watch(group, QStringLiteral("play")),
                watch(group, QStringLiteral("track_loaded")),
                watch(group, QStringLiteral("mute")),
                watch(group, QStringLiteral("volume")),
                watch(group, QStringLiteral("pregain")),
                watch(group, QStringLiteral("orientation")),
        });
    }
    updateEnabled();
}

bool CrossfaderAssignLock::isLocked() const {
    if (static_cast<int>(m_pSoundStatus->get()) != SOUNDMANAGER_CONNECTED) {
        return false;
    }
    CSAMPLE_GAIN leftGain;
    CSAMPLE_GAIN rightGain;
    EngineXfader::getXfadeGains(m_pCrossfader->get(),
            m_pXfaderCurve->get(),
            m_pXfaderCalibration->get(),
            m_pXfaderMode->get(),
            m_pXfaderReverse->toBool(),
            &leftGain,
            &rightGain);
    for (const DeckWatch& deck : m_decks) {
        if (!deck.pPlay->toBool() || !deck.pTrackLoaded->toBool() ||
                deck.pMute->toBool() || deck.pVolume->get() <= 0.0 ||
                deck.pPregain->get() <= 0.0) {
            continue;
        }
        switch (static_cast<int>(deck.pOrientation->get())) {
        case EngineChannel::LEFT:
            if (leftGain > 0) {
                return true;
            }
            break;
        case EngineChannel::RIGHT:
            if (rightGain > 0) {
                return true;
            }
            break;
        default:
            return true;
        }
    }
    return false;
}

void CrossfaderAssignLock::updateEnabled() {
    m_pCoEnabled->forceSet(isLocked() ? 0.0 : 1.0);
}

void CrossfaderAssignLock::onOrientationChangeRequest(
        ControlObject* pOrientation, double value) {
    // Decided on the live state rather than the CO the skin greys out with, so
    // a request queued from the controller thread can't slip through between
    // a deck starting and updateEnabled() catching up.
    if (isLocked()) {
        if (auto* pNotifications = Notifications::tryInstance()) {
            pNotifications->publish(
                    tr("Can't change crossfader assignments while a deck is audible"),
                    Notifications::Severity::Warning);
        }
        return;
    }
    pOrientation->setAndConfirm(value);
}
