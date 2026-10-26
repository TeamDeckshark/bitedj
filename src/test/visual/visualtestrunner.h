#pragma once

#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QModelIndex>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

class MixxxMainWindow;
class WTrackTableView;
namespace mixxx {
class CoreServices;
} // namespace mixxx

namespace mixxx::visualtest {

/// One scenario file: the whole application started headless at a fixed
/// window size, driven through a list of steps, with the window grabbed and
/// compared against a golden image at every `snapshot` step.
///
/// Format (JSON):
///
///     {
///       "size": [800, 480],
///       "config": {"[Group],key": "value", "[Browse],QuickLinks": "{scenario}/media/"},
///       "rekordbox": [{"file": "../sine-30.wav", "key": "8A"}],
///       "drives": {"SERATO": {"_Serato_/database V2": "drives/serato/database V2",
///                             "Music/a.flac": "media/same.flac"}},
///       "tolerance": 16,
///       "maxDiffPixels": 0,
///       "steps": [
///         {"load": "../sine-30.wav", "group": "[Channel1]"},
///         {"waitFor": "[Channel1],track_loaded", "value": 1},
///         {"waitForAnalysis": true},
///         {"set": "[Tab],overview", "value": 1},
///         {"sidebar": ["Computer", "Quick Links", "media"]},
///         {"selectRow": "Reference"},
///         {"click": "RelatedSearchChip", "text": "ARTIST"},
///         {"textColor": "Same Key 8A", "column": "Title", "color": "#6ee128"},
///         {"wait": 300},
///         {"snapshot": "grid_rb", "masks": [[x, y, w, h]]}
///       ]
///     }
///
/// `sidebar` expands the library sidebar along the given labels (case
/// insensitive) and activates the last one, as a tap on it would; with
/// `"expand": true` it expands the last one instead, leaving the sidebar up.
/// It waits up to `timeout` ms (default 0) for the labels to appear, for
/// devices found and parsed in the background.
/// `drives` puts a fake USB stick at /media/<name> (a tmpfs of its own) before
/// the application starts, holding the listed files: destination path on the
/// stick -> fixture path.
/// `selectRow` selects the first row of the visible track table with a cell
/// showing that text, as a tap on it would. `click` clicks the visible button
/// with that object name whose text starts with `text` (both optional).
/// `textColor` checks the colour the text is painted in, in the visible track
/// table row with a cell showing `textColor`, in the column whose header is
/// `column` (default: that cell): the most common colour in the grabbed cell
/// well apart from its background, within `tolerance` of `color`. A mismatch
/// fails the scenario like a snapshot does.
/// `{scenario}` in a config string is the scenario's directory.
/// Paths are relative to the scenario file. Golden images live next to it in
/// `golden/<snapshot>.png`.
struct Scenario {
    QString name;
    QString path;
    QDir dir;
    QSize size{800, 480};
    QJsonObject config;
    QJsonArray rekordbox;
    QJsonObject drives;
    int tolerance = 16;
    /// Pixels allowed past `tolerance`. Renders are pixel-identical from run
    /// to run on one machine, so anything that moves is a change.
    int maxDiffPixels = 0;
    QJsonArray steps;

    QString resolve(const QString& relativePath) const;
    QString goldenPath(const QString& snapshot) const;

    /// Reads `path`; on failure returns false and fills `error`.
    static bool load(const QString& path, Scenario* pScenario, QString* pError);
};

struct CompareResult {
    bool sizeMatches = false;
    qint64 comparedPixels = 0;
    qint64 differingPixels = 0;
    int maxChannelDelta = 0;
};

/// Compares `actual` against `golden`, ignoring the `masks` rectangles. A pixel
/// differs when any channel is more than `tolerance` apart. `pDiff`, if given,
/// receives a darkened copy of `actual` with differing pixels in red and masks
/// in blue.
CompareResult compareImages(const QImage& actual,
        const QImage& golden,
        const QVector<QRect>& masks,
        int tolerance,
        QImage* pDiff);

/// Steps a running application through a scenario. Lives on the GUI thread;
/// each step runs from the event loop so the application keeps processing
/// events (track loads, skin updates) in between.
class Runner : public QObject {
    Q_OBJECT
  public:
    struct Options {
        QString outDir;
        bool updateGolden = false;
        int startTimeoutMs = 60000;
    };

    Runner(Scenario scenario,
            Options options,
            std::shared_ptr<mixxx::CoreServices> pCoreServices,
            MixxxMainWindow* pMainWindow);

    /// Starts once the skin is up. Quits the application with exit code 0 when
    /// every snapshot matched, 1 on a mismatch and 2 on a scenario error.
    void start();

  private:
    void waitForSkin();
    void beginSteps();
    void scheduleNext(int delayMs = 0);
    void runNextStep();
    void fail(const QString& message);
    void finish();

    bool runSnapshot(const QJsonObject& step);
    void pollControl(const QString& key, double value, int remainingMs);
    void pollAnalysis(int remainingMs);
    void applySize(const QSize& size);
    void installRekordboxEntries();
    bool activateSidebarItem(const QStringList& labels, bool expandLast, QString* pError);
    void runSidebarStep(int index,
            const QStringList& labels,
            bool expandLast,
            int remainingMs);
    WTrackTableView* visibleTrackTable() const;
    /// The first cell of the visible track table showing exactly `text`.
    QModelIndex findCell(const QString& text, QString* pError) const;
    bool selectRow(const QString& text, QString* pError);
    /// A `textColor` step. Returns false on a scenario error; a wrong colour
    /// is a failure like a mismatched snapshot.
    bool checkTextColor(const QJsonObject& step, QString* pError);
    bool clickButton(const QString& objectName, const QString& text, QString* pError);

    Scenario m_scenario;
    Options m_options;
    std::shared_ptr<mixxx::CoreServices> m_pCoreServices;
    MixxxMainWindow* m_pMainWindow;
    int m_nextStep = 0;
    int m_skinWaitedMs = 0;
    QStringList m_failures;
    bool m_error = false;
    bool m_analysisActive = false;
    QElapsedTimer m_sinceAnalysisEvent;
};

} // namespace mixxx::visualtest
