#include "library/rekordbox/rekordboxanlz.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QTemporaryDir>

#include "test/mixxxtest.h"
#include "track/cue.h"
#include "track/cueinfo.h"
#include "track/track.h"

namespace {

constexpr auto kSampleRate = mixxx::audio::SampleRate(44100);

// rekordbox cue tags come in two flavours: the legacy PCOB tag in the .DAT
// file, and the extended PCO2 tag the nxs2 line added (hot cues D-H, colours,
// comments) in the .EXT file. Both carry hot cues and memory cues in separate
// tags, distinguished by their list type.
constexpr uint32_t kCueListTypeMemory = 0;
constexpr uint32_t kCueListTypeHotCue = 1;
constexpr uint8_t kCueEntryTypeCue = 1;
constexpr uint8_t kCueEntryTypeLoop = 2;

/// Builds ANLZ files byte by byte, so the tests drive the real kaitai parser
/// rather than a stand-in. Layout follows lib/rekordbox-metadata/
/// rekordbox_anlz.ksy.
class AnlzBuilder {
  public:
    struct Cue {
        uint32_t hotCue; ///< 0 for a memory cue, else the 1-based pad number
        uint8_t type;    ///< kCueEntryTypeCue or kCueEntryTypeLoop
        uint32_t timeMs;
        uint32_t loopTimeMs; ///< only read when type is a loop
        QString comment;     ///< extended tags only
    };

    /// Legacy PCOB tag.
    void addCueTag(uint32_t listType, const QList<Cue>& cues) {
        QByteArray body;
        appendU32(&body, listType);
        body.append(2, '\0');
        appendU16(&body, static_cast<uint16_t>(cues.size()));
        appendU32(&body, static_cast<uint32_t>(cues.size()));
        for (const Cue& cue : cues) {
            QByteArray entry;
            entry.append("PCPT", 4);
            appendU32(&entry, 0x1c); // len_header
            appendU32(&entry, 0x38); // len_entry
            appendU32(&entry, cue.hotCue);
            appendU32(&entry, 0); // status
            appendU32(&entry, 0x10000);
            appendU16(&entry, 0xffff); // order_first
            appendU16(&entry, 0xffff); // order_last
            entry.append(static_cast<char>(cue.type));
            entry.append(3, '\0');
            appendU32(&entry, cue.timeMs);
            appendU32(&entry, cue.loopTimeMs);
            entry.append(16, '\0');
            body.append(entry);
        }
        addSection("PCOB", body);
    }

    /// Extended PCO2 tag.
    void addCueExtendedTag(uint32_t listType, const QList<Cue>& cues) {
        QByteArray body;
        appendU32(&body, listType);
        appendU16(&body, static_cast<uint16_t>(cues.size()));
        body.append(2, '\0');
        for (const Cue& cue : cues) {
            // Comments are UTF-16 big endian, NUL terminated.
            QByteArray comment;
            for (const QChar ch : cue.comment) {
                appendU16(&comment, ch.unicode());
            }
            appendU16(&comment, 0);

            // Fixed part is 40 bytes, then len_comment (4) + the comment, then
            // the four colour bytes. The parser gates those tail fields on
            // len_entry and len_comment, so the declared size has to be exact.
            QByteArray entry;
            entry.append("PCP2", 4);
            appendU32(&entry, 0x10); // len_header
            appendU32(&entry,
                    static_cast<uint32_t>(48 + comment.size())); // len_entry
            appendU32(&entry, cue.hotCue);
            entry.append(static_cast<char>(cue.type));
            entry.append(3, '\0');
            appendU32(&entry, cue.timeMs);
            appendU32(&entry, cue.loopTimeMs);
            entry.append(static_cast<char>(0)); // color_id
            entry.append(7, '\0');
            appendU16(&entry, 0); // loop_numerator
            appendU16(&entry, 0); // loop_denominator
            appendU32(&entry,
                    static_cast<uint32_t>(comment.size())); // len_comment
            entry.append(comment);
            entry.append(static_cast<char>(0)); // color_code
            entry.append(static_cast<char>(0)); // color_red
            entry.append(static_cast<char>(0)); // color_green
            entry.append(static_cast<char>(0)); // color_blue
            body.append(entry);
        }
        addSection("PCO2", body);
    }

    QByteArray build() const {
        QByteArray header;
        header.append("PMAI", 4);
        appendU32(&header, 0x1c); // len_header
        appendU32(&header,
                static_cast<uint32_t>(0x1c + m_sections.size())); // len_file
        header.append(0x1c - header.size(), '\0');
        return header + m_sections;
    }

  private:
    void addSection(const char* fourcc, const QByteArray& body) {
        QByteArray section;
        section.append(fourcc, 4);
        appendU32(&section, 12); // len_header
        appendU32(&section, static_cast<uint32_t>(12 + body.size()));
        section.append(body);
        m_sections.append(section);
    }

    static void appendU32(QByteArray* pOut, uint32_t value) {
        // Every multi-byte field in an ANLZ file is big endian.
        for (int shift = 24; shift >= 0; shift -= 8) {
            pOut->append(static_cast<char>((value >> shift) & 0xff));
        }
    }
    static void appendU16(QByteArray* pOut, uint16_t value) {
        pOut->append(static_cast<char>((value >> 8) & 0xff));
        pOut->append(static_cast<char>(value & 0xff));
    }

    QByteArray m_sections;
};

class RekordboxAnlzTest : public MixxxTest {
  protected:
    void SetUp() override {
        MixxxTest::SetUp();
        ASSERT_TRUE(m_tempDir.isValid());
    }

    TrackPointer createTrack() {
        const auto pTrack = Track::newTemporary(mixxx::FileAccess(
                mixxx::FileInfo(getTestDir().filePath(QStringLiteral("sine-30.wav")))));
        pTrack->setAudioProperties(
                mixxx::audio::ChannelCount(2),
                kSampleRate,
                mixxx::audio::Bitrate(),
                mixxx::Duration::fromSeconds(180));
        return pTrack;
    }

    /// Writes the file and imports its cues onto `pTrack`.
    void importCues(const TrackPointer& pTrack, const AnlzBuilder& builder) {
        const QString path = m_tempDir.filePath(
                QStringLiteral("ANLZ%1.DAT").arg(m_fileCounter++));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const QByteArray data = builder.build();
        ASSERT_EQ(data.size(), file.write(data));
        file.close();

        mixxx::rekordbox::readAnalyze(pTrack, kSampleRate, 0, false, path);
    }

    static CuePointer findHotcue(const TrackPointer& pTrack, int hotcueIndex) {
        const QList<CuePointer> cues = pTrack->getCuePoints();
        for (const CuePointer& pCue : cues) {
            if (pCue->getHotCue() == hotcueIndex) {
                return pCue;
            }
        }
        return CuePointer();
    }

    static QList<int> hotcueIndices(const TrackPointer& pTrack) {
        QList<int> indices;
        const QList<CuePointer> cues = pTrack->getCuePoints();
        for (const CuePointer& pCue : cues) {
            if (pCue->getHotCue() != Cue::kNoHotCue) {
                indices << pCue->getHotCue();
            }
        }
        std::sort(indices.begin(), indices.end());
        return indices;
    }

    /// ANLZ times are milliseconds; cue positions are frames.
    static mixxx::audio::FramePos framesForMs(uint32_t ms) {
        return mixxx::audio::FramePos((kSampleRate / 1000.0) * ms);
    }

    QTemporaryDir m_tempDir;
    int m_fileCounter = 0;
};

// Hot cues occupy the hot cue bank, addressed by the pad rekordbox assigned
// them rather than by the order they appear in the file.
TEST_F(RekordboxAnlzTest, HotCuesUseHotCueBank) {
    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue,
            {
                    {3, kCueEntryTypeCue, 3000, 0, {}}, // pad C
                    {1, kCueEntryTypeCue, 1000, 0, {}}, // pad A
            });

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart, mixxx::kHotCueBankStart + 2}),
            hotcueIndices(pTrack));
    EXPECT_EQ(framesForMs(1000), findHotcue(pTrack, mixxx::kHotCueBankStart)->getPosition());
    EXPECT_EQ(framesForMs(3000), findHotcue(pTrack, mixxx::kHotCueBankStart + 2)->getPosition());
}

// A loop saved on a hot cue pad must keep its end position and import as a
// saved loop. Dropping the end here is what made looping hot cues silently
// behave like plain cue points.
TEST_F(RekordboxAnlzTest, HotCueLoopKeepsEndPosition) {
    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue,
            {
                    {1, kCueEntryTypeLoop, 1000, 5000, {}},
                    {2, kCueEntryTypeCue, 2000, 0, {}},
            });

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    const CuePointer pLoop = findHotcue(pTrack, mixxx::kHotCueBankStart);
    ASSERT_TRUE(pLoop);
    EXPECT_EQ(mixxx::CueType::Loop, pLoop->getType());
    EXPECT_EQ(framesForMs(1000), pLoop->getPosition());
    EXPECT_EQ(framesForMs(5000), pLoop->getEndPosition());

    // A plain hot cue in the same tag stays a plain hot cue.
    const CuePointer pCue = findHotcue(pTrack, mixxx::kHotCueBankStart + 1);
    ASSERT_TRUE(pCue);
    EXPECT_EQ(mixxx::CueType::HotCue, pCue->getType());
    EXPECT_FALSE(pCue->getEndPosition().isValid());
}

// Same, through the extended tag the nxs2 line writes.
TEST_F(RekordboxAnlzTest, ExtendedHotCueLoopKeepsEndPosition) {
    AnlzBuilder builder;
    builder.addCueExtendedTag(kCueListTypeHotCue,
            {
                    {1, kCueEntryTypeLoop, 1000, 5000, QStringLiteral("drop")},
            });

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    const CuePointer pLoop = findHotcue(pTrack, mixxx::kHotCueBankStart);
    ASSERT_TRUE(pLoop);
    EXPECT_EQ(mixxx::CueType::Loop, pLoop->getType());
    EXPECT_EQ(framesForMs(5000), pLoop->getEndPosition());
    EXPECT_EQ(QStringLiteral("drop"), pLoop->getLabel());
}

// Memory cues go to their own bank in chronological order. They used to be
// appended after the last hot cue, which put them on the controller's pads.
TEST_F(RekordboxAnlzTest, MemoryCuesUseMemoryBankAndLeaveHotCuesAlone) {
    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue,
            {
                    {1, kCueEntryTypeCue, 1000, 0, {}},
                    {2, kCueEntryTypeCue, 2000, 0, {}},
            });
    builder.addCueTag(kCueListTypeMemory,
            {
                    // Deliberately out of order: the bank is chronological.
                    {0, kCueEntryTypeCue, 30000, 0, {}},
                    {0, kCueEntryTypeCue, 10000, 0, {}},
                    {0, kCueEntryTypeLoop, 20000, 24000, {}},
            });

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    EXPECT_EQ(QList<int>({
                      mixxx::kHotCueBankStart,
                      mixxx::kHotCueBankStart + 1,
                      mixxx::kMemoryCueBankStart,
                      mixxx::kMemoryCueBankStart + 1,
                      mixxx::kMemoryCueBankStart + 2,
              }),
            hotcueIndices(pTrack));

    EXPECT_EQ(framesForMs(10000),
            findHotcue(pTrack, mixxx::kMemoryCueBankStart)->getPosition());
    EXPECT_EQ(framesForMs(20000),
            findHotcue(pTrack, mixxx::kMemoryCueBankStart + 1)->getPosition());
    EXPECT_EQ(framesForMs(30000),
            findHotcue(pTrack, mixxx::kMemoryCueBankStart + 2)->getPosition());

    // The memory-cue loop keeps its end position too.
    const CuePointer pLoop = findHotcue(pTrack, mixxx::kMemoryCueBankStart + 1);
    EXPECT_EQ(mixxx::CueType::Loop, pLoop->getType());
    EXPECT_EQ(framesForMs(24000), pLoop->getEndPosition());
}

// The first chronological memory cue becomes the main cue, and keeps its slot
// in the bank so it stays callable from the memory pads.
TEST_F(RekordboxAnlzTest, FirstMemoryCueBecomesMainCue) {
    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeMemory,
            {
                    {0, kCueEntryTypeCue, 8000, 0, {}},
                    {0, kCueEntryTypeCue, 4000, 0, {}},
            });

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    EXPECT_EQ(framesForMs(4000), pTrack->getMainCuePosition());
    EXPECT_EQ(framesForMs(4000),
            findHotcue(pTrack, mixxx::kMemoryCueBankStart)->getPosition());
    EXPECT_EQ(framesForMs(8000),
            findHotcue(pTrack, mixxx::kMemoryCueBankStart + 1)->getPosition());
}

// The bank is finite; anything past it is dropped rather than spilling into
// whatever slots follow.
TEST_F(RekordboxAnlzTest, MemoryCuesPastBankAreDropped) {
    QList<AnlzBuilder::Cue> cues;
    for (int i = 0; i < mixxx::kMemoryCueBankSize + 4; ++i) {
        cues << AnlzBuilder::Cue{0,
                kCueEntryTypeCue,
                static_cast<uint32_t>(1000 * (i + 1)),
                0,
                {}};
    }
    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeMemory, cues);

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    EXPECT_EQ(mixxx::kMemoryCueBankSize, hotcueIndices(pTrack).size());
    EXPECT_EQ(mixxx::kMemoryCueBankStart + mixxx::kMemoryCueBankSize - 1,
            hotcueIndices(pTrack).last());
}

// A hot cue index outside the bank is ignored rather than landing on top of
// the memory cues.
TEST_F(RekordboxAnlzTest, OutOfBankHotCueIsIgnored) {
    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue,
            {
                    {1, kCueEntryTypeCue, 1000, 0, {}},
                    {static_cast<uint32_t>(mixxx::kHotCueBankSize + 1),
                            kCueEntryTypeCue,
                            2000,
                            0,
                            {}},
            });

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    EXPECT_EQ(QList<int>({mixxx::kHotCueBankStart}), hotcueIndices(pTrack));
}

// Re-importing must not rebuild cues that haven't changed. readAnalyze runs on
// every load and hands back the same cached Track a deck may already be
// playing, so recreating the Cue objects would blank that deck's pads and drop
// an active saved loop mid-set.
TEST_F(RekordboxAnlzTest, ReimportUpdatesCuesInPlace) {
    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue,
            {
                    {1, kCueEntryTypeLoop, 1000, 5000, {}},
            });
    builder.addCueTag(kCueListTypeMemory,
            {
                    {0, kCueEntryTypeCue, 9000, 0, {}},
            });

    const TrackPointer pTrack = createTrack();
    importCues(pTrack, builder);

    const CuePointer pHotCueBefore = findHotcue(pTrack, mixxx::kHotCueBankStart);
    const CuePointer pMemoryBefore = findHotcue(pTrack, mixxx::kMemoryCueBankStart);
    ASSERT_TRUE(pHotCueBefore);
    ASSERT_TRUE(pMemoryBefore);

    importCues(pTrack, builder);

    // Same objects, not replacements.
    EXPECT_EQ(pHotCueBefore.get(), findHotcue(pTrack, mixxx::kHotCueBankStart).get());
    EXPECT_EQ(pMemoryBefore.get(), findHotcue(pTrack, mixxx::kMemoryCueBankStart).get());
    EXPECT_EQ(2, hotcueIndices(pTrack).size());
}

// Slots the file no longer describes are cleared: cues deleted in rekordbox,
// and memory cues left in the hot cue slots by the banking this replaced.
TEST_F(RekordboxAnlzTest, StaleSlotsArePruned) {
    const TrackPointer pTrack = createTrack();

    // Stand in for an import made by the old banking, which trailed memory
    // cues directly after the hot cues.
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart + 1,
            framesForMs(15000),
            mixxx::audio::kInvalidFramePos);
    pTrack->createAndAddCue(mixxx::CueType::HotCue,
            mixxx::kHotCueBankStart + 2,
            framesForMs(25000),
            mixxx::audio::kInvalidFramePos);

    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue,
            {
                    {1, kCueEntryTypeCue, 1000, 0, {}},
            });
    builder.addCueTag(kCueListTypeMemory,
            {
                    {0, kCueEntryTypeCue, 15000, 0, {}},
                    {0, kCueEntryTypeCue, 25000, 0, {}},
            });
    importCues(pTrack, builder);

    EXPECT_EQ(QList<int>({
                      mixxx::kHotCueBankStart,
                      mixxx::kMemoryCueBankStart,
                      mixxx::kMemoryCueBankStart + 1,
              }),
            hotcueIndices(pTrack));
}

// Pruning is limited to hotcue slots; the analysis cues Mixxx keeps on the
// side must survive an import.
TEST_F(RekordboxAnlzTest, PruningLeavesNonHotcueCuesAlone) {
    const TrackPointer pTrack = createTrack();
    pTrack->createAndAddCue(mixxx::CueType::Intro,
            Cue::kNoHotCue,
            framesForMs(500),
            mixxx::audio::kInvalidFramePos);
    pTrack->createAndAddCue(mixxx::CueType::Outro,
            Cue::kNoHotCue,
            framesForMs(60000),
            mixxx::audio::kInvalidFramePos);

    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue, {{1, kCueEntryTypeCue, 1000, 0, {}}});
    importCues(pTrack, builder);

    EXPECT_TRUE(pTrack->findCueByType(mixxx::CueType::Intro));
    EXPECT_TRUE(pTrack->findCueByType(mixxx::CueType::Outro));
}

// The beats pass must not touch cues: it reads the legacy file purely for its
// beat grid, and runs before the cue pass reads the extended one.
TEST_F(RekordboxAnlzTest, BeatsPassLeavesCuesAlone) {
    const TrackPointer pTrack = createTrack();

    AnlzBuilder builder;
    builder.addCueTag(kCueListTypeHotCue, {{1, kCueEntryTypeCue, 1000, 0, {}}});
    importCues(pTrack, builder);
    ASSERT_EQ(1, hotcueIndices(pTrack).size());

    const QString path = m_tempDir.filePath(QStringLiteral("beats.DAT"));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    AnlzBuilder empty;
    file.write(empty.build());
    file.close();

    mixxx::rekordbox::readAnalyze(pTrack, kSampleRate, 0, true, path);

    EXPECT_EQ(1, hotcueIndices(pTrack).size());
}

// A pad that changed from a saved loop to a plain cue in rekordbox has to
// change type here too, not just lose its end position.
TEST_F(RekordboxAnlzTest, LoopDemotedToCueChangesType) {
    const TrackPointer pTrack = createTrack();

    AnlzBuilder loopBuilder;
    loopBuilder.addCueTag(kCueListTypeHotCue, {{1, kCueEntryTypeLoop, 1000, 5000, {}}});
    importCues(pTrack, loopBuilder);
    ASSERT_EQ(mixxx::CueType::Loop, findHotcue(pTrack, mixxx::kHotCueBankStart)->getType());

    AnlzBuilder cueBuilder;
    cueBuilder.addCueTag(kCueListTypeHotCue, {{1, kCueEntryTypeCue, 1000, 0, {}}});
    importCues(pTrack, cueBuilder);

    const CuePointer pCue = findHotcue(pTrack, mixxx::kHotCueBankStart);
    ASSERT_TRUE(pCue);
    EXPECT_EQ(mixxx::CueType::HotCue, pCue->getType());
    EXPECT_FALSE(pCue->getEndPosition().isValid());
}

} // namespace
