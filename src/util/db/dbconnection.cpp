#include <QSqlDriver>
#include <QSqlError>
#include <QSqlQuery>

#ifdef __SQLITE3__
#include <sqlite3.h>
#endif // __SQLITE3__

#include "util/db/dbconnection.h"

#include "track/keyutils.h"
#include "util/db/sqllikewildcards.h"
#include "util/logger.h"
#include "util/assert.h"


// Originally from public domain code:
// http://www.archivum.info/qt-interest@trolltech.com/2008-12/00584/Re-%28Qt-interest%29-Qt-Sqlite-UserDefinedFunction.html

namespace mixxx {

namespace {

const mixxx::Logger kLogger("DbConnection");

QSqlDatabase createDatabase(
        const DbConnection::Params& params,
        const QString& connectionName) {
    kLogger.info()
        << "Available drivers for database connections:"
        << QSqlDatabase::drivers();

    QSqlDatabase database =
            QSqlDatabase::addDatabase(params.type, connectionName);
    database.setConnectOptions(params.connectOptions);
    database.setHostName(params.hostName);
    database.setDatabaseName(params.filePath);
    database.setUserName(params.userName);
    database.setPassword(params.password);
    return database;
}

QSqlDatabase cloneDatabase(
        const QSqlDatabase& database,
        const QString& connectionName) {
    DEBUG_ASSERT(!database.isOpen());
    return QSqlDatabase::cloneDatabase(database, connectionName);
}

void removeDatabase(
        QSqlDatabase* pDatabase) {
    DEBUG_ASSERT(pDatabase);
    DEBUG_ASSERT(!pDatabase->isOpen());
    // pDatabase must be the last reference to the implicitly shared
    // QSqlDatabase object
    QString connectionName = pDatabase->connectionName();
    // Drop the last reference before actually removing the database
    // to avoid the following warning:
    // "Warning [Main]: QSqlDatabasePrivate::removeDatabase: connection
    // '...' is still in use, all queries will cease to work."
    *pDatabase = QSqlDatabase();
    // After all references have been dropped we can safely remove the
    // connection. If still some of the afore mentioned warnings appear
    // in the log than a component is misbehaving and still holding an
    // invalid copy of the QSqlDatabase object that it shouldn't have!!
    QSqlDatabase::removeDatabase(connectionName);
}

void makeLatinLow(QChar* c, int count) {
    for (int i = 0; i < count; ++i) {
        if (c[i].decompositionTag() != QChar::NoDecomposition) {
            QString decomposition = c[i].decomposition();
            if (!decomposition.isEmpty() && !decomposition[0].isSpace()) {
                // here we remove the decoration from all characters.
                // We want "o" matching "ó" and all other variants but we
                // do not decompose decoration only characters like "˚" where
                // the base character is a space
                c[i] = decomposition.at(0);
            }
        }
        if (c[i].isUpper()) {
            c[i] = c[i].toLower();
        }
    }
}

// Compare two strings for equality where the first string is
// a "LIKE" expression. Return true (1) if they are the same and
// false (0) if they are different.
// This is the original sqlite3 icuLikeCompare rewritten for QChar
int likeCompareInner(
        const QChar* pattern, // LIKE pattern
        int patternSize,
        const QChar* string, // The string to compare against
        int stringSize,
        const QChar esc) { // The escape character
    int iPattern = 0; // Current index in pattern
    int iString = 0; // Current index in string

    bool prevEscape = false; // True if the previous character was uEsc

    while (iPattern < patternSize) {
        // Read (and consume) the next character from the input pattern.
        QChar uPattern = pattern[iPattern++];
        // There are now 4 possibilities:
        // 1. uPattern is an unescaped match-all character "%",
        // 2. uPattern is an unescaped match-one character "_",
        // 3. uPattern is an unescaped escape character, or
        // 4. uPattern is to be handled as an ordinary character

        if (!prevEscape && uPattern == kSqlLikeMatchAll) {
            // Case 1.
            QChar c;

            // Skip any kSqlLikeMatchAll or kSqlLikeMatchOne characters that follow a
            // kSqlLikeMatchAll. For each kSqlLikeMatchOne, skip one character in the
            // test string.

            if (iPattern >= patternSize) {
                // Tailing %
                return 1;
            }

            while ((c = pattern[iPattern]) == kSqlLikeMatchAll || c == kSqlLikeMatchOne) {
                if (c == kSqlLikeMatchOne) {
                    if (++iString == stringSize) {
                        return 0;
                    }
                }
                if (++iPattern == patternSize) {
                    // Two or more tailing %
                    return 1;
                }
            }

            while (iString < stringSize) {
                if (likeCompareInner(&pattern[iPattern], patternSize - iPattern,
                                &string[iString], stringSize - iString, esc)) {
                    return 1;
                }
                iString++;
            }
            return 0;
        } else if (!prevEscape && uPattern == kSqlLikeMatchOne) {
            // Case 2.
            if (++iString == stringSize) {
                return 0;
            }
        } else if (!prevEscape && uPattern == esc) {
            // Case 3.
            prevEscape = true;
        } else {
            // Case 4.
            if (iString == stringSize) {
                return 0;
            }
            QChar uString = string[iString++];
            if (uString != uPattern) {
                return 0;
            }
            prevEscape = false;
        }
    }
    return iString == stringSize;
}

#ifdef __SQLITE3__

namespace {

const QChar kSqlLikeEscapeDefault = '\0';

} // anonymous namespace

// The collating function callback is invoked with a copy of the pArg
// application data pointer and with two strings in the encoding specified
// by the eTextRep argument.
// The collating function must return an integer that is negative, zero,
// or positive if the first string is less than, equal to, or greater
// than the second, respectively.
int sqliteStringCompareUTF16(void* pArg,
                             int len1, const void* data1,
                             int len2, const void* data2) {
    const auto* pCollator = static_cast<mixxx::StringCollator*>(pArg);
    // Construct a QString without copy
    QString string1 = QString::fromRawData(static_cast<const QChar*>(data1),
                                           len1 / sizeof(QChar));
    QString string2 = QString::fromRawData(static_cast<const QChar*>(data2),
                                           len2 / sizeof(QChar));
    return pCollator->compare(string1, string2);
}

const char kLexicographicalCollationFunc[] = "mixxxLexicographicalCollationFunc";

// This implements the like() SQL function. This is used by the LIKE operator.
// The SQL statement 'A LIKE B' is implemented as 'like(B, A)', and if there is
// an escape character, say E, it is implemented as 'like(B, A, E)'
//static
void sqliteLikeUtf8(sqlite3_context* context,
        int aArgc,
        sqlite3_value** aArgv) {
    VERIFY_OR_DEBUG_ASSERT(aArgc == 2 || aArgc == 3) {
        return;
    }

    const char* b = reinterpret_cast<const char*>(
            sqlite3_value_text(aArgv[0]));
    const char* a = reinterpret_cast<const char*>(
            sqlite3_value_text(aArgv[1]));

    if (!a || !b) {
        return;
    }

    QString stringB = QString::fromUtf8(b); // Like String
    QString stringA = QString::fromUtf8(a);

    QChar esc = kSqlLikeEscapeDefault;
    if (aArgc == 3) {
        const char* e = reinterpret_cast<const char*>(
                sqlite3_value_text(aArgv[2]));
        if (e) {
            QString stringE = QString::fromUtf8(e);
            if (!stringE.isEmpty()) {
                esc = stringE.data()[0];
            }
        }
    }

    int ret = DbConnection::likeCompareLatinLow(&stringB, &stringA, esc);
    sqlite3_result_int64(context, ret);
    return;
}

// Bite DJ: SQLite scalar mapping a free-form key text (e.g. "9A", "Dbm",
// "1d") to its circle-of-fifths order under the requested notation. Used by
// ColumnCache::slotSetKeySortOrder for external library track caches
// (Rekordbox, Serato, iTunes) that store only `key TEXT` and have no
// `key_id` column — referencing a non-existent `key_id` in the ORDER BY
// would fail the query and hide every row in the view. Routing through
// KeyUtils::guessKeyFromText gives us musical (chromatic) ordering across
// all spelling variants, not the alphabetic ordering that places "10A"
// before "2A". Unparseable text returns NULL so it sorts to the end.
void sqliteKeyTextSortOrder(sqlite3_context* context,
        int argc,
        sqlite3_value** argv) {
    VERIFY_OR_DEBUG_ASSERT(argc == 2) {
        sqlite3_result_null(context);
        return;
    }
    const auto* pKeyText = sqlite3_value_text(argv[0]);
    if (!pKeyText) {
        sqlite3_result_null(context);
        return;
    }
    const QString keyText = QString::fromUtf8(
            reinterpret_cast<const char*>(pKeyText));
    const auto key = KeyUtils::guessKeyFromText(keyText);
    if (key == mixxx::track::io::key::INVALID) {
        sqlite3_result_null(context);
        return;
    }
    const auto notation = KeyUtils::keyNotationFromNumericValue(
            static_cast<double>(sqlite3_value_int(argv[1])));
    sqlite3_result_int64(context,
            KeyUtils::keyToCircleOfFifthsOrder(key, notation));
}

#endif // __SQLITE3__

// Bite DJ: put the database in write-ahead-log mode.
//
// In the default rollback-journal mode a writer that has spilled its page
// cache holds an EXCLUSIVE lock until it commits, and every other connection
// -- including the GUI thread's -- blocks on SQLite's busy handler until then.
// Background work that writes to the library database while reading from a USB
// stick (RekordboxFeature::parseDeviceDB, SeratoFeature, the analyzers) can
// hold that lock for seconds, so a tap that only reads (opening a playlist)
// froze the interface for as long as the writer took.
//
// Under WAL readers and a writer never block each other: the reader keeps
// seeing the snapshot it started with while the writer appends. Writers still
// exclude each other, which is why the long-running scans additionally keep
// their transactions short (see parseDeviceDB).
//
// journal_mode is persisted in the database header, so this is a no-op after
// the first run. In-memory databases (the tests) silently stay in "memory"
// mode, which already has no cross-connection locking.
void enableWriteAheadLogging(const QSqlDatabase& database) {
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL"))) {
        kLogger.warning()
                << "Failed to enable WAL journal mode:"
                << query.lastError();
        return;
    }
    if (query.next() && kLogger.debugEnabled()) {
        kLogger.debug() << "SQLite journal mode:" << query.value(0).toString();
    }
}

bool initDatabase(const QSqlDatabase& database, mixxx::StringCollator* pCollator) {
    DEBUG_ASSERT(database.isOpen());
    enableWriteAheadLogging(database);
#ifdef __SQLITE3__
    QVariant v = database.driver()->handle();
    VERIFY_OR_DEBUG_ASSERT(v.isValid()) {
        kLogger.warning() << "Driver handle is invalid";
        return false; // abort
    }
    if (strcmp(v.typeName(), "sqlite3*") != 0) {
        kLogger.warning()
                << "Unsupported database driver:"
               << v.typeName();
        return false; // abort
    }
    // Ensure initialization. sqlite recommends doing this before using it
    // and might become required in future versions
    int rc = sqlite3_initialize();
    VERIFY_OR_DEBUG_ASSERT(rc == SQLITE_OK) {
        kLogger.warning()
            << "sqlite3_initialize failed with the code: "
            << rc;
    }

    // v.data() returns a pointer to the handle
    sqlite3* handle = *static_cast<sqlite3**>(v.data());
    VERIFY_OR_DEBUG_ASSERT(handle != nullptr) {
        kLogger.warning()
                << "SQLite3 handle is invalid";
        return false; // abort
    }

    int result = sqlite3_create_collation(
                    handle,
                    kLexicographicalCollationFunc,
                    SQLITE_UTF16,
                    pCollator,
                    sqliteStringCompareUTF16);
    VERIFY_OR_DEBUG_ASSERT(result == SQLITE_OK) {
        kLogger.warning()
                << "Failed to install locale-aware lexicographical collation function for SQLite3:"
                << result;
    }

    result = sqlite3_create_function(
            handle,
            "like",
            2,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC,
            nullptr,
            sqliteLikeUtf8,
            nullptr,
            nullptr);
    VERIFY_OR_DEBUG_ASSERT(result == SQLITE_OK) {
        kLogger.warning()
                << "Failed to install custom 2-arg LIKE function for SQLite3:"
                << result;
    }

    result = sqlite3_create_function(
            handle,
            "like",
            3, // 3rd arg = ESCAPE
            SQLITE_UTF8 | SQLITE_DETERMINISTIC,
            nullptr,
            sqliteLikeUtf8,
            nullptr,
            nullptr);
    VERIFY_OR_DEBUG_ASSERT(result == SQLITE_OK) {
        kLogger.warning()
                << "Failed to install custom 3-arg LIKE function for SQLite3:"
                << result;
    }

    // Bite DJ: see sqliteKeyTextSortOrder.
    result = sqlite3_create_function(
            handle,
            "key_text_sort_order",
            2,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC,
            nullptr,
            sqliteKeyTextSortOrder,
            nullptr,
            nullptr);
    VERIFY_OR_DEBUG_ASSERT(result == SQLITE_OK) {
        kLogger.warning()
                << "Failed to install key_text_sort_order function for SQLite3:"
                << result;
    }
#else
    Q_UNUSED(database);
    Q_UNUSED(pCollator);
#endif // __SQLITE3__
    return true;
}

} // anonymous namespace

DbConnection::DbConnection(
        const Params& params,
        const QString& connectionName)
    : m_sqlDatabase(createDatabase(params, connectionName)) {
}

DbConnection::DbConnection(
        const DbConnection& prototype,
        const QString& connectionName)
    : m_sqlDatabase(cloneDatabase(prototype.m_sqlDatabase, connectionName)) {
}

DbConnection::~DbConnection() {
    close();
    removeDatabase(&m_sqlDatabase);
}

bool DbConnection::open() {
    if (kLogger.debugEnabled()) {
        kLogger.debug()
                << "Opening database connection"
                << *this;
    }
    if (!m_sqlDatabase.open()) {
        kLogger.warning()
                << "Failed to open database connection"
                << *this
                << m_sqlDatabase.lastError();
        return false; // abort
    }
    if (!initDatabase(m_sqlDatabase, &m_collator)) {
        kLogger.warning()
                << "Failed to initialize database connection"
                << *this;
        m_sqlDatabase.close();
        return false; // abort
    }
    return true;
}

void DbConnection::close() {
    if (m_sqlDatabase.isOpen()) {
        // There should never be an outstanding transaction when this code is
        // called. If there is, it means we probably aren't committing a
        // transaction somewhere that should be.
        VERIFY_OR_DEBUG_ASSERT(!m_sqlDatabase.rollback()) {
            kLogger.warning()
                << "Rolled back open transaction before closing database connection:"
                << *this;
        }
        if (kLogger.debugEnabled()) {
            kLogger.debug()
                    << "Closing database connection:"
                    << *this;
        }
        m_sqlDatabase.close();
    }
}

//static
QString DbConnection::collateLexicographically(const QString& orderByQuery) {
#ifdef __SQLITE3__
    return orderByQuery + QStringLiteral(" COLLATE ") + kLexicographicalCollationFunc;
#else
        return orderByQuery;
#endif //  __SQLITE3__
}

//static
int DbConnection::likeCompareLatinLow(
        QString* pattern,
        QString* string,
        QChar esc) {
    makeLatinLow(pattern->data(), pattern->length());
    makeLatinLow(string->data(), string->length());
    return likeCompareInner(
            pattern->data(), pattern->length(),
            string->data(), string->length(),
            esc);
}

//static
void DbConnection::makeStringLatinLow(QString* string) {
    makeLatinLow(string->data(), string->length());
}

QDebug operator<<(QDebug debug, const DbConnection& connection) {
    return debug
            << connection.name()
            << connection.m_sqlDatabase;
}

} // namespace mixxx
