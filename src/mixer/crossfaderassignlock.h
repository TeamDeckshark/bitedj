#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <memory>

class ControlObject;
class ControlProxy;

/// Locks every deck's crossfader assignment ([ChannelN],orientation) while any
/// deck is playing *and* audible on the main mix.
///
/// Re-assigning a deck mid-mix moves it onto a different side of the fader (or
/// off the fader altogether), so whatever is coming out of the speakers jumps
/// in level the instant the tap lands. The lock is enforced on the control
/// itself through a value-change request handler, so it covers the Settings
/// page buttons, controller mappings and the orientation_left/right/center
/// push buttons alike. [BiteDJ],xfader_assign_enabled (read-only, 0 while
/// locked) lets the skin grey the buttons out instead of having them silently
/// do nothing.
///
/// "Audible" mirrors what the engine actually mixes: the deck is playing with a
/// track loaded, it is not muted, its level fader and pregain are above zero,
/// and the crossfader passes its side (Thru always passes). With no connected
/// sound device nothing is audible, and the play COs cannot be trusted anyway
/// (see AudioDeviceSettings::anyDeckPlaying()).
class CrossfaderAssignLock : public QObject {
    Q_OBJECT
  public:
    CrossfaderAssignLock();
    ~CrossfaderAssignLock() override;

    bool isLocked() const;

  private:
    struct DeckWatch {
        ControlProxy* pPlay;
        ControlProxy* pTrackLoaded;
        ControlProxy* pMute;
        ControlProxy* pVolume;
        ControlProxy* pPregain;
        ControlProxy* pOrientation;
    };

    ControlProxy* watch(const QString& group, const QString& item);
    // Picks up decks added after construction; decks are never removed.
    void addNewDecks();
    void updateEnabled();
    void onOrientationChangeRequest(ControlObject* pOrientation, double value);

    std::unique_ptr<ControlObject> m_pCoEnabled;

    ControlProxy* m_pNumDecks;
    ControlProxy* m_pSoundStatus;
    // The crossfader position and the [Mixer Profile] curve the engine shapes
    // it with, so the side gains match EngineMixer's.
    ControlProxy* m_pCrossfader;
    ControlProxy* m_pXfaderCurve;
    ControlProxy* m_pXfaderCalibration;
    ControlProxy* m_pXfaderMode;
    ControlProxy* m_pXfaderReverse;
    QList<DeckWatch> m_decks;
};
