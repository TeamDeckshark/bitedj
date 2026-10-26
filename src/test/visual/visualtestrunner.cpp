#include "test/visual/visualtestrunner.h"

#include <QAbstractButton>
#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPainter>
#include <QTimer>
#include <QtDebug>
#include <algorithm>
#include <cstdio>

#include "analyzer/analyzerprogress.h"
#include "control/controlobject.h"
#include "coreservices.h"
#include "library/rekordbox/rekordboxanalysis.h"
#include "mixer/playermanager.h"
#include "mixxxmainwindow.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wtracktableview.h"

namespace mixxx::visualtest {

namespace {

// Polling interval for waitFor steps and for the skin to come up.
constexpr int kPollMs = 50;
// Default time a waitFor step may take before the scenario fails.
constexpr int kDefaultWaitForTimeoutMs = 15000;
// Default time a waitForAnalysis step may take.
constexpr int kDefaultAnalysisTimeoutMs = 60000;
// How long the analyzer must have been quiet for waitForAnalysis to pass.
constexpr int kAnalysisQuietMs = 500;
// Event-loop passes run right before every grab, so queued repaints and
// layout requests from the previous step have landed.
constexpr int kSettlePasses = 20;

// The skin creates this; once it exists the main window is fully built.
const ConfigKey kSkinReadyKey(QStringLiteral("[Tab]"), QStringLiteral("current"));

void report(const QString& message) {
    fprintf(stderr, "visual-test: %s\n", qPrintable(message));
    fflush(stderr);
}

QVector<QRect> parseRects(const QJsonArray& array) {
    QVector<QRect> rects;
    for (const QJsonValue& value : array) {
        const QJsonArray rect = value.toArray();
        if (rect.size() == 4) {
            rects.append(QRect(rect[0].toInt(), rect[1].toInt(), rect[2].toInt(), rect[3].toInt()));
        }
    }
    return rects;
}

} // anonymous namespace

QString Scenario::resolve(const QString& relativePath) const {
    return QDir::cleanPath(dir.absoluteFilePath(relativePath));
}

QString Scenario::goldenPath(const QString& snapshot) const {
    return dir.absoluteFilePath(QStringLiteral("golden/%1.png").arg(snapshot));
}

// static
bool Scenario::load(const QString& path, Scenario* pScenario, QString* pError) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *pError = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        *pError = QStringLiteral("%1: %2").arg(path, parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    const QFileInfo fileInfo(path);
    pScenario->name = fileInfo.completeBaseName();
    pScenario->path = fileInfo.absoluteFilePath();
    pScenario->dir = fileInfo.absoluteDir();
    const QJsonArray size = root.value(QStringLiteral("size")).toArray();
    if (size.size() == 2) {
        pScenario->size = QSize(size[0].toInt(), size[1].toInt());
    }
    pScenario->config = root.value(QStringLiteral("config")).toObject();
    pScenario->rekordbox = root.value(QStringLiteral("rekordbox")).toArray();
    pScenario->drives = root.value(QStringLiteral("drives")).toObject();
    pScenario->tolerance = root.value(QStringLiteral("tolerance")).toInt(pScenario->tolerance);
    pScenario->maxDiffPixels =
            root.value(QStringLiteral("maxDiffPixels")).toInt(pScenario->maxDiffPixels);
    pScenario->steps = root.value(QStringLiteral("steps")).toArray();
    if (pScenario->steps.isEmpty()) {
        *pError = QStringLiteral("%1: no steps").arg(path);
        return false;
    }
    return true;
}

CompareResult compareImages(const QImage& actualImage,
        const QImage& goldenImage,
        const QVector<QRect>& masks,
        int tolerance,
        QImage* pDiff) {
    CompareResult result;
    const QImage actual = actualImage.convertToFormat(QImage::Format_ARGB32);
    const QImage golden = goldenImage.convertToFormat(QImage::Format_ARGB32);
    result.sizeMatches = actual.size() == golden.size();
    if (pDiff) {
        *pDiff = actual.copy();
        QPainter painter(pDiff);
        painter.fillRect(pDiff->rect(), QColor(0, 0, 0, 170));
        for (const QRect& mask : masks) {
            painter.fillRect(mask, QColor(0, 80, 255, 120));
        }
    }
    if (!result.sizeMatches) {
        return result;
    }
    for (int y = 0; y < actual.height(); ++y) {
        const auto* pActual = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const auto* pGolden = reinterpret_cast<const QRgb*>(golden.constScanLine(y));
        for (int x = 0; x < actual.width(); ++x) {
            if (std::any_of(masks.cbegin(), masks.cend(), [x, y](const QRect& mask) {
                    return mask.contains(x, y);
                })) {
                continue;
            }
            ++result.comparedPixels;
            const QRgb a = pActual[x];
            const QRgb g = pGolden[x];
            const int delta = std::max({std::abs(qRed(a) - qRed(g)),
                    std::abs(qGreen(a) - qGreen(g)),
                    std::abs(qBlue(a) - qBlue(g)),
                    std::abs(qAlpha(a) - qAlpha(g))});
            result.maxChannelDelta = std::max(result.maxChannelDelta, delta);
            if (delta > tolerance) {
                ++result.differingPixels;
                if (pDiff) {
                    pDiff->setPixel(x, y, qRgb(255, 0, 0));
                }
            }
        }
    }
    return result;
}

Runner::Runner(Scenario scenario,
        Options options,
        std::shared_ptr<mixxx::CoreServices> pCoreServices,
        MixxxMainWindow* pMainWindow)
        : m_scenario(std::move(scenario)),
          m_options(std::move(options)),
          m_pCoreServices(std::move(pCoreServices)),
          m_pMainWindow(pMainWindow) {
    m_sinceAnalysisEvent.start();
    PlayerManager* pPlayerManager = m_pCoreServices->getPlayerManager().get();
    connect(pPlayerManager,
            &PlayerManager::trackAnalyzerProgress,
            this,
            [this](TrackId, AnalyzerProgress progress) {
                m_analysisActive = progress < kAnalyzerProgressDone;
                m_sinceAnalysisEvent.restart();
            });
    connect(pPlayerManager, &PlayerManager::trackAnalyzerIdle, this, [this]() {
        m_analysisActive = false;
        m_sinceAnalysisEvent.restart();
    });
}

void Runner::start() {
    QTimer::singleShot(0, this, &Runner::waitForSkin);
}

void Runner::waitForSkin() {
    if (ControlObject::exists(kSkinReadyKey)) {
        beginSteps();
        return;
    }
    m_skinWaitedMs += kPollMs;
    if (m_skinWaitedMs > m_options.startTimeoutMs) {
        fail(QStringLiteral("the skin did not load within %1 ms").arg(m_options.startTimeoutMs));
        return;
    }
    QTimer::singleShot(kPollMs, this, &Runner::waitForSkin);
}

void Runner::beginSteps() {
    installRekordboxEntries();
    applySize(m_scenario.size);
    scheduleNext();
}

void Runner::applySize(const QSize& size) {
    // The screen is the size it is: a skin that wants more room than that is
    // cut off on the device, and the grab must show it cut off rather than
    // quietly growing the window to the layout's minimum.
    const QSize layoutMinimum = m_pMainWindow->minimumSizeHint();
    if (layoutMinimum.width() > size.width() || layoutMinimum.height() > size.height()) {
        report(QStringLiteral("%1: the layout wants at least %2x%3; forcing %4x%5")
                        .arg(m_scenario.name)
                        .arg(layoutMinimum.width())
                        .arg(layoutMinimum.height())
                        .arg(size.width())
                        .arg(size.height()));
    }
    m_pMainWindow->setFixedSize(size);
}

void Runner::installRekordboxEntries() {
    if (m_scenario.rekordbox.isEmpty()) {
        return;
    }
    // Stands in for a scanned rekordbox export on a drive, so the tracks named
    // here count as rekordbox-analysed wherever they are loaded from.
    QHash<QString, RekordboxAnalysis::Entry> entries;
    for (const QJsonValue& value : std::as_const(m_scenario.rekordbox)) {
        const QJsonObject object = value.toObject();
        RekordboxAnalysis::Entry entry;
        entry.keyText = object.value(QStringLiteral("key")).toString();
        const QString anlz = object.value(QStringLiteral("anlz")).toString();
        if (!anlz.isEmpty()) {
            entry.anlzPath = m_scenario.resolve(anlz);
        }
        entries.insert(m_scenario.resolve(object.value(QStringLiteral("file")).toString()), entry);
    }
    RekordboxAnalysis::setLookup(
            [entries](const QString& location) -> std::optional<RekordboxAnalysis::Entry> {
                const auto it = entries.constFind(QDir::cleanPath(location));
                if (it == entries.constEnd()) {
                    return std::nullopt;
                }
                return *it;
            });
}

bool Runner::activateSidebarItem(
        const QStringList& labels, bool expandLast, QString* pError) {
    WLibrarySidebar* pSidebar = m_pMainWindow->findChild<WLibrarySidebar*>();
    if (!pSidebar || !pSidebar->model()) {
        *pError = QStringLiteral("no library sidebar");
        return false;
    }
    if (labels.isEmpty()) {
        *pError = QStringLiteral("sidebar: no labels");
        return false;
    }
    QAbstractItemModel* pModel = pSidebar->model();
    QModelIndex parent;
    QModelIndex found;
    for (const QString& label : labels) {
        if (parent.isValid()) {
            // Lazily populated nodes (drives, folders) fill in on expansion.
            pSidebar->expand(parent);
            for (int i = 0; i < kSettlePasses; ++i) {
                QCoreApplication::processEvents();
            }
        }
        found = QModelIndex();
        QStringList seen;
        for (int row = 0; row < pModel->rowCount(parent); ++row) {
            const QModelIndex child = pModel->index(row, 0, parent);
            const QString text = child.data(Qt::DisplayRole).toString();
            if (text.compare(label, Qt::CaseInsensitive) == 0) {
                found = child;
                break;
            }
            seen.append(text);
        }
        if (!found.isValid()) {
            *pError = QStringLiteral("sidebar: no item \"%1\" (found: %2)")
                              .arg(label, seen.join(QStringLiteral(", ")));
            return false;
        }
        parent = found;
    }
    pSidebar->setCurrentIndex(found);
    if (expandLast) {
        pSidebar->expand(found);
        return true;
    }
    emit pSidebar->clicked(found);
    // A tap on a leaf also swaps the sidebar for the track table.
    pSidebar->activateSelectedLeaf();
    return true;
}

void Runner::runSidebarStep(int index,
        const QStringList& labels,
        bool expandLast,
        int remainingMs) {
    QString error;
    if (activateSidebarItem(labels, expandLast, &error)) {
        scheduleNext();
        return;
    }
    if (remainingMs <= 0) {
        fail(QStringLiteral("step %1: %2").arg(index).arg(error));
        return;
    }
    QTimer::singleShot(kPollMs, this, [this, index, labels, expandLast, remainingMs]() {
        runSidebarStep(index, labels, expandLast, remainingMs - kPollMs);
    });
}

WTrackTableView* Runner::visibleTrackTable() const {
    const auto views = m_pMainWindow->findChildren<WTrackTableView*>();
    for (WTrackTableView* pView : views) {
        if (pView->isVisible() && pView->model()) {
            return pView;
        }
    }
    return nullptr;
}

QModelIndex Runner::findCell(const QString& text, QString* pError) const {
    WTrackTableView* pView = visibleTrackTable();
    if (!pView) {
        *pError = QStringLiteral("no visible track table");
        return {};
    }
    const QAbstractItemModel* pModel = pView->model();
    for (int row = 0; row < pModel->rowCount(); ++row) {
        for (int column = 0; column < pModel->columnCount(); ++column) {
            const QModelIndex index = pModel->index(row, column);
            if (index.data(Qt::DisplayRole).toString() == text) {
                return index;
            }
        }
    }
    *pError = QStringLiteral("no row shows \"%1\"").arg(text);
    return {};
}

bool Runner::selectRow(const QString& text, QString* pError) {
    const QModelIndex index = findCell(text, pError);
    if (!index.isValid()) {
        pError->prepend(QStringLiteral("selectRow: "));
        return false;
    }
    WTrackTableView* pView = visibleTrackTable();
    pView->setCurrentIndex(index);
    pView->selectRow(index.row());
    return true;
}

bool Runner::checkTextColor(const QJsonObject& step, QString* pError) {
    const QString text = step.value(QStringLiteral("textColor")).toString();
    const QString columnLabel = step.value(QStringLiteral("column")).toString();
    const QColor expected(step.value(QStringLiteral("color")).toString());
    const QString name = QStringLiteral("textColor \"%1\"%2")
                                 .arg(text,
                                         columnLabel.isEmpty()
                                                 ? QString()
                                                 : QStringLiteral(" ") + columnLabel);
    if (!expected.isValid()) {
        *pError = name + QStringLiteral(": no valid \"color\"");
        return false;
    }
    QModelIndex index = findCell(text, pError);
    if (!index.isValid()) {
        pError->prepend(name + QStringLiteral(": "));
        return false;
    }
    WTrackTableView* pView = visibleTrackTable();
    if (!columnLabel.isEmpty()) {
        const QAbstractItemModel* pModel = pView->model();
        int column = -1;
        for (int i = 0; i < pModel->columnCount(); ++i) {
            if (!pView->isColumnHidden(i) &&
                    pModel->headerData(i, Qt::Horizontal)
                                    .toString()
                                    .compare(columnLabel, Qt::CaseInsensitive) == 0) {
                column = i;
                break;
            }
        }
        if (column < 0) {
            *pError = name + QStringLiteral(": no visible column \"%1\"").arg(columnLabel);
            return false;
        }
        index = index.sibling(index.row(), column);
    }

    pView->scrollTo(index);
    for (int i = 0; i < kSettlePasses; ++i) {
        QCoreApplication::processEvents();
    }
    // The cell as the window paints it, delegates and style sheet included.
    const QRect cellRect = pView->visualRect(index);
    const QPoint topLeft = pView->viewport()->mapTo(m_pMainWindow, cellRect.topLeft());
    const QImage cell = m_pMainWindow->grab()
                                .toImage()
                                .copy(QRect(topLeft, cellRect.size()))
                                .convertToFormat(QImage::Format_RGB32);

    // The background is the cell's most common colour, the text the most
    // common one well apart from it: antialiased edges blend the two, but at
    // these sizes the glyphs' solid pixels outnumber any single blend.
    QHash<QRgb, int> counts;
    for (int y = 0; y < cell.height(); ++y) {
        const auto* pLine = reinterpret_cast<const QRgb*>(cell.constScanLine(y));
        for (int x = 0; x < cell.width(); ++x) {
            ++counts[pLine[x] & RGB_MASK];
        }
    }
    QRgb background = 0;
    int backgroundCount = -1;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        if (it.value() > backgroundCount) {
            background = it.key();
            backgroundCount = it.value();
        }
    }
    constexpr int kForegroundDistance = 96;
    QRgb foreground = 0;
    int foregroundCount = 0;
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        const int distance = std::max({std::abs(qRed(it.key()) - qRed(background)),
                std::abs(qGreen(it.key()) - qGreen(background)),
                std::abs(qBlue(it.key()) - qBlue(background))});
        if (distance >= kForegroundDistance && it.value() > foregroundCount) {
            foreground = it.key();
            foregroundCount = it.value();
        }
    }
    if (foregroundCount == 0) {
        m_failures.append(name);
        report(QStringLiteral("%1: FAIL no text drawn in the cell").arg(name));
        return true;
    }
    const QColor actual(foreground);
    const int tolerance = step.value(QStringLiteral("tolerance")).toInt(m_scenario.tolerance);
    const int delta = std::max({std::abs(actual.red() - expected.red()),
            std::abs(actual.green() - expected.green()),
            std::abs(actual.blue() - expected.blue())});
    if (delta > tolerance) {
        m_failures.append(name);
        report(QStringLiteral("%1: FAIL text is %2, expected %3")
                        .arg(name, actual.name(), expected.name()));
    } else {
        report(QStringLiteral("%1: ok (%2)").arg(name, actual.name()));
    }
    return true;
}

bool Runner::clickButton(const QString& objectName, const QString& text, QString* pError) {
    const auto buttons = m_pMainWindow->findChildren<QAbstractButton*>();
    for (QAbstractButton* pButton : buttons) {
        if (!pButton->isVisible() ||
                (!objectName.isEmpty() && pButton->objectName() != objectName) ||
                !pButton->text().startsWith(text)) {
            continue;
        }
        pButton->click();
        return true;
    }
    *pError = QStringLiteral("click: no visible button %1 \"%2\"").arg(objectName, text);
    return false;
}

void Runner::scheduleNext(int delayMs) {
    QTimer::singleShot(delayMs, this, &Runner::runNextStep);
}

void Runner::runNextStep() {
    if (m_nextStep >= m_scenario.steps.size()) {
        finish();
        return;
    }
    const int index = m_nextStep++;
    const QJsonObject step = m_scenario.steps.at(index).toObject();

    if (step.contains(QStringLiteral("set"))) {
        const ConfigKey key = ConfigKey::parseCommaSeparated(
                step.value(QStringLiteral("set")).toString());
        if (!ControlObject::exists(key)) {
            fail(QStringLiteral("step %1: no control %2").arg(index).arg(key.group + "," + key.item));
            return;
        }
        ControlObject::set(key, step.value(QStringLiteral("value")).toDouble());
        scheduleNext();
    } else if (step.contains(QStringLiteral("load"))) {
        const QString location = m_scenario.resolve(step.value(QStringLiteral("load")).toString());
        if (!QFileInfo::exists(location)) {
            fail(QStringLiteral("step %1: no file %2").arg(index).arg(location));
            return;
        }
        const QString group =
                step.value(QStringLiteral("group")).toString(QStringLiteral("[Channel1]"));
        m_pCoreServices->getPlayerManager()->slotLoadLocationToPlayer(location, group, false);
        scheduleNext();
    } else if (step.contains(QStringLiteral("sidebar"))) {
        QStringList labels;
        for (const QJsonValue& label : step.value(QStringLiteral("sidebar")).toArray()) {
            labels.append(label.toString());
        }
        runSidebarStep(index,
                labels,
                step.value(QStringLiteral("expand")).toBool(false),
                step.value(QStringLiteral("timeout")).toInt(0));
    } else if (step.contains(QStringLiteral("selectRow"))) {
        QString error;
        if (!selectRow(step.value(QStringLiteral("selectRow")).toString(), &error)) {
            fail(QStringLiteral("step %1: %2").arg(index).arg(error));
            return;
        }
        scheduleNext();
    } else if (step.contains(QStringLiteral("textColor"))) {
        QString error;
        if (!checkTextColor(step, &error)) {
            fail(QStringLiteral("step %1: %2").arg(index).arg(error));
            return;
        }
        scheduleNext();
    } else if (step.contains(QStringLiteral("click"))) {
        QString error;
        if (!clickButton(step.value(QStringLiteral("click")).toString(),
                    step.value(QStringLiteral("text")).toString(),
                    &error)) {
            fail(QStringLiteral("step %1: %2").arg(index).arg(error));
            return;
        }
        scheduleNext();
    } else if (step.contains(QStringLiteral("waitFor"))) {
        const QString key = step.value(QStringLiteral("waitFor")).toString();
        pollControl(key,
                step.value(QStringLiteral("value")).toDouble(1.0),
                step.value(QStringLiteral("timeout")).toInt(kDefaultWaitForTimeoutMs));
    } else if (step.contains(QStringLiteral("waitForAnalysis"))) {
        pollAnalysis(step.value(QStringLiteral("timeout")).toInt(kDefaultAnalysisTimeoutMs));
    } else if (step.contains(QStringLiteral("wait"))) {
        scheduleNext(step.value(QStringLiteral("wait")).toInt());
    } else if (step.contains(QStringLiteral("resize"))) {
        const QJsonArray size = step.value(QStringLiteral("resize")).toArray();
        applySize(QSize(size.at(0).toInt(), size.at(1).toInt()));
        scheduleNext();
    } else if (step.contains(QStringLiteral("snapshot"))) {
        if (runSnapshot(step)) {
            scheduleNext();
        }
    } else {
        fail(QStringLiteral("step %1: unknown step %2")
                        .arg(index)
                        .arg(QString::fromUtf8(QJsonDocument(step).toJson(QJsonDocument::Compact))));
    }
}

void Runner::pollAnalysis(int remainingMs) {
    // Idle, and for long enough that a load or source switch that has only
    // just been queued would have reported in by now.
    if (!m_analysisActive && m_sinceAnalysisEvent.elapsed() >= kAnalysisQuietMs) {
        scheduleNext();
        return;
    }
    if (remainingMs <= 0) {
        fail(QStringLiteral("timed out waiting for the analysis to finish"));
        return;
    }
    QTimer::singleShot(kPollMs, this, [this, remainingMs]() {
        pollAnalysis(remainingMs - kPollMs);
    });
}

void Runner::pollControl(const QString& key, double value, int remainingMs) {
    const ConfigKey configKey = ConfigKey::parseCommaSeparated(key);
    if (ControlObject::exists(configKey) && ControlObject::get(configKey) == value) {
        scheduleNext();
        return;
    }
    if (remainingMs <= 0) {
        fail(QStringLiteral("timed out waiting for %1 == %2").arg(key).arg(value));
        return;
    }
    QTimer::singleShot(kPollMs, this, [this, key, value, remainingMs]() {
        pollControl(key, value, remainingMs - kPollMs);
    });
}

bool Runner::runSnapshot(const QJsonObject& step) {
    const QString name = step.value(QStringLiteral("snapshot")).toString();
    if (name.isEmpty()) {
        fail(QStringLiteral("snapshot without a name"));
        return false;
    }
    for (int i = 0; i < kSettlePasses; ++i) {
        QCoreApplication::processEvents();
    }
    const QImage actual = m_pMainWindow->grab().toImage();
    if (actual.size() != m_scenario.size) {
        report(QStringLiteral("%1: window is %2x%3, scenario asks for %4x%5")
                        .arg(name)
                        .arg(actual.width())
                        .arg(actual.height())
                        .arg(m_scenario.size.width())
                        .arg(m_scenario.size.height()));
    }

    const QDir outDir(m_options.outDir);
    const QString actualPath = outDir.absoluteFilePath(name + QStringLiteral(".png"));
    actual.save(actualPath);

    const QString goldenPath = m_scenario.goldenPath(name);
    if (m_options.updateGolden) {
        QDir().mkpath(QFileInfo(goldenPath).absolutePath());
        if (!actual.save(goldenPath)) {
            fail(QStringLiteral("cannot write %1").arg(goldenPath));
            return false;
        }
        report(QStringLiteral("%1: golden updated (%2)").arg(name, goldenPath));
        return true;
    }

    const QImage golden(goldenPath);
    if (golden.isNull()) {
        m_failures.append(name);
        report(QStringLiteral("%1: FAIL no golden image %2 (actual: %3; "
                              "run with --update to create it)")
                        .arg(name, goldenPath, actualPath));
        return true;
    }

    const QVector<QRect> masks = parseRects(step.value(QStringLiteral("masks")).toArray());
    const int tolerance = step.value(QStringLiteral("tolerance")).toInt(m_scenario.tolerance);
    const int maxDiffPixels =
            step.value(QStringLiteral("maxDiffPixels")).toInt(m_scenario.maxDiffPixels);
    QImage diff;
    const CompareResult result = compareImages(actual, golden, masks, tolerance, &diff);
    const QString diffPath = outDir.absoluteFilePath(name + QStringLiteral(".diff.png"));
    if (!result.sizeMatches) {
        m_failures.append(name);
        report(QStringLiteral("%1: FAIL size %2x%3 != golden %4x%5 (actual: %6)")
                        .arg(name)
                        .arg(actual.width())
                        .arg(actual.height())
                        .arg(golden.width())
                        .arg(golden.height())
                        .arg(actualPath));
        return true;
    }
    if (result.differingPixels > maxDiffPixels) {
        diff.save(diffPath);
        m_failures.append(name);
        report(QStringLiteral("%1: FAIL %2 of %3 pixels differ (allowed %4), "
                              "max channel delta %5 (actual: %6, diff: %7)")
                        .arg(name)
                        .arg(result.differingPixels)
                        .arg(result.comparedPixels)
                        .arg(maxDiffPixels)
                        .arg(result.maxChannelDelta)
                        .arg(actualPath, diffPath));
    } else {
        report(QStringLiteral("%1: ok (%2 pixels differ, max channel delta %3)")
                        .arg(name)
                        .arg(result.differingPixels)
                        .arg(result.maxChannelDelta));
    }
    return true;
}

void Runner::fail(const QString& message) {
    report(QStringLiteral("%1: ERROR %2").arg(m_scenario.name, message));
    m_error = true;
    finish();
}

void Runner::finish() {
    int exitCode = 0;
    if (m_error) {
        exitCode = 2;
    } else if (!m_failures.isEmpty()) {
        report(QStringLiteral("%1: %2 check(s) failed: %3")
                        .arg(m_scenario.name)
                        .arg(m_failures.size())
                        .arg(m_failures.join(QStringLiteral(", "))));
        exitCode = 1;
    } else {
        report(QStringLiteral("%1: passed").arg(m_scenario.name));
    }
    QCoreApplication::exit(exitCode);
}

} // namespace mixxx::visualtest
