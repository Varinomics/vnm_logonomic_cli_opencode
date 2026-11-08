#include "opencode_conversation.h"

#include <logonomic_cli_addon/package_source.h>

#ifdef _WIN32
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <algorithm>
#include <filesystem>
#include <initializer_list>
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

std::optional<std::string_view> column_view(sqlite3_stmt* statement, int index, addon::Contract_failure* failure)
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
    return std::string_view(bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement, index)));
}

std::optional<std::string> column(sqlite3_stmt* statement, int index, addon::Contract_failure* failure)
{
    const auto bytes = column_view(statement, index, failure);
    return bytes ? std::optional(std::string(*bytes)) : std::nullopt;
}

std::optional<Json> json_column(sqlite3_stmt* statement, int index, addon::Contract_failure* failure)
{
    const auto bytes = column_view(statement, index, failure);
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
                static_cast<int>(revert.message.size()), SQLITE_STATIC) != SQLITE_OK ||
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

bool only_members(const Json& value, std::initializer_list<std::string_view> names)
{
    return value.is_object() && std::all_of(value.as_object().begin(), value.as_object().end(),
        [&](const auto& member) { return std::find(names.begin(), names.end(), member.name) != names.end(); });
}

bool native_model(const Json& value)
{
    if (!only_members(value, {"providerID", "id", "variant"})) return false;
    for (const auto* name : {"providerID", "id"}) {
        const auto* member = value.find(name);
        if (!member || !member->is_string() || member->as_string().empty()) return false;
    }
    const auto* variant = value.find("variant");
    return !variant || variant->is_string();
}

bool selection_metadata(const Json& message, std::string_view type)
{
    const auto* metadata = message.find("metadata");
    if (metadata && !metadata->is_object()) return false;
    const auto* time = message.find("time");
    if (time && (!only_members(*time, {"created"}) || !time->find("created") ||
        !time->find("created")->is_integer())) return false;
    if (type == "model-switched" && only_members(message, {"id", "type", "metadata", "time", "model", "previous"})) {
        const auto* model = message.find("model");
        const auto* previous = message.find("previous");
        return model && native_model(*model) && (!previous || native_model(*previous));
    }
    if (type == "agent-switched" && only_members(message, {"id", "type", "metadata", "time", "agent", "previous"})) {
        const auto* agent = message.find("agent");
        const auto* previous = message.find("previous");
        return agent && agent->is_string() && !agent->as_string().empty() &&
            (!previous || (previous->is_string() && !previous->as_string().empty()));
    }
    // Other metadata schemas are intentionally UNKNOWN until understood.
    return false;
}

} // namespace

bool is_conversation_source(std::string_view source)
{
    return (source.starts_with("v1|") || source.starts_with("v2|")) && source.size() > 3 &&
        source.find('\0') == std::string_view::npos && addon::path_from_utf8(source.substr(3)).is_absolute();
}

addon::Resume_inspect_result inspect_conversation(std::string_view source, std::string_view session,
    const std::function<bool()>& cancelled)
{
    addon::Resume_inspect_result result;
    if (!is_conversation_source(source) || (cancelled && cancelled())) {
        return result;
    }
    const auto path = addon::path_from_utf8(source.substr(3));
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) {
        return result;
    }
    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(addon::path_to_utf8(path).c_str(), &raw,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
    if (opened != SQLITE_OK || sqlite3_db_readonly(raw, "main") != 1) {
        return result;
    }
    sqlite3_busy_timeout(raw, 1000);
    auto stop = cancelled;
    sqlite3_progress_handler(raw, 1000, [](void* context) {
        const auto& stop = *static_cast<const std::function<bool()>*>(context);
        return stop && stop() ? 1 : 0;
    }, &stop);
    Read_transaction transaction(raw);
    if (!transaction.begin()) {
        return result;
    }
    const bool tui = source.starts_with("v2|");
    if (!tui && !legacy_identity(raw, session, nullptr)) {
        return result;
    }
    auto root = query(raw, tui
        ? "SELECT parent_id, revert, fork_session_id, fork_boundary FROM session_v2 WHERE id = ?1"
        : "SELECT parent_id, revert FROM session WHERE id = ?1", session, nullptr);
    if (!root) {
        return result;
    }
    const int found = sqlite3_step(root.get());
    if (found == SQLITE_DONE) {
        if (transaction.finish()) result.availability = addon::Resume_availability::ABSENT;
        return result;
    }
    if (found != SQLITE_ROW || sqlite3_column_type(root.get(), 0) != SQLITE_NULL) {
        return result;
    }
    result.availability = addon::Resume_availability::AVAILABLE;
    // A branch or revert may retain content outside the local message set.
    // The read-only inspector does not flatten or delete that native history.
    for (int index = 1; index < (tui ? 4 : 2); ++index) {
        if (sqlite3_column_type(root.get(), index) != SQLITE_NULL) return result;
    }
    const auto no_rows = [&](const char* sql) {
        auto statement = query(raw, sql, session, nullptr);
        return statement && sqlite3_step(statement.get()) == SQLITE_DONE;
    };
    const auto optional_empty = [&](const char* table, const char* sql) {
        auto exists = query(raw, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1", table, nullptr);
        if (!exists) return false;
        const int status = sqlite3_step(exists.get());
        return status == SQLITE_DONE || (status == SQLITE_ROW && no_rows(sql));
    };
    if (tui && (!no_rows("SELECT 1 FROM session_inbox WHERE session_id = ?1 LIMIT 1") ||
        !optional_empty("session_pending", "SELECT 1 FROM session_pending WHERE session_id = ?1 LIMIT 1")))
    {
        return result;
    }
    bool meaningful = false;
    bool understood = true;
    auto messages = query(raw, tui
        ? "SELECT id, type, data FROM session_message WHERE session_id = ?1 ORDER BY seq"
        : "SELECT id, data, data FROM message WHERE session_id = ?1 ORDER BY time_created, id",
        session, nullptr);
    if (!messages) return result;
    int status;
    while ((status = sqlite3_step(messages.get())) == SQLITE_ROW) {
        const auto id = column(messages.get(), 0, nullptr);
        auto type = tui ? column(messages.get(), 1, nullptr) : std::optional<std::string>{};
        auto message = json_column(messages.get(), 2, nullptr);
        if (!tui && message) {
            const auto* role = message->find("role");
            if (role && role->is_string()) type = role->as_string();
        }
        if (!id || !type || !message || !identity_member(*message, "id", *id, nullptr) ||
            !identity_member(*message, tui ? "type" : "role", *type, nullptr) ||
            (!tui && !identity_member(*message, "sessionID", session, nullptr)))
        {
            return result;
        }
        if (tui) {
            if (*type == "user") {
                const auto* text = message->find("text");
                const auto* files = message->find("files");
                if (!text || !text->is_string() || (files && !files->is_array())) return result;
                if (text->as_string().empty() && (!files || files->as_array().empty())) return result;
                meaningful = true;
            }
            else
            if (!selection_metadata(*message, *type))
            {
                // Assistant parts, compaction and synthetic records have
                // changed layouts across native generations. Their presence
                // cannot establish emptiness even when this reader cannot
                // prove meaningful main-conversation content.
                understood = false;
            }
        }
        else {
            if (*type != "user" && *type != "assistant") return result;
            auto parts = query(raw, "SELECT session_id, data FROM part WHERE message_id = ?1 ORDER BY id", *id, nullptr);
            if (!parts) return result;
            bool content = false;
            int part_status;
            while ((part_status = sqlite3_step(parts.get())) == SQLITE_ROW) {
                const auto owner = column(parts.get(), 0, nullptr);
                auto part = json_column(parts.get(), 1, nullptr);
                if (!owner || *owner != session || !part || !identity_member(*part, "messageID", *id, nullptr) ||
                    !identity_member(*part, "sessionID", session, nullptr)) return result;
                const auto* kind = part->find("type");
                const auto* text = part->find("text");
                if (!kind || !kind->is_string()) return result;
                if (kind->as_string() == "text" && text && text->is_string()) {
                    content = content || !text->as_string().empty();
                }
                else {
                    return result;
                }
            }
            if (part_status != SQLITE_DONE || !content) return result;
            meaningful = true;
        }
    }
    if (status != SQLITE_DONE || (cancelled && cancelled())) return result;
    if (!meaningful && (!no_rows(tui
        ? "SELECT 1 FROM session_v2 WHERE parent_id = ?1 LIMIT 1"
        : "SELECT 1 FROM session WHERE parent_id = ?1 LIMIT 1") ||
        !(tui ? (optional_empty("session_part", "SELECT 1 FROM session_part WHERE session_id = ?1 LIMIT 1") &&
                 optional_empty("blob_ref", "SELECT 1 FROM blob_ref WHERE session_id = ?1 LIMIT 1"))
              : no_rows("SELECT 1 FROM part WHERE session_id = ?1 LIMIT 1"))))
    {
        return result;
    }
    if (transaction.finish() && (meaningful || understood)) {
        result.content = meaningful ? addon::Resume_content::NONEMPTY : addon::Resume_content::EMPTY;
    }
    return result;
}

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
