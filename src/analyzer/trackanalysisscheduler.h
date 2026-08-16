#pragma once

#include <QList>
#include <QMutex>
#include <QString>
#include <deque>
#include <memory>
#include <set>
#include <vector>

#include "analyzer/analyzerscheduledtrack.h"
#include "analyzer/analyzerthread.h"
#include "util/db/dbconnectionpool.h"

/// Callbacks for triggering side-effects in the outer context of
/// TrackAnalysisScheduler.
///
/// All functions will only be called from the host thread of
/// TrackAnalysisScheduler, not from worker threads.
class TrackAnalysisSchedulerEnvironment {
  public:
    virtual ~TrackAnalysisSchedulerEnvironment() = default;

    virtual TrackPointer loadTrackById(TrackId trackId) const = 0;
};

class TrackAnalysisScheduler : public QObject {
    Q_OBJECT

  public:
    typedef std::unique_ptr<TrackAnalysisScheduler, void(*)(TrackAnalysisScheduler*)> Pointer;
    // Subclass that provides a default constructor and nothing else
    class NullPointer: public Pointer {
      public:
        NullPointer();
    };

    static Pointer createInstance(
            std::unique_ptr<const TrackAnalysisSchedulerEnvironment> pEnvironment,
            int numWorkerThreads,
            const mixxx::DbConnectionPoolPtr& pDbConnectionPool,
            const UserSettingsPointer& pConfig,
            AnalyzerModeFlags modeFlags);

    /*private*/ TrackAnalysisScheduler(
            std::unique_ptr<const TrackAnalysisSchedulerEnvironment> pEnvironment,
            int numWorkerThreads,
            const mixxx::DbConnectionPoolPtr& pDbConnectionPool,
            const UserSettingsPointer& pUserSettings,
            AnalyzerModeFlags modeFlags);
    ~TrackAnalysisScheduler() override;

    // Schedule single or multiple tracks. After all tracks have been scheduled
    // the caller must invoke resume() once.
    bool scheduleTrack(AnalyzerScheduledTrack track);
    int scheduleTracks(const QList<AnalyzerScheduledTrack>& tracks);

    // Drops a single track from analysis: removes it from the pending queue if
    // it has not started yet, and aborts the worker currently analyzing it (if
    // any) without tearing the worker thread down. Used when a deck replaces a
    // track whose analysis is still running so the new track can be analyzed
    // immediately. No-op if the track is not scheduled or in progress.
    void cancelTrack(TrackId trackId);

    // Drops every scheduled or in-progress track whose file lives at or below
    // the given filesystem path. Aborting the in-progress workers releases the
    // open audio file descriptors so the volume can be unmounted. Called on the
    // host thread while ejecting a USB drive.
    void cancelTracksOnPath(const QString& path);

    // Applies cancelTracksOnPath() to every live scheduler instance. Lets the
    // eject path reach both the deck-load scheduler (PlayerManager) and the
    // batch-analysis scheduler (AnalysisFeature) without plumbing pointers to
    // either. Must be called on the host thread that owns the schedulers (the
    // GUI thread).
    static void cancelAnalysisUnderPath(const QString& path);

  public slots:
    void suspend();

    // After scheduling tracks the analysis must be resumed once.
    // Resume must also be called after suspending the analysis.
    void resume();

    // Stops a running analysis and discards all enqueued tracks.
    void stop();

  signals:
    // Progress for individual tracks is passed-through from the workers
    void trackProgress(TrackId trackId, AnalyzerProgress analyzerProgress);
    // Current average progress for all scheduled tracks and from all workers
    void progress(AnalyzerProgress currentTrackProgress, int currentTrackNumber, int totalTracksCount);
    void finished();

  private slots:
    void onWorkerThreadProgress(int threadId, AnalyzerThreadState threadState, TrackId trackId, AnalyzerProgress analyzerProgress);

  private:
    // Owns an analyzer thread and buffers the most recent progress update
    // received from this thread during analysis. It does not need to be
    // thread-safe, because all functions are invoked from the host thread
    // that runs the TrackAnalysisScheduler.
    class Worker {
      public:
        explicit Worker(AnalyzerThread::Pointer thread = AnalyzerThread::NullPointer())
            : m_thread(std::move(thread)),
              m_analyzerProgress(kAnalyzerProgressUnknown) {
        }
        Worker(const Worker&) = delete;
        Worker(Worker&&) = default;

        operator bool() const {
            return static_cast<bool>(m_thread);
        }

        AnalyzerThread* thread() const {
            DEBUG_ASSERT(m_thread);
            return m_thread.get();
        }

        AnalyzerProgress analyzerProgress() const {
            return m_analyzerProgress;
        }

        bool submitNextTrack(const AnalyzerTrack& track) {
            DEBUG_ASSERT(m_thread);
            return m_thread->submitNextTrack(std::move(track));
        }

        // Records what this worker is analyzing so the scheduler can target it
        // for cancellation by track id or by file location. Called by the
        // scheduler right after a successful submitNextTrack(), where the full
        // Track type is available.
        void recordSubmittedTrack(TrackId trackId, const QString& location) {
            m_submittedTrackId = trackId;
            m_submittedTrackLocation = location;
        }

        // The track currently submitted to this worker, or an invalid id/empty
        // location when the worker is idle.
        TrackId submittedTrackId() const {
            return m_submittedTrackId;
        }
        const QString& submittedTrackLocation() const {
            return m_submittedTrackLocation;
        }

        // Asks the worker thread to abort its current track (see
        // AnalyzerThread::cancelTrack). No-op if the worker is idle.
        void cancelSubmittedTrack() {
            if (m_thread && m_submittedTrackId.isValid()) {
                m_thread->cancelTrack(m_submittedTrackId);
            }
        }

        void forgetSubmittedTrack() {
            m_submittedTrackId = TrackId();
            m_submittedTrackLocation.clear();
        }

        void suspendThread() {
            if (m_thread) {
                m_thread->suspend();
            }
        }

        void resumeThread() {
            if (m_thread) {
                m_thread->resume();
            }
        }

        void stopThread() {
            if (m_thread) {
                m_thread->stop();
            }
        }

        void onAnalyzerProgress(AnalyzerProgress analyzerProgress) {
            DEBUG_ASSERT(m_thread);
            m_analyzerProgress = analyzerProgress;
        }

        void onThreadExit() {
            DEBUG_ASSERT(m_thread);
            m_thread.reset();
            m_analyzerProgress = kAnalyzerProgressUnknown;
            forgetSubmittedTrack();
        }

      private:
        AnalyzerThread::Pointer m_thread;
        AnalyzerProgress m_analyzerProgress;
        TrackId m_submittedTrackId;
        QString m_submittedTrackLocation;
    };

    bool submitNextTrack(Worker* worker);
    void emitProgressOrFinished();

    bool allTracksFinished() const {
        return m_queuedTracks.empty() &&
                m_pendingTrackIds.empty();
    }

    const std::unique_ptr<const TrackAnalysisSchedulerEnvironment> m_pEnvironment;

    std::vector<Worker> m_workers;

    std::deque<AnalyzerScheduledTrack> m_queuedTracks;

    // Tracks that have already been submitted to workers
    // and not yet reported back as finished.
    std::set<TrackId> m_pendingTrackIds;

    AnalyzerProgress m_currentTrackProgress;

    int m_currentTrackNumber;

    int m_dequeuedTracksCount;

    typedef std::chrono::steady_clock Clock;
    Clock::time_point m_lastProgressEmittedAt;

    // Registry of all live scheduler instances, used by cancelAnalysisUnderPath.
    // Every instance registers itself on construction and removes itself on
    // destruction. The schedulers all live on the host (GUI) thread, so the
    // mutex only guards the list bookkeeping.
    static QMutex s_instancesMutex;
    static QList<TrackAnalysisScheduler*> s_instances;
};
