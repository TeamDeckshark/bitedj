#include "coreservices.h"

#include <QApplication>
#include <QProcess>
#include <QProcessEnvironment>
#include <QtGlobal>

#ifdef __BROADCAST__
#include "broadcast/broadcastmanager.h"
#endif
#include "control/controlindicatortimer.h"
#include "controllers/controllermanager.h"
#include "controllers/keyboard/keyboardeventfilter.h"
#include "database/mixxxdb.h"
#include "effects/effectsmanager.h"
#include "engine/controls/raterangecontrol.h"
#include "engine/enginemixer.h"
#include "library/coverartcache.h"
#include "library/library.h"
#include "library/library_prefs.h"
#include "library/playedtracks.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "mixer/samplerdrive.h"
#include "moc_coreservices.cpp"
#include "notifications/notifications.h"
#include "preferences/audiodevicesettings.h"
#include "preferences/controllersettings.h"
#include "preferences/systemsettings.h"
#include "preferences/dialog/dlgpreferences.h"
#include "preferences/settingsmanager.h"
#ifdef __MODPLUG__
#include "preferences/dialog/dlgprefmodplug.h"
#endif
#include "skin/highcontrast.h"
#include "skin/skincontrols.h"
#include "soundio/soundmanager.h"
#include "sources/soundsourceproxy.h"
#include "util/clipboard.h"
#include "util/db/dbconnectionpooled.h"
#include "util/font.h"
#include "util/logger.h"
#include "util/screensavermanager.h"
#include "util/statsmanager.h"
#include "util/time.h"
#include "util/translations.h"
#include "util/versionstore.h"
#include "vinylcontrol/vinylcontrolmanager.h"

#ifdef __APPLE__
#include "util/sandbox.h"
#endif

namespace {
const mixxx::Logger kLogger("CoreServices");
constexpr int kMicrophoneCount = 4;
constexpr int kAuxiliaryCount = 4;
constexpr int kSamplerCount = 4;

#define CLEAR_AND_CHECK_DELETED(x) clearHelper(x, #x);

template<typename T>
void clearHelper(std::shared_ptr<T>& ref_ptr, const char* name) {
    std::weak_ptr<T> weak(ref_ptr);
    ref_ptr.reset();
    if (auto shared = weak.lock()) {
        qWarning() << name << "was leaked! Use count:" << shared.use_count();
        DEBUG_ASSERT(false);
    }
}


#if defined(Q_OS_LINUX)
QLocale localeFromXkbSymbol(const QString& xkbLayout) {
    // This maps XKB layouts to locales of keyboard mappings that are shipped with Mixxx
    static const QMap<QString, QLocale> xkbToLocaleMap = {
            {"cz", QLocale(QLocale::Czech, QLocale::CzechRepublic)},              // cs_CZ.kbd.cfg
            {"de", QLocale(QLocale::German, QLocale::Germany)},                   // de_DE.kbd.cfg
            {"de+nodeadkeys", QLocale(QLocale::German, QLocale::Germany)},        // de_DE.kbd.cfg
            {"es", QLocale(QLocale::Spanish, QLocale::Spain)},                    // es_ES.kbd.cfg
            {"es+nodeadkeys", QLocale(QLocale::Spanish, QLocale::Spain)},         // es_ES.kbd.cfg
            {"fr", QLocale(QLocale::French, QLocale::France)},                    // fr_FR.kbd.cfg
            {"fr+nodeadkeys", QLocale(QLocale::French, QLocale::France)},         // fr_FR.kbd.cfg
            {"dk", QLocale(QLocale::Danish, QLocale::Denmark)},                   // da_DK.kbd.cfg
            {"dk+nodeadkeys", QLocale(QLocale::Danish, QLocale::Denmark)},        // da_DK.kbd.cfg
            {"gr", QLocale(QLocale::Greek, QLocale::Greece)},                     // el_GR.kbd.cfg
            {"gr+nodeadkeys", QLocale(QLocale::Greek, QLocale::Greece)},          // el_GR.kbd.cfg
            {"fi", QLocale(QLocale::Finnish, QLocale::Finland)},                  // fi_FI.kbd.cfg
            {"it", QLocale(QLocale::Italian, QLocale::Italy)},                    // it_IT.kbd.cfg
            {"it+nodeadkeys", QLocale(QLocale::Italian, QLocale::Italy)},         // it_IT.kbd.cfg
            {"us", QLocale(QLocale::English, QLocale::UnitedStates)},             // en_US.kbd.cfg
            {"ru", QLocale(QLocale::Russian, QLocale::Russia)},                   // ru_RU.kbd.cfg
            {"ch", QLocale(QLocale::German, QLocale::Switzerland)},               // de_CH.kbd.cfg
            {"ch+de_nodeadkeys", QLocale(QLocale::German, QLocale::Switzerland)}, // de_CH.kbd.cfg
            {"ch+fr", QLocale(QLocale::French, QLocale::Switzerland)},            // fr_CH.kbd.cfg
            {"ch+fr_nodeadkeys", QLocale(QLocale::French, QLocale::Switzerland)}  // fr_CH.kbd.cfg
    };
    return xkbToLocaleMap.value(xkbLayout, QLocale(QLocale::English, QLocale::UnitedStates));
}

inline bool isGnomeSession() {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString desktop = env.value("XDG_CURRENT_DESKTOP").toLower();
    return desktop.contains("gnome");
}

inline bool isXfceSession() {
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString desktop = env.value("XDG_CURRENT_DESKTOP").toLower();
    return desktop.contains("xfce");
}

#endif

// Returns the locale of the current keyboard layout
// On macOS and Windows QGuiApplication::inputMethod() is used straight away.
// On Linux it tries dconf/xfconf-query first, then falls back to
// QGuiApplication::inputMethod() which is equivalent to "ibus engine".
// QGuiApplication::inputMethod() does not work with GNOME and XFCE
// https://bugreports.qt.io/browse/QTBUG-137302
inline QLocale inputLocale() {
#if defined(Q_OS_LINUX)
    if (isGnomeSession()) {
        // In a Gnome session QGuiApplication::inputMethod() is not necessarily correct
        // https://github.com/mixxxdj/mixxx/issues/14838
        // If this auto detection still fails the user may use a Custom.kb.cfg
        QProcess sourcesProc;
        sourcesProc.start("dconf",
                {"read", "/org/gnome/desktop/input-sources/mru-sources"});
        if (sourcesProc.waitForFinished(100)) {
            const QString sourcesStr = sourcesProc.readAllStandardOutput().trimmed();
            // Expecting something like this: [('xkb', 'de'), ('xkb', 'us')]
            // The first match is the current layout.
            // This matches entries like ('xkb', 'us') and extracts the layout
            // code (e.g. 'us', 'de')
            static const QRegularExpression re(QStringLiteral("\\('xkb',\\s*'([^']+)'\\)"));
            QRegularExpressionMatch match = re.match(sourcesStr);
            if (match.hasMatch()) {
                const QString layout = match.captured(1);
                ;
                qDebug() << "Keyboard Layout from GNOME dconf:" << layout;
                return localeFromXkbSymbol(layout);
            } else {
                // mru-sources (most recently used source) is empty when user
                // has only one keyboard layout enabled. Use it from sources.
                sourcesProc.start("dconf",
                        {"read", "/org/gnome/desktop/input-sources/sources"});
                if (sourcesProc.waitForFinished(100)) {
                    const QString sourcesStr = sourcesProc.readAllStandardOutput().trimmed();
                    // Expecting something like this: [('xkb', 'de')]
                    QRegularExpressionMatch match = re.match(sourcesStr);
                    if (match.hasMatch()) {
                        const QString layout = match.captured(1);
                        qDebug() << "Keyboard Layout from GNOME dconf:" << layout;
                        return localeFromXkbSymbol(layout);
                    } else {
                        qDebug() << "No valid keyboard layout found in dconf:" << sourcesStr;
                    }
                } else {
                    qDebug() << "Failed to read Keyboard Layout from dconf.";
                }
            }
        } else {
            qDebug() << "Failed to read Keyboard Layout from dconf.";
        }
    } else if (isXfceSession()) {
        // In a Xfce session QGuiApplication::inputMethod() is not necessarily correct
        // https://github.com/mixxxdj/mixxx/issues/14838
        // If this auto detection still fails the user may use a Custom.kb.cfg
        const QStringList args{"-c", "keyboard-layout", "-p", "/Default/XkbLayout"};
        QProcess sourcesProc;
        sourcesProc.start("xfconf-query", args);
        if (sourcesProc.waitForFinished(100)) {
            QString sourcesStr = sourcesProc.readAllStandardOutput().trimmed();
            // Expecting comma-separated layouts: de,gr,cz
            // The first is the current layout.
            if (sourcesStr.length() >= 2) {
                const QStringList allLayouts = sourcesStr.split(',');
                const QString currLayout = allLayouts[0];
                qDebug() << "Keyboard Layout from XFCE xfconf:" << currLayout;
                return localeFromXkbSymbol(currLayout);
            } else {
                qDebug() << "No valid keyboard layout found in xfconf:" << sourcesStr;
            }
        } else {
            qDebug() << "Failed to read Keyboard Layout from xfconf.";
        }
    }
#endif

    QInputMethod* pInputMethod = QGuiApplication::inputMethod();
    return pInputMethod ? pInputMethod->locale() : QLocale(QLocale::English);
}

} // anonymous namespace

namespace mixxx {

CoreServices::CoreServices(const CmdlineArgs& args, QApplication* pApp)
        : m_runtime_timer(QLatin1String("CoreServices::runtime")),
          m_cmdlineArgs(args),
          m_isInitialized(false) {
    m_runtime_timer.start();
    mixxx::Time::start();
    ScopedTimer t(u"CoreServices::CoreServices");
    // All this here is running without without start up screen
    // Defer long initializations to CoreServices::initialize() which is
    // called after the GUI is initialized
    initializeSettings();
    initializeLogging();
    // Only record stats in developer mode.
    if (m_cmdlineArgs.getDeveloper()) {
        StatsManager::createInstance();
    }
    mixxx::Translations::initializeTranslations(
            m_pSettingsManager->settings(), pApp, m_cmdlineArgs.getLocale());
    initializeKeyboard();
}

CoreServices::~CoreServices() {
    if (m_isInitialized) {
        finalize();
    }

    // Tear down remaining stuff that was initialized in the constructor.
    CLEAR_AND_CHECK_DELETED(m_pKeyboardEventFilter);
    CLEAR_AND_CHECK_DELETED(m_pKbdConfig);
    CLEAR_AND_CHECK_DELETED(m_pKbdConfigEmpty);

    if (m_cmdlineArgs.getDeveloper()) {
        StatsManager::destroy();
    }

    // HACK: Save config again. We saved it once before doing some dangerous
    // stuff. We only really want to save it here, but the first one was just
    // a precaution. The earlier one can be removed when stuff is more stable
    // at exit.
    m_pSettingsManager->save();
    m_pSettingsManager.reset();

    Sandbox::shutdown();

    // Check for leaked ControlObjects and give warnings.
    {
        const QList<QSharedPointer<ControlDoublePrivate>> leakedControls =
                ControlDoublePrivate::takeAllInstances();
        if (!leakedControls.isEmpty()) {
            qWarning()
                    << "The following"
                    << leakedControls.size()
                    << "controls were leaked:";
            for (auto pCDP : leakedControls) {
                ConfigKey key = pCDP->getKey();
                qWarning() << key.group << key.item << pCDP->getCreatorCO();
                // Deleting leaked objects helps to satisfy valgrind.
                // These delete calls could cause crashes if a destructor for a control
                // we thought was leaked is triggered after this one exits.
                // So, only delete so if developer mode is on.
                if (CmdlineArgs::Instance().getDeveloper()) {
                    pCDP->deleteCreatorCO();
                }
            }
            DEBUG_ASSERT(!"Controls were leaked!");
        }
        // Finally drop all shared pointers by exiting this scope
    }

    // Report the total time we have been running.
    m_runtime_timer.elapsed(true);
}

void CoreServices::initializeSettings() {
#ifdef Q_OS_MACOS
    // TODO: At this point it is too late to provide the same settings path to all components
    // and too early to log errors and give users advises in their system language.
    // Calling this from main.cpp before the QApplication is initialized may cause a crash
    // due to potential QMessageBox invocations within migrateOldSettings().
    // Solution: Start Mixxx with default settings, migrate the preferences, and then restart
    // immediately.
    if (!m_cmdlineArgs.getSettingsPathSet()) {
        CmdlineArgs::Instance().setSettingsPath(Sandbox::migrateOldSettings());
    }
#endif
    QString settingsPath = m_cmdlineArgs.getSettingsPath();
    m_pSettingsManager = std::make_unique<SettingsManager>(settingsPath);
}

void CoreServices::initializeLogging() {
    mixxx::LogFlags logFlags = mixxx::LogFlag::LogToFile;
    if (m_cmdlineArgs.getDebugAssertBreak()) {
        logFlags.setFlag(mixxx::LogFlag::DebugAssertBreak);
    }

    // Both knobs come from mixxx.cfg, which initializeSettings() has already
    // parsed by the time we get here. The settings directory is the fallback
    // for a log path that cannot be written, since that is where the log
    // always used to go and it is writable by definition.
    UserSettingsPointer pConfig = m_pSettingsManager->settings();
    const QString settingsPath = pConfig->getSettingsPath();

    QString logDirPath = pConfig->getValue(
            ConfigKey(QString::fromLatin1(mixxx::kLogConfigGroup),
                    QString::fromLatin1(mixxx::kLogPathConfigItem)),
            QString::fromLatin1(mixxx::kLogDirPathDefault));
    if (logDirPath.isEmpty()) {
        logDirPath = settingsPath;
    }

    int logFileKeepCount = pConfig->getValue(
            ConfigKey(QString::fromLatin1(mixxx::kLogConfigGroup),
                    QString::fromLatin1(mixxx::kLogKeepFilesConfigItem)),
            mixxx::kLogFileKeepCountDefault);
    if (logFileKeepCount < 1) {
        // Cannot use qWarning() yet, the message handler is not installed.
        fprintf(stderr,
                "Invalid log file keep count %d, using %d\n",
                logFileKeepCount,
                mixxx::kLogFileKeepCountDefault);
        logFileKeepCount = mixxx::kLogFileKeepCountDefault;
    }

    mixxx::Logging::initialize(
            logDirPath,
            settingsPath,
            logFileKeepCount,
            m_cmdlineArgs.getLogLevel(),
            m_cmdlineArgs.getLogFlushLevel(),
            logFlags);
}

void CoreServices::initialize(QApplication* pApp) {
    VERIFY_OR_DEBUG_ASSERT(!m_isInitialized) {
        return;
    }

    ScopedTimer t(u"CoreServices::initialize");

    VERIFY_OR_DEBUG_ASSERT(SoundSourceProxy::registerProviders()) {
        qCritical() << "Failed to register any SoundSource providers";
        return;
    }

    VersionStore::logBuildDetails();

    Q_UNUSED(pApp);

    UserSettingsPointer pConfig = m_pSettingsManager->settings();

    Sandbox::setPermissionsFilePath(QDir(pConfig->getSettingsPath()).filePath("sandbox.cfg"));

    QString resourcePath = pConfig->getResourcePath();

    emit initializationProgressUpdate(0, tr("fonts"));

    FontUtils::initializeFonts(resourcePath); // takes a long time

    emit initializationProgressUpdate(10, tr("database"));
    m_pDbConnectionPool = MixxxDb(pConfig).connectionPool();
    if (!m_pDbConnectionPool) {
        exit(-1);
    }
    // Create a connection for the main thread
    m_pDbConnectionPool->createThreadLocalConnection();
    if (!initializeDatabase()) {
        exit(-1);
    }

    m_pControlIndicatorTimer = std::make_shared<mixxx::ControlIndicatorTimer>(this);

    auto pChannelHandleFactory = std::make_shared<ChannelHandleFactory>();

    emit initializationProgressUpdate(20, tr("effects"));
    m_pEffectsManager = std::make_shared<EffectsManager>(pConfig, pChannelHandleFactory);

    m_pEngine = std::make_shared<EngineMixer>(
            pConfig,
            "[Master]",
            m_pEffectsManager.get(),
            pChannelHandleFactory,
            true);

    emit initializationProgressUpdate(30, tr("audio interface"));
    // Although m_pSoundManager is created here, m_pSoundManager->setupDevices()
    // needs to be called after m_pPlayerManager registers sound IO for each EngineChannel.
    m_pSoundManager = std::make_shared<SoundManager>(pConfig, m_pEngine.get());
    m_pEngine->registerNonEngineChannelSoundIO(m_pSoundManager.get());

    m_pRecordingManager = std::make_shared<RecordingManager>(pConfig, m_pEngine.get());

#ifdef __BROADCAST__
    m_pBroadcastManager = std::make_shared<BroadcastManager>(
            m_pSettingsManager.get(),
            m_pSoundManager.get());
#endif

#ifdef __VINYLCONTROL__
    m_pVCManager = std::make_shared<VinylControlManager>(this, pConfig, m_pSoundManager.get());
#else
    m_pVCManager = nullptr;
#endif

    emit initializationProgressUpdate(40, tr("decks"));
    // Create the player manager. (long)
    m_pPlayerManager = std::make_shared<PlayerManager>(
            pConfig,
            m_pSoundManager.get(),
            m_pEffectsManager.get(),
            m_pEngine.get());
    // TODO: connect input not configured error dialog slots
    PlayerInfo::create();
    // Bite DJ: start recording which tracks get played this session (tints
    // their library rows). Touched here so it is constructed on the GUI thread,
    // listening to PlayerInfo, before any deck can start.
    PlayedTracks::instance();

    for (int i = 0; i < kMicrophoneCount; ++i) {
        m_pPlayerManager->addMicrophone();
    }

    for (int i = 0; i < kAuxiliaryCount; ++i) {
        m_pPlayerManager->addAuxiliary();
    }

    m_pPlayerManager->addConfiguredDecks();

    for (int i = 0; i < kSamplerCount; ++i) {
        m_pPlayerManager->addSampler();
    }

    m_pPlayerManager->addPreviewDeck();

    m_pEffectsManager->setup();

#ifdef __VINYLCONTROL__
    m_pVCManager->init();
#endif

#ifdef __MODPLUG__
    // Restore the configuration for the modplug library before trying to load a module.
    DlgPrefModplug modplugPrefs{nullptr, pConfig};
    modplugPrefs.loadSettings();
    modplugPrefs.applySettings();
#endif

    // Inhibit Screensaver
    m_pScreensaverManager = std::make_shared<ScreensaverManager>(pConfig);
    connect(&PlayerInfo::instance(),
            &PlayerInfo::currentPlayingDeckChanged,
            m_pScreensaverManager.get(),
            &ScreensaverManager::slotCurrentPlayingDeckChanged);

    emit initializationProgressUpdate(50, tr("library"));
    CoverArtCache::createInstance();
    Clipboard::createInstance();

    m_pTrackCollectionManager = std::make_shared<TrackCollectionManager>(
            this,
            pConfig,
            m_pDbConnectionPool);

    m_pLibrary = std::make_shared<Library>(
            this,
            pConfig,
            m_pDbConnectionPool,
            m_pTrackCollectionManager.get(),
            m_pPlayerManager.get(),
            m_pRecordingManager.get());

    // Binding the PlayManager to the Library may already trigger
    // loading of tracks which requires that the GlobalTrackCache has
    // been created. Otherwise Mixxx might hang when accessing
    // the uninitialized singleton instance!
    m_pPlayerManager->bindToLibrary(m_pLibrary.get());

    // Bite DJ: never pop the native "Choose music library directory" dialog
    // on first launch. The unit is touch-only with no keyboard, and the
    // intended workflow is plugging in USB drives (Rekordbox / browse), not
    // managing a local library. If a future in-skin settings page wants to
    // expose root-dir picking, it can call Library::requestAddDir directly.
    const bool musicDirAdded = false;

    emit initializationProgressUpdate(60, tr("controllers"));
    // Initialize controller sub-system,
    // but do not set up controllers until the end of the application startup
    // (long)
    qDebug() << "Creating ControllerManager";
    m_pControllerManager = std::make_shared<ControllerManager>(pConfig);

    // Scan the library for new files and directories
    bool rescan = pConfig->getValue<bool>(
            library::prefs::kRescanOnStartupConfigKey);
    // rescan the library if we get a new plugin
    QList<QString> prev_plugins_list =
            pConfig->getValueString(
                           ConfigKey("[Library]", "SupportedFileExtensions"))
                    .split(',', Qt::SkipEmptyParts);

    QSet<QString> prev_plugins(prev_plugins_list.begin(), prev_plugins_list.end());

    const QList<QString> supportedFileSuffixes = SoundSourceProxy::getSupportedFileSuffixes();
    auto curr_plugins = QSet<QString>(supportedFileSuffixes.begin(), supportedFileSuffixes.end());

    rescan = rescan || (prev_plugins != curr_plugins);
    pConfig->set(ConfigKey("[Library]", "SupportedFileExtensions"),
            supportedFileSuffixes.join(","));

    // Scan the library directory. Do this after the skinloader has
    // loaded a skin, see issue #6625
    if (rescan || musicDirAdded || m_pSettingsManager->shouldRescanLibrary()) {
        m_pTrackCollectionManager->startLibraryScan();
    }

    // Bite DJ: stock Mixxx restores the sampler grid here from the samplers.xml
    // it wrote at exit (m_pPlayerManager->loadSamplers()). Deliberately not
    // called: the samplers are filled from the USB drive the DJ picked on the
    // Samplers tab and from nowhere else, so a second, invisible source of
    // slots on the boot volume is exactly the ambiguity SamplerDrive exists to
    // remove. The grid is restored below instead, once that drive is resolved.

    m_pTouchShift = std::make_unique<ControlPushButton>(ConfigKey("[Controls]", "touch_shift"));

    m_pRateRangeControl = std::make_unique<RateRangeControl>(pConfig);

    // Singleton inline-notification publisher. Must exist before the skin
    // parses so WNotificationStrip can subscribe at construction time.
    m_pNotifications = std::make_unique<Notifications>();

    // Bite DJ: daylight (high-contrast) mode. Must exist before the skin
    // parses — LegacySkinParser routes every stylesheet it applies through
    // HighContrast::styleSheetFor(), which both inverts the sheet when the
    // mode is already on and records the widget so a later toggle can
    // re-apply without rebooting the skin view.
    m_pHighContrast = std::make_unique<HighContrast>(pConfig);

    // Bite DJ: in-skin audio device picker singleton. Constructed after
    // Notifications (it publishes status via Notifications::publish) and
    // before the skin parses (WAudioDeviceList subscribes at construction).
    m_pAudioDeviceSettings = std::make_unique<AudioDeviceSettings>(
            pConfig, m_pSoundManager);

    // Bite DJ: in-skin MIDI controller picker singleton. Subscribes to
    // ControllerManager::devicesChanged for hot-plug updates and routes
    // mapping applies back through slotApplyMapping with a blocking-queued
    // connection (controller mutation must run on the controller thread).
    m_pControllerSettings = std::make_unique<ControllerSettings>(
            pConfig, m_pControllerManager);

    // Bite DJ: backs the in-skin Settings -> System sub-page (USB eject,
    // per-drive recording + safe shutdown). Holds a shared_ptr to PlayerManager
    // so it can unload tracks loaded from a drive before unmounting it, and one
    // to RecordingManager so a USB row can record the main output onto its
    // drive; both dropped in finalize() before those two are destroyed.
    m_pSystemSettings = std::make_unique<SystemSettings>(
            pConfig, m_pPlayerManager, m_pRecordingManager);

    // Bite DJ: the samplers are filled from one USB drive the DJ picks on the
    // Samplers tab. Constructed after SystemSettings (which enumerates the
    // drives and reports every plug and unplug) and before the skin parses, so
    // that the [Samplers] controls the grid binds to already exist and
    // WSamplerDrive can subscribe at construction. Resolving the stored
    // selection here is also what restores the grid at startup.
    m_pSamplerDrive = std::make_unique<SamplerDrive>(pConfig, m_pPlayerManager.get());

    // Bite DJ: forward drive-removal events into the library so removable-media
    // features (Rekordbox) can drop an unmounted device from the browser sidebar
    // immediately, instead of waiting on their own slow background poll.
    connect(m_pSystemSettings.get(),
            &SystemSettings::mountEjected,
            m_pLibrary.get(),
            &Library::mountEjected);

    // The UI controls must be created here so that controllers can bind to
    // them on startup.
    m_pSkinControls = std::make_unique<SkinControls>();

    // Load tracks in args.qlMusicFiles (command line arguments) into player
    // 1 and 2:
    const QList<QString>& musicFiles = m_cmdlineArgs.getMusicFiles();
    const int numTracks = std::min(m_pPlayerManager->numberOfDecks(),
            static_cast<int>(musicFiles.count()));
    for (int i = 0; i < numTracks; ++i) {
        if (SoundSourceProxy::isFileNameSupported(musicFiles.at(i))) {
            m_pPlayerManager->slotLoadToDeck(musicFiles.at(i), i + 1);
        }
    }

    m_isInitialized = true;
}

void CoreServices::initializeKeyboard() {
    UserSettingsPointer pConfig = m_pSettingsManager->settings();
    QString resourcePath = pConfig->getResourcePath();

    // Set the default value in settings file
    if (pConfig->getValueString(ConfigKey("[Keyboard]", "Enabled")).length() == 0) {
        pConfig->set(ConfigKey("[Keyboard]", "Enabled"), ConfigValue(1));
    }

    // Read keyboard configuration and set kdbConfig object in WWidget
    // Check first in user's Mixxx directory
    QString userKeyboard = QDir(pConfig->getSettingsPath()).filePath("Custom.kbd.cfg");

    // Empty keyboard configuration
    m_pKbdConfigEmpty = std::make_shared<ConfigObject<ConfigValueKbd>>(QString());

    if (QFile::exists(userKeyboard)) {
        qDebug() << "Found and will use custom keyboard mapping" << userKeyboard;
        m_pKbdConfig = std::make_shared<ConfigObject<ConfigValueKbd>>(userKeyboard);
    } else {
        // Default to the locale for the main input method (e.g. keyboard).
        QLocale locale = inputLocale();

        // check if a default keyboard exists
        QString defaultKeyboard = QString(resourcePath).append("keyboard/");
        defaultKeyboard += locale.name();
        defaultKeyboard += ".kbd.cfg";
        qDebug() << "Found and will use default keyboard mapping" << defaultKeyboard;

        if (!QFile::exists(defaultKeyboard)) {
            qDebug() << defaultKeyboard << " not found, using en_US.kbd.cfg";
            defaultKeyboard = QString(resourcePath).append("keyboard/").append("en_US.kbd.cfg");
            if (!QFile::exists(defaultKeyboard)) {
                qDebug() << defaultKeyboard << " not found, starting without shortcuts";
                defaultKeyboard = "";
            }
        }
        m_pKbdConfig = std::make_shared<ConfigObject<ConfigValueKbd>>(defaultKeyboard);
    }

    // TODO(XXX) leak pKbdConfig, KeyboardEventFilter owns it? Maybe roll all keyboard
    // initialization into KeyboardEventFilter
    // Workaround for today: KeyboardEventFilter calls delete
    bool keyboardShortcutsEnabled = pConfig->getValue<bool>(
            ConfigKey("[Keyboard]", "Enabled"));
    m_pKeyboardEventFilter = std::make_shared<KeyboardEventFilter>(
            keyboardShortcutsEnabled ? m_pKbdConfig.get() : m_pKbdConfigEmpty.get());
}

void CoreServices::slotOptionsKeyboard(bool toggle) {
    UserSettingsPointer pConfig = m_pSettingsManager->settings();
    if (toggle) {
        //qDebug() << "Enable keyboard shortcuts/mappings";
        m_pKeyboardEventFilter->setKeyboardConfig(m_pKbdConfig.get());
        pConfig->set(ConfigKey("[Keyboard]", "Enabled"), ConfigValue(1));
    } else {
        //qDebug() << "Disable keyboard shortcuts/mappings";
        m_pKeyboardEventFilter->setKeyboardConfig(m_pKbdConfigEmpty.get());
        pConfig->set(ConfigKey("[Keyboard]", "Enabled"), ConfigValue(0));
    }
}

bool CoreServices::initializeDatabase() {
    kLogger.info() << "Connecting to database";
    QSqlDatabase dbConnection = mixxx::DbConnectionPooled(m_pDbConnectionPool);
    if (!dbConnection.isOpen()) {
        QMessageBox::critical(nullptr,
                tr("Cannot open database"),
                tr("Unable to establish a database connection.\n"
                   "Mixxx requires QT with SQLite support. Please read "
                   "the Qt SQL driver documentation for information on how "
                   "to build it.\n\n"
                   "Click OK to exit."),
                QMessageBox::Ok);
        return false;
    }

    kLogger.info() << "Initializing or upgrading database schema";
    return MixxxDb::initDatabaseSchema(dbConnection);
}

std::shared_ptr<QDialog> CoreServices::makeDlgPreferences() const {
    // Note: We return here the base class pointer to make the coreservices.h usable
    // in test classes where header included from dlgpreferences.h are not accessible.
    std::shared_ptr<DlgPreferences> pDlgPreferences = std::make_shared<DlgPreferences>(
            getScreensaverManager(),
            nullptr,
            getSoundManager(),
            getControllerManager(),
            getVinylControlManager(),
            getEffectsManager(),
            getSettingsManager(),
            getLibrary());
    return pDlgPreferences;
}

void CoreServices::finalize() {
    VERIFY_OR_DEBUG_ASSERT(m_isInitialized) {
        qDebug() << "Skipping CoreServices finalization because it was never initialized.";
        return;
    }

    Timer t("CoreServices::~CoreServices");
    t.start();

    // Stop all pending library operations
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "stopping pending Library tasks";
    m_pTrackCollectionManager->stopLibraryScan();
    m_pLibrary->stopPendingTasks();

    qDebug() << t.elapsed(false).debugMillisWithUnit() << "saving configuration";
    m_pSettingsManager->save();

    // Bite DJ fork: AudioDeviceSettings / ControllerSettings each hold a
    // shared_ptr to their manager and would otherwise keep it alive past the
    // CLEAR_AND_CHECK_DELETED below, leaving the SoundDeviceNetwork audio
    // thread running while EngineMixer/EngineSync is torn down (crash) and
    // preventing EffectsManager::~EffectsManager() from running
    // saveEffectsXml(). Drop our refs first so the manager destructors
    // actually run here.
    m_pAudioDeviceSettings.reset();
    m_pControllerSettings.reset();
    // Holds shared_ptrs to PlayerManager and RecordingManager (both deleted
    // further below); drop it here. Its destructor also stops a per-drive
    // recording, which needs both of them alive.
    m_pSystemSettings.reset();

    // SoundManager depend on Engine and Config
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting SoundManager";
    CLEAR_AND_CHECK_DELETED(m_pSoundManager);

    // ControllerManager depends on Config
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting ControllerManager";
    CLEAR_AND_CHECK_DELETED(m_pControllerManager);

#ifdef __VINYLCONTROL__
    // VinylControlManager depends on a CO the engine owns
    // (vinylcontrol_enabled in VinylControlControl)
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting VinylControlManager";
    CLEAR_AND_CHECK_DELETED(m_pVCManager);
#endif

    // CoverArtCache is fairly independent of everything else.
    CoverArtCache::destroy();

    Clipboard::destroy();

    // Watches the decks and samplers it is about to outlive, so it goes first.
    m_pSamplerDrive.reset();

    // PlayerManager depends on Engine, SoundManager, VinylControlManager, and Config
    // The player manager has to be deleted before the library to ensure
    // that all modified track metadata of loaded tracks is saved.
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting PlayerManager";
    CLEAR_AND_CHECK_DELETED(m_pPlayerManager);

    // Delete the library after the view so there are no dangling pointers to
    // the data models.
    // Depends on RecordingManager and PlayerManager
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting Library";
    CLEAR_AND_CHECK_DELETED(m_pLibrary);

    // RecordingManager depends on config, engine
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting RecordingManager";
    CLEAR_AND_CHECK_DELETED(m_pRecordingManager);

#ifdef __BROADCAST__
    // BroadcastManager depends on config, engine
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting BroadcastManager";
    CLEAR_AND_CHECK_DELETED(m_pBroadcastManager);
#endif

    // EngineMixer depends on Config and m_pEffectsManager.
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting EngineMixer";
    CLEAR_AND_CHECK_DELETED(m_pEngine);

    // Destroy PlayerInfo explicitly to release the track
    // pointers of tracks that were still loaded in decks
    // or samplers when PlayerManager was destroyed!
    // Do this after deleting EngineMixer which makes use of
    // PlayerInfo in EngineRecord.
    PlayerInfo::destroy();
    PlayedTracks::destroy();

    qDebug() << t.elapsed(false).debugMillisWithUnit() << "deleting EffectsManager";
    CLEAR_AND_CHECK_DELETED(m_pEffectsManager);

    // Delete the track collections after all internal track pointers
    // in other components have been released by deleting those components
    // beforehand!
    qDebug() << t.elapsed(false).debugMillisWithUnit() << "detaching all track collections";
    CLEAR_AND_CHECK_DELETED(m_pTrackCollectionManager);

    qDebug() << t.elapsed(false).debugMillisWithUnit() << "closing database connection(s)";
    m_pDbConnectionPool->destroyThreadLocalConnection();
    m_pDbConnectionPool.reset(); // should drop the last reference

    m_pTouchShift.reset();

    m_pRateRangeControl.reset();

    m_pNotifications.reset();

    m_pSkinControls.reset();

    m_pControlIndicatorTimer.reset();

    t.elapsed(true);
}

} // namespace mixxx
