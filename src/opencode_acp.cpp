#include "opencode_acp.h"

#include <logonomic_cli_addon/limits.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <string_view>
#include <utility>

namespace opencode {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;
using Keys = std::initializer_list<std::string_view>;

constexpr std::size_t k_max_record_bytes = 256 * 1024;
constexpr std::size_t k_max_result_bytes = 1024 * 1024;
constexpr double k_max_exact_integer = 9007199254740991.0;

const Json& member(const Json& value, std::string_view key)
{
    static const Json absent;
    const auto* found = value.find(key);
    return found ? *found : absent;
}

std::string text(const Json& value)
{
    return value.is_string() ? value.as_string() : std::string{};
}

bool one_of(std::string_view value, Keys choices)
{
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool only_keys(const Json& value, Keys keys)
{
    return value.is_object() && std::all_of(
        value.as_object().begin(), value.as_object().end(), [&](const auto& entry) {
            return one_of(entry.name, keys);
        });
}

bool exact_keys(const Json& value, Keys keys)
{
    return only_keys(value, keys) && value.as_object().size() == keys.size();
}

bool valid_meta(const Json& value)
{
    const auto& meta = member(value, "_meta");
    return meta.is_null() || meta.is_object();
}

// ACP's native adapter bounds decoded strings in UTF-16 code units.
std::size_t utf16_size(std::string_view value)
{
    std::size_t size = 0;
    for (unsigned char byte : value) {
        if ((byte & 0xc0) != 0x80) {
            size += byte >= 0xf0 ? 2 : 1;
        }
    }
    return size;
}

bool bounded_string(const Json& value, std::size_t maximum)
{
    return value.is_string() && utf16_size(value.as_string()) <= maximum;
}

bool bounded_nonempty(const Json& value, std::size_t maximum)
{
    return bounded_string(value, maximum) && !value.as_string().empty();
}

bool exact_integer(const Json& value)
{
    if (!value.is_number()) {
        return false;
    }
    const double number = value.as_number();
    return std::isfinite(number) && number >= 0 &&
        number <= k_max_exact_integer && std::floor(number) == number;
}

Json object(std::initializer_list<addon::Json_member> members)
{
    return Json(Json::Object(members));
}

std::string json_line(const Json& value)
{
    return addon::serialize_json(value) + '\n';
}

bool response(const Json& value, int id, Json* result)
{
    if (!exact_keys(value, {"jsonrpc", "id", "result"}) ||
        text(member(value, "jsonrpc")) != "2.0" ||
        !member(value, "id").is_number() || member(value, "id").as_number() != id ||
        !member(value, "result").is_object())
    {
        return false;
    }
    *result = member(value, "result");
    return true;
}

bool text_chunk(const Json& update, std::string_view type, std::string* message_id)
{
    const auto& content = member(update, "content");
    const auto& message = member(update, "messageId");
    if (!only_keys(update, {"_meta", "content", "messageId", "sessionUpdate"}) ||
        !valid_meta(update) || text(member(update, "sessionUpdate")) != type ||
        !only_keys(content, {"_meta", "text", "type"}) || !valid_meta(content) ||
        text(member(content, "type")) != "text" || !member(content, "text").is_string() ||
        (!message.is_null() && !bounded_nonempty(message, 512)))
    {
        return false;
    }
    *message_id = text(message);
    return true;
}

bool available_commands(const Json& update)
{
    const auto& commands = member(update, "availableCommands");
    if (!only_keys(update, {"_meta", "availableCommands", "sessionUpdate"}) ||
        !valid_meta(update) || !commands.is_array() || commands.as_array().size() > 1024)
    {
        return false;
    }
    for (const auto& command : commands.as_array()) {
        if (!only_keys(command, {"_meta", "description", "input", "name"}) ||
            !valid_meta(command) || !bounded_nonempty(member(command, "name"), 512) ||
            !bounded_string(member(command, "description"), 4096))
        {
            return false;
        }
        const auto& input = member(command, "input");
        if (!input.is_null() &&
            (!only_keys(input, {"_meta", "hint"}) || !valid_meta(input) ||
             !bounded_string(member(input, "hint"), 4096)))
        {
            return false;
        }
    }
    return true;
}

bool tool_content(const Json& content)
{
    const auto type = text(member(content, "type"));
    if (type == "content") {
        const auto& block = member(content, "content");
        if (!only_keys(content, {"_meta", "content", "type"}) || !valid_meta(content)) {
            return false;
        }
        const auto block_type = text(member(block, "type"));
        if (block_type == "text") {
            return only_keys(block, {"_meta", "text", "type"}) && valid_meta(block) &&
                bounded_string(member(block, "text"), k_max_record_bytes);
        }
        if (block_type == "image") {
            return only_keys(block, {"_meta", "data", "mimeType", "type"}) && valid_meta(block) &&
                bounded_nonempty(member(block, "mimeType"), 256) &&
                bounded_nonempty(member(block, "data"), k_max_record_bytes);
        }
        return false;
    }
    if (type == "diff") {
        const auto& old_text = member(content, "oldText");
        return only_keys(content, {"_meta", "newText", "oldText", "path", "type"}) &&
            valid_meta(content) && bounded_string(member(content, "newText"), k_max_record_bytes) &&
            bounded_string(member(content, "path"), 32 * 1024) &&
            (old_text.is_null() || bounded_string(old_text, k_max_record_bytes));
    }
    return false;
}

bool tool_locations(const Json& locations)
{
    if (!locations.is_array() || locations.as_array().size() > 1024) {
        return false;
    }
    for (const auto& location : locations.as_array()) {
        const auto& line = member(location, "line");
        if (!only_keys(location, {"_meta", "line", "path"}) || !valid_meta(location) ||
            !bounded_string(member(location, "path"), 32 * 1024) ||
            (!line.is_null() && !exact_integer(line)))
        {
            return false;
        }
    }
    return true;
}

bool tool_update(const Json& update)
{
    if (!only_keys(update, {"_meta", "content", "kind", "locations", "rawInput", "rawOutput",
                          "sessionUpdate", "status", "title", "toolCallId"}) ||
        !valid_meta(update) || !bounded_nonempty(member(update, "toolCallId"), 512))
    {
        return false;
    }
    const auto type = text(member(update, "sessionUpdate"));
    if (!one_of(type, {"tool_call", "tool_call_update"}) ||
        (type == "tool_call" && !bounded_nonempty(member(update, "title"), 4096)))
    {
        return false;
    }
    const auto& title     = member(update, "title");
    const auto& kind      = member(update, "kind");
    const auto& status    = member(update, "status");
    const auto& content   = member(update, "content");
    const auto& locations = member(update, "locations");
    if ((!title.is_null() && !bounded_string(title, 4096)) ||
        (!kind.is_null() && !one_of(text(kind), {"read", "edit", "delete", "move", "search",
                                              "execute", "think", "fetch", "switch_mode", "other"})) ||
        (!status.is_null() && !one_of(text(status), {"pending", "in_progress", "completed", "failed"})) ||
        (!locations.is_null() && !tool_locations(locations)))
    {
        return false;
    }
    return content.is_null() || (content.is_array() && content.as_array().size() <= 1024 &&
        std::all_of(content.as_array().begin(), content.as_array().end(), tool_content));
}

bool plan_update(const Json& update)
{
    const auto& entries = member(update, "entries");
    if (!only_keys(update, {"_meta", "entries", "sessionUpdate"}) || !valid_meta(update) ||
        !entries.is_array() || entries.as_array().empty() || entries.as_array().size() > 1024)
    {
        return false;
    }
    for (const auto& entry : entries.as_array()) {
        if (!only_keys(entry, {"_meta", "content", "priority", "status"}) || !valid_meta(entry) ||
            !bounded_nonempty(member(entry, "content"), 16 * 1024) ||
            !one_of(text(member(entry, "priority")), {"low", "medium", "high"}) ||
            !one_of(text(member(entry, "status")), {"pending", "in_progress", "completed"}))
        {
            return false;
        }
    }
    return true;
}

bool initialize_response(const Json& result, const std::string& version)
{
    const auto& protocol     = member(result, "protocolVersion");
    const auto& capabilities = member(result, "agentCapabilities");
    const auto& auth         = member(result, "authMethods");
    const auto& info         = member(result, "agentInfo");
    const auto& title        = member(info, "title");
    return only_keys(result, {"_meta", "agentCapabilities", "agentInfo", "authMethods", "protocolVersion"}) &&
        valid_meta(result) && protocol.is_number() && protocol.as_number() == 1 &&
        (!result.find("agentCapabilities") || capabilities.is_object()) &&
        (!result.find("authMethods") || auth.is_array()) &&
        only_keys(info, {"_meta", "name", "title", "version"}) && valid_meta(info) &&
        text(member(info, "name")) == "OpenCode" && text(member(info, "version")) == version &&
        (title.is_null() || bounded_string(title, 512));
}

bool new_session_response(const Json& result, std::string* session_id)
{
    const auto& config = member(result, "configOptions");
    const auto& models = member(result, "models");
    const auto& modes  = member(result, "modes");
    if (!only_keys(result, {"_meta", "configOptions", "models", "modes", "sessionId"}) ||
        !valid_meta(result) || !bounded_nonempty(member(result, "sessionId"), 512) ||
        (!config.is_null() && !config.is_array()) ||
        (!models.is_null() && !models.is_object()) || (!modes.is_null() && !modes.is_object()))
    {
        return false;
    }
    *session_id = text(member(result, "sessionId"));
    return true;
}

bool usage_valid(const Json& usage)
{
    if (!only_keys(usage, {"cachedReadTokens", "cachedWriteTokens", "inputTokens",
                         "outputTokens", "thoughtTokens", "totalTokens"}) ||
        !exact_integer(member(usage, "inputTokens")) ||
        !exact_integer(member(usage, "outputTokens")) || !exact_integer(member(usage, "totalTokens")))
    {
        return false;
    }
    for (const auto key : {"cachedReadTokens", "cachedWriteTokens", "thoughtTokens"}) {
        const auto& value = member(usage, key);
        if (!value.is_null() && !exact_integer(value)) {
            return false;
        }
    }
    return true;
}

bool prompt_response(const Json& result)
{
    const auto& usage   = member(result, "usage");
    const auto& message = member(result, "userMessageId");
    return only_keys(result, {"_meta", "stopReason", "usage", "userMessageId"}) &&
        valid_meta(result) && text(member(result, "stopReason")) == "end_turn" &&
        (usage.is_null() || usage_valid(usage)) && (message.is_null() || bounded_nonempty(message, 512));
}

bool apply_update(
    const Json& value, const std::string& stage, std::string* session_id,
    std::string* result, std::string* result_message_id)
{
    const auto& params = member(value, "params");
    const auto& update = member(params, "update");
    if (!only_keys(value, {"jsonrpc", "method", "params"}) ||
        text(member(value, "jsonrpc")) != "2.0" || text(member(value, "method")) != "session/update" ||
        !only_keys(params, {"_meta", "sessionId", "update"}) || !valid_meta(params) ||
        !bounded_nonempty(member(params, "sessionId"), 512) || !update.is_object())
    {
        return false;
    }
    const auto update_session = text(member(params, "sessionId"));
    if (stage == "await_new") {
        if (!session_id->empty()) {
            return false;
        }
        *session_id = update_session;
    }
    else
    if (*session_id != update_session) {
        return false;
    }
    const auto type = text(member(update, "sessionUpdate"));
    if (type == "available_commands_update") {
        return one_of(stage, {"await_new", "await_prompt", "await_close"}) && available_commands(update);
    }
    if (type == "agent_message_chunk") {
        std::string message_id;
        if (stage != "await_prompt" || !text_chunk(update, type, &message_id)) {
            return false;
        }
        if (*result_message_id != message_id) {
            *result_message_id = message_id;
            result->clear();
        }
        result->append(text(member(member(update, "content"), "text")));
        return result->size() <= k_max_result_bytes;
    }
    if (type == "agent_thought_chunk") {
        std::string message_id;
        return stage == "await_prompt" && text_chunk(update, type, &message_id);
    }
    if (one_of(type, {"tool_call", "tool_call_update"})) {
        return stage == "await_prompt" && tool_update(update);
    }
    if (type == "plan") {
        return stage == "await_prompt" && plan_update(update);
    }
    if (type == "usage_update") {
        const auto& cost = member(update, "cost");
        bool cost_valid = cost.is_null();
        if (cost.is_object()) {
            const auto& amount = member(cost, "amount");
            const auto currency = text(member(cost, "currency"));
            cost_valid = exact_keys(cost, {"amount", "currency"}) && amount.is_number() &&
                std::isfinite(amount.as_number()) && amount.as_number() >= 0 && currency.size() == 3 &&
                std::all_of(currency.begin(), currency.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
        }
        return one_of(stage, {"await_prompt", "await_close"}) &&
            only_keys(update, {"_meta", "cost", "sessionUpdate", "size", "used"}) && valid_meta(update) &&
            exact_integer(member(update, "used")) && exact_integer(member(update, "size")) &&
            member(update, "size").as_number() > 0 && cost_valid;
    }
    if (type == "session_info_update") {
        const auto& title   = member(update, "title");
        const auto& updated = member(update, "updatedAt");
        return one_of(stage, {"await_prompt", "await_close"}) &&
            only_keys(update, {"_meta", "sessionUpdate", "title", "updatedAt"}) && valid_meta(update) &&
            (title.is_null() || bounded_string(title, 4096)) &&
            (updated.is_null() || bounded_nonempty(updated, 128));
    }
    return false;
}

bool attestations_ready(const Json& values)
{
    namespace fs = std::filesystem;
    if (!values.is_array() || values.as_array().empty() || values.as_array().size() > 8) {
        return false;
    }
    std::set<std::string> server_ids;
    for (const auto& value : values.as_array()) {
        if (!exact_keys(value, {"server_id", "path", "nonce"})) {
            return false;
        }
        const auto server = text(member(value, "server_id"));
        const auto nonce  = text(member(value, "nonce"));
        const auto path_text = text(member(value, "path"));
        const fs::path path(std::u8string(path_text.begin(), path_text.end()));
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (server.empty() || server.size() > 64 || server.front() < 'a' || server.front() > 'z' ||
            !std::all_of(server.begin(), server.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            }) || !server_ids.insert(server).second || !path.is_absolute() ||
            nonce.size() != 32 || !std::all_of(nonce.begin(), nonce.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }) || error || !fs::is_regular_file(status) || fs::is_symlink(status))
        {
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        char bytes[33]{};
        file.read(bytes, sizeof(bytes));
        if (file.gcount() != 32 || !file.eof() || std::string_view(bytes, 32) != nonce) {
            return false;
        }
    }
    return true;
}

std::string permission_response(const Json& request, const std::string& session_id, Json* permission_ids)
{
    const auto& id     = member(request, "id");
    const auto& params = member(request, "params");
    const auto& options = member(params, "options");
    if (!exact_keys(request, {"jsonrpc", "id", "method", "params"}) ||
        text(member(request, "jsonrpc")) != "2.0" ||
        text(member(request, "method")) != "session/request_permission" ||
        (!id.is_number() && !id.is_string()) ||
        !only_keys(params, {"_meta", "options", "sessionId", "toolCall"}) || !valid_meta(params) ||
        text(member(params, "sessionId")) != session_id || !member(params, "toolCall").is_object() ||
        !options.is_array())
    {
        return {};
    }
    Json tool = member(params, "toolCall");
    if (tool.find("sessionUpdate")) {
        return {};
    }
    tool.set("sessionUpdate", Json("tool_call_update"));
    if (!tool_update(tool)) {
        return {};
    }
    bool allow_once = false;
    std::set<std::string> option_ids;
    for (const auto& option : options.as_array()) {
        const auto option_id = text(member(option, "optionId"));
        const auto kind      = text(member(option, "kind"));
        if (!only_keys(option, {"_meta", "kind", "name", "optionId"}) || !valid_meta(option) ||
            !bounded_nonempty(member(option, "name"), 512) ||
            !bounded_nonempty(member(option, "optionId"), 512) || !option_ids.insert(option_id).second ||
            !one_of(kind, {"allow_once", "allow_always", "reject_once", "reject_always"}))
        {
            return {};
        }
        if (option_id == "once" && kind == "allow_once") {
            allow_once = true;
        }
    }
    if (!allow_once || option_ids.empty() || option_ids.size() > 64 ||
        permission_ids->as_array().size() >= 256)
    {
        return {};
    }
    // Numeric JSON IDs compare by value, including alternate exponent/fraction spellings.
    const Json normalized_id = id.is_number() ? Json(id.as_number() == 0 ? 0.0 : id.as_number()) : id;
    const auto encoded_id = addon::serialize_json(Json(Json::Array{normalized_id}));
    for (const auto& previous : permission_ids->as_array()) {
        if (text(previous) == encoded_id) {
            return {};
        }
    }
    permission_ids->push_back(Json(encoded_id));
    return json_line(object({
        {"jsonrpc", Json("2.0")}, {"id", id},
        {"result", object({{"outcome", object({
            {"outcome", Json("selected")}, {"optionId", Json("once")},
        })}})},
    }));
}

addon::Structured_inspect_result failure(const char* reason)
{
    addon::Structured_inspect_result result;
    result.complete       = true;
    result.failure_reason = reason;
    return result;
}

addon::Structured_inspect_result partial(const Json& state, const std::string& input)
{
    addon::Structured_inspect_result result;
    result.protocol_state = addon::serialize_json(state);
    if (!input.empty()) {
        result.stdin_bytes = input;
    }
    return result;
}

} // namespace

addon::Inspection_setup make_acp_inspection(
    const std::string& prompt,
    const std::string& working_directory,
    const std::string& provider_version,
    const Json& mcp_servers,
    const Json& attestations)
{
    const auto state = object({
        {"stage", Json("await_initialize")}, {"pending_base64", Json("")},
        {"deferred_input_base64", Json("")}, {"deferred_input_is_request", Json(false)},
        {"prompt_base64", Json(addon::encode_base64(prompt))}, {"session_id", Json("")},
        {"working_directory", Json(working_directory)}, {"provider_version", Json(provider_version)},
        {"result_base64", Json("")}, {"result_message_id", Json("")},
        {"permission_ids", Json::array()}, {"mcp_servers", mcp_servers}, {"attestations", attestations},
    });
    addon::Inspection_setup setup;
    setup.incremental    = true;
    setup.protocol_state = addon::serialize_json(state);
    setup.keep_stdin_open = true;
    return setup;
}

std::string acp_initial_input()
{
    return json_line(object({
        {"jsonrpc", Json("2.0")}, {"id", Json(1)}, {"method", Json("initialize")},
        {"params", object({
            {"protocolVersion", Json(1)}, {"clientCapabilities", Json::object()},
            {"clientInfo", object({{"name", Json("Logonomic")}, {"version", Json("1.0.0")}})},
        })},
    }));
}

addon::Structured_inspect_result inspect_acp(const addon::Structured_inspection& inspection)
{
    if (utf16_size(inspection.protocol_state) > 2 * 1024 * 1024) {
        return failure("acp_invalid_state");
    }
    addon::Json_parse_limits state_limits;
    state_limits.max_bytes        = 6 * 1024 * 1024;
    state_limits.max_string_bytes = state_limits.max_bytes;
    auto parsed_state = addon::parse_json(inspection.protocol_state, state_limits, nullptr);
    if (!parsed_state ||
        !exact_keys(*parsed_state, {"stage", "pending_base64", "prompt_base64", "session_id",
                                  "working_directory", "provider_version", "result_base64",
                                  "result_message_id", "permission_ids", "mcp_servers", "attestations",
                                  "deferred_input_base64", "deferred_input_is_request"}))
    {
        return failure("acp_invalid_state");
    }
    Json state = std::move(*parsed_state);
    if (!member(state, "stage").is_string() || !member(state, "session_id").is_string() ||
        !member(state, "working_directory").is_string() ||
        !bounded_nonempty(member(state, "provider_version"), 64) ||
        !member(state, "result_message_id").is_string() || !member(state, "permission_ids").is_array() ||
        !member(state, "mcp_servers").is_array() || !member(state, "attestations").is_array() ||
        !member(state, "pending_base64").is_string() || !member(state, "prompt_base64").is_string() ||
        !member(state, "result_base64").is_string() ||
        !member(state, "deferred_input_base64").is_string() ||
        !member(state, "deferred_input_is_request").is_boolean())
    {
        return failure("acp_invalid_state");
    }
    auto pending_bytes = addon::decode_base64(text(member(state, "pending_base64")));
    auto prompt_bytes  = addon::decode_base64(text(member(state, "prompt_base64")));
    auto result_bytes  = addon::decode_base64(text(member(state, "result_base64")));
    auto deferred_input = addon::decode_base64(text(member(state, "deferred_input_base64")));
    if (!pending_bytes || !prompt_bytes || !result_bytes || !deferred_input) {
        return failure("acp_invalid_state");
    }
    auto pending    = std::move(*pending_bytes);
    auto result     = std::move(*result_bytes);
    auto message_id = text(member(state, "result_message_id"));
    auto permissions = member(state, "permission_ids");
    pending.append(inspection.stdout_bytes);
    if (pending.size() > k_max_record_bytes && pending.find('\n') == std::string::npos) {
        return failure("acp_record_bound_exceeded");
    }
    // Preserve the causal boundary while a record preceding these writes is partial.
    std::string next_input = std::move(*deferred_input);
    bool next_input_contains_request = member(state, "deferred_input_is_request").as_boolean();
    for (;;) {
        const auto line_end = pending.find('\n');
        if (line_end == std::string::npos) {
            break;
        }
        if (line_end == 0 || line_end > k_max_record_bytes) {
            return failure("acp_invalid_record");
        }
        const auto record = pending.substr(0, line_end);
        pending.erase(0, line_end + 1);
        addon::Json_parse_limits record_limits;
        record_limits.max_bytes        = k_max_record_bytes;
        record_limits.max_string_bytes = k_max_record_bytes;
        auto parsed = addon::parse_json(record, record_limits, nullptr);
        if (!parsed || !parsed->is_object()) {
            return failure("acp_invalid_json");
        }
        const auto& value = *parsed;
        const auto stage = text(member(state, "stage"));
        if (value.find("method")) {
            if (value.find("id")) {
                if (stage != "await_prompt" || next_input_contains_request) {
                    return failure("acp_out_of_order_reverse_request");
                }
                const auto reply = permission_response(value, text(member(state, "session_id")), &permissions);
                if (reply.empty()) {
                    return failure("acp_unsupported_reverse_request");
                }
                if (reply.size() > addon::limits::k_max_structured_stream_bytes - next_input.size()) {
                    return failure("acp_permission_reply_bound_exceeded");
                }
                // Independent reverse requests may share a transport read. Their
                // replies do not authorize responses to an unsent session request.
                next_input += reply;
                state.set("permission_ids", permissions);
                continue;
            }
            if (next_input_contains_request) {
                const auto type = text(member(member(member(value, "params"), "update"), "sessionUpdate"));
                if (type != "available_commands_update" &&
                    !(stage == "await_close" && one_of(type, {"usage_update", "session_info_update"})))
                {
                    return failure("acp_out_of_order_notification");
                }
            }
            auto session = text(member(state, "session_id"));
            if (!apply_update(value, stage, &session, &result, &message_id)) {
                return failure("acp_unauthenticated_notification");
            }
            state.set("result_base64", Json(addon::encode_base64(result)));
            state.set("result_message_id", Json(message_id));
            state.set("session_id", Json(session));
            continue;
        }
        Json response_result;
        if (stage == "await_initialize") {
            if (!next_input.empty() || !response(value, 1, &response_result) ||
                !initialize_response(response_result, text(member(state, "provider_version"))))
            {
                return failure("acp_invalid_initialization");
            }
            state.set("stage", Json("await_new"));
            next_input = json_line(object({
                {"jsonrpc", Json("2.0")}, {"id", Json(2)}, {"method", Json("session/new")},
                {"params", object({
                    {"cwd", member(state, "working_directory")}, {"mcpServers", member(state, "mcp_servers")},
                })},
            }));
            // The request owns these definitions after emission; retaining a second
            // copy in every subsequent continuation needlessly consumes the frame.
            state.set("mcp_servers", Json::array());
            state.set("working_directory", Json(""));
        }
        else
        if (stage == "await_new") {
            std::string session;
            if (!next_input.empty() || !response(value, 2, &response_result) ||
                !new_session_response(response_result, &session))
            {
                return failure("acp_invalid_root_session");
            }
            // ACP lets the agent announce the new session before or after it answers
            // session/new (OpenCode 1.18.32 announces after). The response names the
            // session; an announcement that preceded it must name the same one.
            const auto announced = text(member(state, "session_id"));
            if (!announced.empty() && announced != session) {
                return failure("acp_unauthenticated_notification");
            }
            // OpenCode answers only after listing every task server's tools, and the
            // host writes each nonce before it forwards that list.
            if (!attestations_ready(member(state, "attestations"))) {
                return failure("acp_task_servers_not_ready");
            }
            if (prompt_bytes->empty() || !addon::is_valid_utf8(*prompt_bytes)) {
                return failure("acp_invalid_prompt");
            }
            state.set("stage", Json("await_prompt"));
            state.set("session_id", Json(session));
            state.set("prompt_base64", Json(""));
            state.set("mcp_servers", Json::array());
            state.set("attestations", Json::array());
            next_input = json_line(object({
                {"jsonrpc", Json("2.0")}, {"id", Json(3)}, {"method", Json("session/prompt")},
                {"params", object({
                    {"sessionId", Json(session)},
                    {"prompt", Json(Json::Array{object({{"type", Json("text")}, {"text", Json(*prompt_bytes)}})})},
                })},
            }));
        }
        else
        if (stage == "await_prompt") {
            if (!next_input.empty() || !response(value, 3, &response_result) ||
                !prompt_response(response_result) || result.empty())
            {
                return failure("acp_invalid_prompt_completion");
            }
            state.set("stage", Json("await_close"));
            next_input = json_line(object({
                {"jsonrpc", Json("2.0")}, {"id", Json(4)}, {"method", Json("session/close")},
                {"params", object({{"sessionId", member(state, "session_id")}})},
            }));
        }
        else
        if (stage == "await_close") {
            if (inspection.timed_out) {
                return failure("acp_timed_out");
            }
            if (!next_input.empty() || !response(value, 4, &response_result) ||
                !only_keys(response_result, {"_meta"}) || !valid_meta(response_result) || !pending.empty())
            {
                return failure("acp_invalid_close");
            }
            if (inspection.final && inspection.exit_code != 0) {
                return failure("acp_nonzero_exit");
            }
            addon::Structured_inspect_result complete;
            complete.complete     = true;
            complete.result_bytes = std::move(result);
            return complete;
        }
        else {
            return failure("acp_out_of_order_output");
        }
        next_input_contains_request = true;
    }
    state.set("pending_base64", Json(addon::encode_base64(pending)));
    state.set("result_base64", Json(addon::encode_base64(result)));
    state.set("result_message_id", Json(message_id));
    const bool defer_input = !pending.empty() && !next_input.empty();
    state.set("deferred_input_base64", Json(defer_input ? addon::encode_base64(next_input) : ""));
    state.set("deferred_input_is_request", Json(defer_input && next_input_contains_request));
    if (!next_input.empty() && !defer_input) {
        return partial(state, next_input);
    }
    if (inspection.final) {
        if (inspection.timed_out) {
            return failure("acp_timed_out");
        }
        return failure(inspection.exit_code != 0 ? "acp_nonzero_exit" : "acp_exit_before_close");
    }
    return partial(state, {});
}

} // namespace opencode
