#pragma once

#include <QAtomicPointer>
#include <QFileSystemWatcher>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <optional>
#include <vector>

#include "preferences/usersettings.h"

class BaseTrackPlayer;
class ControlObject;
class ControlProxy;
class ControlPushButton;
class PlayerManager;
class RecordingManager;

// Bite DJ: backs the in-skin Settings -> System sub-page, including the
// per-drive Record button (main-output recording onto a USB stick). Owns the [System],*
// COs the skin binds to and a Qt signal carrying the USB mount labels (CO
// transport carries doubles only, so list strings ride a signal alongside —
// same pattern as ControllerSettings::rowsChanged / Notifications). Runs OS
// actions (umount, shutdown) directly; the appliance process is root.
//
// Soft contract with stock Mixxx: every CO no-ops and tryInstance() returns
// nullptr when the singleton isn't constructed, so the skin parses end-to-end
// on an unpatched binary.
class SystemSettings : public QObject {
    Q_OBJECT
  public:
    SystemSettings(UserSettingsPointer pConfig,
            std::shared_ptr<PlayerManager> pPlayerManager,
            std::shared_ptr<RecordingManager> pRecordingManager);
    ~SystemSettings() override;

    static SystemSettings* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    // Pre-rendered per-mount labels ("<mountpoint>  (<device>)"), in the same
    // order as ejectRow()'s index. Lets a freshly-built WUsbList seed itself.
    QStringList usbRowLabels() const {
        return m_usbRowLabels;
    }

    struct UsbMount {
        QString device;
        QString mountPoint;
    };

    // Mount points (cleaned paths) of the USB drives currently mounted under
    // the removable-media roots, freshly enumerated on each call.
    //
    // Static and instance-free for the same reason removableRoots() is: code
    // that has no business owning the settings singleton — the history feature
    // listing the drives it can log to — has to see exactly the drives the
    // Settings tab ejects, and this class is constructed after the library, so
    // there is no instance to ask while that is being built.
    static QStringList usbMountPoints();
    static QList<UsbMount> enumerateUsbMounts();

    // The directories removable drives are mounted under. Static and
    // dependency-free (no instance, no syscalls, safe from any thread) so that
    // code far from the GUI — e.g. the per-drive cue store deciding whether a
    // track's filesystem is one a DJ can pull out — can ask the same question
    // this class answers, instead of keeping its own copy of the list.
    static QStringList removableRoots();

    // Whether `path` lives on a filesystem mounted under one of those roots.
    // Same set of drives usbMountPoints() enumerates, so anything written
    // there is also reachable by the settings actions that clear it again.
    static bool isOnRemovableMedia(const QString& path);

    // Unloads every track loaded from the indexed mount, then unmounts it and
    // re-enumerates. Idempotent on out-of-range. Safe to call from the GUI
    // thread.
    //
    // Ejects the whole physical drive, not just the one filesystem: any other
    // mounted filesystem on the same USB device (a second partition, a second
    // LUN) is unloaded and unmounted too, since the stick cannot be pulled
    // safely while any of them is still mounted.
    void ejectRow(int index);

    // Whether a track loaded from the drive on physical USB port `driveNumber`
    // is currently playing in any deck, sampler or preview deck. 1-based,
    // mirroring ejectDrive(); false for an unconfigured port or one with
    // nothing mounted. This is what [BiteDJ],drive<N>_playing publishes.
    bool isDrivePlaying(int driveNumber) const;

    // The drive-playing rule, in four static pieces. updateDrivePlaying() is
    // these plus the CO writes, and nothing else — split out because resolving a
    // drive *number* to a mountpoint walks the real USB topology in sysfs, so a
    // test that went in through the drive number could only ever run on the
    // appliance. Given mountpoints, the rule itself is testable anywhere.

    // Locations of the tracks currently playing in `players`. "Playing" is the
    // player's own play CO, not audibility: a track running into a closed fader
    // is still the drive being read, which is what the LED reports.
    static QStringList playingTrackLocations(const QList<BaseTrackPlayer*>& players);

    // Whether any of `locations` lives on the filesystem mounted at
    // `mountPoint`. Matches on the mountpoint plus a separator, so /media/USB
    // does not swallow /media/USB2. Always false for an empty mountpoint —
    // that is how an unconfigured or empty USB port is spelled.
    static bool anyLocationUnderMount(const QStringList& locations, const QString& mountPoint);

    // The first of `mountPoints` that has one of `locations` under it, or an
    // empty string when none does — anyLocationUnderMount() over a list, with
    // the match named rather than reduced to a flag, so a refusal can say which
    // drive is the one being read from.
    static QString mountPlayingFrom(
            const QStringList& locations, const QStringList& mountPoints);

    // One flag per entry of `driveMountPoints`, in the same order. Each entry
    // is every filesystem mounted off that drive: a stick with two partitions
    // is one drive and one LED, and a track off either of them lights it.
    static QList<bool> drivesPlaying(const QList<BaseTrackPlayer*>& players,
            const QList<QStringList>& driveMountPoints);

    // Ejects whatever drive is currently mounted on the physical USB port
    // configured as [BiteDJ],usb_drive_path_<driveNumber> (a sysfs topology
    // name like "1-1.5"). 1-based; no-op with a notification when the port is
    // unconfigured or nothing is mounted on it.
    void ejectDrive(int driveNumber);

    // Index of the drive the main output is currently being recorded onto, or
    // -1 when nothing is recording. Lets a freshly-built WUsbList seed its
    // per-row Record buttons, the same way usbRowLabels() seeds the rows.
    int recordingRowIndex() const;

    // Starts recording the main output into <mount>/Recordings on the indexed
    // drive, or stops the recording in progress when that drive is the one
    // being recorded to. Idempotent on out-of-range.
    //
    // Only one drive can be recorded to at a time: a request for a second one
    // while a recording is running is refused with a notification (the in-skin
    // buttons for the other drives are disabled, so this only guards callers
    // reaching the API directly).
    //
    // A drive that is backing a playing track is refused as well: the recorder
    // would be writing to the same stick a deck is reading its audio off.
    void toggleRecordRow(int index);

    // Support hatch (triple-tap on a drive's name in the Settings tab): runs
    // the export action defined in res/support/debuglogexport.xml (compiled
    // in) against the indexed drive. As shipped, that writes a gzipped tarball
    // of /var/log to the root of the drive as
    // bitedj-<firmware>-<timestamp>-debug.tar.gz, replacing any debug archive
    // an earlier export left there, and syncs it so the stick can be pulled as
    // soon as the success notification shows. Asynchronous — the action runs
    // as a SCHED_OTHER, niced child so it can't compete with the audio or GUI
    // threads — and one export at a time. Idempotent on out-of-range.
    void exportDebugLogsToRow(int index);

  signals:
    // Emitted whenever the mounted-drive list changes. Drives WUsbList row
    // rebuilds (string transport alongside the [System],usb_count CO).
    void usbRowsChanged(const QStringList& labels);

    // Emitted once per mount that has disappeared since the previous refresh(),
    // carrying its (cleaned) mountpoint path. Covers the in-skin Eject button,
    // a physical yank, and any external unmount the watcher/poll catches. Lets
    // the library browser (Rekordbox feature) drop the device row immediately
    // instead of waiting on its own slower background poll.
    void mountEjected(const QString& mountPoint);

    // Emitted whenever the drive being recorded to changes, carrying its row
    // index (or -1 once nothing is recording). Drives the per-row Record button
    // state in WUsbList — including the disabling of every other row's button,
    // which is what enforces one recording at a time on screen.
    void usbRecordingChanged(int recordingIndex);

  private slots:
    void onRefreshRequested(double value);
    void onShutdownRequested(double value);
    void onVinylModeChanged(double value);
    void onVinylBrakeChanged(double value);
    void onHotcueActivatePlaysChanged(double value);
    void onLoadIntoPlayingChanged(double value);
    void onScreenRotationChanged(double value);
    // RecordingManager::isRecording — the engine's own view of whether the
    // sidechain recorder is running, which is what confirms a start took (the
    // file is opened on the audio thread, a few callbacks after we ask) and
    // what reports a stop we did not ask for (write error, encoder failure).
    void onEngineRecordingChanged(bool active);
    void onRecordingStartTimeout();
    // Recomputes every [BiteDJ],drive<N>_playing CO from the current player and
    // mount state, writing only the ones that actually changed. Cheap enough to
    // call from any of the many things that can change the answer.
    void updateDrivePlaying();
    void onDebugExportFinished(int exitCode, QProcess::ExitStatus exitStatus);

  private:
    // Re-enumerates mounted USB drives. Unless `force` is set, returns without
    // touching anything when the mount set is unchanged from the last refresh —
    // this keeps the automatic watcher/poll from rebuilding the WUsbList on every
    // tick. On a real change (or when forced) it updates m_usbMounts/
    // m_usbRowLabels/the count CO and emits usbRowsChanged.
    void refresh(bool force = false);
    // Adds a QFileSystemWatcher watch for each removable root that currently
    // exists and is not already watched. Cheap and idempotent; re-run whenever a
    // root may have appeared (e.g. on the poll tick).
    void rearmUsbWatches();
    // Mount points of every currently enumerated filesystem that lives on the
    // same physical USB device as the mount at `index` — the indexed mount
    // first, then its siblings. Just the indexed mount when it is the only
    // filesystem on the drive or its USB topology can't be resolved. Empty on
    // out-of-range.
    QStringList mountsOnSameUsbDevice(int index) const;
    // Unloads the tracks on one mount (writing the count to *pUnloaded when
    // non-null) and unmounts it, retrying while the asynchronous track release
    // completes. Returns true once unmounted; on failure writes the last umount
    // error to *pError. Does not notify or re-enumerate — ejectRow() does that
    // once for the whole drive.
    bool ejectMountPoint(const QString& mountPoint, int* pUnloaded, QString* pError);
    // For every deck/sampler/preview deck whose loaded track lives under
    // mountPoint, stops playback and ejects it. Returns the number unloaded.
    int unloadTracksOnMount(const QString& mountPoint);
    // Runs `umount <mountPoint>` once. Returns true on success; on failure
    // writes the trimmed stderr (or a synthesized message) to *pError.
    bool tryUnmount(const QString& mountPoint, QString* pError);
    // Row index of the mount at `mountPoint` in the current enumeration, or -1.
    int rowIndexForMountPoint(const QString& mountPoint) const;
    // Every deck, sampler and preview deck, in that order. Null entries are
    // possible (a player index that does not resolve) and callers skip them.
    QList<BaseTrackPlayer*> allPlayers() const;
    // Re-points the per-player playback watches (each player's play CO and its
    // load/unload signals) at the players that exist right now, dropping the
    // previous set. Run once at construction and again whenever the deck or
    // sampler count changes.
    void rebuildPlaybackWatches();
    // Fills m_driveMountPoints from [BiteDJ],usb_drive_path_<N> and the current
    // enumeration: entry N-1 is every mountpoint of the drive on USB port N
    // (more than one for a multi-partition stick), or empty when that port is
    // unconfigured or carries nothing.
    void refreshDriveMountPoints();

    // Mountpoint of the filesystem on the same physical USB device as the mount
    // at `index` that is currently backing a playing track, or an empty string
    // when nothing on that drive is playing. Not just the indexed mount: a
    // second partition on the same stick is the same flash and the same bus.
    QString playingMountOnUsbDeviceOf(int index) const;

    // Warns once when a track starts playing off the drive being recorded to.
    // The start of a recording is refused in that situation (see
    // toggleRecordRow), but the DJ can still load and play a track off the
    // recording drive afterwards, and nothing stops them mid-set — this only
    // tells them the risk they just took. Cheap when nothing is recording,
    // which is the state it is called in almost every time.
    //
    // Latched: re-warns only after the contention has cleared, so it does not
    // repeat on every play/load edge and poll tick for as long as the track runs.
    void warnOnRecordingDriveContention();

    // Points [Recording],Directory at <mount>/Recordings on the indexed drive
    // and asks RecordingManager to start. Notifies and does nothing when the
    // directory cannot be created, or when a track is playing off that drive.
    void startRecordingToRow(int index);
    // Stops a recording in progress (no-op when there is none) and reports the
    // file it wrote. `reason` is prepended to that notification when the stop
    // was not a user tap (drive removed, engine gave up), and is empty for one
    // that was.
    void stopRecordingToDrive(const QString& reason = QString());
    // Drops the recording bookkeeping and puts [Recording],Directory back the
    // way we found it, then emits usbRecordingChanged(-1). Idempotent: the
    // engine reports the stop we asked for as well as one we did not, so this
    // runs on both paths and must survive running twice.
    void releaseRecordingTarget();
    // Kills a debug-log export writing to any of `mountPoints` and removes its
    // partial file, so the eject that called this is not refused with EBUSY.
    // An export is only diagnostics; the DJ asking for their stick back wins.
    void abortDebugExportOn(const QStringList& mountPoints);

    // Re-entrancy guard: ejectRow() pumps the event loop while waiting for the
    // asynchronous track release, which can re-deliver a tap. One eject at a time.
    bool m_ejecting = false;

    static QAtomicPointer<SystemSettings> s_pInstance;

    // The running debug-log export (null when none), the mountpoint it writes
    // to, and the archive's final and in-progress file names there.
    QProcess* m_pDebugExport = nullptr;
    QString m_debugExportMountPoint;
    QString m_debugExportFileName;
    QString m_debugExportPartialFileName;

    UserSettingsPointer m_pConfig;
    std::shared_ptr<PlayerManager> m_pPlayerManager;
    std::shared_ptr<RecordingManager> m_pRecordingManager;

    QList<UsbMount> m_usbMounts;
    QStringList m_usbRowLabels;

    // Mountpoint of the drive being recorded to, empty when nothing is. This —
    // not a row index — is the state that survives a re-enumeration, and it is
    // what the eject path and the vanished-mount diff in refresh() test
    // against; the row index the skin needs is derived from it.
    QString m_recordingMountPoint;
    // [Recording],Directory as it was before we pointed it at the drive, put
    // back when the recording ends so a later recording started from anywhere
    // else does not land on (or worse, at the empty mountpoint of) that drive.
    // Absent when no recording is in flight.
    std::optional<QString> m_savedRecordingDir;
    // Whether the engine has confirmed the recording is actually running. Until
    // it does, the target is provisional and the watchdog below owns it.
    bool m_recordingStarted = false;
    // Whether the drive being recorded to is currently also being read by a
    // playing deck, and has already been warned about. The latch for
    // warnOnRecordingDriveContention(); cleared once nothing on the drive is
    // playing any more, so the next track that starts off it warns again.
    bool m_recordingDriveContended = false;
    // The recorder opens its file on the audio thread, so a start that can
    // never work (no audio device, so no callback; unwritable drive) is
    // reported by nothing happening. Bounds the wait rather than leaving the
    // button stuck on "Stop Recording" with no file being written.
    QTimer m_recordingStartWatchdog;

    std::unique_ptr<ControlObject> m_pCoUsbCount;
    std::unique_ptr<ControlObject> m_pCoUsbRefresh;
    std::unique_ptr<ControlObject> m_pCoShutdownArm;
    std::unique_ptr<ControlObject> m_pCoShutdown;
    // [BiteDJ],vinyl_mode — 1 = Vinyl, 0 = CDJ jog behaviour. Persisted to
    // config; read by the controller mapping to toggle jog-touch scratching.
    std::unique_ptr<ControlObject> m_pCoVinylMode;
    // [BiteDJ],vinyl_brake — vinyl-brake time in seconds: how long a jog wheel
    // released at normal (1x) speed takes to coast to a standstill. 0 disables
    // the brake. Persisted to config; read by ControllerScriptInterfaceLegacy
    // on each jog release.
    std::unique_ptr<ControlObject> m_pCoVinylBrake;
    // [Controls],HotcueActivatePlays — 1 = ungated (a hotcue press plays on
    // from the cue), 0 = gated (previews only while held, then seeks back and
    // stops). CO and config share the key; CueControl reads the config value.
    std::unique_ptr<ControlObject> m_pCoHotcueActivatePlays;
    // [BiteDJ],load_into_playing — a LoadIntoPlaying: what loading a track
    // into a playing deck does (load / ask first / refuse). Persisted to
    // config; read by BaseTrackPlayerImpl on each load.
    std::unique_ptr<ControlObject> m_pCoLoadIntoPlaying;
    // [BiteDJ],screen_rotation — display rotation in degrees (0 or 180).
    std::unique_ptr<ControlObject> m_pCoScreenRotation;
    // Emitted from the controller thread; the queued connection hops the eject
    // onto this (main) thread, which ejectRow() requires.
    std::vector<std::unique_ptr<ControlPushButton>> m_ejectDriveCos;

    // [BiteDJ],drive<N>_playing — 1 while a track that lives on the drive
    // plugged into USB port N is playing somewhere. Read-only: the value is
    // derived state, and the only consumer is the eject-button daemon's MIDI
    // output mapping, which lights that port's red LED with it.
    std::vector<std::unique_ptr<ControlObject>> m_drivePlayingCos;
    // Mountpoints per drive number, index N-1. Recomputed on every enumeration
    // change rather than per query: resolving a drive number to a mountpoint
    // walks sysfs, and the playing state is recomputed far more often than the
    // mount set changes.
    QList<QStringList> m_driveMountPoints;
    // One proxy per player on its play CO, plus the load/unload connections on
    // the players themselves. Together these are every event that can change
    // what drive<N>_playing should be, short of the mount set moving.
    std::vector<std::unique_ptr<ControlProxy>> m_playWatches;
    QList<QMetaObject::Connection> m_playerConnections;

    // Automatic USB detection. The watcher fires directoryChanged when a drive is
    // mounted/unmounted under a removable root, giving near-instant updates; the
    // debounce coalesces the burst and lets the mount settle before we
    // re-enumerate. The poll is a slow backstop that also catches a root that did
    // not exist when the watcher was first armed and any missed watcher event.
    QFileSystemWatcher m_usbWatcher;
    QTimer m_usbDebounce;
    QTimer m_usbPoll;
};
