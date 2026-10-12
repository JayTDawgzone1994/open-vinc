#include <vinc/bag/detail/database.hpp>
#include <vinc/message/detail/image_format.hpp>
#include <vinc/osal/path.hpp>
#include <vinc/osal/error.hpp>
#include <sqlite3.h>
#include <charconv>
#include <utility>
#include <chrono>

namespace vinc::bag {
namespace {
constexpr int application_id = 0x56424147; // VBAG; independent of message image version.
constexpr int format_version = 1;
constexpr std::uint64_t page_bytes = 4096;
// SQLite's pinned format limit; multiply only after widening to uint64_t.
constexpr std::uint64_t maximum_page_count = 4294967294ULL;
constexpr std::uint64_t maximum_database_bytes = maximum_page_count * page_bytes;
// Allow SQL row metadata in addition to the message BLOB; not a disk reserve.
constexpr std::uint64_t row_metadata_allowance = 1024 * 1024;
constexpr std::size_t name_bytes = config::bag_name_bytes;

Error database_error(int code, ErrorOperation operation) {
    const auto primary = code & 0xff;
    const auto category = primary == SQLITE_FULL ? ErrorCode::CapacityExceeded : ErrorCode::DatabaseFailed;
    return Error{category, operation, {NativeErrorSource::Sqlite, code}};
}

Result<void> execute(sqlite3* database, const char* sql, ErrorOperation operation) {
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK) {
        return std::unexpected(database_error(sqlite3_extended_errcode(database), operation));
    }

    return {};
}

// Statements own their sqlite handle; text/blob bindings borrow only through step().
class Statement {
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
    int code_;

public:
    Statement(sqlite3* db, const char* sql)
        : db_(db), code_(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr)) {
    }

    ~Statement() {
        sqlite3_finalize(statement_);
    }

    void text(int column, const std::string& value) {
        if (code_ == SQLITE_OK) {
            code_ = sqlite3_bind_text64(statement_, column, value.data(), value.size(), SQLITE_TRANSIENT,
                                        SQLITE_UTF8);
        }
    }

    void integer(int column, std::int64_t value) {
        if (code_ == SQLITE_OK) {
            code_ = sqlite3_bind_int64(statement_, column, value);
        }
    }

    void blob(int column, std::span<const std::byte> value) {
        if (code_ == SQLITE_OK) {
            code_ = sqlite3_bind_blob64(statement_, column, value.data(), value.size(), SQLITE_STATIC);
        }
    }

    Result<bool> step(ErrorOperation operation) {
        if (code_ != SQLITE_OK) {
            return std::unexpected(database_error(code_, operation));
        }

        const auto result = sqlite3_step(statement_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) {
            return std::unexpected(database_error(sqlite3_extended_errcode(db_), operation));
        }

        return result == SQLITE_ROW;
    }

    Result<std::string> string(int column, std::size_t maximum = name_bytes) {
        if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
            return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
        }

        const int length = sqlite3_column_bytes(statement_, column);
        if (length < 0 || static_cast<std::size_t>(length) > maximum) {
            return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
        }

        // SQLite exposes UTF-8 as unsigned bytes valid until the next step/finalize.
        const auto* bytes = sqlite3_column_text(statement_, column);
        if (!bytes) {
            return std::unexpected(Error{ErrorCode::ResourceExhausted, ErrorOperation::Inspect});
        }

        std::string result(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
        if (result.find('\0') != std::string::npos) {
            return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
        }

        return result;
    }

    Result<std::uint64_t> count(int column) {
        if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER ||
            sqlite3_column_int64(statement_, column) < 0) {
            return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
        }

        return static_cast<std::uint64_t>(sqlite3_column_int64(statement_, column));
    }
};

// Fixed-width decimal preserves every uint64 value and sorts chronologically in SQL.
std::string number(std::uint64_t value) {
    auto result = std::to_string(value);
    result.insert(result.begin(), 20 - result.size(), '0');
    return result;
}

Result<std::uint64_t> unsigned_column(Statement& statement, int column) {
    auto text = statement.string(column, 20);
    if (!text) {
        return std::unexpected(text.error());
    }

    std::uint64_t value = 0;
    auto parsed = std::from_chars(text->data(), text->data() + text->size(), value);
    if (text->size() != 20 || parsed.ec != std::errc{} || parsed.ptr != text->data() + text->size()) {
        return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
    }

    return value;
}

Result<void> done(Statement& statement, ErrorOperation operation) {
    auto result = statement.step(operation);
    if (!result) {
        return std::unexpected(result.error());
    }

    if (*result) {
        return std::unexpected(Error{ErrorCode::InvalidFormat, operation});
    }

    return {};
}

// Borrows the writer connection. The statement is finalized on every exit;
// this check neither owns the connection nor changes transaction state.
Result<void> verify_page_limit(sqlite3* database, std::uint64_t pages) {
    // PRAGMA may succeed while choosing a different value. Verify on the writer
    // connection before creating schema or admitting any recording data.
    Statement limit(database, "PRAGMA max_page_count");
    auto row = limit.step(ErrorOperation::Create);
    if (!row) {
        return std::unexpected(row.error());
    }

    if (!*row) {
        return std::unexpected(Error{ErrorCode::DatabaseFailed, ErrorOperation::Create});
    }

    auto applied = limit.count(0);
    if (!applied || *applied != pages) {
        return std::unexpected(Error{ErrorCode::DatabaseFailed, ErrorOperation::Create});
    }

    auto exhausted = done(limit, ErrorOperation::Create);
    if (!exhausted) {
        return exhausted;
    }

    return {};
}

Result<void> open_database(sqlite3*& db, const std::filesystem::path& directory, bool read_only) {
    const auto operation = read_only ? ErrorOperation::Inspect : ErrorOperation::Create;
    auto filename = osal::path_to_utf8(directory / "recording.sqlite");
    if (!filename) {
        return std::unexpected(wrap_error(ErrorCode::FileFailed, operation, filename.error()));
    }

    const auto flags = read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    const auto result = sqlite3_open_v2(filename->c_str(), &db, flags | SQLITE_OPEN_NOMUTEX, nullptr);
    if (result != SQLITE_OK) {
        return std::unexpected(
            database_error(result, read_only ? ErrorOperation::Inspect : ErrorOperation::Create));
    }

    sqlite3_extended_result_codes(db, 1);
    // A short-lived metadata reader is ordinary contention, not a disk fault.
    // Only this connection's owning thread waits; intake retains its drop budget.
    sqlite3_busy_timeout(db, config::bag_sqlite_busy_timeout_ms);
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    sqlite3_limit(db, SQLITE_LIMIT_LENGTH,
                  static_cast<int>(config::bag_message_bytes + row_metadata_allowance));
    return {};
}

// The caller owns the database; any failure leaves its transaction for rollback
// by close. No partially initialized database is presented as a completed bag.
Result<void> initialize_schema(sqlite3* database) {
    auto schema = execute(database, R"SQL(
        BEGIN IMMEDIATE;
        PRAGMA application_id=1447182663;
        PRAGMA user_version=1;
        CREATE TABLE recording(id INTEGER PRIMARY KEY CHECK(id=1), start_utc_ns INTEGER NOT NULL,
            completed INTEGER NOT NULL, received INTEGER NOT NULL, written INTEGER NOT NULL,
            queue_dropped INTEGER NOT NULL, oversized INTEGER NOT NULL, bus_lost INTEGER NOT NULL);
        CREATE TABLE topics(name TEXT NOT NULL, schema TEXT NOT NULL, type TEXT NOT NULL,
            supported INTEGER NOT NULL, PRIMARY KEY(name,schema));
        CREATE TABLE messages(id INTEGER PRIMARY KEY, topic TEXT NOT NULL, schema TEXT NOT NULL,
            publisher TEXT NOT NULL, publisher_id TEXT NOT NULL, sequence TEXT NOT NULL,
            source_ns TEXT NOT NULL, receive_ns TEXT NOT NULL, image_version INTEGER NOT NULL, payload BLOB NOT NULL);
        CREATE INDEX topic_messages ON messages(topic,schema);
    )SQL",
                          ErrorOperation::Create);
    if (!schema) {
        return schema;
    }

    Statement recording(database, "INSERT INTO recording VALUES(1,?,0,0,0,0,0,0)");
    const auto utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    recording.integer(1, utc);
    auto inserted = done(recording, ErrorOperation::Create);
    if (!inserted) {
        return inserted;
    }

    return execute(database, "COMMIT", ErrorOperation::Create);
}

Result<void> read_header(sqlite3* database, Info& result) {
    Statement header(database,
                     "SELECT completed,start_utc_ns,received,written,queue_dropped,oversized,bus_lost FROM "
                     "recording WHERE id=1");
    auto row = header.step(ErrorOperation::Inspect);
    if (!row) {
        return std::unexpected(row.error());
    }

    if (!*row) {
        return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
    }

    std::uint64_t fields[7]{};
    for (int index = 0; index < 7; ++index) {
        auto value = header.count(index);
        if (!value) {
            return std::unexpected(value.error());
        }

        fields[index] = *value;
    }

    if (fields[0] > 1) {
        return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
    }

    result.completed = fields[0] != 0;
    result.start_utc_ns = static_cast<std::int64_t>(fields[1]);
    result.counters = {fields[2], fields[3], fields[4], fields[5], fields[6]};
    return {};
}

Result<void> read_topics(sqlite3* database, Info& result) {
    Statement topics(database,
                     "SELECT "
                     "t.name,t.schema,t.type,t.supported,COUNT(m.id),COALESCE(MIN(m.source_ns),'"
                     "00000000000000000000'),COALESCE(MAX(m.source_ns),'00000000000000000000') FROM topics t "
                     "LEFT JOIN messages m ON m.topic=t.name AND m.schema=t.schema GROUP BY t.name,t.schema");
    for (;;) {
        auto next = topics.step(ErrorOperation::Inspect);
        if (!next) {
            return std::unexpected(next.error());
        }

        if (!*next) {
            break;
        }

        auto name = topics.string(0);
        auto schema = unsigned_column(topics, 1);
        auto type = topics.string(2);
        auto supported = topics.count(3);
        auto count = topics.count(4);
        auto first = unsigned_column(topics, 5);
        auto last = unsigned_column(topics, 6);
        if (result.topics.size() >= config::bag_max_topics) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Inspect});
        }

        if (!name || name->empty() || !schema || !*schema || !type || !supported || *supported > 1 ||
            !count || !first || !last) {
            return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
        }

        result.topics.push_back({{*name, *type, *schema, *supported != 0}, *count, *first, *last});
    }

    return {};
}

Result<void> read_sources(sqlite3* database, Info& result) {
    Statement sources(database,
                      "SELECT publisher,publisher_id,COUNT(*) FROM messages GROUP BY publisher_id,publisher");
    for (;;) {
        auto next = sources.step(ErrorOperation::Inspect);
        if (!next) {
            return std::unexpected(next.error());
        }

        if (!*next) {
            break;
        }

        auto name = sources.string(0);
        auto id = unsigned_column(sources, 1);
        auto count = sources.count(2);
        if (result.sources.size() >= config::bag_max_sources) {
            return std::unexpected(Error{ErrorCode::CapacityExceeded, ErrorOperation::Inspect});
        }

        if (!name || !id || !count) {
            return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
        }

        result.sources.push_back({*name, *id, *count});
    }

    return {};
}

}

namespace detail {
Database::~Database() {
    if (db_) {
        sqlite3_close_v2(db_);
    }
}

Result<void> Database::create(const std::filesystem::path& directory, const Options& options) {
    if (directory.empty() || options.database_bytes < config::bag_min_database_bytes ||
        options.database_bytes > maximum_database_bytes) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, ErrorOperation::Create});
    }

    // The directory is the exclusive recording identity. Never reuse old data or
    // remove an incomplete recording after a failure; it remains diagnosable.
    std::error_code error;
    const auto created = std::filesystem::create_directory(directory, error);
    if (!created) {
        return std::unexpected(Error{error ? ErrorCode::FileFailed : ErrorCode::AlreadyExists,
                                     ErrorOperation::Create, osal::native_error(error)});
    }

    auto opened = open_database(db_, directory, false);
    if (!opened) {
        return opened;
    }

    // Round down to whole pages. The journal is outside this main-file budget.
    const auto pages = options.database_bytes / page_bytes;
    const auto setup = "PRAGMA page_size=4096; PRAGMA journal_mode=DELETE; PRAGMA synchronous=EXTRA; "
                       "PRAGMA temp_store=MEMORY; PRAGMA cache_size=-" +
                       std::to_string(config::bag_sqlite_cache_kib) +
                       "; PRAGMA max_page_count=" + std::to_string(pages) + ";";
    auto configured = execute(db_, setup.c_str(), ErrorOperation::Create);
    if (!configured) {
        return configured;
    }

    auto verified = verify_page_limit(db_, pages);
    if (!verified) {
        return verified;
    }

    return initialize_schema(db_);
}

Result<void> Database::begin() {
    return execute(db_, "BEGIN IMMEDIATE", ErrorOperation::Write);
}

Result<void> Database::topic(const Topic& value) {
    Statement statement(db_, "INSERT INTO topics VALUES(?,?,?,?) ON CONFLICT(name,schema) DO UPDATE SET "
                             "type=excluded.type,supported=excluded.supported");
    statement.text(1, value.name);
    statement.text(2, number(value.schema));
    statement.text(3, value.type);
    statement.integer(4, value.supported);
    return done(statement, ErrorOperation::Write);
}

Result<void> Database::append(const Record& record, std::span<const std::byte> bytes) {
    const auto& message = *record.message;
    Statement statement(db_, "INSERT INTO messages VALUES(NULL,?,?,?,?,?,?,?,?,?)");
    statement.text(1, message.topic_name());
    statement.text(2, number(message.type_hash()));
    statement.text(3, message.publisher_name());
    statement.text(4, number(message.publisher_instance()));
    statement.text(5, number(message.get_topic_count()));
    statement.text(6, number(message.get_message_timestamp()));
    statement.text(7, number(record.receive_ns));
    statement.integer(8, message::detail::format::version);
    statement.blob(9, bytes);
    return done(statement, ErrorOperation::Write);
}

Result<void> Database::commit(const Counters& counters, bool completed) {
    Statement statement(db_,
                        "UPDATE recording SET "
                        "completed=?,received=?,written=?,queue_dropped=?,oversized=?,bus_lost=? WHERE id=1");
    statement.integer(1, completed);
    statement.integer(2, static_cast<std::int64_t>(counters.received));
    statement.integer(3, static_cast<std::int64_t>(counters.written));
    statement.integer(4, static_cast<std::int64_t>(counters.queue_dropped));
    statement.integer(5, static_cast<std::int64_t>(counters.oversized));
    statement.integer(6, static_cast<std::int64_t>(counters.bus_lost));
    auto updated = done(statement, ErrorOperation::Write);
    if (!updated) {
        return updated;
    }

    return execute(db_, "COMMIT", ErrorOperation::Write);
}

void Database::rollback() noexcept {
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
}

Result<void> Database::close() {
    const auto result = sqlite3_close(db_);
    if (result != SQLITE_OK) {
        return std::unexpected(database_error(result, ErrorOperation::Finish));
    }

    db_ = nullptr;
    return {};
}
}

Result<Info> inspect(const std::filesystem::path& directory) {
    sqlite3* database = nullptr;
    std::uint64_t progress_calls = 0;

    struct Close {
        sqlite3*& database;

        ~Close() {
            if (database) {
                sqlite3_close_v2(database);
            }
        }
    } close{database};

    auto opened = open_database(database, directory, true);
    if (!opened) {
        return std::unexpected(opened.error());
    }

    // Untrusted database schemas/queries must not consume an unbounded amount
    // of VM work. A large legitimate file can require a dedicated offline query.
    sqlite3_progress_handler(
        database, config::bag_info_progress_interval,
        [](void* context) {
            auto& calls = *static_cast<std::uint64_t*>(context);
            return ++calls > config::bag_info_progress_calls ? 1 : 0;
        },
        &progress_calls);
    for (const auto& [sql, expected] :
         {std::pair{"PRAGMA application_id", application_id}, {"PRAGMA user_version", format_version}}) {
        Statement check(database, sql);
        auto row = check.step(ErrorOperation::Inspect);
        if (!row) {
            return std::unexpected(row.error());
        }

        auto value = check.count(0);
        if (!*row || !value || *value != static_cast<std::uint64_t>(expected)) {
            return std::unexpected(Error{ErrorCode::InvalidFormat, ErrorOperation::Inspect});
        }
    }

    auto transaction = execute(database, "BEGIN", ErrorOperation::Inspect);
    if (!transaction) {
        return std::unexpected(transaction.error());
    }

    Info result;
    for (auto reader : {read_header, read_topics, read_sources}) {
        auto read = reader(database, result);
        if (!read) {
            return std::unexpected(read.error());
        }
    }

    return result;
}
}
