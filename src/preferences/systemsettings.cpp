#include "preferences/systemsettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileInfoList>
#include <QProcess>
#include <QSet>
#include <QStorageInfo>
#include <QStringList>
#include <QThread>
#include <QtDebug>

#include "analyzer/trackanalysisscheduler.h"
#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "library/dao/fsanalysiscache.h"
#include "library/dao/fshistoryworker.h"
#include "mixer/basetrackplayer.h"
#include "mixer/playermanager.h"
#include "mixer/samplerdrive.h"
#include "mixer/previewdeck.h"
#include "mixer/sampler.h"
#include "moc_systemsettings.cpp"
#include "notifications/notifications.h"
#include "recording/defs_recording.h"
#include "recording/recordingmanager.h"
#include "track/track.h"
#include "util/usbdevice.h"

namespace {
const QString kGroup = QStringLiteral("[System]");
// Fork-wide preference namespace (distinct from the [System] tab COs above).
const QString kBiteDj = QStringLiteral("[BiteDJ]");
// Jog-wheel behaviour: 1 = Vinyl (touching the platter scratches), 0 = CDJ
// (platter pitch-bends, no scratch on touch). Defaults to Vinyl to preserve the
// controller mapping's historical hard-coded behaviour.
constexpr double kVinylModeDefault = 1.0;

// Vinyl brake (General settings tab). Seconds a jog wheel released at normal
// (1x) speed takes to coast to a standstill, so throwing the platter gives a
// backspin whose length the DJ picks here; 0 restores stock Mixxx's near-instant
// ramp. Read by ControllerScriptInterfaceLegacy::scratchDisable on each release,
// so a change applies to the very next throw. Defaults off (the Off segment in
// the settings tab) so a released platter behaves like stock Mixxx until the DJ
// asks for a backspin; Long is a Technics-like brake and Short is half of it.
constexpr double kVinylBrakeDefault = 0.0;

// Hot cue gating (General settings tab). Shares one key with the config value
// CueControl reads, so the CO seeds from it and writes straight back to it.
// 1 = ungated: a press jumps to the cue and plays on from there, even from a
// paused deck. 0 = gated: the deck previews the cue for as long as the button
// is held, then seeks back and stops, which is stock Mixxx behaviour.
const QString kControlsGroup = QStringLiteral("[Controls]");
const QString kHotcueActivatePlaysKey = QStringLiteral("HotcueActivatePlays");
constexpr double kHotcueActivatePlaysDefault = 1.0;

// Mixxx track-time display mode stored in mixxx.cfg:
// 0 = elapsed, 1 = remaining, 2 = elapsed and remaining.
const QString kPositionDisplayKey = QStringLiteral("PositionDisplay");
constexpr double kPositionDisplayRemaining = 1.0;
constexpr double kPositionDisplayElapsedAndRemaining = 2.0;

// Unloading a track is asynchronous, so the drive stays busy for a short while
// after we request the eject (see ejectRow). Retry the unmount while pumping the
// event loop, up to this many attempts spaced this many milliseconds apart
// (~1.2 s worst case) before giving up.
constexpr int kUnmountAttempts = 12;
constexpr int kUnmountRetryMs = 100;

// Number of [BiteDJ],eject_drive<N> COs, one per physical USB port
constexpr int kNumEjectDrives = 4;

// Folder a per-drive recording is written into, at the root of the stick. A
// plain visible directory rather than the hidden .bitedj store the cue/sampler
// databases use: the whole point of recording to the stick is that the DJ pulls
// it out and finds the set on it from another machine.
const QString kRecordingsSubdir = QStringLiteral("Recordings");

// How long to wait for the engine to confirm a recording actually started. The
// recorder opens its file on the audio thread on one of the next callbacks, so
// this only has to cover a handful of buffers — it is generous because the
// failure it is really there for is a start that can never be confirmed at all
// (no audio device configured, so no callback ever runs).
constexpr int kRecordingStartTimeoutMs = 5000;

// True when the block device (e.g. "/dev/sda1") sits on the USB device with
// the given sysfs topology name (e.g. "1-1.5"). Resolved through
// /sys/class/block, whose canonical path walks the full USB topology
// (.../usb1/1-1/1-1.5/1-1.5:1.0/host0/.../block/sda/sda1), so an exact
// path-component match identifies the physical port without touching
// /proc/mounts (see the mount-namespace note in enumerateUsbMounts).
bool deviceOnUsbPath(const QString& device, const QString& usbPath) {
    const QString name = QFileInfo(device).fileName();
    if (name.isEmpty()) {
        return false;
    }
    const QString sysfs =
            QFileInfo(QStringLiteral("/sys/class/block/") + name).canonicalFilePath();
    if (sysfs.isEmpty()) {
        return false;
    }
    return sysfs.split(QLatin1Char('/')).contains(usbPath);
}

void notify(const QString& message, Notifications::Severity severity) {
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        pNotifications->publish(message, severity);
    }
}
} // namespace

QAtomicPointer<SystemSettings> SystemSettings::s_pInstance = nullptr;

SystemSettings::SystemSettings(UserSettingsPointer pConfig,
        std::shared_ptr<PlayerManager> pPlayerManager,
        std::shared_ptr<RecordingManager> pRecordingManager)
        : m_pConfig(pConfig),
          m_pPlayerManager(std::move(pPlayerManager)),
          m_pRecordingManager(std::move(pRecordingManager)) {
    // BiteDJ has one visible track-time field, so Mixxx's combined elapsed and
    // remaining mode leaves it blank. Normalize an unset/empty value and the
    // upstream default (2) to remaining time (1). Preserve explicit elapsed
    // (0) and remaining (1) choices.
    const ConfigKey positionDisplayKey(kControlsGroup, kPositionDisplayKey);
    const QString positionDisplay =
            m_pConfig->getValueString(positionDisplayKey).trimmed();
    bool positionDisplayIsNumber = false;
    const double positionDisplayValue =
            positionDisplay.toDouble(&positionDisplayIsNumber);
    if (positionDisplay.isEmpty() ||
            (positionDisplayIsNumber &&
                    positionDisplayValue == kPositionDisplayElapsedAndRemaining)) {
        m_pConfig->setValue(positionDisplayKey, kPositionDisplayRemaining);
    }

    m_pCoUsbCount = std::make_unique<ControlObject>(ConfigKey(kGroup, "usb_count"));
    m_pCoUsbCount->setReadOnly();

    m_pCoUsbRefresh = std::make_unique<ControlObject>(ConfigKey(kGroup, "usb_refresh"));
    connect(m_pCoUsbRefresh.get(),
            &ControlObject::valueChanged,
            this,
            &SystemSettings::onRefreshRequested);

    // Drives the shutdown confirm WidgetStack in settings.xml: 0 = "Shut Down"
    // page, 1 = "Confirm / Cancel" page. Pre-created so the skin parser's
    // controlFromConfigKey() reuses it and the WidgetStack has a value to read
    // on its first showEvent.
    m_pCoShutdownArm = std::make_unique<ControlObject>(ConfigKey(kGroup, "shutdown_arm"));

    m_pCoShutdown = std::make_unique<ControlObject>(ConfigKey(kGroup, "shutdown"));
    connect(m_pCoShutdown.get(),
            &ControlObject::valueChanged,
            this,
            &SystemSettings::onShutdownRequested);

    // Vinyl/CDJ jog mode (General settings tab). Seeded from the persisted config
    // value and written back on every change so the choice survives restarts. The
    // controller mapping (e.g. Pioneer-DDJ-400-script.js) reads and subscribes to
    // this CO to switch jog-touch between scratch and pitch-bend live.
    const ConfigKey vinylModeKey(kBiteDj, QStringLiteral("vinyl_mode"));
    m_pCoVinylMode = std::make_unique<ControlObject>(vinylModeKey);
    m_pCoVinylMode->set(m_pConfig->getValue(vinylModeKey, kVinylModeDefault));
    connect(m_pCoVinylMode.get(),
            &ControlObject::valueChanged,
            this,
            &SystemSettings::onVinylModeChanged);

    // Vinyl brake (General settings tab). Same seed-then-persist pattern as the
    // jog mode above; the scratch engine reads this CO directly rather than
    // through the config, so the choice takes effect on the next jog release.
    const ConfigKey vinylBrakeKey(kBiteDj, QStringLiteral("vinyl_brake"));
    m_pCoVinylBrake = std::make_unique<ControlObject>(vinylBrakeKey);
    m_pCoVinylBrake->set(m_pConfig->getValue(vinylBrakeKey, kVinylBrakeDefault));
    connect(m_pCoVinylBrake.get(),
            &ControlObject::valueChanged,
            this,
            &SystemSettings::onVinylBrakeChanged);

    // Hot cue gating (General settings tab). CueControl reads the config value
    // on each activation rather than holding a proxy, so writing back on every
    // change is what makes the choice take effect live as well as persist.
    const ConfigKey hotcueActivatePlaysKey(kControlsGroup, kHotcueActivatePlaysKey);
    m_pCoHotcueActivatePlays = std::make_unique<ControlObject>(hotcueActivatePlaysKey);
    m_pCoHotcueActivatePlays->set(m_pConfig->getValue(
            hotcueActivatePlaysKey, kHotcueActivatePlaysDefault));
    connect(m_pCoHotcueActivatePlays.get(),
            &ControlObject::valueChanged,
            this,
            &SystemSettings::onHotcueActivatePlaysChanged);

    // Screen rotation in degrees, 0 or 180 (System settings tab).
    const ConfigKey screenRotationKey(kBiteDj, QStringLiteral("screen_rotation"));
    m_pCoScreenRotation = std::make_unique<ControlObject>(screenRotationKey);
    m_pCoScreenRotation->set(m_pConfig->getValue(screenRotationKey, 0.0));
    connect(m_pCoScreenRotation.get(),
            &ControlObject::valueChanged,
            this,
            &SystemSettings::onScreenRotationChanged);

    // Drive-level eject, addressed by physical USB port.
    for (int drive = 1; drive <= kNumEjectDrives; ++drive) {
        auto pCo = std::make_unique<ControlPushButton>(ConfigKey(
                kBiteDj, QStringLiteral("eject_drive%1").arg(drive)));
        connect(pCo.get(),
                &ControlObject::valueChanged,
                this,
                [this, drive](double value) {
                    if (value == 0.0) {
                        return;
                    }
                    ejectDrive(drive);
                });
        m_ejectDriveCos.push_back(std::move(pCo));
    }

    // Per-drive recording (Record button on each USB row). The engine is the
    // authority on whether the recorder is running: it opens the file on the
    // audio thread, so a start is only real once it says so, and it is also
    // where an unwritable drive or a failing encoder surfaces as a stop we did
    // not ask for. Both edges land here so the button can never sit on "Stop
    // Recording" over a recording that is not happening.
    if (m_pRecordingManager) {
        connect(m_pRecordingManager.get(),
                &RecordingManager::isRecording,
                this,
                &SystemSettings::onEngineRecordingChanged);
    }
    m_recordingStartWatchdog.setSingleShot(true);
    m_recordingStartWatchdog.setInterval(kRecordingStartTimeoutMs);
    connect(&m_recordingStartWatchdog,
            &QTimer::timeout,
            this,
            &SystemSettings::onRecordingStartTimeout);

    // Automatic USB detection: keep the in-skin device list in sync with what is
    // actually mounted, without the user having to tap "refresh". A watcher on the
    // removable roots fires the moment a drive is mounted or removed; a short
    // debounce coalesces the event burst (and lets a freshly mounted filesystem
    // become ready before we stat it); a slow poll backstops both.
    m_usbDebounce.setSingleShot(true);
    m_usbDebounce.setInterval(500);
    connect(&m_usbDebounce, &QTimer::timeout, this, [this]() {
        refresh();
    });
    connect(&m_usbWatcher,
            &QFileSystemWatcher::directoryChanged,
            this,
            [this](const QString&) {
                m_usbDebounce.start();
            });

    m_usbPoll.setSingleShot(false);
    m_usbPoll.setInterval(3000);
    connect(&m_usbPoll, &QTimer::timeout, this, [this]() {
        // A root (e.g. /run/media) may have come into existence since startup;
        // pick it up before re-enumerating.
        rearmUsbWatches();
        refresh();
    });
    m_usbPoll.start();

    s_pInstance.storeRelease(this);

    rearmUsbWatches();
    refresh(true);
}

SystemSettings::~SystemSettings() {
    s_pInstance.storeRelease(nullptr);
    // Close the file while the audio callback that writes it is still running.
    // EngineRecord's destructor does finalize an open file, but that happens
    // much further down the teardown, and this is the DJ's set: give the
    // recorder the same ordinary stop a tap would.
    stopRecordingToDrive();
}

// static
QStringList SystemSettings::removableRoots() {
    // These mirror the roots the Rekordbox library feature uses to find USB
    // devices (rekordboxfeature.cpp), so anything the user sees in the browser
    // sidebar is enumerated here too.
    QStringList roots{
            QStringLiteral("/media"),
            QStringLiteral("/run/media"),
            QStringLiteral("/mnt"),
    };

    // Desktop automounters such as udisks2 mount removable filesystems below
    // a per-user directory. Keep the base roots for appliance configurations
    // that mount drives directly below /media or /run/media, and only add the
    // user roots when USER is available to avoid scanning the same paths twice.
    const QString user = QString::fromLocal8Bit(qgetenv("USER"));
    if (!user.isEmpty()) {
        roots.append(QStringLiteral("/media/") + user);
        roots.append(QStringLiteral("/run/media/") + user);
    }
    return roots;
}

// static
bool SystemSettings::isOnRemovableMedia(const QString& path) {
    const QString cleaned = QDir::cleanPath(path);
    const QStringList roots = removableRoots();
    for (const QString& root : roots) {
        if (cleaned.startsWith(root + QLatin1Char('/'))) {
            return true;
        }
    }
    return false;
}

QList<SystemSettings::UsbMount> SystemSettings::enumerateUsbMounts() {
    QList<UsbMount> mounts;
    QSet<QString> seen;

    // Scan directories under the removable-media roots and keep the ones that
    // are an actual filesystem mountpoint. We deliberately do NOT parse
    // /proc/mounts: the GUI process can run in a mount namespace where the
    // auto-mounted drives are absent from the mount table, yet the directories
    // are still fully usable (and visible in the Rekordbox sidebar). QStorageInfo
    // stats the path directly, so it sees the mount regardless of namespace.
    for (const QString& root : removableRoots()) {
        const QFileInfoList entries = QDir(root).entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QFileInfo& entry : entries) {
            const QString dir = entry.absoluteFilePath();
            QStorageInfo info(dir);
            if (!info.isValid() || !info.isReady()) {
                continue;
            }
            // Only include the directory if it is itself the filesystem root
            // (a genuine mountpoint), not a plain folder sitting on the parent
            // filesystem (whose rootPath would be "/" or the tmpfs above it).
            const QString rootPath = QDir::cleanPath(info.rootPath());
            if (rootPath != QDir::cleanPath(dir)) {
                continue;
            }
            if (seen.contains(rootPath)) {
                continue;
            }
            seen.insert(rootPath);
            mounts.append(UsbMount{
                    QString::fromUtf8(info.device()),
                    rootPath});
        }
    }
    return mounts;
}

QStringList SystemSettings::usbMountPoints() {
    QStringList mountPoints;
    const QList<UsbMount> mounts = enumerateUsbMounts();
    mountPoints.reserve(mounts.size());
    for (const UsbMount& mount : mounts) {
        mountPoints.append(mount.mountPoint);
    }
    return mountPoints;
}

void SystemSettings::rearmUsbWatches() {
    const QStringList watched = m_usbWatcher.directories();
    for (const QString& root : removableRoots()) {
        if (!watched.contains(root) && QDir(root).exists()) {
            m_usbWatcher.addPath(root);
        }
    }
}

void SystemSettings::refresh(bool force) {
    QList<UsbMount> mounts = enumerateUsbMounts();

    // Skip the rebuild when nothing actually changed, so the automatic watcher
    // and poll don't churn the WUsbList (and re-emit the count CO) every tick.
    // enumerateUsbMounts() returns a deterministically ordered list, so an
    // element-wise compare is sufficient.
    bool changed = force || mounts.size() != m_usbMounts.size();
    for (int i = 0; !changed && i < mounts.size(); ++i) {
        if (mounts.at(i).mountPoint != m_usbMounts.at(i).mountPoint ||
                mounts.at(i).device != m_usbMounts.at(i).device) {
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    // Determine which mounts vanished since the last enumeration (compared by
    // mountpoint, which is stable across the eject/yank we care about). Computed
    // before the move below so we still have the previous set to diff against.
    QStringList removedMountPoints;
    for (const UsbMount& old : std::as_const(m_usbMounts)) {
        bool stillPresent = false;
        for (const UsbMount& cur : std::as_const(mounts)) {
            if (cur.mountPoint == old.mountPoint) {
                stillPresent = true;
                break;
            }
        }
        if (!stillPresent) {
            removedMountPoints.append(old.mountPoint);
        }
    }

    // Whether the drive being recorded to is still the drive it was. Its
    // mountpoint having gone away is the obvious case (a yank, or an unmount
    // from outside the app). A mountpoint still there but backed by a different
    // block device is the same thing with the gap between the unmount and the
    // remount too short for the watcher to have caught — the recording would
    // otherwise carry on writing onto whichever stick was pushed in next.
    bool recordingDriveGone = false;
    if (!m_recordingMountPoint.isEmpty()) {
        recordingDriveGone = removedMountPoints.contains(m_recordingMountPoint);
        if (!recordingDriveGone) {
            const auto deviceFor = [this](const QList<UsbMount>& list) {
                for (const UsbMount& mount : list) {
                    if (mount.mountPoint == m_recordingMountPoint) {
                        return mount.device;
                    }
                }
                return QString();
            };
            recordingDriveGone = deviceFor(m_usbMounts) != deviceFor(mounts);
        }
    }

    m_usbMounts = std::move(mounts);

    // Drop a recording whose drive is gone before the rows are rebuilt, so the
    // list comes back with no row claiming to be recording.
    if (recordingDriveGone) {
        stopRecordingToDrive(tr("Drive removed"));
    }

    m_usbRowLabels.clear();
    for (const UsbMount& mount : std::as_const(m_usbMounts)) {
        // Show the short volume name (the mountpoint's final path component)
        // rather than the full path — it fits the small screen and leaves room
        // for the Eject button. Fall back to the full mountpoint if empty.
        QString name = QDir(mount.mountPoint).dirName();
        if (name.isEmpty()) {
            name = mount.mountPoint;
        }
        m_usbRowLabels.append(name);
    }
    m_pCoUsbCount->forceSet(static_cast<double>(m_usbMounts.size()));
    emit usbRowsChanged(m_usbRowLabels);

    // Notify subscribers (the Rekordbox browser) after the list is rebuilt so a
    // dropped drive disappears from the sidebar the moment it is unmounted.
    for (const QString& mountPoint : std::as_const(removedMountPoints)) {
        emit mountEjected(mountPoint);
    }
}

int SystemSettings::unloadTracksOnMount(const QString& mountPoint) {
    if (!m_pPlayerManager) {
        return 0;
    }

    QString prefix = QDir::cleanPath(mountPoint);
    if (!prefix.endsWith(QLatin1Char('/'))) {
        prefix.append(QLatin1Char('/'));
    }

    QList<BaseTrackPlayer*> players;
    for (int i = 0; i < m_pPlayerManager->numberOfDecks(); ++i) {
        players.append(m_pPlayerManager->getDeckBase(i));
    }
    for (int i = 0; i < m_pPlayerManager->numberOfSamplers(); ++i) {
        players.append(m_pPlayerManager->getSampler(i));
    }
    for (int i = 0; i < m_pPlayerManager->numberOfPreviewDecks(); ++i) {
        players.append(m_pPlayerManager->getPreviewDeck(i));
    }

    int unloaded = 0;
    for (BaseTrackPlayer* pPlayer : std::as_const(players)) {
        if (!pPlayer) {
            continue;
        }
        TrackPointer pTrack = pPlayer->getLoadedTrack();
        if (!pTrack) {
            continue;
        }
        if (!pTrack->getLocation().startsWith(prefix)) {
            continue;
        }
        const QString& group = pPlayer->getGroup();
        // Stop playback first: BaseTrackPlayerImpl::slotEjectTrack refuses to
        // eject while a deck is playing.
        ControlObject::set(ConfigKey(group, QStringLiteral("play")), 0.0);
        ControlObject::set(ConfigKey(group, QStringLiteral("eject")), 1.0);
        ++unloaded;
    }
    return unloaded;
}

bool SystemSettings::tryUnmount(const QString& mountPoint, QString* pError) {
    QProcess umount;
    umount.start(QStringLiteral("umount"), QStringList{mountPoint});
    umount.waitForFinished();
    if (umount.exitStatus() == QProcess::NormalExit && umount.exitCode() == 0) {
        return true;
    }
    if (pError) {
        QString error = QString::fromLocal8Bit(umount.readAllStandardError()).trimmed();
        if (error.isEmpty()) {
            error = tr("umount exited with code %1").arg(umount.exitCode());
        }
        *pError = error;
    }
    return false;
}

QStringList SystemSettings::mountsOnSameUsbDevice(int index) const {
    QStringList mountPoints;
    if (index < 0 || index >= m_usbMounts.size()) {
        return mountPoints;
    }
    const UsbMount& target = m_usbMounts.at(index);
    // The tapped mount always goes first, so it is the one ejected while the
    // others are still mounted (i.e. the behaviour is unchanged for the common
    // single-filesystem case, and a failure on a sibling cannot pre-empt it).
    mountPoints.append(target.mountPoint);

    const QString node = mixxx::usbDeviceNodeForBlockDevice(target.device);
    if (node.isEmpty()) {
        // Not resolvable to a USB device (e.g. a loop/network mount under one of
        // the removable roots): eject it on its own rather than guessing.
        return mountPoints;
    }
    for (int i = 0; i < m_usbMounts.size(); ++i) {
        if (i == index) {
            continue;
        }
        if (mixxx::usbDeviceNodeForBlockDevice(m_usbMounts.at(i).device) == node) {
            mountPoints.append(m_usbMounts.at(i).mountPoint);
        }
    }
    return mountPoints;
}

bool SystemSettings::ejectMountPoint(const QString& mountPoint,
        int* pUnloaded,
        QString* pError) {
    // A recording writing to this drive holds an open file descriptor on it, so
    // the unmount below would fail EBUSY for as long as it runs. Stop it first
    // and let the retry loop at the end of this function cover the close, which
    // like the track release happens on another thread. Stopped without a
    // reason because this is not a failure: the tap that got here is a
    // deliberate eject, which ejectRow() reports on its own.
    if (mountPoint == m_recordingMountPoint) {
        stopRecordingToDrive();
    }

    // Before anything is unloaded: the drive is still mounted, so a sampler row
    // emptied by the eject below would be written straight back over the bank
    // the drive is carrying. The eject is not the DJ clearing their samples.
    if (SamplerDrive* pSamplerDrive = SamplerDrive::tryInstance()) {
        pSamplerDrive->suppressSavesTo(mountPoint);
    }

    const int unloaded = unloadTracksOnMount(mountPoint);
    if (pUnloaded) {
        *pUnloaded = unloaded;
    }

    // Abort any analysis (deck-load or batch) reading a track off this volume.
    // A worker thread mid-analysis keeps the audio file open, which holds the
    // filesystem busy and makes umount fail EBUSY. Cancelling lets the worker
    // release the descriptor before the retry loop below attempts the unmount.
    TrackAnalysisScheduler::cancelAnalysisUnderPath(mountPoint);

    // Let any history append that is still queued for this drive reach it
    // before the filesystem goes away: the write is deliberately off the GUI
    // thread (see FsHistoryWorker), so unlike every other store here it can
    // still have work outstanding at this point. Bounded, so a stick that has
    // stopped answering delays the eject rather than freezing the unit.
    FsHistoryWorker::flushFilesystem(mountPoint);

    // Close any per-filesystem analysis caches open on this drive. A track that
    // was analyzed from the USB leaves the FsAnalysisCache SQLite connection (and
    // its file descriptor) open on the stick; unless it is closed the kernel keeps
    // the filesystem busy and umount fails EBUSY. This blocks until any in-flight
    // cache write on a worker thread finishes, then drops the connection.
    FsAnalysisCache::closeFilesystemConnections(mountPoint);

    // Unloading does not free the drive synchronously. Setting [group],eject only
    // *requests* the unload: the engine's reader worker thread still has the audio
    // file open and closes it on its own thread, and the deck's TrackPointer is
    // dropped from a worker thread, so the GlobalTrackCache eviction that finally
    // deletes the Track arrives as a queued slot on THIS (main) thread. Until both
    // have happened the kernel keeps the filesystem busy and umount fails EBUSY.
    //
    // So we must let the event loop run (which drains the queued cache eviction —
    // i.e. clears the lingering in-memory references to the track) and give the
    // worker thread a moment to close the file, retrying umount until it takes.
    QString error = tr("device is busy");
    for (int attempt = 0; attempt < kUnmountAttempts; ++attempt) {
        // Drain queued track-release / cache-eviction events posted to this thread.
        QCoreApplication::processEvents(QEventLoop::AllEvents, kUnmountRetryMs);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (tryUnmount(mountPoint, &error)) {
            return true;
        }
        // Yield so the reader worker thread can finish closing the audio source.
        QThread::msleep(kUnmountRetryMs);
    }
    if (pError) {
        *pError = error;
    }
    return false;
}

void SystemSettings::ejectRow(int index) {
    if (index < 0 || index >= m_usbMounts.size() || m_ejecting) {
        return;
    }
    m_ejecting = true;

    // A drive with two mounted filesystems (e.g. a Rekordbox export partition
    // and a separate audio/Serato partition) is only safe to pull once BOTH are
    // unmounted, and the user tapped Eject on the drive, not on a partition. So
    // eject every filesystem sitting on the same physical USB device.
    const QStringList mountPoints = mountsOnSameUsbDevice(index);

    int unloaded = 0;
    QStringList ejected;
    QStringList failures;
    for (const QString& mountPoint : mountPoints) {
        int mountUnloaded = 0;
        QString error;
        const bool ok = ejectMountPoint(mountPoint, &mountUnloaded, &error);
        // Tracks are unloaded even when the unmount ends up failing.
        unloaded += mountUnloaded;
        if (ok) {
            ejected.append(mountPoint);
        } else {
            failures.append(QStringLiteral("%1: %2").arg(mountPoint, error));
        }
    }

    m_ejecting = false;

    if (!failures.isEmpty()) {
        // Partial success still leaves the stick unsafe to remove, so report the
        // failures rather than the volumes that did come off.
        notify(tr("Failed to eject %1").arg(failures.join(QStringLiteral("; "))),
                Notifications::Severity::Error);
        refresh();
        return;
    }

    notify(tr("Ejected %1 (unloaded %n track(s))", "", unloaded)
                    .arg(ejected.join(QStringLiteral(", "))),
            Notifications::Severity::Info);
    refresh();
}

void SystemSettings::ejectDrive(int driveNumber) {
    const ConfigKey pathKey(kBiteDj,
            QStringLiteral("usb_drive_path_%1").arg(driveNumber));
    const QString usbPath = m_pConfig->getValue(pathKey, QString());
    if (usbPath.isEmpty()) {
        qWarning() << "SystemSettings: eject requested for drive" << driveNumber
                   << "but" << pathKey.group << pathKey.item
                   << "is not set in mixxx.cfg";
        notify(tr("USB port %1 is not configured").arg(driveNumber),
                Notifications::Severity::Error);
        return;
    }

    // Re-enumerate first so a drive mounted since the last watcher/poll tick
    // is eligible (refresh() is a no-op when nothing changed).
    refresh();

    for (int i = 0; i < m_usbMounts.size(); ++i) {
        if (deviceOnUsbPath(m_usbMounts.at(i).device, usbPath)) {
            ejectRow(i);
            return;
        }
    }
    qWarning() << "SystemSettings: eject requested for drive" << driveNumber
               << "(USB path" << usbPath << ") but no mounted drive is on it";
    notify(tr("No drive mounted on USB port %1").arg(driveNumber),
            Notifications::Severity::Warning);
}

int SystemSettings::rowIndexForMountPoint(const QString& mountPoint) const {
    for (int i = 0; i < m_usbMounts.size(); ++i) {
        if (m_usbMounts.at(i).mountPoint == mountPoint) {
            return i;
        }
    }
    return -1;
}

int SystemSettings::recordingRowIndex() const {
    if (m_recordingMountPoint.isEmpty()) {
        return -1;
    }
    return rowIndexForMountPoint(m_recordingMountPoint);
}

void SystemSettings::toggleRecordRow(int index) {
    if (index < 0 || index >= m_usbMounts.size()) {
        return;
    }
    const QString mountPoint = m_usbMounts.at(index).mountPoint;
    if (mountPoint == m_recordingMountPoint) {
        stopRecordingToDrive();
        return;
    }
    if (!m_recordingMountPoint.isEmpty()) {
        // One drive at a time: the skin disables the other rows' buttons, so
        // this only catches a caller reaching the API directly.
        notify(tr("Already recording to %1")
                        .arg(QDir(m_recordingMountPoint).dirName()),
                Notifications::Severity::Warning);
        return;
    }
    startRecordingToRow(index);
}

void SystemSettings::startRecordingToRow(int index) {
    if (!m_pRecordingManager) {
        return;
    }
    if (m_pRecordingManager->isRecordingActive()) {
        // A recording started from outside this page (a controller mapping on
        // [Recording],toggle_recording) is writing somewhere we did not choose;
        // taking it over would silently redirect it mid-file.
        notify(tr("A recording is already running"),
                Notifications::Severity::Warning);
        return;
    }

    const QString mountPoint = m_usbMounts.at(index).mountPoint;
    QDir drive(mountPoint);
    if (!drive.exists(kRecordingsSubdir) && !drive.mkpath(kRecordingsSubdir)) {
        notify(tr("Cannot write to %1").arg(drive.dirName()),
                Notifications::Severity::Error);
        return;
    }

    // RecordingManager builds the filename from [Recording],Directory on every
    // start (and on every file split), which is the whole mechanism for
    // choosing where a recording lands. Remember what was there so the drive
    // does not stay the target once this recording ends.
    const ConfigKey dirKey(RECORDING_PREF_KEY, "Directory");
    m_savedRecordingDir = m_pConfig->getValueString(dirKey);
    m_pConfig->set(dirKey, ConfigValue(drive.filePath(kRecordingsSubdir)));

    m_recordingMountPoint = mountPoint;
    m_recordingStarted = false;
    m_pRecordingManager->startRecording();
    m_recordingStartWatchdog.start();
    // Flip the button now rather than on the engine's confirmation: the tap has
    // to acknowledge itself immediately, and the watchdog above takes the state
    // back if the start turns out never to have happened.
    emit usbRecordingChanged(index);
}

void SystemSettings::stopRecordingToDrive(const QString& reason) {
    if (m_recordingMountPoint.isEmpty()) {
        return;
    }
    const QString driveName = QDir(m_recordingMountPoint).dirName();
    const bool wasRecording = m_recordingStarted;

    // Unconditionally, not only when the engine says it is recording: a stop
    // inside the start window (an eject or a yank seconds after the tap) has to
    // take the status CO back off READY as well, or the recorder opens its file
    // on the next callback for a target that is already gone.
    if (m_pRecordingManager) {
        m_pRecordingManager->stopRecording();
    }
    releaseRecordingTarget();

    if (!wasRecording) {
        // Never actually started: whoever gave up on it says why.
        return;
    }
    // No filename in either message: the strip elides, and the drive is what the
    // DJ needs to be told — the file is a timestamp in Recordings on that stick.
    if (reason.isEmpty()) {
        notify(tr("Recording saved on %1").arg(driveName),
                Notifications::Severity::Info);
    } else {
        // Not a clean stop, so don't claim the file is complete.
        notify(tr("%1 — recording stopped").arg(reason),
                Notifications::Severity::Warning);
    }
}

void SystemSettings::releaseRecordingTarget() {
    if (m_recordingMountPoint.isEmpty()) {
        return;
    }
    m_recordingStartWatchdog.stop();
    m_recordingMountPoint.clear();
    m_recordingStarted = false;
    if (m_savedRecordingDir) {
        m_pConfig->set(ConfigKey(RECORDING_PREF_KEY, "Directory"),
                ConfigValue(*m_savedRecordingDir));
        m_savedRecordingDir.reset();
    }
    emit usbRecordingChanged(-1);
}

void SystemSettings::onEngineRecordingChanged(bool active) {
    if (m_recordingMountPoint.isEmpty()) {
        // Not our recording (or our own stop, which has already cleaned up).
        return;
    }
    if (active) {
        // Also re-emitted on every file split; only the first one is news.
        if (!m_recordingStarted) {
            m_recordingStarted = true;
            m_recordingStartWatchdog.stop();
            notify(tr("Recording to %1").arg(QDir(m_recordingMountPoint).dirName()),
                    Notifications::Severity::Info);
        }
        return;
    }
    // The engine stopped a recording we did not stop: a write error or a
    // failed encoder init, both of which report themselves through
    // RecordingManager. Just give the button back.
    releaseRecordingTarget();
}

void SystemSettings::onRecordingStartTimeout() {
    if (m_recordingMountPoint.isEmpty() || m_recordingStarted) {
        return;
    }
    const QString driveName = QDir(m_recordingMountPoint).dirName();
    // Stops silently (nothing was ever recording), which leaves this the only
    // report of why the button went back to Record.
    stopRecordingToDrive();
    notify(tr("Recording to %1 did not start").arg(driveName),
            Notifications::Severity::Error);
}

void SystemSettings::onRefreshRequested(double value) {
    // The skin's trigger button (EmitOnDownPress) latches this CO at 1; only
    // act on that edge, then reset to 0 so the next tap is a fresh edge —
    // same momentary-trigger handshake as ControllerSettings' rescan CO.
    if (value == 0.0) {
        return;
    }
    refresh(true);
    m_pCoUsbRefresh->forceSet(0.0);
}

void SystemSettings::onVinylModeChanged(double value) {
    // Persist so the jog mode is restored on the next launch.
    m_pConfig->setValue(ConfigKey(kBiteDj, QStringLiteral("vinyl_mode")), value);
}

void SystemSettings::onVinylBrakeChanged(double value) {
    // Persist so the brake time is restored on the next launch.
    m_pConfig->setValue(ConfigKey(kBiteDj, QStringLiteral("vinyl_brake")), value);
}

void SystemSettings::onHotcueActivatePlaysChanged(double value) {
    // Persist under the same key CueControl reads, so the change applies to
    // the next hotcue press and survives the next launch.
    m_pConfig->setValue(ConfigKey(kControlsGroup, kHotcueActivatePlaysKey),
            value != 0.0 ? 1 : 0);
}

void SystemSettings::onScreenRotationChanged(double value) {
    const int degrees = value == 180.0 ? 180 : 0;
    m_pConfig->setValue(ConfigKey(kBiteDj, QStringLiteral("screen_rotation")),
            static_cast<double>(degrees));
    m_pConfig->save();

    // Rotate the live session.
    if (qEnvironmentVariableIsEmpty("SWAYSOCK")) {
        qInfo() << "SystemSettings: SWAYSOCK not set, screen rotation"
                << degrees << "persisted but not applied to a live compositor";
        return;
    }
    if (!QProcess::startDetached(QStringLiteral("swaymsg"),
                QStringList{QStringLiteral("output"),
                        QStringLiteral("*"),
                        QStringLiteral("transform"),
                        QString::number(degrees)})) {
        notify(tr("Failed to apply screen rotation"),
                Notifications::Severity::Error);
    }
}

void SystemSettings::onShutdownRequested(double value) {
    if (value == 0.0) {
        return;
    }
    if (Notifications* pNotifications = Notifications::tryInstance()) {
        pNotifications->publishSticky(tr("Shutting down..."),
                Notifications::Severity::Info);
    }
    if (!QProcess::startDetached(QStringLiteral("poweroff"))) {
        m_pCoShutdownArm->set(0.0);
        m_pCoShutdown->set(0.0);
        notify(tr("Shutdown command failed to start"),
                Notifications::Severity::Error);
    }
}
