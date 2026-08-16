#include "mixxxapplication.h"

#include <QDialog>
#include <QThreadPool>
#include <QTouchEvent>
#include <QtDebug>
#include <QtGlobal>

#include "audio/frame.h"
#include "audio/types.h"
#include "library/relocatedtrack.h"
#include "library/trackset/crate/crateid.h"
#include "moc_mixxxapplication.cpp"
#include "soundio/soundmanagerutil.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/assert.h"
#include "util/cache.h"
#include "util/cmdlineargs.h"
#include "util/color/rgbcolor.h"
#include "util/fileinfo.h"
#include "util/math.h"

// When linking Qt statically, the Q_IMPORT_PLUGIN is needed for each linked plugin.
// https://doc.qt.io/qt-5/plugins-howto.html#details-of-linking-static-plugins
#ifdef QT_STATIC
#include <QtPlugin>
#if defined(Q_OS_WASM)
Q_IMPORT_PLUGIN(QWasmIntegrationPlugin)
#elif defined(Q_OS_WIN)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin)
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
Q_IMPORT_PLUGIN(QModernWindowsStylePlugin)
#else
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin)
#endif
#elif defined(Q_OS_IOS)
Q_IMPORT_PLUGIN(QIOSIntegrationPlugin)
#elif defined(Q_OS_MACOS)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin)
Q_IMPORT_PLUGIN(QMacStylePlugin)
#elif defined(Q_OS_LINUX)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin)
#else
#error "Q_IMPORT_PLUGIN() for the current patform is missing"
#endif
#if !defined(Q_OS_WASM)
Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin)
Q_IMPORT_PLUGIN(QMinimalIntegrationPlugin)
#endif

Q_IMPORT_PLUGIN(QSQLiteDriverPlugin)
Q_IMPORT_PLUGIN(QTlsBackendOpenSSL)
Q_IMPORT_PLUGIN(QSvgPlugin)
Q_IMPORT_PLUGIN(QICOPlugin)
Q_IMPORT_PLUGIN(QJpegPlugin)
Q_IMPORT_PLUGIN(QGifPlugin)

#endif // QT_STATIC

namespace {

// kEventNotifyExecTimeWarningThreshold defines the threshold duration for event
// processing warnings. If the processing time of an event exceeds this duration
// in developer mode, a warning will be logged. This is used to identify
// potentially slow event processing in the application, which could impact
// performance. With a 60Hz waveform update rate, paint and swap events must be
// processed through the event queue every 16.6ms, to ensure smooth rendering.
// Exceeding this processing time can lead to visible delays, therefore 10ms is a
// reasonable threshold.
constexpr int kDefaultEventNotifyExecTimeWarningThreshold = 10;

// Bite DJ: the preferences dialog is the one dialog we keep, so the
// engine can still be tuned on the unit. Any dialog spawned from within
// it (file pickers, mapping prompts, ...) is exempt as well, otherwise
// those workflows would silently cancel themselves.
bool belongsToPreferencesDialog(const QWidget* pWidget) {
    for (const QWidget* p = pWidget; p; p = p->parentWidget()) {
        if (p->inherits("DlgPreferences")) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

MixxxApplication::MixxxApplication(int& argc, char** argv)
        : QApplication(argc, argv),
          m_isDeveloper(CmdlineArgs::Instance().getDeveloper()),
          m_eventNotifyExecTimeWarningThreshold(
                  mixxx::Duration::fromMillis(kDefaultEventNotifyExecTimeWarningThreshold)) {
    registerMetaTypes();

    // Increase the size of the global thread pool to at least
    // 4 threads, even if less cores are available. These threads
    // will be used for loading external libraries and other tasks.
    QThreadPool::globalInstance()->setMaxThreadCount(
            math_max(4, QThreadPool::globalInstance()->maxThreadCount()));
}

void MixxxApplication::registerMetaTypes() {
    // PCM audio types
    qRegisterMetaType<mixxx::audio::ChannelCount>("mixxx::audio::ChannelCount");
    qRegisterMetaType<mixxx::audio::ChannelLayout>("mixxx::audio::ChannelLayout");
    qRegisterMetaType<mixxx::audio::OptionalChannelLayout>("mixxx::audio::OptionalChannelLayout");
    qRegisterMetaType<mixxx::audio::SampleRate>("mixxx::audio::SampleRate");
    qRegisterMetaType<mixxx::audio::Bitrate>("mixxx::audio::Bitrate");

    // Tracks
    qRegisterMetaType<TrackId>();
    qRegisterMetaType<QSet<TrackId>>();
    qRegisterMetaType<QList<TrackId>>();
    qRegisterMetaType<TrackRef>();
    qRegisterMetaType<QList<TrackRef>>();
    qRegisterMetaType<QList<QPair<TrackRef, TrackRef>>>();
    qRegisterMetaType<TrackPointer>();

    // Crates
    qRegisterMetaType<CrateId>();
    qRegisterMetaType<QSet<CrateId>>();
    qRegisterMetaType<QList<CrateId>>();

    // Sound devices
    qRegisterMetaType<SoundDeviceId>();

    // Library Scanner
    qRegisterMetaType<RelocatedTrack>();
    qRegisterMetaType<QList<RelocatedTrack>>();

    // Various custom data types
    qRegisterMetaType<mixxx::ReplayGain>("mixxx::ReplayGain");
    qRegisterMetaType<mixxx::cache_key_t>("mixxx::cache_key_t");
    qRegisterMetaType<mixxx::Bpm>("mixxx::Bpm");
    qRegisterMetaType<mixxx::Duration>("mixxx::Duration");
    qRegisterMetaType<mixxx::audio::FramePos>("mixxx::audio::FramePos");
    qRegisterMetaType<std::optional<mixxx::RgbColor>>("std::optional<mixxx::RgbColor>");
    qRegisterMetaType<mixxx::FileInfo>("mixxx::FileInfo");
}

void MixxxApplication::setNotifyWarningThreshold(int threshold) {
    if (threshold > kDefaultEventNotifyExecTimeWarningThreshold) {
        m_eventNotifyExecTimeWarningThreshold = mixxx::Duration::fromMillis(threshold);
    }
}

bool MixxxApplication::notify(QObject* pTarget, QEvent* pEvent) {
    // Bite DJ: kiosk-style UI. The unit is touch-only, so tooltips and
    // modal dialog boxes are suppressed application-wide; all interaction
    // that stock Mixxx routes through dialogs is handled by in-skin pages
    // and the NotificationStrip instead. Sole exception: DlgPreferences
    // (and its child dialogs), kept for tuning the engine.
    switch (pEvent->type()) {
    case QEvent::ToolTip:
        // Swallow every tooltip event before it reaches any widget.
        return true;
    case QEvent::Show:
        if (auto* pDialog = qobject_cast<QDialog*>(pTarget)) {
            if (!belongsToPreferencesDialog(pDialog)) {
                // Dismiss as soon as the event loop spins again: a queued
                // reject() also quits a modal exec() loop right after it starts,
                // and callers get the safe "cancelled" result.
                QMetaObject::invokeMethod(pDialog, &QDialog::reject, Qt::QueuedConnection);
            }
        }
        break;
    default:
        break;
    }

    PerformanceTimer time;

    if (m_isDeveloper) {
        time.start();
    }

    bool ret = QApplication::notify(pTarget, pEvent);

    VERIFY_OR_DEBUG_ASSERT(pTarget != nullptr) {
        qWarning() << "Processed" << pEvent->type() << "for null pointer, this is probably a bug!";
        return ret;
    }

    if (m_isDeveloper &&
            time.elapsed() > m_eventNotifyExecTimeWarningThreshold) {
        QDebug debug = qDebug();
        debug << "Processing"
              << pEvent->type()
              << "for object";
        if (pEvent->type() == QEvent::DeferredDelete ||
                pEvent->type() == QEvent::ChildRemoved ||
                pEvent->type() == QEvent::Timer) {
            // pTarget can be already dangling in case of DeferredDelete
            debug << static_cast<void*>(pTarget); // will print dangling address
        } else {
            debug << pTarget // will print address, class and object name
                  << "running in thread:"
                  << pTarget->thread()->objectName();
        }
        debug << "took"
              << time.elapsed().debugMillisWithUnit();
    }

    return ret;
}

