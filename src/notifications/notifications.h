#pragma once

#include <QAtomicPointer>
#include <QObject>
#include <QString>
#include <QTimer>

class ControlObject;

// Bite DJ: in-skin replacement for Qt's modal warning dialogs. Publishes a
// single most-recent notification to the skin via [Notifications],visible
// (bool) and [Notifications],severity (0=info, 1=warning, 2=error). Message
// text is delivered to WNotificationStrip via the messagePosted(QString, int)
// signal — CO values are doubles, so the QString is carried alongside the COs
// (mirrors the LibraryBreadcrumb / leafItemActivated transport in 124e127).
//
// Owned by CoreServices alongside RateRangeControl so a singleton is reachable
// from any caller via tryInstance(). Lives on the GUI thread; publish() must
// be called from the GUI thread (the QMessageBox sites being replaced already
// run there).
class Notifications : public QObject {
    Q_OBJECT
  public:
    enum class Severity {
        Info = 0,
        Warning = 1,
        Error = 2,
    };

    Notifications();
    ~Notifications() override;

    static Notifications* tryInstance() {
        return s_pInstance.loadAcquire();
    }

    // publish() shows `message` and starts a 5-second auto-clear timer.
    // publishSticky() shows the message indefinitely; only an explicit
    // clear() or another publish() removes it. Use sticky for long-running
    // operations whose duration is unknown (audio device re-enumeration,
    // controller re-init, MIDI hot-plug rescan).
    void publish(const QString& message, Severity severity);
    void publishSticky(const QString& message, Severity severity);
    void clear();

    // Busy state: when set, the [Notifications],busy CO flips to 1 and
    // busyChanged(true) is emitted. Widgets subscribe to grey themselves
    // out and ignore input until the operation finishes. Used by
    // ControllerSettings around setUpDevices and applyMapping, where the
    // controller thread (or audio device re-init) introduces a visible
    // delay between tap and effect.
    //
    // Busy also suppresses touchscreen input app-wide (see eventFilter):
    // presses are swallowed while busy and for a grace window after busy
    // clears. The grace window is the load-bearing part — an audio-config
    // apply blocks the GUI thread for several seconds, during which taps
    // pile up in the platform's event queue and would otherwise all fire
    // at once when the thread unblocks. Those queued taps are delivered
    // on the first event-loop iterations after setBusy(false), squarely
    // inside the grace window, and get discarded.
    void setBusy(bool busy);
    bool isBusy() const {
        return m_busy;
    }

    bool eventFilter(QObject* pObj, QEvent* pEvent) override;

  signals:
    void messagePosted(const QString& message, int severity);
    void cleared();
    void busyChanged(bool busy);

  private slots:
    void onIdleTimeout();
    void onInputGraceTimeout();

  private:
    static constexpr int kIdleTimeoutMs = 5000;
    // How long input stays suppressed after busy clears. Long enough to
    // cover the event-loop iterations that flush taps queued during a
    // GUI-thread freeze (they arrive nearly immediately after the loop
    // resumes), short enough that a deliberate tap on the freshly-updated
    // UI isn't noticeably eaten.
    static constexpr int kInputGraceMs = 750;
    static QAtomicPointer<Notifications> s_pInstance;

    ControlObject* m_pCoVisible;
    ControlObject* m_pCoSeverity;
    ControlObject* m_pCoBusy;
    QTimer m_idleTimer;
    QTimer m_inputGraceTimer;
    bool m_busy;
    // True while this object is installed as the application-wide event
    // filter (busy, or inside the post-busy grace window).
    bool m_suppressingInput;
    // Set when a press was swallowed so the matching release is swallowed
    // too. Releases whose press was delivered normally must pass through:
    // WPushButton::mouseReleaseEvent emits the left-up control write even
    // without a preceding press, so an unpaired swallowed press would leave
    // buttons stuck down, while an unpaired *delivered* release lets the
    // tap that kicked off the busy operation complete cleanly.
    bool m_swallowedPress;
};
