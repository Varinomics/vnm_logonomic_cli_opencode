#include "opencode_conversation.h"

#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

namespace opencode {
namespace {

namespace fs = std::filesystem;
using Json = addon::Json_value;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

bool fail(addon::Contract_failure* failure, const char* reason, std::string detail)
{
    if (failure) {
        *failure = {reason, std::move(detail)};
    }
    return false;
}

Statement query(sqlite3* database, const char* sql, std::string_view identity,
    addon::Contract_failure* failure)
{
    sqlite3_stmt* raw = nullptr;
    const int prepared = sqlite3_prepare_v2(database, sql, -1, &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    if (prepared != SQLITE_OK || (!identity.empty() &&
        sqlite3_bind_text(raw, 1, identity.data(), static_cast<int>(identity.size()), SQLITE_TRANSIENT) != SQLITE_OK))
    {
        fail(failure, "conversation_format_unavailable", "The retained OpenCode storage layout is unavailable.");
        return {nullptr, sqlite3_finalize};
    }
    return statement;
}

std::optional<std::string> column(sqlite3_stmt* statement, int index, addon::Contract_failure* failure)
{
    if (sqlite3_column_type(statement, index) != SQLITE_TEXT) {
        fail(failure, "conversation_content_unavailable", "An OpenCode conversation record is not retained text.");
        return {};
    }
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, index));
    if (!bytes) {
        fail(failure, "conversation_source_unavailable", "The OpenCode conversation could not be read.");
        return {};
    }
    return std::string(bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement, index)));
}

std::optional<Json> json_column(sqlite3_stmt* statement, int index, addon::Contract_failure* failure)
{
    const auto bytes = column(statement, index, failure);
    if (!bytes) {
        return {};
    }
    addon::Json_parse_limits limits;
    limits.max_bytes = bytes->size();
    limits.max_string_bytes = bytes->size();
    limits.max_members = bytes->size();
    auto value = addon::parse_json(*bytes, limits, nullptr);
    if (!value || !value->is_object()) {
        fail(failure, "conversation_content_unavailable", "An OpenCode conversation record has an unsupported format.");
        return {};
    }
    return value;
}

bool identity_member(Json& value, const char* name, std::string_view expected, addon::Contract_failure* failure)
{
    if (const auto* member = value.find(name);
        member && (!member->is_string() || member->as_string() != expected))
    {
        return fail(failure, "conversation_content_unavailable", "An OpenCode conversation record has another identity.");
    }
    value.set(name, Json(expected));
    return true;
}

bool complete(int status, addon::Contract_failure* failure)
{
    return status == SQLITE_DONE || fail(failure, "conversation_source_unavailable",
        "The OpenCode conversation snapshot could not be read completely.");
}

struct Revert
{
    std::string message;
    std::string part;
};

std::optional<Revert> session_revert(sqlite3* database, bool tui, std::string_view session,
    addon::Contract_failure* failure)
{
    auto statement = query(database, tui
        ? "SELECT revert FROM session_v2 WHERE id = ?1"
        : "SELECT revert FROM session WHERE id = ?1", session, failure);
    if (!statement) {
        return {};
    }
    const int status = sqlite3_step(statement.get());
    if (status != SQLITE_ROW) {
        fail(failure, status == SQLITE_DONE ? "conversation_not_found" : "conversation_source_unavailable",
            "The exact retained OpenCode conversation is unavailable.");
        return {};
    }
    Revert result;
    if (sqlite3_column_type(statement.get(), 0) == SQLITE_NULL) {
        return result;
    }
    const auto value = json_column(statement.get(), 0, failure);
    if (!value) {
        return {};
    }
    const auto* message = value->find("messageID");
    const auto* part = value->find("partID");
    if (!message || !message->is_string() || message->as_string().empty() ||
        (part && (!part->is_string() || part->as_string().empty())))
    {
        fail(failure, "conversation_branch_unavailable", "The OpenCode revert boundary is unavailable.");
        return {};
    }
    result.message = message->as_string();
    if (part) {
        result.part = part->as_string();
    }
    return result;
}

bool legacy_identity(sqlite3* database, std::string_view session, addon::Contract_failure* failure)
{
    auto table = query(database,
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'session_v2'", {}, failure);
    if (!table) {
        return false;
    }
    const int exists = sqlite3_step(table.get());
    if (exists == SQLITE_DONE) {
        return true;
    }
    if (exists != SQLITE_ROW) {
        return complete(exists, failure);
    }
    auto migrated = query(database, "SELECT 1 FROM session_v2 WHERE id = ?1", session, failure);
    if (!migrated) {
        return false;
    }
    const int status = sqlite3_step(migrated.get());
    if (status == SQLITE_ROW) {
        return fail(failure, "conversation_source_unavailable",
            "This OpenCode conversation was imported into another native generation. "
            "Resume it through Logonomic first.");
    }
    return complete(status, failure);
}

bool legacy_messages(sqlite3* database, std::string_view session, const Revert& revert,
    addon::Conversation_writer& writer, addon::Contract_failure* failure)
{
    auto messages = query(database,
        "SELECT id, data FROM message WHERE session_id = ?1 ORDER BY time_created, id", session, failure);
    if (!messages) {
        return false;
    }
    bool found = false;
    int status;
    while ((status = sqlite3_step(messages.get())) == SQLITE_ROW) {
        const auto id = column(messages.get(), 0, failure);
        auto info = json_column(messages.get(), 1, failure);
        if (!id || !info || !identity_member(*info, "id", *id, failure) ||
            !identity_member(*info, "sessionID", session, failure))
        {
            return false;
        }
        const bool boundary = !revert.message.empty() && *id == revert.message;
        if (boundary && revert.part.empty()) {
            return found || fail(failure, "conversation_empty", "No retained OpenCode discussion precedes the revert.");
        }
        auto source = query(database,
            "SELECT id, session_id, data FROM part WHERE message_id = ?1 ORDER BY id", *id, failure);
        if (!source) {
            return false;
        }
        std::vector<Json> parts;
        int part_status;
        while ((part_status = sqlite3_step(source.get())) == SQLITE_ROW) {
            const auto part_id = column(source.get(), 0, failure);
            const auto owner = column(source.get(), 1, failure);
            auto part = json_column(source.get(), 2, failure);
            if (!part_id || !owner || !part || *owner != session ||
                !identity_member(*part, "id", *part_id, failure) ||
                !identity_member(*part, "messageID", *id, failure) ||
                !identity_member(*part, "sessionID", session, failure))
            {
                return fail(failure, "conversation_content_unavailable",
                    "An OpenCode part has another conversation identity.");
            }
            if (boundary && *part_id == revert.part) {
                break;
            }
            parts.push_back(std::move(*part));
        }
        if (part_status != SQLITE_ROW && !complete(part_status, failure)) {
            return false;
        }
        if (!append_legacy_message(*info, parts, writer, failure)) {
            return false;
        }
        found = true;
        if (boundary) {
            return true;
        }
    }
    return complete(status, failure) && (found || fail(failure, "conversation_empty",
        "No retained OpenCode discussion is available."));
}

bool tui_messages(sqlite3* database, std::string_view session, const Revert& revert,
    addon::Conversation_writer& writer, addon::Contract_failure* failure)
{
    std::optional<sqlite3_int64> boundary;
    if (!revert.message.empty()) {
        auto target = query(database,
            "SELECT seq FROM session_message WHERE session_id = ?1 AND id = ?2", session, failure);
        if (!target || sqlite3_bind_text(target.get(), 2, revert.message.data(),
                static_cast<int>(revert.message.size()), SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(target.get()) != SQLITE_ROW || sqlite3_column_type(target.get(), 0) != SQLITE_INTEGER)
        {
            return fail(failure, "conversation_branch_unavailable", "The OpenCode revert boundary is not retained.");
        }
        boundary = sqlite3_column_int64(target.get(), 0);
    }
    auto messages = query(database,
        "SELECT id, type, seq, data FROM session_message WHERE session_id = ?1 ORDER BY seq", session, failure);
    if (!messages) {
        return false;
    }
    bool found = false;
    int status;
    while ((status = sqlite3_step(messages.get())) == SQLITE_ROW) {
        if (sqlite3_column_type(messages.get(), 2) != SQLITE_INTEGER) {
            return fail(failure, "conversation_content_unavailable", "An OpenCode message has no native sequence.");
        }
        if (boundary && sqlite3_column_int64(messages.get(), 2) >= *boundary) {
            return found || fail(failure, "conversation_empty", "No retained OpenCode discussion precedes the revert.");
        }
        const auto id = column(messages.get(), 0, failure);
        const auto type = column(messages.get(), 1, failure);
        auto message = json_column(messages.get(), 3, failure);
        if (!id || !type || !message || !identity_member(*message, "id", *id, failure) ||
            !identity_member(*message, "type", *type, failure) || !append_tui_message(*message, writer, failure))
        {
            return false;
        }
        found = true;
    }
    return complete(status, failure) && (found || fail(failure, "conversation_empty",
        "No retained OpenCode discussion is available."));
}

class Read_transaction
{
public:
    explicit Read_transaction(sqlite3* database) : m_database(database) {}
    ~Read_transaction()
    {
        if (m_active) {
            sqlite3_progress_handler(m_database, 0, nullptr, nullptr);
            sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    bool begin()
    {
        m_active = sqlite3_exec(m_database, "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK;
        return m_active;
    }
    bool finish()
    {
        if (sqlite3_exec(m_database, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
            return false;
        }
        m_active = false;
        return true;
    }

private:
    sqlite3* m_database;
    bool     m_active = false;
};

} // namespace

std::optional<addon::Conversation_export_result> export_conversation(
    const addon::Conversation_export_request& request, const std::function<bool()>& cancelled,
    addon::Contract_failure* failure)
{
    const auto* locator = request.launch_environment.effective_environment.find(k_export_source_environment);
    if (!locator || !locator->is_string() ||
        (!locator->as_string().starts_with("v1|") && !locator->as_string().starts_with("v2|")))
    {
        fail(failure, "conversation_source_unavailable",
            "The conversation's original OpenCode storage was not recorded.");
        return {};
    }
    const auto source = std::string_view(locator->as_string()).substr(3);
    const auto path = addon::path_from_utf8(source);
    if (source.empty() || source.find('\0') != std::string_view::npos ||
        !path.is_absolute() || !fs::is_regular_file(path))
    {
        fail(failure, "conversation_source_unavailable",
            "The conversation's original OpenCode database is unavailable.");
        return {};
    }
    auto stop = cancelled;
    if (stop && stop()) {
        fail(failure, "cancelled", "Conversation extraction was cancelled.");
        return {};
    }
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(addon::path_to_utf8(path).c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (opened != SQLITE_OK || sqlite3_db_readonly(database.get(), "main") != 1) {
        fail(failure, "conversation_source_unavailable",
            "The conversation's original OpenCode database could not be read.");
        return {};
    }
    sqlite3_busy_timeout(database.get(), 1000);
    sqlite3_progress_handler(database.get(), 1000, [](void* context) {
        const auto& callback = *static_cast<const std::function<bool()>*>(context);
        return callback && callback() ? 1 : 0;
    }, &stop);
    Read_transaction transaction(database.get());
    if (!transaction.begin()) {
        fail(failure, "conversation_source_unavailable", "The OpenCode conversation snapshot is unavailable.");
        return {};
    }
    const bool tui = locator->as_string().starts_with("v2|");
    if (!tui && !legacy_identity(database.get(), request.provider_session_id, failure)) {
        return {};
    }
    const auto revert = session_revert(database.get(), tui, request.provider_session_id, failure);
    if (!revert) {
        return {};
    }
    addon::Conversation_writer writer(request, cancelled);
    if (!writer.open(failure) || !(tui
        ? tui_messages(database.get(), request.provider_session_id, *revert, writer, failure)
        : legacy_messages(database.get(), request.provider_session_id, *revert, writer, failure)))
    {
        return {};
    }
    if (!transaction.finish()) {
        fail(failure, "conversation_source_unavailable", "The OpenCode conversation snapshot could not be completed.");
        return {};
    }
    return writer.finish(false, failure);
}

} // namespace opencode
