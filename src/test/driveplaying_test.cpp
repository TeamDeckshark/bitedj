#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <memory>

#include "control/controlobject.h"
#include "control/controlpushbutton.h"
#include "controllers/legacycontrollermappingfilehandler.h"
#include "controllers/midi/legacymidicontrollermapping.h"
#include "controllers/midi/midicontroller.h"
#include "controllers/midi/midimessage.h"
#include "mixer/basetrackplayer.h"
#include "preferences/systemsettings.h"
#include "test/mixxxtest.h"
#include "track/track.h"

// Covers the per-drive "a track off this stick is playing" interface: the rule
// that decides it, the [BiteDJ],drive<N>_playing controls that publish it, and
// the note numbers bitedj.midi.xml sends it out on.
//
// Deliberately absent: anything that goes in through a drive *number*. Resolving
// one to a mountpoint walks the real USB topology in sysfs (see deviceOnUsbPath),
// so a test written that way could only ever run on the appliance. Everything
// below takes mountpoints, which is why the rule is exposed as static functions
// over mountpoints in the first place.
namespace {

const QString kMountA = QStringLiteral("/media/USB");
const QString kMountB = QStringLiteral("/media/USB2");

// Minimal stand-in for a deck, sampler or preview deck: the two things the rule
// reads are the loaded track and the group's play control, and a real player
// needs an engine, a sound manager and a track collection to supply them.
class FakePlayer : public BaseTrackPlayer {
  public:
    explicit FakePlayer(const QString& group)
            : BaseTrackPlayer(nullptr, group),
              play(ConfigKey(group, QStringLiteral("play"))) {
        play.setButtonMode(ControlPushButton::TOGGLE);
    }

    void loadFrom(const QString& location) {
        m_pLoadedTrack = Track::newDummy(location, TrackId());
    }

    void unload() {
        m_pLoadedTrack.reset();
        play.set(0.0);
    }

    TrackPointer getLoadedTrack() const override {
        return m_pLoadedTrack;
    }

    void setupEqControls() override {
    }
    void slotLoadTrack(TrackPointer pTrack, bool bPlay) override {
        m_pLoadedTrack = pTrack;
        play.set(bPlay ? 1.0 : 0.0);
    }
    void slotCloneFromGroup(const QString&) override {
    }
    void slotCloneDeck() override {
    }
    void slotEjectTrack(double value) override {
        if (value > 0) {
            unload();
        }
    }

    ControlPushButton play;

  private:
    TrackPointer m_pLoadedTrack;
};

QList<BaseTrackPlayer*> playersOf(std::initializer_list<FakePlayer*> players) {
    QList<BaseTrackPlayer*> list;
    for (FakePlayer* pPlayer : players) {
        list.append(pPlayer);
    }
    return list;
}

// One drive per mountpoint, the shape drivesPlaying() takes for single-
// filesystem sticks: drives({A, B}) is two drives with one mount each.
QList<QStringList> drives(std::initializer_list<QString> mountPoints) {
    QList<QStringList> list;
    for (const QString& mountPoint : mountPoints) {
        list.append(QStringList{mountPoint});
    }
    return list;
}

class DrivePlayingTest : public MixxxTest {};

// --- the rule: which locations are on the drive ------------------------------

TEST_F(DrivePlayingTest, LocationOnTheMountCounts) {
    EXPECT_TRUE(SystemSettings::anyLocationUnderMount(
            {kMountA + QStringLiteral("/Music/track.mp3")}, kMountA));
}

TEST_F(DrivePlayingTest, LocationElsewhereDoesNotCount) {
    EXPECT_FALSE(SystemSettings::anyLocationUnderMount(
            {QStringLiteral("/root/Music/track.mp3")}, kMountA));
}

TEST_F(DrivePlayingTest, SiblingMountWithTheSamePrefixDoesNotCount) {
    // /media/USB must not swallow /media/USB2 — the two are different sticks in
    // different ports, and getting this wrong lights both LEDs for one track.
    EXPECT_FALSE(SystemSettings::anyLocationUnderMount(
            {kMountB + QStringLiteral("/track.mp3")}, kMountA));
    EXPECT_TRUE(SystemSettings::anyLocationUnderMount(
            {kMountB + QStringLiteral("/track.mp3")}, kMountB));
}

TEST_F(DrivePlayingTest, TheMountDirectoryItselfIsNotATrackOnIt) {
    EXPECT_FALSE(SystemSettings::anyLocationUnderMount({kMountA}, kMountA));
}

TEST_F(DrivePlayingTest, UnconfiguredPortNeverCounts) {
    // An empty mountpoint is how both "no such USB port configured" and "nothing
    // mounted on it" are spelled, and neither can have anything playing off it.
    EXPECT_FALSE(SystemSettings::anyLocationUnderMount(
            {kMountA + QStringLiteral("/track.mp3")}, QString()));
}

TEST_F(DrivePlayingTest, MountPointIsCleanedBeforeMatching) {
    EXPECT_TRUE(SystemSettings::anyLocationUnderMount(
            {kMountA + QStringLiteral("/track.mp3")}, kMountA + QStringLiteral("/")));
}

// --- the rule: which players are playing -------------------------------------

TEST_F(DrivePlayingTest, OnlyPlayingPlayersContributeALocation) {
    FakePlayer stopped(QStringLiteral("[Channel1]"));
    FakePlayer playing(QStringLiteral("[Channel2]"));
    stopped.loadFrom(kMountA + QStringLiteral("/stopped.mp3"));
    playing.loadFrom(kMountA + QStringLiteral("/playing.mp3"));
    playing.play.set(1.0);

    const QStringList locations =
            SystemSettings::playingTrackLocations(playersOf({&stopped, &playing}));

    EXPECT_EQ(QStringList{kMountA + QStringLiteral("/playing.mp3")}, locations);
}

TEST_F(DrivePlayingTest, EmptyPlayerContributesNothingEvenWithPlaySet) {
    // A deck can be left with play at 1 and no track — reading getLocation() off
    // a null TrackPointer there would be a crash, not a wrong LED.
    FakePlayer empty(QStringLiteral("[Channel1]"));
    empty.play.set(1.0);

    EXPECT_TRUE(SystemSettings::playingTrackLocations(playersOf({&empty})).isEmpty());
}

TEST_F(DrivePlayingTest, NullPlayersAreSkipped) {
    QList<BaseTrackPlayer*> players;
    players.append(nullptr);

    EXPECT_TRUE(SystemSettings::playingTrackLocations(players).isEmpty());
}

TEST_F(DrivePlayingTest, SamplersAndPreviewDecksCountToo) {
    // The daemon's LED means "this drive is being read", so it must not be
    // limited to the main decks: a sampler firing off the stick holds files
    // open on it just the same.
    FakePlayer sampler(QStringLiteral("[Sampler1]"));
    FakePlayer previewDeck(QStringLiteral("[PreviewDeck1]"));
    sampler.loadFrom(kMountA + QStringLiteral("/sample.mp3"));
    sampler.play.set(1.0);
    previewDeck.loadFrom(kMountB + QStringLiteral("/preview.mp3"));
    previewDeck.play.set(1.0);

    const QList<bool> playing = SystemSettings::drivesPlaying(
            playersOf({&sampler, &previewDeck}), drives({kMountA, kMountB}));

    EXPECT_EQ((QList<bool>{true, true}), playing);
}

// --- the rule: per drive -----------------------------------------------------

TEST_F(DrivePlayingTest, OnlyTheDriveHoldingThePlayingTrackIsFlagged) {
    FakePlayer deck1(QStringLiteral("[Channel1]"));
    FakePlayer deck2(QStringLiteral("[Channel2]"));
    deck1.loadFrom(kMountA + QStringLiteral("/a.mp3"));
    deck1.play.set(1.0);
    deck2.loadFrom(kMountB + QStringLiteral("/b.mp3"));

    const QList<bool> playing =
            SystemSettings::drivesPlaying(playersOf({&deck1, &deck2}), drives({kMountA, kMountB}));

    EXPECT_EQ((QList<bool>{true, false}), playing);
}

TEST_F(DrivePlayingTest, TrackPlayingOffTheInternalDiskFlagsNoDrive) {
    FakePlayer deck(QStringLiteral("[Channel1]"));
    deck.loadFrom(QStringLiteral("/root/Music/local.mp3"));
    deck.play.set(1.0);

    const QList<bool> playing =
            SystemSettings::drivesPlaying(playersOf({&deck}), drives({kMountA, kMountB}));

    EXPECT_EQ((QList<bool>{false, false}), playing);
}

TEST_F(DrivePlayingTest, StoppingTheOnlyPlayerClearsTheDrive) {
    FakePlayer deck(QStringLiteral("[Channel1]"));
    deck.loadFrom(kMountA + QStringLiteral("/a.mp3"));
    deck.play.set(1.0);
    ASSERT_EQ((QList<bool>{true}), SystemSettings::drivesPlaying(playersOf({&deck}), drives({kMountA})));

    deck.play.set(0.0);

    EXPECT_EQ((QList<bool>{false}), SystemSettings::drivesPlaying(playersOf({&deck}), drives({kMountA})));
}

TEST_F(DrivePlayingTest, EjectingTheTrackClearsTheDrive) {
    FakePlayer deck(QStringLiteral("[Channel1]"));
    deck.loadFrom(kMountA + QStringLiteral("/a.mp3"));
    deck.play.set(1.0);
    ASSERT_EQ((QList<bool>{true}), SystemSettings::drivesPlaying(playersOf({&deck}), drives({kMountA})));

    deck.unload();

    EXPECT_EQ((QList<bool>{false}), SystemSettings::drivesPlaying(playersOf({&deck}), drives({kMountA})));
}

TEST_F(DrivePlayingTest, LoadingAnotherDrivesTrackIntoARunningDeckMovesTheFlag) {
    // No play edge here at all: the deck is running throughout and only the
    // track changes. This is why the watches cover load/unload as well as play.
    FakePlayer deck(QStringLiteral("[Channel1]"));
    deck.loadFrom(kMountA + QStringLiteral("/a.mp3"));
    deck.play.set(1.0);
    ASSERT_EQ((QList<bool>{true, false}),
            SystemSettings::drivesPlaying(playersOf({&deck}), drives({kMountA, kMountB})));

    deck.loadFrom(kMountB + QStringLiteral("/b.mp3"));

    EXPECT_EQ((QList<bool>{false, true}),
            SystemSettings::drivesPlaying(playersOf({&deck}), drives({kMountA, kMountB})));
}

TEST_F(DrivePlayingTest, DrivesPlayingReturnsOneFlagPerRequestedMount) {
    EXPECT_EQ(4, SystemSettings::drivesPlaying({}, QList<QStringList>(4)).size());
    EXPECT_TRUE(SystemSettings::drivesPlaying({}, QList<QStringList>()).isEmpty());
}

TEST_F(DrivePlayingTest, ATrackOffAnyPartitionOfTheStickFlagsTheDrive) {
    // One stick, two filesystems (an exFAT and a FAT32 partition, say), both on
    // the same USB port. It is one drive and one LED, so a track off the second
    // mount must light it just as one off the first does -- the drive used to
    // resolve to whichever mount sorted first, and the other never lit.
    const QList<QStringList> oneStickTwoMounts{QStringList{kMountA, kMountB}};
    FakePlayer deck(QStringLiteral("[Channel1]"));
    deck.loadFrom(kMountB + QStringLiteral("/b.mp3"));
    deck.play.set(1.0);

    EXPECT_EQ((QList<bool>{true}),
            SystemSettings::drivesPlaying(playersOf({&deck}), oneStickTwoMounts));

    deck.loadFrom(kMountA + QStringLiteral("/a.mp3"));

    EXPECT_EQ((QList<bool>{true}),
            SystemSettings::drivesPlaying(playersOf({&deck}), oneStickTwoMounts));
}

// --- the rule: which drive a record request would collide with ----------------

TEST_F(DrivePlayingTest, RecordTargetBackingThePlayingTrackIsNamed) {
    // What startRecordingToRow() refuses on: the stick the DJ tapped Record on
    // is the one a deck is streaming its audio off.
    EXPECT_EQ(kMountA,
            SystemSettings::mountPlayingFrom(
                    {kMountA + QStringLiteral("/a.mp3")}, {kMountA}));
}

TEST_F(DrivePlayingTest, RecordTargetWithNothingPlayingOnItIsAllowed) {
    EXPECT_TRUE(SystemSettings::mountPlayingFrom(
            {kMountB + QStringLiteral("/b.mp3")}, {kMountA})
                    .isEmpty());
    EXPECT_TRUE(SystemSettings::mountPlayingFrom({}, {kMountA}).isEmpty());
}

TEST_F(DrivePlayingTest, RecordTargetIsRefusedForASiblingFilesystemOnTheSameStick) {
    // The mounts of one physical drive, tapped partition first. Playing off the
    // second partition is the same flash and the same USB bus as recording onto
    // the first, so it is refused — and named as the sibling it is, not as the
    // drive that was tapped.
    EXPECT_EQ(kMountB,
            SystemSettings::mountPlayingFrom(
                    {kMountB + QStringLiteral("/b.mp3")}, {kMountA, kMountB}));
}

TEST_F(DrivePlayingTest, RecordTargetOnADriveWithNoMountsIsAllowed) {
    // Out-of-range rows resolve to no mounts at all; nothing to collide with.
    EXPECT_TRUE(SystemSettings::mountPlayingFrom(
            {kMountA + QStringLiteral("/a.mp3")}, QStringList())
                    .isEmpty());
}

// --- the controls ------------------------------------------------------------

// SystemSettings takes both managers as shared_ptr and null-checks every use, so
// it can be built without an engine behind it. That is enough to assert the
// control surface exists and behaves; the value logic is the static rule above.
class DrivePlayingControlsTest : public MixxxTest {
  protected:
    void SetUp() override {
        m_pSettings = std::make_unique<SystemSettings>(config(), nullptr, nullptr);
    }

    void TearDown() override {
        m_pSettings.reset();
    }

    static ConfigKey drivePlayingKey(int drive) {
        return ConfigKey(QStringLiteral("[BiteDJ]"),
                QStringLiteral("drive%1_playing").arg(drive));
    }

    std::unique_ptr<SystemSettings> m_pSettings;
};

TEST_F(DrivePlayingControlsTest, OneControlPerEjectableDrive) {
    // Four, matching the four eject_drive<N> controls the daemon's notes drive.
    for (int drive = 1; drive <= 4; ++drive) {
        EXPECT_TRUE(ControlObject::exists(drivePlayingKey(drive)))
                << "missing control for drive " << drive;
    }
    EXPECT_FALSE(ControlObject::exists(drivePlayingKey(5)));
}

TEST_F(DrivePlayingControlsTest, NothingIsPlayingWithNoDrivesConfigured) {
    for (int drive = 1; drive <= 4; ++drive) {
        EXPECT_DOUBLE_EQ(0.0, ControlObject::get(drivePlayingKey(drive)));
        EXPECT_FALSE(m_pSettings->isDrivePlaying(drive));
    }
}

TEST_F(DrivePlayingControlsTest, ControlsAreReadOnly) {
    // Derived state: a mapping or script that wrote to it would be claiming a
    // drive is playing when it is not, and the LED would believe it.
    ControlObject::set(drivePlayingKey(1), 1.0);

    EXPECT_DOUBLE_EQ(0.0, ControlObject::get(drivePlayingKey(1)));
    EXPECT_FALSE(m_pSettings->isDrivePlaying(1));
}

TEST_F(DrivePlayingControlsTest, IsDrivePlayingRejectsOutOfRangeDriveNumbers) {
    EXPECT_FALSE(m_pSettings->isDrivePlaying(0));
    EXPECT_FALSE(m_pSettings->isDrivePlaying(-1));
    EXPECT_FALSE(m_pSettings->isDrivePlaying(5));
}

// --- the wire: bitedj.midi.xml ----------------------------------------------

// The note numbers here are half of a contract with deckshark-midi-driver: its
// config.yaml carries the same pairs per port, and its own tests assert that.
class BiteDjMappingTest : public MixxxTest {
  protected:
    void SetUp() override {
        const QDir mappingDir(getTestDir().filePath(QStringLiteral("../../res/controllers/")));
        m_pMapping = std::dynamic_pointer_cast<LegacyMidiControllerMapping>(
                LegacyControllerMappingFileHandler::loadMapping(
                        QFileInfo(mappingDir.filePath(QStringLiteral("bitedj.midi.xml"))),
                        mappingDir));
        ASSERT_TRUE(m_pMapping) << "bitedj.midi.xml failed to load as a MIDI mapping";
    }

    /// The single output mapping bound to [BiteDJ],<key>, or a default-constructed
    /// MidiOutput when there is not exactly one.
    MidiOutput outputFor(const QString& key) const {
        const auto mappings = m_pMapping->getOutputMappings().values(
                ConfigKey(QStringLiteral("[BiteDJ]"), key));
        EXPECT_EQ(1, mappings.size()) << qPrintable(key) << " is not bound exactly once";
        return mappings.size() == 1 ? mappings.first().output : MidiOutput();
    }

    std::shared_ptr<LegacyMidiControllerMapping> m_pMapping;
};

TEST_F(BiteDjMappingTest, EveryDriveHasAPlayingOutput) {
    for (int drive = 1; drive <= 4; ++drive) {
        const MidiOutput output =
                outputFor(QStringLiteral("drive%1_playing").arg(drive));
        EXPECT_EQ(0x90, output.status) << "drive " << drive;
        EXPECT_EQ(0x50 + drive - 1, output.control) << "drive " << drive;
        EXPECT_EQ(0x7F, output.on) << "drive " << drive;
        EXPECT_EQ(0x00, output.off) << "drive " << drive;
    }
}

TEST_F(BiteDjMappingTest, PlayingOutputWindowTreatsZeroAsOff) {
    // MidiOutputHandler sends `on` for any value inside [min, max]. The parser's
    // default window is 0..1, which would call a stopped drive "playing" and
    // leave the LED flashing forever, so the mapping has to narrow it.
    for (int drive = 1; drive <= 4; ++drive) {
        const MidiOutput output =
                outputFor(QStringLiteral("drive%1_playing").arg(drive));
        EXPECT_FALSE(0.0 >= output.min && 0.0 <= output.max)
                << "drive " << drive << " would report not-playing as playing";
        EXPECT_TRUE(1.0 >= output.min && 1.0 <= output.max)
                << "drive " << drive << " would not report playing at all";
    }
}

TEST_F(BiteDjMappingTest, PlayingNotesSitSixteenAboveTheEjectNotes) {
    // The daemon defaults playing_note to eject_note + 0x10 when its config
    // omits one, so the two note blocks have to stay in that relationship.
    for (int drive = 1; drive <= 4; ++drive) {
        const MidiKey ejectKey(0x90, static_cast<unsigned char>(0x40 + drive - 1));
        const auto inputs = m_pMapping->getInputMappings().values(ejectKey.key);
        ASSERT_EQ(1, inputs.size()) << "eject note for drive " << drive;
        EXPECT_EQ(ConfigKey(QStringLiteral("[BiteDJ]"),
                          QStringLiteral("eject_drive%1").arg(drive)),
                std::get<ConfigKey>(inputs.first().control));

        const MidiOutput output =
                outputFor(QStringLiteral("drive%1_playing").arg(drive));
        EXPECT_EQ(ejectKey.control + 0x10, output.control) << "drive " << drive;
    }
}

// --- the wire: a control change actually becoming a sent message -------------

/// Stands in for PortMidiController: records what would have gone out on the
/// ALSA port instead of opening one.
class RecordingMidiController : public MidiController {
  public:
    struct Sent {
        unsigned char status;
        unsigned char control;
        unsigned char value;
        bool operator==(const Sent& other) const {
            return status == other.status && control == other.control &&
                    value == other.value;
        }
    };

    RecordingMidiController()
            : MidiController(QStringLiteral("eject")) {
    }

    /// Marks this as an output device the way PortMidiController does when the
    /// enumerator paired it with an output port (setOutputDevice is protected).
    void becomeOutputDevice() {
        setOutputDevice(true);
    }

    /// Mirrors PortMidiController::open(): the controller is marked open
    /// *before* the mapping is applied, because applyMapping() pushes the
    /// initial value of every output and MidiOutputHandler discards those while
    /// isOpen() is false.
    ///
    /// Note what this can and cannot catch. Because the order lives here as
    /// well as in PortMidiController, these tests hold the *requirement* but
    /// would still pass if that backend regressed — reproducing them needs a
    /// real PortMIDI device, and setOutputDevice() is protected besides. The
    /// field signal for that regression is "MIDI device ... not open for
    /// output!" in mixxx.log at startup; it should not appear at all.
    int open() override {
        setOpen(true);
        startEngine();
        applyMapping();
        return 0;
    }
    int close() override {
        setOpen(false);
        return 0;
    }
    bool isPolling() const override {
        return false;
    }
    void sendShortMsg(unsigned char status, unsigned char byte1, unsigned char byte2) override {
        sent.append(Sent{status, byte1, byte2});
    }
    void sendBytes(const QByteArray&) override {
    }

    QList<Sent> sent;
};

class BiteDjOutputTest : public BiteDjMappingTest {
  protected:
    void SetUp() override {
        BiteDjMappingTest::SetUp();
        // The controls have to exist before the mapping is applied: an output
        // handler validates its control at construction and is thrown away if
        // it does not resolve. SystemSettings owns the real ones and makes them
        // read-only, so drive them directly here.
        for (int drive = 1; drive <= 4; ++drive) {
            m_playingCos.push_back(std::make_unique<ControlObject>(
                    ConfigKey(QStringLiteral("[BiteDJ]"),
                            QStringLiteral("drive%1_playing").arg(drive))));
        }
        m_pController = std::make_unique<RecordingMidiController>();
        m_pController->becomeOutputDevice();
        m_pController->setMapping(m_pMapping);
        ASSERT_EQ(0, m_pController->open());
        m_pController->sent.clear();
    }

    void TearDown() override {
        m_pController->close();
        m_pController.reset();
        m_playingCos.clear();
    }

    void setPlaying(int drive, bool playing) {
        m_playingCos[drive - 1]->set(playing ? 1.0 : 0.0);
        // The output handler is connected to the control through a proxy, which
        // hands the change over by signal.
        QCoreApplication::processEvents();
    }

    std::vector<std::unique_ptr<ControlObject>> m_playingCos;
    std::unique_ptr<RecordingMidiController> m_pController;
};

TEST_F(BiteDjOutputTest, PlayingADriveSendsItsNoteOn) {
    setPlaying(1, true);

    ASSERT_EQ(1, m_pController->sent.size());
    EXPECT_EQ((RecordingMidiController::Sent{0x90, 0x50, 0x7F}), m_pController->sent.first());
}

TEST_F(BiteDjOutputTest, StoppingSendsTheNoteOff) {
    setPlaying(1, true);
    m_pController->sent.clear();

    setPlaying(1, false);

    ASSERT_EQ(1, m_pController->sent.size());
    EXPECT_EQ((RecordingMidiController::Sent{0x90, 0x50, 0x00}), m_pController->sent.first());
}

TEST_F(BiteDjOutputTest, EachDriveSendsItsOwnNote) {
    for (int drive = 1; drive <= 4; ++drive) {
        m_pController->sent.clear();
        setPlaying(drive, true);
        ASSERT_EQ(1, m_pController->sent.size()) << "drive " << drive;
        EXPECT_EQ((RecordingMidiController::Sent{0x90,
                          static_cast<unsigned char>(0x50 + drive - 1),
                          0x7F}),
                m_pController->sent.first());
    }
}

TEST_F(BiteDjOutputTest, ADriveAlreadyPlayingWhenTheControllerOpensIsStillReported) {
    // The first track of a session is routinely already playing by the time
    // Mixxx opens the controller — controllers are set up at the very end of
    // startup, long after a deck can be loaded and started. That control never
    // changes again on its own, so if the state held at open time is not
    // transmitted then, the daemon is never told, and that drive's LED stays
    // solid red for the whole track. This is what made the first track of a
    // session behave differently from every track after it.
    m_playingCos[0]->set(1.0);
    QCoreApplication::processEvents();

    // A controller opening onto that already-playing drive, from scratch.
    RecordingMidiController controller;
    controller.becomeOutputDevice();
    controller.setMapping(m_pMapping);
    ASSERT_EQ(0, controller.open());
    QCoreApplication::processEvents();

    // Opening pushes every output's current state, so the three idle drives are
    // reported too; what matters is that the playing one is among them.
    EXPECT_TRUE(controller.sent.contains(RecordingMidiController::Sent{0x90, 0x50, 0x7F}))
            << "nothing told the daemon about a drive that was already playing "
               "when the controller opened";
    EXPECT_TRUE(controller.sent.contains(RecordingMidiController::Sent{0x90, 0x51, 0x00}));
}

TEST_F(BiteDjOutputTest, RedundantReportsAreNotResent) {
    setPlaying(1, true);
    m_pController->sent.clear();

    setPlaying(1, true);

    EXPECT_TRUE(m_pController->sent.isEmpty());
}

TEST_F(BiteDjMappingTest, NoOtherOutputsAreDeclared) {
    // The daemon acts on any note it recognises the moment it arrives; an output
    // added here without a note number the daemon knows would be silently
    // ignored, and one that collided with a playing note would flash an LED.
    EXPECT_EQ(4, m_pMapping->getOutputMappings().size());
}

} // namespace
