#include "preferences/configobject.h"

#include <QApplication>
#include <QDir>
#include <QIODevice>
#include <QTextStream>
#include <QtDebug>

#if defined(Q_OS_WIN)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

#include "util/cmdlineargs.h"
#include "util/color/rgbcolor.h"
#include "util/logging.h"
#include "util/xml.h"
#include "widget/wwidget.h"

// TODO(rryan): Move to a utility file.
namespace {
const QString kTempFilenameExtension = QStringLiteral(".tmp");
const QString kCMakeCacheFile = QStringLiteral("CMakeCache.txt");
const QLatin1String kSourceDirLine = QLatin1String("mixxx_SOURCE_DIR:STATIC=");

// The fork's own config/control group.
const QString kBiteDjGroup = QStringLiteral("[BiteDJ]");

// [BiteDJ],usb_drive_path_<N> maps a drive number to the sysfs topology
// name of a physical USB port ("1-1.5"). It is hardware provisioning, not a
// user preference: the image build writes it into mixxx.cfg and the MIDI
// eject daemon's GPIO buttons are keyed to it, so Mixxx only ever *reads* it
// (see SystemSettings::ejectDrive). Losing an entry silently un-provisions an
// eject button, and every save() rewrites the whole file from the in-memory
// map, which means anything not in that map at save time is erased from disk —
// a key added to the file after we parsed it, or dropped by a partial parse of
// a file that was being written concurrently, disappears on the next autosave.
//
// So these keys are file-authoritative: set()/remove() refuse to touch them,
// and save() re-reads them off disk first so a rewrite can only ever preserve
// them. The file wins, always.
const QLatin1String kUsbPortItemPrefix = QLatin1String("usb_drive_path_");

// [Logging],Path and [Logging],KeepFiles are the same kind of key: read once
// at startup (before the message handler even exists), hand-edited into
// mixxx.cfg because there is no UI for them, and never written by Mixxx. An
// edit made while Mixxx is running would otherwise be erased by the next
// autosave, so the operator's change would silently not survive the restart
// that is supposed to apply it.
bool isLoggingKey(const ConfigKey& key) {
    return key.group == QLatin1String(mixxx::kLogConfigGroup) &&
            (key.item == QLatin1String(mixxx::kLogPathConfigItem) ||
                    key.item == QLatin1String(mixxx::kLogKeepFilesConfigItem));
}

bool isFileAuthoritativeKey(const ConfigKey& key) {
    return (key.group == kBiteDjGroup && key.item.startsWith(kUsbPortItemPrefix)) ||
            isLoggingKey(key);
}

// Push a file's contents all the way to the storage device. QFile::flush()
// only moves Qt's own buffers into the kernel, which is not enough here: the
// rename that publishes the temp file is a journalled *metadata* operation and
// can therefore commit while the new file's data blocks are still sitting
// dirty in the page cache. The appliance is routinely switched off at the
// mains, and after such an unclean power-off that combination leaves a
// mixxx.cfg that exists but is empty — which parses as "first run", so Mixxx
// rewrites every key it owns and the only entries gone for good are the ones
// it never writes: the [BiteDJ],usb_drive_path_<N> provisioning.
bool fsyncFile(QFile* pFile) {
#if defined(Q_OS_WIN)
    return _commit(pFile->handle()) == 0;
#else
    return ::fsync(pFile->handle()) == 0;
#endif
}

// The rename itself only becomes durable once the *directory* is synced.
// Best effort: a failure here costs durability, not correctness, and there is
// nothing useful to do about it at this point in a save.
void fsyncDirectory(const QString& dirPath) {
#if !defined(Q_OS_WIN)
    const int fd = ::open(QFile::encodeName(dirPath).constData(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
#else
    Q_UNUSED(dirPath);
#endif
}

QString computeResourcePathImpl() {
    // Try to read in the resource directory from the command line
    QString qResourcePath = CmdlineArgs::Instance().getResourcePath();

    if (qResourcePath.isEmpty()) {
#ifdef __EMSCRIPTEN__
        // When targeting Emscripten/WebAssembly, we have a virtual file system
        // that is populated by our preloaded resources located at /res. See
        // also https://emscripten.org/docs/porting/files/packaging_files.html
        qResourcePath = "/res";
#else
        QDir mixxxDir = QCoreApplication::applicationDirPath();

        // We used to support using the mixxx.cfg's [Config],Path setting but
        // this causes issues if you try and use two different versions of Mixxx
        // on the same computer.

        QDir potentialBuildDir = mixxxDir;
#ifdef __APPLE__
        if (potentialBuildDir.absolutePath().endsWith(".app/Contents/MacOS")) {
            // We are in an app bundle (built with `-DMACOS_BUNDLE=ON`).
            // If we are in a development build directory, we need to search three directories up.
            potentialBuildDir.cd("../../..");
        }
#endif

        // Check if there's a `CMakeCache.txt`, if so we are in a development build directory.
        auto cmakecache = QFile(potentialBuildDir.filePath(kCMakeCacheFile));
        if (cmakecache.open(QFile::ReadOnly | QFile::Text)) {
            // We are running from a build dir (CMAKE_CURRENT_BINARY_DIR),
            // Look up the source path from CMakeCache.txt (mixxx_SOURCE_DIR)
            QTextStream in(&cmakecache);
            QString line = in.readLine();
            while (!line.isNull()) {
                if (line.startsWith(kSourceDirLine)) {
                    qResourcePath = line.mid(kSourceDirLine.size()) + QStringLiteral("/res");
                    break;
                }
                line = in.readLine();
            }
            if (!QDir(qResourcePath).exists()) {
                reportCriticalErrorAndQuit(
                        "Resource path listed in " + kCMakeCacheFile +
                        " does not exist. Did you move the build directory? "
                        "Hint: Set an alternative resource path with "
                        "'--resource-path <path>'.");
            }
        }
#if defined(__UNIX__)
        else if (mixxxDir.cd(QStringLiteral("../share/mixxx"))) {
            qResourcePath = mixxxDir.absolutePath();
        }
#elif defined(__WINDOWS__)
        // On Windows, set the config dir relative to the application dir if all
        // of the above fail.
        else {
            qResourcePath = QCoreApplication::applicationDirPath();
        }
#elif defined(Q_OS_IOS)
        // On iOS the bundle contains the resources directly.
        else {
            qResourcePath = QCoreApplication::applicationDirPath();
        }
#elif defined(Q_OS_MACOS)
        else if (mixxxDir.cd("../Resources")) {
            // Release configuration
            qResourcePath = mixxxDir.absolutePath();
        } else {
            // TODO(rryan): What should we do here?
        }
#endif
#endif // !defined(__EMSCRIPTEN__)
    } else {
        //qDebug() << "Setting qResourcePath from location in resourcePath commandline arg:" << qResourcePath;
    }

    if (qResourcePath.isEmpty()) {
        reportCriticalErrorAndQuit(
                "qResourcePath is empty, this should not happen -- did our "
                "developers forget to define __UNIX__, __WINDOWS__ or "
                "__APPLE__??");
    }

    // If the directory does not end with a "/", add one
    if (!qResourcePath.endsWith("/")) {
        qResourcePath.append("/");
    }

    qDebug() << "Loading resources from " << qResourcePath;
    return qResourcePath;
}

QString computeSettingsPath(const QString& configFilename) {
    if (!configFilename.isEmpty()) {
        QFileInfo configFileInfo(configFilename);
        return configFileInfo.absoluteDir().absolutePath();
    }
    return QString();
}

}  // namespace
// static
ConfigKey ConfigKey::parseCommaSeparated(const QString& key) {
    int comma = key.indexOf(",");
    ConfigKey configKey(key.left(comma), key.mid(comma + 1));
    return configKey;
}

ConfigValue::ConfigValue(int iValue)
    : value(QString::number(iValue)) {
}

ConfigValue::ConfigValue(double dValue)
    : value(QString::number(dValue)) {
}

ConfigValueKbd::ConfigValueKbd(const QKeySequence& keys)
        : m_keys(std::move(keys)) {
    QTextStream(&value) << m_keys.toString();
}

template<class ValueType>
ConfigObject<ValueType>::ConfigObject(const QString& file)
        : ConfigObject(file, computeResourcePathImpl(), computeSettingsPath(file)) {
    reopen(file);
}

template<class ValueType>
ConfigObject<ValueType>::ConfigObject(
        const QString& file,
        const QString& resourcePath,
        const QString& settingsPath)
        : m_resourcePath(resourcePath),
          m_settingsPath(settingsPath) {
    reopen(file);
}

template <class ValueType> ConfigObject<ValueType>::~ConfigObject() {
}

template <class ValueType>
void ConfigObject<ValueType>::set(const ConfigKey& k, const ValueType& v) {
    if (isFileAuthoritativeKey(k)) {
        qWarning() << "ConfigObject: refusing to modify read-only key" << k
                   << "- the config file is the authority for it";
        return;
    }
    setUnchecked(k, v);
}

template <class ValueType>
void ConfigObject<ValueType>::setUnchecked(const ConfigKey& k, const ValueType& v) {
    QWriteLocker lock(&m_valuesLock);
    // Exact (case-sensitive) comparison on purpose: ValueType::operator==
    // is case-insensitive, which would swallow e.g. path case changes.
    const auto it = m_values.constFind(k);
    if (it != m_values.constEnd() && it.value().value == v.value) {
        return;
    }
    m_values.insert(k, v);
    markDirtyLocked();
}

template <class ValueType>
ValueType ConfigObject<ValueType>::get(const ConfigKey& k) const {
    QReadLocker lock(&m_valuesLock);
    return m_values.value(k);
}

template <class ValueType>
bool ConfigObject<ValueType>::exists(const ConfigKey& k) const {
    QReadLocker lock(&m_valuesLock);
    return m_values.contains(k);
}

template <class ValueType>
bool ConfigObject<ValueType>::remove(const ConfigKey& k) {
    if (isFileAuthoritativeKey(k)) {
        qWarning() << "ConfigObject: refusing to remove read-only key" << k
                   << "- the config file is the authority for it";
        return false;
    }
    QWriteLocker lock(&m_valuesLock);
    if (m_values.remove(k) > 0) {
        markDirtyLocked();
        return true;
    }
    return false;
}

template <class ValueType>
QString ConfigObject<ValueType>::getValueString(const ConfigKey& k) const {
    ValueType v = get(k);
    return v.value;
}

template <class ValueType> bool ConfigObject<ValueType>::parse() {
    // Open file for reading
    QFile configfile(m_filename);
    if (m_filename.length() < 1 || !configfile.open(QIODevice::ReadOnly)) {
        qDebug() << "ConfigObject: Could not read" << m_filename;
        return false;
    } else {
        //qDebug() << "ConfigObject: Parse" << m_filename;
        // Parse the file
        int group = 0;
        QString groupStr, line;
        QTextStream text(&configfile);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        DEBUG_ASSERT(text.encoding() == QStringConverter::Utf8);
#else
        text.setCodec("UTF-8");
#endif

        while (!text.atEnd()) {
            line = text.readLine().trimmed();
            if (line.length() != 0) {
                if (line.startsWith("[") && line.endsWith("]")) {
                    group++;
                    groupStr = line;
                    //qDebug() << "Group :" << groupStr;
                } else if (group > 0) {
                    QString key;
                    QTextStream(&line) >> key;
                    QString val = line.right(line.length() - key.length()); // finds the value string
                    val = val.trimmed();
                    //qDebug() << "control:" << key << "value:" << val;
                    ConfigKey k(groupStr, key);
                    ValueType m(val);
                    // Unchecked: reading the file is exactly how the
                    // file-authoritative keys are meant to get in.
                    setUnchecked(k, m);
                }
            }
        }
        configfile.close();
    }
    return true;
}

template<class ValueType>
QMap<ConfigKey, ValueType> ConfigObject<ValueType>::parseFileAuthoritativeKeys() const {
    QMap<ConfigKey, ValueType> values;
    if (m_filename.isEmpty()) {
        return values;
    }
    QFile configfile(m_filename);
    if (!configfile.open(QIODevice::ReadOnly)) {
        // No file yet (first run) — nothing to preserve.
        return values;
    }
    QTextStream text(&configfile);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    DEBUG_ASSERT(text.encoding() == QStringConverter::Utf8);
#else
    text.setCodec("UTF-8");
#endif
    QString groupStr;
    while (!text.atEnd()) {
        QString line = text.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith("[") && line.endsWith("]")) {
            groupStr = line;
            continue;
        }
        if (groupStr.isEmpty()) {
            continue;
        }
        QString key;
        QTextStream(&line) >> key;
        const ConfigKey k(groupStr, key);
        if (!isFileAuthoritativeKey(k)) {
            continue;
        }
        values.insert(k, ValueType(line.right(line.length() - key.length()).trimmed()));
    }
    return values;
}

template <class ValueType> void ConfigObject<ValueType>::reopen(const QString& file) {
    m_filename = file;
    if (!m_filename.isEmpty()) {
        parse();
    }
    // Values loaded from the file are in sync with the file.
    m_dirty.store(false);
}

template<class ValueType>
void ConfigObject<ValueType>::setDirtyCallback(std::function<void()> callback) {
    QWriteLocker lock(&m_valuesLock);
    m_dirtyCallback = std::move(callback);
    // Don't lose changes made before the callback was installed (e.g. the
    // version upgrade rewriting config values).
    if (m_dirty.load() && m_dirtyCallback) {
        m_dirtyCallback();
    }
}

/// Save the ConfigObject to disk.
/// Returns true on success
template<class ValueType>
bool ConfigObject<ValueType>::save() {
    // Snapshot the values instead of holding the lock across the file I/O
    // below: writers (including the audio and controller threads via
    // persistent COs) must never block on a disk write. If a change slips
    // in after the snapshot it re-dirties and the next save picks it up.
    //
    // Re-read the file-authoritative keys (see isFileAuthoritativeKey) straight
    // off disk, outside the lock, and fold them back into the map before the
    // snapshot. This rewrite then carries whatever the file currently says —
    // including entries added or edited since we parsed it — so a save can
    // never drop or stale them. Inserted without markDirty(): the file already
    // holds these values, and re-dirtying here would loop the autosave timer.
    const QMap<ConfigKey, ValueType> fileAuthoritativeValues = parseFileAuthoritativeKeys();

    QMap<ConfigKey, ValueType> values;
    {
        QWriteLocker lock(&m_valuesLock);
        for (auto i = fileAuthoritativeValues.constBegin();
                i != fileAuthoritativeValues.constEnd();
                ++i) {
            m_values.insert(i.key(), i.value());
        }
        values = m_values;
        m_dirty.store(false);
    }
    QFile tmpFile(m_filename + kTempFilenameExtension);
    if (!QDir(QFileInfo(tmpFile).absolutePath()).exists()) {
        QDir().mkpath(QFileInfo(tmpFile).absolutePath());
    }
    if (!tmpFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Could not write config file: " << tmpFile.fileName();
        return false;
    }
    QTextStream stream(&tmpFile);
    // UTF-8 is the default in Qt6.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    DEBUG_ASSERT(stream.encoding() == QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    QString group = "";

    // Since it is legit to have a ConfigObject with 0 values, checking
    // the stream.pos alone will yield wrong warnings. We therefore estimate
    // a minimum length as an additional safety check.
    qint64 minLength = 0;
    for (auto i = values.constBegin(); i != values.constEnd(); ++i) {
        //qDebug() << "group:" << it.key().group << "item" << it.key().item << "val" << it.value()->value;
        if (i.key().group != group) {
            group = i.key().group;
            stream << "\n"
                   << group << "\n";
            minLength += i.key().group.length() + 2;
        }
        stream << i.key().item << " " << i.value().value << "\n";
        minLength += i.key().item.length() + i.value().value.length() + 1;
    }

    stream.flush();
    // the stream is usually longer, depending on the amount of encoded data.
    if (stream.pos() < minLength || QFileInfo(tmpFile).size() != stream.pos()) {
        qWarning().nospace() << "Error while writing configuration file: " << tmpFile.fileName();
        return false;
    }

    // Durability before visibility: the temp file's contents must be on the
    // device before the rename can publish it. See fsyncFile().
    if (!fsyncFile(&tmpFile)) {
        qWarning().nospace() << "Could not flush configuration file to disk: "
                             << tmpFile.fileName();
        return false;
    }

    tmpFile.close();
    if (tmpFile.error() !=
            QFile::NoError) { //could be better... should actually say what the error was..
        qWarning().nospace() << "Error while writing configuration file: "
                             << tmpFile.fileName() << ": " << tmpFile.errorString();
        return false;
    }

    // Publish the new config by replacing the old one in a single step.
    //
    // This used to unlink m_filename first, because QFile::rename() refuses to
    // overwrite an existing destination. That was doing real damage on the
    // appliance: it opens a window in which *no* config file exists at all
    // (fatal if we are killed there — the shutdown handler SIGKILLs holdouts),
    // and it also defeats ext4's auto_da_alloc heuristic, which only forces
    // delayed-allocated data out ahead of a rename when the target already
    // exists. Either way the file that comes back after a power cut is missing
    // or empty, and the usb_drive_path_<N> provisioning is the one thing
    // nothing regenerates.
    //
    // rename(2) replaces the destination atomically, so a reader either sees
    // the whole old file or the whole new one.
    const QString tmpFileName = tmpFile.fileName();
#if defined(Q_OS_WIN)
    // No atomic replacing rename via QFile on Windows; keep the old two-step
    // dance there. Windows is not a target for the appliance.
    QFile oldConfig(m_filename);
    // Trying to remove a file that does not exist would fail
    if (oldConfig.exists()) {
        if (!oldConfig.remove()) {
            qWarning().nospace() << "Could not remove old config file: "
                                 << oldConfig.fileName() << ": " << oldConfig.errorString();
            return false;
        }
    }
    if (!tmpFile.rename(m_filename)) {
        qWarning().nospace() << "Could not rename tmp file to config file: "
                             << tmpFileName << ": " << tmpFile.errorString();
        return false;
    }
#else
    if (::rename(QFile::encodeName(tmpFileName).constData(),
                QFile::encodeName(m_filename).constData()) != 0) {
        qWarning().nospace() << "Could not rename tmp file to config file: "
                             << tmpFileName << ": " << strerror(errno);
        return false;
    }
#endif

    // ... and make the rename itself survive a power cut.
    fsyncDirectory(QFileInfo(m_filename).absolutePath());

    return true;
}

template<class ValueType>
QSet<QString> ConfigObject<ValueType>::getGroups() {
    QWriteLocker lock(&m_valuesLock);
    QSet<QString> groups;
    for (const ConfigKey& key : m_values.keys()) {
        groups.insert(key.group);
    }
    return groups;
}

template<class ValueType>
QList<ConfigKey> ConfigObject<ValueType>::getKeysWithGroup(const QString& group) const {
    QWriteLocker lock(&m_valuesLock);
    QList<ConfigKey> filteredList;
    for (const ConfigKey& key : m_values.keys()) {
        if (key.group == group) {
            filteredList.append(key);
        }
    }
    return filteredList;
}

template <class ValueType> ConfigObject<ValueType>::ConfigObject(const QDomNode& node) {
    if (!node.isNull() && node.isElement()) {
        QDomNode ctrl = node.firstChild();

        while (!ctrl.isNull()) {
            if (ctrl.nodeName() == "control") {
                QString group = XmlParse::selectNodeQString(ctrl, "group");
                QString key = XmlParse::selectNodeQString(ctrl, "key");
                ConfigKey k(group, key);
                ValueType m(ctrl);
                set(k, m);
            }
            ctrl = ctrl.nextSibling();
        }
    }
}

template <class ValueType>
QMultiHash<ValueType, ConfigKey> ConfigObject<ValueType>::transpose() const {
    QReadLocker lock(&m_valuesLock);

    QMultiHash<ValueType, ConfigKey> transposedHash;
    for (auto it = m_values.constBegin(); it != m_values.constEnd(); ++it) {
        transposedHash.insert(it.value(), it.key());
    }
    return transposedHash;
}

template class ConfigObject<ConfigValue>;
template class ConfigObject<ConfigValueKbd>;

template <> template <>
void ConfigObject<ConfigValue>::setValue(
        const ConfigKey& key, const QString& value) {
    set(key, ConfigValue(value));
}

template <> template <>
void ConfigObject<ConfigValue>::setValue(
        const ConfigKey& key, const bool& value) {
    set(key, value ? ConfigValue("1") : ConfigValue("0"));
}

template <> template <>
void ConfigObject<ConfigValue>::setValue(
        const ConfigKey& key, const int& value) {
    set(key, ConfigValue(QString::number(value)));
}

template <> template <>
void ConfigObject<ConfigValue>::setValue(
        const ConfigKey& key, const double& value) {
    set(key, ConfigValue(QString::number(value)));
}

template<>
template<>
void ConfigObject<ConfigValue>::setValue(
        const ConfigKey& key, const mixxx::RgbColor::optional_t& value) {
    if (!value) {
        remove(key);
        return;
    }
    set(key, ConfigValue(mixxx::RgbColor::toQString(value)));
}

template<>
template<>
void ConfigObject<ConfigValue>::setValue(
        const ConfigKey& key, const mixxx::RgbColor& value) {
    set(key, ConfigValue(mixxx::RgbColor::toQString(value)));
}

template <> template <>
bool ConfigObject<ConfigValue>::getValue(
        const ConfigKey& key, const bool& default_value) const {
    const ConfigValue value = get(key);
    if (value.isNull()) {
        return default_value;
    }
    bool ok;
    auto result = value.value.toInt(&ok);
    return ok ? result != 0 : default_value;
}

template <> template <>
int ConfigObject<ConfigValue>::getValue(
        const ConfigKey& key, const int& default_value) const {
    const ConfigValue value = get(key);
    if (value.isNull()) {
        return default_value;
    }
    bool ok;
    auto result = value.value.toInt(&ok);
    return ok ? result : default_value;
}

template <> template <>
double ConfigObject<ConfigValue>::getValue(
        const ConfigKey& key, const double& default_value) const {
    const ConfigValue value = get(key);
    if (value.isNull()) {
        return default_value;
    }
    bool ok;
    auto result = value.value.toDouble(&ok);
    return ok ? result : default_value;
}

template<>
template<>
mixxx::RgbColor::optional_t ConfigObject<ConfigValue>::getValue(
        const ConfigKey& key, const mixxx::RgbColor::optional_t& default_value) const {
    const ConfigValue value = get(key);
    if (value.isNull()) {
        return default_value;
    }
    return mixxx::RgbColor::fromQString(value.value, default_value);
}

template<>
template<>
mixxx::RgbColor::optional_t ConfigObject<ConfigValue>::getValue(const ConfigKey& key) const {
    return getValue(key, mixxx::RgbColor::optional_t(std::nullopt));
}

template<>
template<>
mixxx::RgbColor ConfigObject<ConfigValue>::getValue(
        const ConfigKey& key, const mixxx::RgbColor& default_value) const {
    const mixxx::RgbColor::optional_t value = getValue(key, mixxx::RgbColor::optional_t(std::nullopt));
    if (!value) {
        return default_value;
    }
    return *value;
}

template<>
template<>
mixxx::RgbColor ConfigObject<ConfigValue>::getValue(const ConfigKey& key) const {
    return getValue(key, mixxx::RgbColor(0));
}

// For string literal default
template <>
QString ConfigObject<ConfigValue>::getValue(
        const ConfigKey& key, const char* default_value) const {
    const ConfigValue value = get(key);
    if (value.isNull()) {
        return QString(default_value);
    }
    return value.value;
}

template <>
QString ConfigObject<ConfigValueKbd>::getValue(
        const ConfigKey& key, const char* default_value) const {
    const ConfigValueKbd value = get(key);
    if (value.isNull()) {
        return QString(default_value);
    }
    return value.value;
}

template <> template <>
QString ConfigObject<ConfigValue>::getValue(
        const ConfigKey& key, const QString& default_value) const {
    const ConfigValue value = get(key);
    if (value.isNull()) {
        return default_value;
    }
    return value.value;
}

template <> template <>
QString ConfigObject<ConfigValueKbd>::getValue(
        const ConfigKey& key, const QString& default_value) const {
    const ConfigValueKbd value = get(key);
    if (value.isNull()) {
        return default_value;
    }
    return value.value;
}

template<>
QString ConfigObject<ConfigValue>::computeResourcePath() {
    return computeResourcePathImpl();
}

template<>
QString ConfigObject<ConfigValueKbd>::computeResourcePath() {
    return computeResourcePathImpl();
}
