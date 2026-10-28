#pragma once

#include "opencode_conversation.h"

#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace opencode_test {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

inline void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

inline addon::Json_value json(std::string_view text)
{
    const auto value = addon::parse_json(text, {}, nullptr);
    require(value.has_value(), "Native-schema fixture JSON must be valid.");
    return *value;
}

inline std::string read(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "Fixture file must be readable.");
    std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    require(!stream.bad(), "Fixture file must be read completely.");
    return bytes;
}

inline std::size_t occurrences(std::string_view text, std::string_view needle)
{
    require(!needle.empty(), "Occurrence checks need nonempty text.");
    std::size_t count = 0;
    for (std::size_t offset = 0; (offset = text.find(needle, offset)) != std::string_view::npos;
         offset += needle.size())
    {
        ++count;
    }
    return count;
}

class Database
{
public:
    explicit Database(fs::path path)
    :
        m_path(fs::absolute(std::move(path)).lexically_normal())
    {
        fs::create_directories(m_path.parent_path());
        sqlite3* raw = nullptr;
        const int status = sqlite3_open_v2(addon::path_to_utf8(m_path).c_str(), &raw,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
        m_database.reset(raw);
        require(status == SQLITE_OK, "Test-owned SQLite writer must open.");
        execute("PRAGMA journal_mode = WAL");
        execute("PRAGMA wal_autocheckpoint = 0");
        execute("CREATE TABLE session (id TEXT PRIMARY KEY, parent_id TEXT, revert TEXT)");
        execute("CREATE TABLE session_v2 (id TEXT PRIMARY KEY, parent_id TEXT, "
            "fork_session_id TEXT, fork_boundary TEXT, revert TEXT)");
        execute("CREATE TABLE message (id TEXT PRIMARY KEY, session_id TEXT NOT NULL, "
            "time_created INTEGER NOT NULL, data TEXT NOT NULL)");
        execute("CREATE TABLE part (id TEXT PRIMARY KEY, message_id TEXT NOT NULL, "
            "session_id TEXT NOT NULL, data TEXT NOT NULL)");
        execute("CREATE TABLE session_message (id TEXT PRIMARY KEY, session_id TEXT NOT NULL, "
            "seq INTEGER NOT NULL, type TEXT NOT NULL, data TEXT NOT NULL, UNIQUE(session_id, seq))");
    }

    ~Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    const fs::path& path() const { return m_path; }

    void execute(std::string_view sql)
    {
        const std::string statement(sql);
        require(sqlite3_exec(m_database.get(), statement.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK,
            "Test-owned SQLite statement must complete.");
    }

    void session(std::string_view id, bool tui, const addon::Json_value& revert = addon::Json_value())
    {
        auto statement = prepare(tui
            ? "INSERT INTO session_v2(id, revert) VALUES (?1, ?2) "
              "ON CONFLICT(id) DO UPDATE SET revert = excluded.revert"
            : "INSERT INTO session(id, revert) VALUES (?1, ?2) "
              "ON CONFLICT(id) DO UPDATE SET revert = excluded.revert");
        bind(statement.get(), 1, id);
        if (revert.is_null()) {
            require(sqlite3_bind_null(statement.get(), 2) == SQLITE_OK, "Fixture revert must bind.");
        }
        else {
            bind(statement.get(), 2, addon::serialize_json(revert));
        }
        finish(statement.get());
    }

    void legacy_message(std::string_view id, std::string_view session, std::int64_t created,
        const addon::Json_value& data)
    {
        require(data.is_object() && !data.find("id") && !data.find("sessionID"),
            "Native message data excludes its canonical identity columns.");
        auto statement = prepare("INSERT INTO message(id, session_id, time_created, data) VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(id) DO UPDATE SET session_id = excluded.session_id, "
            "time_created = excluded.time_created, data = excluded.data");
        bind(statement.get(), 1, id);
        bind(statement.get(), 2, session);
        require(sqlite3_bind_int64(statement.get(), 3, created) == SQLITE_OK, "Fixture message time must bind.");
        bind(statement.get(), 4, addon::serialize_json(data));
        finish(statement.get());
    }

    void legacy_part(std::string_view id, std::string_view message, std::string_view session,
        const addon::Json_value& data)
    {
        require(data.is_object() && !data.find("id") && !data.find("messageID") && !data.find("sessionID"),
            "Native part data excludes its canonical identity columns.");
        auto statement = prepare("INSERT INTO part(id, message_id, session_id, data) VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(id) DO UPDATE SET message_id = excluded.message_id, "
            "session_id = excluded.session_id, data = excluded.data");
        bind(statement.get(), 1, id);
        bind(statement.get(), 2, message);
        bind(statement.get(), 3, session);
        bind(statement.get(), 4, addon::serialize_json(data));
        finish(statement.get());
    }

    void tui_message(std::string_view id, std::string_view session, std::int64_t seq,
        std::string_view type, const addon::Json_value& data)
    {
        require(data.is_object() && !data.find("id") && !data.find("type"),
            "Native projected message data excludes its canonical identity and type columns.");
        auto statement = prepare("INSERT INTO session_message(id, session_id, seq, type, data) "
            "VALUES (?1, ?2, ?3, ?4, ?5) ON CONFLICT(id) DO UPDATE SET "
            "session_id = excluded.session_id, seq = excluded.seq, type = excluded.type, data = excluded.data");
        bind(statement.get(), 1, id);
        bind(statement.get(), 2, session);
        require(sqlite3_bind_int64(statement.get(), 3, seq) == SQLITE_OK, "Fixture message sequence must bind.");
        bind(statement.get(), 4, type);
        bind(statement.get(), 5, addon::serialize_json(data));
        finish(statement.get());
    }

private:
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    Statement prepare(const char* sql)
    {
        sqlite3_stmt* raw = nullptr;
        const int status = sqlite3_prepare_v2(m_database.get(), sql, -1, &raw, nullptr);
        Statement statement(raw, sqlite3_finalize);
        require(status == SQLITE_OK, "Test-owned SQLite insert must prepare.");
        return statement;
    }

    static void bind(sqlite3_stmt* statement, int index, std::string_view value)
    {
        require(sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
            SQLITE_TRANSIENT) == SQLITE_OK,
            "Test-owned SQLite value must bind.");
    }

    static void finish(sqlite3_stmt* statement)
    {
        require(sqlite3_step(statement) == SQLITE_DONE, "Test-owned SQLite insert must commit.");
    }

    fs::path m_path;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> m_database{nullptr, sqlite3_close};
};

struct Export
{
    std::optional<addon::Conversation_export_result> result;
    addon::Contract_failure failure;
    std::string text;
    fs::path output;
};

inline Export extract(const fs::path& database, bool tui, std::string_view session,
    const fs::path& output, addon::Conversation_export_format format = addon::Conversation_export_format::MARKDOWN,
    bool compact = false, const std::function<bool()>& cancelled = {})
{
    fs::create_directories(output);
    Export exported;
    exported.output = fs::canonical(output);
    addon::Conversation_export_request request;
    request.provider_session_id = std::string(session);
    request.output_directory = addon::path_to_utf8(exported.output);
    request.format = format;
    request.compact = compact;
#ifdef _WIN32
    request.launch_environment.platform_id = "windows-x86_64";
#elif defined(__APPLE__)
    request.launch_environment.platform_id = "macos-arm64";
#else
    request.launch_environment.platform_id = "linux-x86_64";
#endif
    request.launch_environment.effective_environment = addon::Json_value::object();
    request.launch_environment.effective_environment.set(opencode::k_export_source_environment,
        addon::Json_value(std::string(tui ? "v2|" : "v1|") + addon::path_to_utf8(fs::canonical(database))));
    exported.result = opencode::export_conversation(request, cancelled, &exported.failure);
    if (exported.result) {
        exported.text = read(exported.output / exported.result->file_name);
    }
    return exported;
}

bool tui_conversation_tests(const fs::path& root);

} // namespace opencode_test
