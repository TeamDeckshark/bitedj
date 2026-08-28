#include "engine/cachingreader/cachingreaderworker.h"

#include <QAtomicInt>
#include <QDir>
#include <QtDebug>

#include "analyzer/analyzersilence.h"
#include "moc_cachingreaderworker.cpp"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "util/compatibility/qmutex.h"
#include "util/event.h"
#include "util/fifo.h"
#include "util/logger.h"
#include "util/rtscheduling.h"
#include "util/span.h"

namespace {

mixxx::Logger kLogger("CachingReaderWorker");

// Bite DJ: how long a deck may keep failing to read before the track is
// ejected. Long enough for a USB drive to re-enumerate and remount (the whole
// point of retrying at all), short enough that a DJ staring at a silent moving
// waveform gets a definite answer instead of an indefinite one.
constexpr int kUnreadableGraceMillis = 5000;
// Minimum spacing between re-open attempts. Re-opening means a full decoder
// open, so it must not run once per chunk request while a drive is genuinely
// absent.
constexpr int kReopenRetryIntervalMillis = 250;

// we need the last silence frame and the first sound frame
constexpr SINT kNumSoundFrameToVerify = 2;

} // anonymous namespace

CachingReaderWorker::CachingReaderWorker(
        const QString& group,
        FIFO<CachingReaderChunkReadRequest>* pChunkReadRequestFIFO,
        FIFO<ReaderStatusUpdate>* pReaderStatusFIFO)
        : m_group(group),
          m_tag(QString("CachingReaderWorker %1").arg(m_group)),
          m_pChunkReadRequestFIFO(pChunkReadRequestFIFO),
          m_pReaderStatusFIFO(pReaderStatusFIFO),
          m_gaveUpOnTrack(false) {
}

ReaderStatusUpdate CachingReaderWorker::processReadRequest(
        const CachingReaderChunkReadRequest& request) {
    CachingReaderChunk* pChunk = request.chunk;
    DEBUG_ASSERT(pChunk);

    // Before trying to read any data we need to check if the audio source
    // is available and if any audio data that is needed by the chunk is
    // actually available.
    auto chunkFrameIndexRange = pChunk->frameIndexRange(m_pAudioSource);
    DEBUG_ASSERT(!m_pAudioSource ||
            chunkFrameIndexRange.isSubrangeOf(m_pAudioSource->frameIndexRange()));
    if (chunkFrameIndexRange.empty()) {
        ReaderStatusUpdate result;
        result.init(CHUNK_READ_INVALID, pChunk, m_pAudioSource ? m_pAudioSource->frameIndexRange() : mixxx::IndexRange());
        return result;
    }

    // Try to read the data required for the chunk from the audio source
    mixxx::IndexRange bufferedFrameIndexRange = pChunk->bufferSampleFrames(
            m_pAudioSource,
            mixxx::SampleBuffer::WritableSlice(m_tempReadBuffer));

    // Bite DJ: nothing at all came back for a range the source says it has.
    // That is a dead file handle, not the end of the track, so re-open the
    // file and read the chunk again rather than handing the engine silence.
    // See reopenAudioSource().
    if (bufferedFrameIndexRange.empty() && !m_gaveUpOnTrack) {
        if (!m_readFailureTimer.isValid()) {
            m_readFailureTimer.start();
        }
        if (reopenAudioSource()) {
            chunkFrameIndexRange = pChunk->frameIndexRange(m_pAudioSource);
            if (!chunkFrameIndexRange.empty()) {
                bufferedFrameIndexRange = pChunk->bufferSampleFrames(
                        m_pAudioSource,
                        mixxx::SampleBuffer::WritableSlice(m_tempReadBuffer));
            }
        }
        if (bufferedFrameIndexRange.empty() &&
                m_readFailureTimer.elapsed() > kUnreadableGraceMillis) {
            // Out of patience. Flag it; run() ejects once this request has
            // been answered, so the engine always gets its chunk back.
            m_gaveUpOnTrack = true;
        }
    } else if (!bufferedFrameIndexRange.empty()) {
        // Reads are working again (or never stopped).
        m_readFailureTimer.invalidate();
    }

    DEBUG_ASSERT(!m_pAudioSource ||
            bufferedFrameIndexRange.isSubrangeOf(m_pAudioSource->frameIndexRange()));
    // The readable frame range might have changed
    chunkFrameIndexRange = intersect(chunkFrameIndexRange, m_pAudioSource->frameIndexRange());
    DEBUG_ASSERT(bufferedFrameIndexRange.empty() ||
            bufferedFrameIndexRange.isSubrangeOf(chunkFrameIndexRange));

    ReaderStatus status = bufferedFrameIndexRange.empty() ? CHUNK_READ_EOF : CHUNK_READ_SUCCESS;
    if (bufferedFrameIndexRange != chunkFrameIndexRange) {
        kLogger.warning()
                << m_group
                << "Failed to read chunk samples for frame index range:"
                << "expected =" << chunkFrameIndexRange
                << ", actual =" << bufferedFrameIndexRange;
        if (bufferedFrameIndexRange.empty()) {
            status = CHUNK_READ_INVALID; // overwrite EOF (see above)
        }
    }

    if (status == CHUNK_READ_SUCCESS) {
        // This call here assumes that the caching reader will read the first sound cue at
        // one of the first chunks. The check serves as a sanity check to ensure that the
        // sample data has not changed since it has ben analyzed. This could happen because
        // of a change in actual audio data or because the file was decoded using a different
        // decoder
        // This is part of a first prove of concept and needs to be replaces with a different
        // solution which is still under discussion. This might be also extended
        // to further checks whether a automatic offset adjustment is possible or a the
        // sample position metadata shall be treated as outdated.
        // Failures of the sanity check only result in an entry into the log at the moment.
        verifyFirstSound(pChunk);
    }

    ReaderStatusUpdate result;
    result.init(status, pChunk, m_pAudioSource ? m_pAudioSource->frameIndexRange() : mixxx::IndexRange());
    return result;
}

// WARNING: Always called from a different thread (GUI)
void CachingReaderWorker::newTrack(TrackPointer pTrack) {
    {
        const auto locker = lockMutex(&m_newTrackMutex);
        m_pNewTrack = pTrack;
        m_newTrackAvailable.storeRelease(1);
    }
    workReady();
}

void CachingReaderWorker::run() {
    // the id of this thread, for debugging purposes
    static auto lastId = QAtomicInt(0);
    const auto id = lastId.fetchAndAddRelaxed(1) + 1;
    QThread::currentThread()->setObjectName(
            QStringLiteral("CachingReaderWorker ") + QString::number(id));

    // This thread is the only thing standing between the audio callback and
    // the disk: it decodes the chunk the deck is about to play. The
    // QThread::HighPriority CachingReader passes to start() is a no-op under
    // SCHED_OTHER on Linux (same as the Controller thread, see
    // ControllerManager::slotInitialize), which left the fetch at nice 0 on
    // the housekeeping cores, competing on equal terms with track analysis and
    // preemptable by the SCHED_FIFO GUI thread. A repaint delaying the samples
    // that are due in the next few milliseconds is what a cache miss hears as
    // a pop, so take a real rung on the ladder just below the engine (see
    // util/rtscheduling.h).
    //
    // Steady-state chunk reads are lock-free (both FIFOs), so this priority
    // does not invert against anything. loadTrack() below is the exception:
    // it takes the global track cache and source locks that the analyzer
    // threads also hold. Those are demoted, not SCHED_IDLE, precisely so they
    // keep making progress and release the lock while this thread waits — and
    // a stall there delays a track *load*, not playback of the track already
    // running.
    mixxx::promoteCurrentThreadToRealtime(
            mixxx::kRtPrioTrackReader, "CachingReaderWorker");

    Event::start(m_tag);
    while (!m_stop.loadAcquire()) {
        // Request is initialized by reading from FIFO
        CachingReaderChunkReadRequest request;
        if (m_newTrackAvailable.loadAcquire()) {
            TrackPointer pLoadTrack;
            { // locking scope
                const auto locker = lockMutex(&m_newTrackMutex);
                pLoadTrack = m_pNewTrack;
                m_pNewTrack.reset();
                m_newTrackAvailable.storeRelease(0);
            } // implicitly unlocks the mutex
            if (pLoadTrack) {
                // in this case the engine is still running with the old track
                loadTrack(pLoadTrack);
            } else {
                // here, the engine is already stopped
                unloadTrack();
            }
        } else if (m_pChunkReadRequestFIFO->read(&request, 1) == 1) {
            // Read the requested chunk and send the result
            const ReaderStatusUpdate update = processReadRequest(request);
            m_pReaderStatusFIFO->writeBlocking(&update, 1);
            if (m_gaveUpOnTrack) {
                // Only after the chunk has been handed back: the engine owns
                // that chunk and must get it returned whatever happens next.
                declareTrackUnreadable();
            }
        } else {
            Event::end(m_tag);
            m_semaRun.acquire();
            Event::start(m_tag);
        }
    }
}

void CachingReaderWorker::discardAllPendingRequests() {
    CachingReaderChunkReadRequest request;
    while (m_pChunkReadRequestFIFO->read(&request, 1) == 1) {
        const auto update = ReaderStatusUpdate::readDiscarded(request.chunk);
        m_pReaderStatusFIFO->writeBlocking(&update, 1);
    }
}

void CachingReaderWorker::closeAudioSource() {
    discardAllPendingRequests();

    if (m_pAudioSource) {
        // Closes open file handles of the old track.
        m_pAudioSource->close();
        m_pAudioSource.reset();
    }
    // Drop the extra reference this worker holds so the track is released on
    // the same schedule as before (GlobalTrackCache eviction is already async
    // relative to the eject; this must not add another owner beyond it).
    m_pTrack.reset();
    m_readFailureTimer.invalidate();
    m_lastReopenAttempt.invalidate();
    m_gaveUpOnTrack = false;

    // This function has to be called with the engine stopped only
    // to avoid collecting new requests for the old track
    DEBUG_ASSERT(!m_pChunkReadRequestFIFO->readAvailable());
}

void CachingReaderWorker::unloadTrack() {
    closeAudioSource();

    const auto update = ReaderStatusUpdate::trackUnloaded();
    m_pReaderStatusFIFO->writeBlocking(&update, 1);
}

void CachingReaderWorker::loadTrack(const TrackPointer& pTrack) {
    // This emit is directly connected and returns synchronized
    // after the engine has been stopped.
    emit trackLoading();

    closeAudioSource();

    if (!pTrack->getFileInfo().checkFileExists()) {
        kLogger.warning()
                << m_group
                << "File not found"
                << pTrack->getFileInfo();
        const auto update = ReaderStatusUpdate::trackUnloaded();
        m_pReaderStatusFIFO->writeBlocking(&update, 1);
        emit trackLoadFailed(pTrack,
                tr("The file '%1' could not be found.")
                        .arg(QDir::toNativeSeparators(pTrack->getLocation())));
        return;
    }

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(CachingReaderChunk::kChannels);
    m_pAudioSource = SoundSourceProxy(pTrack).openAudioSource(config);
    // Kept so reopenAudioSource() can re-open this same file in place if its
    // handle later dies. Cleared by closeAudioSource() along with the source.
    m_pTrack = pTrack;
    if (!m_pAudioSource) {
        kLogger.warning()
                << m_group
                << "Failed to open file"
                << pTrack->getFileInfo();
        const auto update = ReaderStatusUpdate::trackUnloaded();
        m_pReaderStatusFIFO->writeBlocking(&update, 1);
        emit trackLoadFailed(pTrack,
                tr("The file '%1' could not be loaded.")
                        .arg(QDir::toNativeSeparators(pTrack->getLocation())));
        return;
    }

    // Initially assume that the complete content offered by audio source
    // is available for reading. Later if read errors occur this value will
    // be decreased to avoid repeated reading of corrupt audio data.
    if (m_pAudioSource->frameIndexRange().empty()) {
        m_pAudioSource.reset(); // Close open file handles
        kLogger.warning()
                << m_group
                << "Failed to open empty file"
                << pTrack->getFileInfo();
        const auto update = ReaderStatusUpdate::trackUnloaded();
        m_pReaderStatusFIFO->writeBlocking(&update, 1);
        emit trackLoadFailed(pTrack,
                tr("The file '%1' is empty and could not be loaded.")
                        .arg(QDir::toNativeSeparators(pTrack->getLocation())));
        return;
    }

    // Adjust the internal buffer
    const SINT tempReadBufferSize =
            m_pAudioSource->getSignalInfo().frames2samples(
                    CachingReaderChunk::kFrames);
    if (m_tempReadBuffer.size() != tempReadBufferSize) {
        mixxx::SampleBuffer(tempReadBufferSize).swap(m_tempReadBuffer);
    }

    const auto update =
            ReaderStatusUpdate::trackLoaded(
                    m_pAudioSource->frameIndexRange());
    m_pReaderStatusFIFO->writeBlocking(&update, 1);

    // Emit that the track is loaded.
    const double sampleCount =
            CachingReaderChunk::dFrames2samples(
                    m_pAudioSource->frameLength());

    // This code is a workaround until we have found a better solution to
    // verify and correct offsets.
    CuePointer pN60dBSound =
            pTrack->findCueByType(mixxx::CueType::N60dBSound);
    if (pN60dBSound) {
        m_firstSoundFrameToVerify = pN60dBSound->getPosition();
    }

    // The engine must not request any chunks before receiving the
    // trackLoaded() signal
    DEBUG_ASSERT(!m_pChunkReadRequestFIFO->readAvailable());

    emit trackLoaded(
            pTrack,
            m_pAudioSource->getSignalInfo().getSampleRate(),
            sampleCount);
}

bool CachingReaderWorker::reopenAudioSource() {
    if (!m_pTrack || !m_pAudioSource) {
        return false;
    }
    // Rate-limit: a drive that is really gone would otherwise get a full
    // decoder open per chunk request, on the reader thread, at real-time
    // priority.
    if (m_lastReopenAttempt.isValid() &&
            m_lastReopenAttempt.elapsed() < kReopenRetryIntervalMillis) {
        return false;
    }
    m_lastReopenAttempt.start();

    if (!m_pTrack->getFileInfo().checkFileExists()) {
        // The drive has not come back (or came back somewhere else). Nothing
        // to open; the grace window in processReadRequest decides when to
        // stop waiting for it.
        return false;
    }

    const mixxx::IndexRange previousFrameIndexRange =
            m_pAudioSource->frameIndexRange();

    mixxx::AudioSource::OpenParams config;
    config.setChannelCount(CachingReaderChunk::kChannels);
    // Open into a local first and only swap on success. The caller
    // dereferences m_pAudioSource unconditionally after this returns, and a
    // failed re-open must leave the deck exactly as it was.
    mixxx::AudioSourcePointer pAudioSource =
            SoundSourceProxy(m_pTrack).openAudioSource(config);
    if (!pAudioSource) {
        return false;
    }
    if (pAudioSource->frameIndexRange() != previousFrameIndexRange) {
        // Same path, different content. Every position the engine holds — the
        // playhead, cues, the beatgrid — describes the old file, so resuming
        // would play the wrong audio from the wrong place. Let it eject
        // instead.
        kLogger.warning()
                << m_group
                << "Re-opened file has different content:"
                << "before =" << previousFrameIndexRange
                << ", after =" << pAudioSource->frameIndexRange()
                << m_pTrack->getFileInfo();
        pAudioSource->close();
        return false;
    }

    kLogger.info()
            << m_group
            << "Re-opened audio source after a failed read"
            << m_pTrack->getFileInfo();
    m_pAudioSource->close();
    m_pAudioSource = std::move(pAudioSource);
    return true;
}

void CachingReaderWorker::declareTrackUnreadable() {
    const TrackPointer pTrack = m_pTrack;
    m_readFailureTimer.invalidate();
    if (!pTrack) {
        m_gaveUpOnTrack = false;
        return;
    }
    kLogger.warning()
            << m_group
            << "Giving up on a track that can no longer be read"
            << pTrack->getFileInfo();
    // Deliberately no status update here. This is a directly-connected signal
    // into EngineBuffer::slotTrackLoadFailed(), which ejects the deck and so
    // calls CachingReader::newTrack(nullptr) — that is what walks the reader
    // state machine through STATE_TRACK_UNLOADING and gets unloadTrack() to
    // post the TRACK_UNLOADED update. Posting one from here instead would
    // arrive while the state is still STATE_TRACK_LOADED and desync it.
    //
    // m_gaveUpOnTrack stays set until closeAudioSource() clears it, so the
    // handful of requests the engine still has in flight before the eject
    // lands do not restart the retry loop.
    emit trackLoadFailed(pTrack,
            tr("The file '%1' could no longer be read. "
               "Check that the drive it is on is still connected.")
                    .arg(QDir::toNativeSeparators(pTrack->getLocation())));
}

void CachingReaderWorker::quitWait() {
    m_stop = 1;
    m_semaRun.release();
    wait();
}

void CachingReaderWorker::verifyFirstSound(const CachingReaderChunk* pChunk) {
    if (!m_firstSoundFrameToVerify.isValid()) {
        return;
    }

    const int firstSoundIndex =
            CachingReaderChunk::indexForFrame(static_cast<SINT>(
                    m_firstSoundFrameToVerify.toLowerFrameBoundary()
                            .value()));
    if (pChunk->getIndex() == firstSoundIndex) {
        auto sampleBuffer = mixxx::SampleBuffer(
                kNumSoundFrameToVerify * mixxx::kEngineChannelCount);
        // We read two frames, the last silence frame and the first non-silence frame from
        // m_firstSoundFrameToVerify. end points to one position after them.
        sampleBuffer.clear(); // we need to clear the buffer, for the case the
                              // very first sample is the first sound.
        SINT end = static_cast<SINT>(m_firstSoundFrameToVerify.toLowerFrameBoundary().value()) + 1;
        mixxx::IndexRange probeFrameIndexRange =
                mixxx::IndexRange::between(end - kNumSoundFrameToVerify, end);
        mixxx::IndexRange bufferedFrameIndexRange =
                pChunk->readBufferedSampleFrames(
                        sampleBuffer.data(), probeFrameIndexRange);
        VERIFY_OR_DEBUG_ASSERT(bufferedFrameIndexRange.end() == probeFrameIndexRange.end()) {
            qWarning() << "skipping verifyFirstSound()";
            return;
        }
        if (AnalyzerSilence::verifyFirstSound(sampleBuffer.span(), mixxx::audio::FramePos(1))) {
            qDebug() << "First sound found at the previously stored position";
        } else {
            // This can happen in case of track edits or replacements, changed
            // encoders or encoding issues.
            qWarning() << "First sound has been moved! The beatgrid and "
                          "other annotations are no longer valid"
                       << m_pAudioSource->getUrlString();
        }
        m_firstSoundFrameToVerify = mixxx::audio::FramePos();
    }
}
