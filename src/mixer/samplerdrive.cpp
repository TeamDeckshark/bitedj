#include "mixer/samplerdrive.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>

#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "control/controlpushbutton.h"
#include "library/dao/fssamplerbankstore.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "mixer/sampler.h"
#include "moc_samplerdrive.cpp"
#include "notifications/notifications.h"
#include "preferences/systemsettings.h"
#include "track/track.h"
#include "util/usbdevice.h"

namespace {

const QString kGroup = QStringLiteral("[Samplers]");

const ConfigKey kConfigKeyDriveUuid(kGroup, QStringLiteral("drive_uuid"));
const ConfigKey kConfigKeyDriveMount(kGroup, QStringLiteral("drive_mount"));
const ConfigKey kConfigKeyDriveLabel(kGroup, QStringLiteral("drive_label"));

// How long a restore may spend settling before the row is taken as it stands.
// A slot whose file has gone from the drive never reports back at all, and the
// bank must not stay write-locked for the rest of the set because of it.
constexpr int kRestoreWatchdogMillis = 5000;

void notify(const QString& message, Notifications::Severity severity) {
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        pNotifications->publish(message, severity);
    }
}

} // anonymous namespace

QAtomicPointer<SamplerDrive> SamplerDrive::s_pInstance = nullptr;

bool SamplerDrive::Selection::matches(const Drive& drive) const {
    if (!uuid.isEmpty() && !drive.uuid.isEmpty()) {
        return uuid == drive.uuid;
    }
    // No UUID on one side or the other (a tmpfs, an exotic filesystem): the
    // mount point is all there is to go on. Weaker — a different stick that
    // lands on the same path passes — but the alternative is no persistence.
    return !mountPoint.isEmpty() && mountPoint == drive.mountPoint;
}

// Deliberately unparented: CoreServices owns this through a unique_ptr and
// drops it just before the PlayerManager it watches, so a QObject parent would
// only set up a double delete.
SamplerDrive::SamplerDrive(UserSettingsPointer pConfig, PlayerManager* pPlayerManager)
        : m_pConfig(pConfig),
          m_pPlayerManager(pPlayerManager) {
    DEBUG_ASSERT(m_pPlayerManager);
    s_pInstance.storeRelease(this);

    m_banks.resize(kBankCount);

    // Created the way the skin would have created it (see
    // LegacySkinParser::controlFromConfigKey), so that parsing samplers.xml
    // later finds this one and binds the SOURCE segments to it.
    auto pSourceDeck = std::make_unique<ControlPushButton>(
            ConfigKey(kGroup, QStringLiteral("source_deck_index")));
    pSourceDeck->setButtonMode(ControlPushButton::TOGGLE);
    m_pCoSourceDeck = std::move(pSourceDeck);
    connect(m_pCoSourceDeck.get(),
            &ControlObject::valueChanged,
            this,
            [this](double) {
                updateCanLoad();
                // The DJ just tapped a source; if it cannot be used, this is
                // the moment to say why (the LOAD buttons are greyed out, so
                // they cannot report it themselves).
                notifySourceUnusable();
            });

    m_pCoCanLoad = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("can_load")));
    m_pCoCanLoad->setReadOnly();
    m_pCoDriveMounted = std::make_unique<ControlObject>(
            ConfigKey(kGroup, QStringLiteral("drive_mounted")));
    m_pCoDriveMounted->setReadOnly();

    connect(m_pPlayerManager,
            &PlayerManagerInterface::numberOfDecksChanged,
            this,
            [this] { rebuildConnections(); });
    connect(m_pPlayerManager,
            &PlayerManagerInterface::numberOfSamplersChanged,
            this,
            [this] { rebuildConnections(); });
    rebuildConnections();

    if (SystemSettings* pSettings = SystemSettings::tryInstance()) {
        // The mount list is what the selection is resolved against, so every
        // plug and unplug has to re-run that resolution — this is the signal
        // the old per-deck design never listened to, which is why a re-plugged
        // stick restored nothing.
        connect(pSettings,
                &SystemSettings::usbRowsChanged,
                this,
                [this](const QStringList&) { onMountsChanged(); });
    }

    loadSelection();
    refreshDrives();
}

SamplerDrive::~SamplerDrive() {
    s_pInstance.storeRelease(nullptr);
}

void SamplerDrive::rebuildConnections() {
    const int deckCount = m_pPlayerManager->numberOfDecks();
    for (int deckIndex = m_connectedDecks; deckIndex < deckCount; ++deckIndex) {
        BaseTrackPlayer* pDeck = m_pPlayerManager->getDeckBase(deckIndex);
        if (!pDeck) {
            break;
        }
        // Only ever affects whether LOAD is offered: what a deck holds decides
        // whether it can be cloned into a slot, nothing more.
        connect(pDeck,
                &BaseTrackPlayer::newTrackLoaded,
                this,
                [this](TrackPointer) { updateCanLoad(); });
        connect(pDeck,
                &BaseTrackPlayer::trackUnloaded,
                this,
                [this](TrackPointer) { updateCanLoad(); });
        m_connectedDecks = deckIndex + 1;
    }

    const int samplerCount = m_pPlayerManager->numberOfSamplers();
    for (int samplerIndex = m_connectedSamplers; samplerIndex < samplerCount;
            ++samplerIndex) {
        Sampler* pSampler = m_pPlayerManager->getSampler(samplerIndex);
        if (!pSampler) {
            break;
        }
        // Both directions matter: a slot the DJ fills reports newTrackLoaded,
        // one they empty reports only trackUnloaded.
        connect(pSampler,
                &BaseTrackPlayer::newTrackLoaded,
                this,
                [this, samplerIndex] { onSamplerChanged(samplerIndex); });
        connect(pSampler,
                &BaseTrackPlayer::trackUnloaded,
                this,
                [this, samplerIndex] { onSamplerChanged(samplerIndex); });

        // A slot that is playing when its row changes keeps its sample until it
        // stops, so the stop is an event this has to see.
        auto* pPlay = new ControlProxy(
                PlayerManager::groupForSampler(samplerIndex),
                QStringLiteral("play"),
                this);
        pPlay->connectValueChanged(this, [this, samplerIndex](double value) {
            onSamplerPlayChanged(samplerIndex, value > 0);
        });
        m_samplerPlayProxies.append(pPlay);
        m_connectedSamplers = samplerIndex + 1;
    }
}

QList<SamplerDrive::Drive> SamplerDrive::enumerateDrives() const {
    QList<Drive> drives;
    SystemSettings* pSettings = SystemSettings::tryInstance();
    if (!pSettings) {
        // No removable-media enumeration in this build: the feature is inert
        // rather than wrong — nothing is selected, so nothing is saved and no
        // load is refused.
        return drives;
    }
    const QStringList mountPoints = pSettings->usbMountPoints();
    drives.reserve(mountPoints.size());
    for (const QString& mountPoint : mountPoints) {
        Drive drive;
        drive.mountPoint = mountPoint;
        drive.uuid = mixxx::volumeUuidForMountPoint(mountPoint);
        // The mountpoint's last path component is the volume name the
        // automounter used, which is what the DJ sees printed on the stick —
        // and what the USB list in Settings shows for the same drive.
        drive.label = QDir(mountPoint).dirName();
        if (drive.label.isEmpty()) {
            drive.label = mountPoint;
        }
        drives.append(drive);
    }
    return drives;
}

QStringList SamplerDrive::driveLabels() const {
    QStringList labels;
    labels.reserve(m_drives.size());
    for (const Drive& drive : m_drives) {
        labels.append(drive.label);
    }
    return labels;
}

int SamplerDrive::selectedDriveIndex() const {
    if (m_selection.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < m_drives.size(); ++i) {
        if (m_selection.matches(m_drives.at(i))) {
            return i;
        }
    }
    return -1;
}

void SamplerDrive::loadSelection() {
    m_selection.uuid = m_pConfig->getValue(kConfigKeyDriveUuid, QString());
    m_selection.mountPoint = m_pConfig->getValue(kConfigKeyDriveMount, QString());
    m_selection.label = m_pConfig->getValue(kConfigKeyDriveLabel, QString());
}

void SamplerDrive::storeSelection() {
    m_pConfig->set(kConfigKeyDriveUuid, ConfigValue(m_selection.uuid));
    m_pConfig->set(kConfigKeyDriveMount, ConfigValue(m_selection.mountPoint));
    m_pConfig->set(kConfigKeyDriveLabel, ConfigValue(m_selection.label));
}

void SamplerDrive::onMountsChanged() {
    refreshDrives();
}

void SamplerDrive::refreshDrives() {
    m_drives = enumerateDrives();

    QString resolvedRoot;
    if (!m_selection.isEmpty()) {
        for (const Drive& drive : std::as_const(m_drives)) {
            if (!m_selection.matches(drive)) {
                continue;
            }
            resolvedRoot = drive.mountPoint;
            if (m_selection.uuid != drive.uuid ||
                    m_selection.mountPoint != drive.mountPoint ||
                    m_selection.label != drive.label) {
                // Same stick, re-plugged somewhere else (or seen with a UUID
                // for the first time). Keep the stored identity current so the
                // next restart still finds it.
                m_selection.uuid = drive.uuid;
                m_selection.mountPoint = drive.mountPoint;
                m_selection.label = drive.label;
                storeSelection();
            }
            break;
        }
    } else if (m_drives.size() == 1) {
        // Nothing chosen yet and exactly one stick in the unit: that is the
        // sample drive as surely as if it had been tapped, and the samplers
        // work out of the box. With two plugged in the DJ has to say which —
        // guessing would build a set on the wrong stick.
        const Drive& drive = m_drives.first();
        m_selection.uuid = drive.uuid;
        m_selection.mountPoint = drive.mountPoint;
        m_selection.label = drive.label;
        storeSelection();
        resolvedRoot = drive.mountPoint;
    }

    setMountRoot(resolvedRoot);
    updateCanLoad();
    emit drivesChanged(driveLabels(), selectedDriveIndex());
}

void SamplerDrive::selectDriveAt(int index) {
    Selection selection;
    if (index >= 0 && index < m_drives.size()) {
        const Drive& drive = m_drives.at(index);
        selection.uuid = drive.uuid;
        selection.mountPoint = drive.mountPoint;
        selection.label = drive.label;
        if (drive.mountPoint == m_mountRoot) {
            // Already the sample drive; re-running the switch would empty and
            // refill the grid for nothing.
            return;
        }
    }
    m_selection = selection;
    storeSelection();
    setMountRoot(selection.mountPoint);
    updateCanLoad();
    emit drivesChanged(driveLabels(), selectedDriveIndex());
}

void SamplerDrive::setMountRoot(const QString& mountRoot) {
    const QString cleanRoot = mountRoot.isEmpty() ? QString() : QDir::cleanPath(mountRoot);
    if (cleanRoot == m_mountRoot) {
        return;
    }

    // Whatever the grid holds belongs to the drive it is leaving, so push it
    // there before repointing — otherwise a swap loses the last edit. A write
    // to a drive that has been pulled out fails harmlessly: the store refuses a
    // mount point that is no longer a mount point.
    for (int bank = 0; bank < kBankCount; ++bank) {
        flushBank(bank);
    }

    m_mountRoot = cleanRoot;
    for (Bank& bank : m_banks) {
        bank.baseline = QByteArray();
        bank.restoring = false;
        bank.restoreAwaiting.clear();
        // Retire any restore still outstanding for the drive being left.
        ++bank.restoreGeneration;
    }
    m_pCoDriveMounted->forceSet(m_mountRoot.isEmpty() ? 0.0 : 1.0);

    if (m_mountRoot.isEmpty()) {
        // The samples are on a drive that is not here; the grid says so.
        clearAllSlots();
    } else {
        restoreAllBanks();
    }
}

void SamplerDrive::restoreAllBanks() {
    for (int bank = 0; bank < kBankCount; ++bank) {
        if (!hasBank(bank)) {
            continue;
        }
        QStringList stored;
        if (!FsSamplerBankStore::readBank(m_mountRoot,
                    bankIndexFor(bank),
                    kSamplersPerBank,
                    &stored)) {
            // This drive holds no bank for this row yet. The row is emptied
            // rather than left as it was: its slots came from a different
            // drive, and a sample this one does not have is not part of the
            // set it stores. The first slot the DJ fills writes the bank.
            stored = QStringList(kSamplersPerBank, QString());
        }
        applyBank(bank, stored);
    }
}

void SamplerDrive::applyBank(int bank, const QStringList& stored) {
    if (!hasBank(bank)) {
        return;
    }
    Bank& bankState = m_banks[bank];

    // Decide the whole row before loading any of it. The restore has to be
    // armed first: BaseTrackPlayerImpl::slotLoadTrack unloads what a slot holds
    // *synchronously*, so the very first load below re-enters onSamplerChanged
    // with the row half empty — and unarmed, that partial row would be written
    // straight to the drive the grid has only just been pointed at.
    QStringList expected;
    expected.reserve(kSamplersPerBank);
    for (int slot = 0; slot < kSamplersPerBank; ++slot) {
        const int samplerIndex = bank * kSamplersPerBank + slot;
        QString target = stored.at(slot);
        if (!target.isEmpty() && !QFileInfo::exists(target)) {
            // The bank names a sample that is no longer on the drive. Empty the
            // slot rather than asking for it: slotLoadTrack refuses a missing
            // file with a notification of its own, and an automatic restore is
            // not worth putting one of those on the screen for — let alone one
            // per sample the DJ has since deleted.
            target.clear();
        }
        expected.append(target);
        if (isSamplerPlaying(samplerIndex)) {
            // Never pull a sample out from under the DJ mid-set. The slot is
            // owed this one and gets it when it stops; until then the row is
            // accounted for as though it had already landed.
            m_pendingSlots.insert(samplerIndex, target);
        } else {
            m_pendingSlots.remove(samplerIndex);
        }
    }

    bankState.restoreExpected = expected;
    bankState.restoreTarget = FsSamplerBankStore::serializeBank(m_mountRoot, expected);
    bankState.restoreAwaiting.clear();
    for (int slot = 0; slot < kSamplersPerBank; ++slot) {
        const int samplerIndex = bank * kSamplersPerBank + slot;
        // A deferred slot is accounted for already; a slot that holds what it
        // was asked for has nothing to do.
        if (!m_pendingSlots.contains(samplerIndex) &&
                expected.at(slot) != samplerLocation(samplerIndex)) {
            bankState.restoreAwaiting.insert(slot);
        }
    }
    if (bankState.restoreAwaiting.isEmpty()) {
        // The row already holds this bank; there is nothing to load or wait for.
        bankState.restoring = false;
        bankState.baseline = bankState.restoreTarget;
        return;
    }

    bankState.restoring = true;
    const int generation = ++bankState.restoreGeneration;
    QTimer::singleShot(kRestoreWatchdogMillis, this, [this, bank, generation] {
        onRestoreTimeout(bank, generation);
    });

    // Only now, with writes suppressed, are the samplers touched.
    const QSet<int> awaiting = bankState.restoreAwaiting;
    for (int slot : awaiting) {
        loadSlot(bank * kSamplersPerBank + slot, expected.at(slot));
    }
}

void SamplerDrive::clearAllSlots() {
    for (int bank = 0; bank < kBankCount; ++bank) {
        if (!hasBank(bank)) {
            continue;
        }
        for (int slot = 0; slot < kSamplersPerBank; ++slot) {
            const int samplerIndex = bank * kSamplersPerBank + slot;
            if (isSamplerPlaying(samplerIndex)) {
                m_pendingSlots.insert(samplerIndex, QString());
                continue;
            }
            m_pendingSlots.remove(samplerIndex);
            if (!samplerLocation(samplerIndex).isEmpty()) {
                loadSlot(samplerIndex, QString());
            }
        }
    }
}

void SamplerDrive::onRestoreTimeout(int bank, int generation) {
    if (bank < 0 || bank >= m_banks.size()) {
        return;
    }
    Bank& bankState = m_banks[bank];
    if (!bankState.restoring || bankState.restoreGeneration != generation) {
        return;
    }
    // A slot in the stored bank never arrived: its file is gone from the drive,
    // or the stick was pulled mid-restore. Take the row as it actually stands
    // as the baseline so later edits save again, but write nothing — a sample
    // that failed to load is no reason to drop its path from the drive.
    bankState.restoring = false;
    bankState.restoreAwaiting.clear();
    bankState.baseline = FsSamplerBankStore::serializeBank(
            m_mountRoot, effectiveBankLocations(bank));
}

void SamplerDrive::loadSlot(int samplerIndex, const QString& location) {
    const QString group = PlayerManager::groupForSampler(samplerIndex);
    if (location.isEmpty()) {
        m_pPlayerManager->slotLoadTrackToPlayer(TrackPointer(), group, false);
    } else {
        m_pPlayerManager->slotLoadLocationToPlayer(location, group, false);
    }
}

void SamplerDrive::applyPending(int samplerIndex) {
    const QString target = m_pendingSlots.take(samplerIndex);
    if (target == samplerLocation(samplerIndex)) {
        return;
    }
    loadSlot(samplerIndex, target);
}

void SamplerDrive::onSamplerPlayChanged(int samplerIndex, bool playing) {
    if (playing || !m_pendingSlots.contains(samplerIndex)) {
        return;
    }
    // The sample the DJ was listening to has finished; the change it was
    // holding up can land now.
    applyPending(samplerIndex);
}

void SamplerDrive::onSamplerChanged(int samplerIndex) {
    const int bank = samplerIndex / kSamplersPerBank;
    if (!hasBank(bank)) {
        return;
    }
    Bank& bankState = m_banks[bank];
    if (!bankState.restoring) {
        // Whatever the slot holds now is what somebody put there deliberately —
        // the DJ, or our own deferred apply. Either way the promise made to it
        // while it was playing is spent.
        m_pendingSlots.remove(samplerIndex);
    }
    if (m_mountRoot.isEmpty()) {
        // No drive: nowhere for the row to go. This is also what makes the
        // unloads of an eject (and of a drive going away) harmless.
        return;
    }

    if (bankState.restoring) {
        // Slots settling one by one as the restore's loads complete. Every
        // state on the way there is a partial row that must not be written back
        // over the drive's bank, so nothing is saved until the last slot the
        // restore asked for has arrived — checked slot by slot, since the
        // synchronous unload inside slotLoadTrack reports a slot that is
        // emphatically not there yet.
        // Over a copy: the set is being pruned as it is walked.
        const QSet<int> awaiting = bankState.restoreAwaiting;
        for (int slot : awaiting) {
            if (effectiveSlotLocation(bank * kSamplersPerBank + slot) ==
                    bankState.restoreExpected.at(slot)) {
                bankState.restoreAwaiting.remove(slot);
            }
        }
        if (!bankState.restoreAwaiting.isEmpty()) {
            return;
        }
        // The drive holds the row the restore asked for. Anything the row shows
        // beyond that is an edit the DJ made while it was settling, and falls
        // through to the save below rather than being lost.
        bankState.restoring = false;
        bankState.baseline = bankState.restoreTarget;
    }

    const QStringList locations = effectiveBankLocations(bank);
    const QByteArray payload = FsSamplerBankStore::serializeBank(m_mountRoot, locations);
    if (payload == bankState.baseline) {
        return;
    }
    if (FsSamplerBankStore::writeBank(m_mountRoot, bankIndexFor(bank), locations)) {
        bankState.baseline = payload;
    }
}

void SamplerDrive::flushBank(int bank) {
    if (m_mountRoot.isEmpty() || !hasBank(bank)) {
        return;
    }
    Bank& bankState = m_banks[bank];
    if (bankState.restoring) {
        return;
    }
    const QStringList locations = effectiveBankLocations(bank);
    const QByteArray payload = FsSamplerBankStore::serializeBank(m_mountRoot, locations);
    if (payload == bankState.baseline) {
        return;
    }
    if (FsSamplerBankStore::writeBank(m_mountRoot, bankIndexFor(bank), locations)) {
        bankState.baseline = payload;
    }
}

QString SamplerDrive::samplerLocation(int samplerIndex) const {
    Sampler* pSampler = m_pPlayerManager->getSampler(samplerIndex);
    const TrackPointer pTrack = pSampler ? pSampler->getLoadedTrack() : TrackPointer();
    return pTrack ? pTrack->getLocation() : QString();
}

QString SamplerDrive::effectiveSlotLocation(int samplerIndex) const {
    const auto pending = m_pendingSlots.constFind(samplerIndex);
    return pending != m_pendingSlots.constEnd() ? pending.value()
                                                : samplerLocation(samplerIndex);
}

QStringList SamplerDrive::effectiveBankLocations(int bank) const {
    QStringList locations;
    locations.reserve(kSamplersPerBank);
    for (int slot = 0; slot < kSamplersPerBank; ++slot) {
        locations.append(effectiveSlotLocation(bank * kSamplersPerBank + slot));
    }
    return locations;
}

bool SamplerDrive::hasBank(int bank) const {
    return bank >= 0 && bank < kBankCount &&
            m_pPlayerManager->numberOfSamplers() >= (bank + 1) * kSamplersPerBank;
}

bool SamplerDrive::isSamplerPlaying(int samplerIndex) const {
    return ControlObject::toBool(ConfigKey(
            PlayerManager::groupForSampler(samplerIndex), QStringLiteral("play")));
}

bool SamplerDrive::isOnSelectedDrive(const QString& location) const {
    if (m_mountRoot.isEmpty() || location.isEmpty()) {
        return false;
    }
    const QString absPath = QFileInfo(location).absoluteFilePath();
    const QString relPath = QDir(m_mountRoot).relativeFilePath(absPath);
    if (relPath.isEmpty() || relPath.startsWith(QLatin1String("..")) ||
            QDir::isAbsolutePath(relPath)) {
        return false;
    }
    // "On the drive" means the file is there to be played, not merely that its
    // path would resolve there — a bank naming a sample the DJ has since
    // deleted must not make LOAD look available.
    return QFileInfo::exists(absPath);
}

void SamplerDrive::suppressSavesTo(const QString& mountRoot) {
    if (m_mountRoot.isEmpty() || m_mountRoot != QDir::cleanPath(mountRoot)) {
        return;
    }
    // Forgetting the drive is enough: onSamplerChanged has nowhere to put a
    // row, so the unloads the eject is about to cause write nothing. The
    // selection is kept, so the banks come back when the drive does.
    m_mountRoot.clear();
    for (Bank& bank : m_banks) {
        bank.baseline = QByteArray();
        bank.restoring = false;
        bank.restoreAwaiting.clear();
        ++bank.restoreGeneration;
    }
    m_pCoDriveMounted->forceSet(0.0);
    updateCanLoad();
}

bool SamplerDrive::canLoadFromSourceDeck() const {
    const int deckIndex = static_cast<int>(m_pCoSourceDeck->get());
    if (deckIndex < 0 || deckIndex >= m_pPlayerManager->numberOfDecks()) {
        return false;
    }
    BaseTrackPlayer* pDeck = m_pPlayerManager->getDeckBase(deckIndex);
    if (!pDeck) {
        return false;
    }
    const TrackPointer pTrack = pDeck->getLoadedTrack();
    if (!pTrack) {
        return false;
    }
    return isOnSelectedDrive(pTrack->getLocation());
}

void SamplerDrive::updateCanLoad() {
    m_pCoCanLoad->forceSet(canLoadFromSourceDeck() ? 1.0 : 0.0);
}

void SamplerDrive::notifySourceUnusable() {
    const int deckIndex = static_cast<int>(m_pCoSourceDeck->get());
    BaseTrackPlayer* pDeck = deckIndex >= 0 && deckIndex < m_pPlayerManager->numberOfDecks()
            ? m_pPlayerManager->getDeckBase(deckIndex)
            : nullptr;
    const TrackPointer pTrack = pDeck ? pDeck->getLoadedTrack() : TrackPointer();
    if (!pTrack) {
        // An empty deck is not a source the DJ can have meant to use; the
        // segment is greyed out for it already.
        return;
    }
    if (isOnSelectedDrive(pTrack->getLocation())) {
        return;
    }
    if (m_mountRoot.isEmpty()) {
        notify(m_selection.label.isEmpty()
                        ? tr("Select a USB drive for the samplers first.")
                        : tr("%1 is not plugged in.").arg(m_selection.label),
                Notifications::Severity::Warning);
        return;
    }
    notify(tr("Deck %1 is not playing from %2, so it cannot be sampled.")
                    .arg(QString::number(deckIndex + 1), m_selection.label),
            Notifications::Severity::Warning);
}

bool SamplerDrive::refuseSampleLoad(const QString& group, const QString& location) {
    int samplerNumber = 0;
    if (!PlayerManager::isSamplerGroup(group, &samplerNumber)) {
        return false;
    }
    const int bank = (samplerNumber - 1) / kSamplersPerBank;
    if (!hasBank(bank)) {
        // A sampler outside the two banks the skin shows is none of this
        // class's business, so it is not policed either.
        return false;
    }
    if (isOnSelectedDrive(location)) {
        return false;
    }
    if (m_banks.at(bank).restoring) {
        // A load this class asked for, arriving after the drive it was read
        // from went away. Refuse it — the file is gone — but silently: the DJ
        // did not ask for it and would get one message per restored slot.
        return true;
    }
    if (m_mountRoot.isEmpty()) {
        notify(m_selection.label.isEmpty()
                        ? tr("Select a USB drive for the samplers first.")
                        : tr("%1 is not plugged in.").arg(m_selection.label),
                Notifications::Severity::Warning);
    } else {
        notify(tr("Samples must be on %1.").arg(m_selection.label),
                Notifications::Severity::Warning);
    }
    return true;
}
