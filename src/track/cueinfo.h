#pragma once

#include <QFlags>

#include "audio/signalinfo.h"
#include "util/color/rgbcolor.h"
#include "util/optional.h"

namespace mixxx {

enum class CueType {
    Invalid = 0,
    HotCue = 1,
    MainCue = 2,
    Beat = 3, // unused (what is this for?)
    Loop = 4,
    Jump = 5,
    Intro = 6,
    Outro = 7,
    N60dBSound = 8, // range that covers beginning and end of audible
                    // sound; not shown to user
};

enum class CueFlag {
    None = 0,
    /// Currently only used when importing locked loops from Serato Metadata.
    Locked = 1,
};
Q_DECLARE_FLAGS(CueFlags, CueFlag);
Q_DECLARE_OPERATORS_FOR_FLAGS(CueFlags);

/// Hot cues are sequentially indexed starting with kFirstHotCueIndex (inclusive)
static constexpr int kFirstHotCueIndex = 0;

/// The hotcue slots are split into two banks so that rekordbox hot cues and
/// rekordbox memory cues keep separate identities instead of being flattened
/// into one run of slots. Both banks are addressed through the ordinary
/// `hotcue_N_*` controls, so the engine treats them identically; only the
/// importer and the skin care which bank a cue lives in.
///
/// Hot cue bank: rekordbox pads A-P. Controllers with eight physical pads
/// address A-H directly while the remaining slots stay available to the
/// library, waveform and mappings that provide a second page.
static constexpr int kHotCueBankStart = kFirstHotCueIndex;
static constexpr int kHotCueBankSize = 16;

/// Memory cue bank: chronological, the first of which also becomes the main
/// cue. Starts clear of the hot cue bank so that growing one never shifts the
/// other.
static constexpr int kMemoryCueBankStart = 16;
static constexpr int kMemoryCueBankSize = 10;

// DTO for Cue information without dependencies on the actual Track object
class CueInfo {
  public:
    CueInfo();
    CueInfo(CueType type,
            const std::optional<double>& startPositionMillis,
            const std::optional<double>& endPositionMillis,
            const std::optional<int>& hotCueIndex,
            QString label,
            const RgbColor::optional_t& color,
            CueFlags flags = CueFlag::None);

    CueType getType() const;
    void setType(CueType type);

    std::optional<double> getStartPositionMillis() const;
    void setStartPositionMillis(
            const std::optional<double>& positionMillis = std::nullopt);

    std::optional<double> getEndPositionMillis() const;
    void setEndPositionMillis(
            const std::optional<double>& positionMillis = std::nullopt);

    std::optional<int> getHotCueIndex() const;
    void setHotCueIndex(int hotCueIndex);

    QString getLabel() const;
    void setLabel(
            const QString& label = QString());

    mixxx::RgbColor::optional_t getColor() const;
    void setColor(
            const mixxx::RgbColor::optional_t& color = std::nullopt);

    CueFlags flags() const {
        return m_flags;
    }

    /// Set flags for the cue.
    ///
    /// These flags are currently only set during Serato cue import and *not*
    /// saved in the Database (only used for roundtrip testing purposes).
    void setFlags(CueFlags flags) {
        m_flags = flags;
    }

    /// Checks if the `CueFlag::Locked` flag is set for this cue.
    bool isLocked() const {
        return m_flags.testFlag(CueFlag::Locked);
    }

  private:
    CueType m_type;
    std::optional<double> m_startPositionMillis;
    std::optional<double> m_endPositionMillis;
    std::optional<int> m_hotCueIndex;
    QString m_label;
    RgbColor::optional_t m_color;
    CueFlags m_flags;
};

bool operator==(
        const CueInfo& lhs,
        const CueInfo& rhs);

inline bool operator!=(
        const CueInfo& lhs,
        const CueInfo& rhs) {
    return !(lhs == rhs);
}

QDebug operator<<(QDebug debug, const CueType& cueType);
QDebug operator<<(QDebug debug, const CueInfo& cueInfo);

} // namespace mixxx
