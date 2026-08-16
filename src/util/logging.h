#pragma once

#include <QFlags>

namespace mixxx {

enum class LogLevel {
    Critical = 0,
    Warning = 1,
    Info = 2,
    Debug = 3,
    Trace = 4, // DEPRECATED (not available in Qt, used for profiling etc.)
};

enum class LogFlag {
    None = 0,
    LogToFile = 1,
    DebugAssertBreak = 1 << 1,
};
Q_DECLARE_FLAGS(LogFlags, LogFlag);
Q_DECLARE_OPERATORS_FOR_FLAGS(LogFlags);

/// Default log level for (console) logs.
constexpr LogLevel kLogLevelDefault = LogLevel::Warning;
constexpr qint64 kLogMaxFileSizeDefault = 100'000'000; // 100 MB

/// Directory the log files are written to, unless mixxx.cfg says otherwise.
/// The appliance process runs as root, so this is writable; when it is not
/// (or does not exist and cannot be created) logging falls back to the
/// settings directory rather than dropping the log file entirely.
constexpr char kLogDirPathDefault[] = "/var/log";

/// Total number of log files to retain, including the one currently being
/// written. 1 keeps only mixxx.log, 3 also keeps mixxx.log.1 and mixxx.log.2.
constexpr int kLogFileKeepCountDefault = 1;

/// Both settings are read from mixxx.cfg at startup (before the log file is
/// opened) and are hand-edited only — there is no preferences UI for them,
/// so they are file-authoritative in ConfigObject to survive an autosave.
constexpr char kLogConfigGroup[] = "[Logging]";
constexpr char kLogKeepFilesConfigItem[] = "KeepFiles";
constexpr char kLogPathConfigItem[] = "Path";

/// Default log level for flushing the buffered log stream.
/// This is required to ensure that all buffered messages have
/// been written before Mixxx crashes.
///
/// Warning rather than upstream's Critical: at the default log level the file
/// only ever receives warnings and criticals, which is far too little traffic
/// to fill QIODevice's 16 KB write buffer. Anything short of a clean shutdown
/// - a crash, or the power-off that ends a normal set on an appliance - would
/// otherwise discard the entire log, which is precisely the log worth having.
/// Warnings are rare enough (single digits per startup) that flushing each one
/// costs nothing.
constexpr LogLevel kLogFlushLevelDefault = LogLevel::Warning;

/// Utility class for accessing the logging settings that are
/// configured at startup.
class Logging {
  public:
    // These are not thread safe. Only call them on Mixxx startup and shutdown.
    /// Writes the log to `logDirPath`, retaining `logFileKeepCount` files in
    /// total (see kLogFileKeepCountDefault). If that directory cannot be used
    /// `fallbackLogDirPath` is tried next; pass an empty string for no fallback.
    static void initialize(
            const QString& logDirPath,
            const QString& fallbackLogDirPath,
            int logFileKeepCount,
            LogLevel logLevel,
            LogLevel logFlushLevel,
            LogFlags flags);

    /// Absolute path of the log file currently being written, or an empty
    /// string when nothing is being logged to a file.
    static QString logFilePath();

    // Sets only the loglevel without the on-disk settings. Used by mixxx-test.
    static void setLogLevel(
            LogLevel logLevel) {
        s_logLevel = logLevel;
    }

    static void shutdown();

    static void flushLogFile();

    static bool shouldFlush(
            LogLevel logFlushLevel) {
        // Log levels are ordered by severity, i.e. more
        // severe log levels have a lower ordinal
        return s_logFlushLevel >= logFlushLevel;
    }

    static bool enabled(
            LogLevel logLevel) {
        return s_logLevel >= logLevel;
    }

  private:
    // Almost constant, i.e. initialized once at startup and
    // then could safely be read from multiple threads.
    static LogLevel s_logLevel;
    static LogLevel s_logFlushLevel;

    Logging() = delete;
};

} // namespace mixxx
