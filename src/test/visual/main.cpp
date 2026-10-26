// bitedj-visual-test: starts the whole application headless (Qt's offscreen
// platform) at a scenario's window size, drives it through the scenario's
// steps and compares grabs of the main window against golden images.
//
//     bitedj-visual-test [--update] [--out DIR] scenario.json
//
// Exit code 0: every snapshot matched; 1: a snapshot differs or has no golden
// image; 2: the scenario could not be run. See src/test/visual/README.md.

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <memory>

#include "config.h"
#include "controllers/controllermanager.h"
#include "coreservices.h"
#include "errordialoghandler.h"
#include "mixxxapplication.h"
#include "mixxxmainwindow.h"
#include "preferences/configobject.h"
#include "test/visual/visualtestrunner.h"
#include "util/cmdlineargs.h"
#include "util/logging.h"

#ifdef __linux__
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace {

constexpr int kErrorExitCode = 2;

#ifdef __linux__
bool writeFile(const char* path, const QByteArray& content) {
    const int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return false;
    }
    const bool ok = write(fd, content.constData(), content.size()) == content.size();
    close(fd);
    return ok;
}

/// Hides every drive mounted on this machine from the application: empty
/// tmpfs over the directories it scans for USB sticks, in a private mount
/// namespace, so the sidebar and drive state in the screenshots do not depend
/// on what happens to be plugged in. Needs unprivileged user namespaces and
/// must run before any thread is started.
bool isolateRemovableMedia() {
    const uid_t uid = getuid();
    const gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) {
        fprintf(stderr, "visual-test: cannot isolate drives (unshare: %s)\n", strerror(errno));
        return false;
    }
    writeFile("/proc/self/setgroups", "deny");
    writeFile("/proc/self/uid_map", QByteArray::number(uid) + ' ' + QByteArray::number(uid) + " 1");
    writeFile("/proc/self/gid_map", QByteArray::number(gid) + ' ' + QByteArray::number(gid) + " 1");
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0) {
        fprintf(stderr, "visual-test: cannot isolate drives (mount: %s)\n", strerror(errno));
        return false;
    }
    for (const char* path : {"/mnt", "/media", "/run/media"}) {
        if (QDir(QString::fromLatin1(path)).exists()) {
            mount("tmpfs", path, "tmpfs", 0, "mode=0755");
        }
    }
    return true;
}
#endif

#ifdef __linux__
/// Puts the scenario's fake USB sticks under /media, each a tmpfs of its own
/// like a real stick's mount, holding the listed fixture files. Needs the
/// private mount namespace isolateRemovableMedia() set up.
bool installDrives(const mixxx::visualtest::Scenario& scenario, QString* pError) {
    for (auto drive = scenario.drives.constBegin(); drive != scenario.drives.constEnd(); ++drive) {
        const QString mountPoint = QStringLiteral("/media/") + drive.key();
        if (!QDir().mkpath(mountPoint) ||
                mount("tmpfs", mountPoint.toLocal8Bit().constData(), "tmpfs", 0, "mode=0755") !=
                        0) {
            *pError = QStringLiteral("cannot mount drive %1: %2")
                              .arg(mountPoint, QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
        const QJsonObject files = drive.value().toObject();
        for (auto file = files.constBegin(); file != files.constEnd(); ++file) {
            const QString target = QDir(mountPoint).filePath(file.key());
            const QString source = scenario.resolve(file.value().toString());
            if (!QDir().mkpath(QFileInfo(target).absolutePath()) ||
                    !QFile::copy(source, target)) {
                *pError = QStringLiteral("cannot put %1 on drive %2 as %3")
                                  .arg(source, drive.key(), file.key());
                return false;
            }
        }
    }
    return true;
}
#endif

/// `{scenario}` in a string value stands for the scenario's directory, so a
/// config entry can point at fixtures next to the scenario file.
void writeConfig(const QString& settingsPath,
        const QJsonObject& values,
        const QString& scenarioDir) {
    ConfigObject<ConfigValue> config(QDir(settingsPath).filePath(MIXXX_SETTINGS_FILE),
            QString(),
            QString());
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const QJsonValue value = it.value();
        config.set(ConfigKey::parseCommaSeparated(it.key()),
                ConfigValue(value.isString()
                                ? value.toString().replace(
                                          QStringLiteral("{scenario}"), scenarioDir)
                                : QString::number(value.toDouble())));
    }
    config.save();
}

} // anonymous namespace

int main(int argc, char* argv[]) {
#ifdef __linux__
    isolateRemovableMedia();
#endif
    // Headless and identical everywhere: no display server, and none of the
    // desktop's scaling or style.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // With a DISPLAY around (an ssh -X session), the offscreen platform asks
    // that X server for GLX and aborts when it has none; renders must not
    // depend on whatever display the test happens to inherit.
    qputenv("QT_QPA_OFFSCREEN_NO_GLX", "1");
    qunsetenv("DISPLAY");
    qunsetenv("WAYLAND_DISPLAY");
    qunsetenv("QT_SCALE_FACTOR");
    qunsetenv("QT_STYLE_OVERRIDE");

    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Bite DJ screenshot tests"));
    parser.addHelpOption();
    const QCommandLineOption updateOption(QStringLiteral("update"),
            QStringLiteral("Write the grabs as the new golden images instead of comparing."));
    const QCommandLineOption outOption(QStringLiteral("out"),
            QStringLiteral("Directory for the grabbed and diff images."),
            QStringLiteral("dir"));
    const QCommandLineOption resourceOption(QStringLiteral("resource-path"),
            QStringLiteral("The application's res/ directory."),
            QStringLiteral("dir"),
            QStringLiteral(BITEDJ_VISUAL_TEST_SOURCE_DIR "/res"));
    const QCommandLineOption logOption(QStringLiteral("log-level"),
            QStringLiteral("Application log level."),
            QStringLiteral("level"),
            QStringLiteral("warning"));
    parser.addOptions({updateOption, outOption, resourceOption, logOption});
    parser.addPositionalArgument(QStringLiteral("scenario"), QStringLiteral("Scenario JSON file."));
    parser.process(arguments);
    if (parser.positionalArguments().size() != 1) {
        fputs("visual-test: give exactly one scenario file\n", stderr);
        return kErrorExitCode;
    }

    mixxx::visualtest::Scenario scenario;
    QString error;
    if (!mixxx::visualtest::Scenario::load(parser.positionalArguments().first(), &scenario, &error)) {
        fprintf(stderr, "visual-test: %s\n", qPrintable(error));
        return kErrorExitCode;
    }

#ifdef __linux__
    if (!installDrives(scenario, &error)) {
        fprintf(stderr, "visual-test: %s\n", qPrintable(error));
        return kErrorExitCode;
    }
#endif

    // A fresh profile every run: nothing from a previous run or from the
    // developer's own ~/.bitedj may show up in the screenshots.
    QTemporaryDir settingsDir;
    if (!settingsDir.isValid()) {
        fputs("visual-test: cannot create a settings directory\n", stderr);
        return kErrorExitCode;
    }
    writeConfig(settingsDir.path(), scenario.config, scenario.dir.absolutePath());

    mixxx::visualtest::Runner::Options options;
    options.updateGolden = parser.isSet(updateOption);
    options.outDir = parser.isSet(outOption)
            ? parser.value(outOption)
            : QDir::temp().filePath(QStringLiteral("bitedj-visual-test"));
    options.outDir = QDir(options.outDir).absoluteFilePath(scenario.name);
    QDir().mkpath(options.outDir);

    const QByteArray program = QByteArray(argv[0]);
    const QList<QByteArray> appArguments = {program,
            QByteArrayLiteral("--settings-path"),
            settingsDir.path().toLocal8Bit(),
            QByteArrayLiteral("--resource-path"),
            parser.value(resourceOption).toLocal8Bit(),
            QByteArrayLiteral("--log-level"),
            parser.value(logOption).toLocal8Bit()};
    std::vector<char*> appArgv;
    for (const QByteArray& argument : appArguments) {
        appArgv.push_back(const_cast<char*>(argument.constData()));
    }
    int appArgc = static_cast<int>(appArgv.size());
    appArgv.push_back(nullptr);

    QCoreApplication::setOrganizationDomain(QStringLiteral("mixxx.org"));
#ifdef MIXXX_USE_QOPENGL
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif
    CmdlineArgs& args = CmdlineArgs::Instance();
    if (!args.parse(appArgc, appArgv.data())) {
        return kErrorExitCode;
    }
    QThread::currentThread()->setObjectName(QStringLiteral("Main"));
    ErrorDialogHandler::instance();

    MixxxApplication app(appArgc, appArgv.data());
    QObject::connect(&app, &MixxxApplication::lastWindowClosed, &app, &MixxxApplication::quit);

    int exitCode = kErrorExitCode;
    {
        const auto pCoreServices = std::make_shared<mixxx::CoreServices>(args, &app);
        // Destroyed before CoreServices shuts down, as in main.cpp.
        MixxxMainWindow mainWindow(pCoreServices);
        app.processEvents();
        app.installEventFilter(&mainWindow);
        QObject::connect(pCoreServices.get(),
                &mixxx::CoreServices::initializationProgressUpdate,
                &mainWindow,
                &MixxxMainWindow::initializationProgressUpdate);

        pCoreServices->initialize(&app);
#ifdef MIXXX_USE_QOPENGL
        mainWindow.initializeQOpenGL();
#else
        mainWindow.initialize();
#endif
        pCoreServices->getControllerManager()->setUpDevices();

        if (ErrorDialogHandler::instance()->checkError()) {
            fputs("visual-test: the application failed to start\n", stderr);
        } else {
            mainWindow.resize(scenario.size);
            mainWindow.show();
            mixxx::visualtest::Runner runner(scenario, options, pCoreServices, &mainWindow);
            runner.start();
            exitCode = app.exec();
        }
    }
    mixxx::Logging::shutdown();
    return exitCode;
}
