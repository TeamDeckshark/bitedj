#pragma once

#include <QDataStream>
#include <QFile>

#include "audio/types.h"
#include "control/pollingcontrolproxy.h"
#include "encoder/encoder.h"
#include "encoder/encodercallback.h"
#include "engine/sidechain/sidechainworker.h"
#include "preferences/usersettings.h"
#include "track/track_decl.h"

class ControlProxy;

class EngineRecord : public QObject, public EncoderCallback, public SideChainWorker {
    Q_OBJECT
  public:
    EngineRecord(UserSettingsPointer pConfig);
    ~EngineRecord() override;

    void process(const CSAMPLE* pBuffer, const int iBufferSize) override;
    void shutdown() override {}

    // writes compressed audio to file
    void write(const unsigned char *header, const unsigned char *body, int headerLen, int bodyLen) override;
    // gets stream position
    qint64 tell() override;
    // sets stream position
    void seek(qint64 pos) override;
    // gets stream length
    qint64 filelen() override;

    // creates or opens an audio file
    bool openFile();
    // closes the audio file
    void closeFile();
    int updateFromPreferences();
    bool fileOpen();
    bool openCueFile();
    void closeCueFile();

  signals:
    // emitted to notify RecordingManager
    void bytesRecorded(int bytes);

    // Free space on the volume being recorded to, in bytes, sampled every
    // kFreeSpaceProbeIntervalBytes written. Measured here, on the sidechain
    // thread, rather than by RecordingManager on the GUI thread: the query is a
    // blocking stat of the very filesystem this thread may already be waiting
    // on, and a USB stick that has stopped answering must not be able to take
    // the UI down with it. This thread is allowed to wait on it — it is the one
    // writing to it, and it is the lowest-priority thread in the process.
    void freeSpaceAvailable(qint64 bytesAvailable);

    // Emitted when recording state changes. 'recording' represents whether
    // recording is active and 'error' is true if an error occurred: the file
    // could not be opened, written or finalized, or the encoder failed.
    void isRecording(bool recording, bool error);
    void durationRecorded(quint64 durationInt);

  private:
    int getActiveTracks();
    // Check if the metadata has changed since the previous check. We also check
    // when was the last check performed to avoid using too much CPU and as well
    // to avoid changing the metadata during scratches.
    bool metaDataHasChanged();

    void writeCueLine();

    // Samples the free space on the recording volume once every
    // kFreeSpaceProbeIntervalBytes and reports it through
    // freeSpaceAvailable(). Called with the size of each write.
    void probeFreeSpace(int bytesWritten);
    // Ends the take after m_writeFailed: finalizes whatever reached the drive,
    // turns recording off and reports it to RecordingManager as an error.
    void stopAfterWriteFailure();

    UserSettingsPointer m_pConfig;
    EncoderPointer m_pEncoder;
    QString m_encoding;
    QString m_fileName;
    QString m_baTitle;
    QString m_baAuthor;
    QString m_baAlbum;

    QFile m_file;
    QFile m_cueFile;
    QDataStream m_dataStream;
    // Bytes still to be written before the next free-space probe.
    qint64 m_freeSpaceProbeCountdown;
    // Set when a write, seek or flush of the current file fails; cleared when
    // the next file is opened. Checked after each encoded buffer.
    bool m_writeFailed = false;

    PollingControlProxy m_sampleRateControl;
    ControlProxy* m_pRecReady;
    quint64 m_frames;
    mixxx::audio::SampleRate m_sampleRate;
    quint64 m_recordedDuration;
    QString getRecordedDurationStr();

    int m_iMetaDataLife;
    TrackPointer m_pCurrentTrack;

    QString m_cueFileName;
    quint64 m_cueTrack;
    bool m_bCueIsEnabled;
};
