#include "opencode_acp.h"

#include <logonomic_cli_addon/json_utilities.h>
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

using addon::json::exact_keys;
using addon::json::member;
using addon::json::object;
using addon::json::only_keys;

constexpr std::size_t k_max_record_bytes = 256 * 1024;
constexpr std::size_t k_max_result_bytes = 1024 * 1024;
constexpr double k_max_exact_integer = 9007199254740991.0;

std::string text(const Json& value)
{
    return value.is_string() ? value.as_string() : std::string{};
}

bool one_of(std::string_view value, Keys choices)
{
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

bool valid_meta(const Json& value)
{
    const auto& meta = member(value, "_meta");
    return meta.is_null() || meta.is_object();
}

bool child_projection(const Json& value)
{
    return member(value, "_meta").find("opencode/child-session") != nullptr;
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
    if (!only_keys(update, {"_meta", "content", "kind", "locations", "name", "rawInput", "rawOutput",
                          "sessionUpdate", "status", "title", "toolCallId"}) ||
        !valid_meta(update) || !bounded_nonempty(member(update, "toolCallId"), 512) ||
        (update.find("name") && !bounded_nonempty(member(update, "name"), 512)))
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

std::optional<std::string> configuration_value(const Json& options, std::string_view id)
{
    if (!options.is_array()) {
        return std::nullopt;
    }
    std::optional<std::string> value;
    for (const auto& option : options.as_array()) {
        if (text(member(option, "id")) != id) {
            continue;
        }
        if (value || text(member(option, "type")) != "select" ||
            !member(option, "currentValue").is_string())
        {
            return std::nullopt;
        }
        value = member(option, "currentValue").as_string();
    }
    return value;
}

bool selection_confirmed(const Json& options, const addon::Delegated_model_selection& selection)
{
    return (!selection.model || configuration_value(options, "model") == selection.model) &&
        (!selection.reasoning_effort || configuration_value(options, "effort") == selection.reasoning_effort);
}

std::string configure_input(Json& state, bool model, const std::string& value,
    const std::optional<std::string>& previous)
{
    state.set("stage", Json(model ? "await_model" : "await_effort"));
    state.set("configuration_before", previous ? Json(*previous) : Json());
    return json_line(object({
        {"jsonrpc", Json("2.0")}, {"id", Json((model ? 5 : 6) + (previous ? 0 : 2))},
        {"method", Json("session/set_config_option")},
        {"params", object({
            {"sessionId", member(state, "session_id")},
            {"configId", Json(model ? "model" : "effort")}, {"value", Json(value)},
        })},
    }));
}

std::string prompt_input(Json& state, const std::string& prompt)
{
    state.erase("configuration_before");
    state.set("stage", Json("await_prompt"));
    state.set("prompt_base64", Json(""));
    return json_line(object({
        {"jsonrpc", Json("2.0")}, {"id", Json(3)}, {"method", Json("session/prompt")},
        {"params", object({
            {"sessionId", member(state, "session_id")},
            {"prompt", Json(Json::Array{object({{"type", Json("text")}, {"text", Json(prompt)}})})},
        })},
    }));
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
        return one_of(stage, {"await_new", "await_model", "await_effort", "await_prompt", "await_close"}) &&
            available_commands(update);
    }
    if (type == "config_option_update") {
        return one_of(stage, {"await_model", "await_effort", "await_prompt"}) &&
            only_keys(update, {"_meta", "sessionUpdate", "configOptions"}) && valid_meta(update) &&
            member(update, "configOptions").is_array();
    }
    if (type == "agent_message_chunk") {
        std::string message_id;
        if (stage != "await_prompt" || !text_chunk(update, type, &message_id)) {
            return false;
        }
        // OpenCode 2 can project a subagent's text into the root ACP session.
        if (child_projection(update)) return true;
        if (*result_message_id != message_id) {
            *result_message_id = message_id;
            result->clear();
        }
        result->append(member(member(update, "content"), "text").as_string());
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
        return one_of(stage, {"await_prompt", "await_close"}) &&
            only_keys(update, {"_meta", "cost", "sessionUpdate", "size", "used"}) && valid_meta(update) &&
            exact_integer(member(update, "used")) && exact_integer(member(update, "size")) &&
            member(update, "size").as_number() > 0;
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

// Without an explicit auto mode, the package approves once only a call of one
// of the task's own Task tools: OpenCode announced it with a `tool_call` titled with
// the tool's id, and the request carries that id, title and kind `other`.
// Anything else gets the request's own reject option, since nobody can approve
// it in a headless run; OpenCode titles a shell request with the command text,
// so a title alone could be chosen by the command.
bool announced_task_tool(const Json& tool, const Json& announced)
{
    const auto call  = text(member(tool, "toolCallId"));
    const auto title = text(member(tool, "title"));
    return !child_projection(tool) && text(member(tool, "kind")) == "other" &&
        (!tool.find("name") || text(member(tool, "name")) == title) &&
        std::any_of(announced.as_array().begin(), announced.as_array().end(), [&](const Json& entry) {
            return text(member(entry, "toolCallId")) == call && text(member(entry, "title")) == title;
        });
}

bool record_request_id(const Json& id, Json* request_ids)
{
    if ((!id.is_number() && !id.is_string()) || request_ids->as_array().size() >= 256) {
        return false;
    }
    // Numeric JSON IDs compare by value, including alternate exponent/fraction spellings.
    const Json normalized_id = id.is_number() ? Json(id.as_number() == 0 ? 0.0 : id.as_number()) : id;
    const auto encoded_id = addon::serialize_json(Json(Json::Array{normalized_id}));
    for (const auto& previous : request_ids->as_array()) {
        if (text(previous) == encoded_id) {
            return false;
        }
    }
    request_ids->push_back(Json(encoded_id));
    return true;
}

std::string unsupported_write_response(
    const Json& request, const std::string& session_id, Json* request_ids)
{
    const auto& id     = member(request, "id");
    const auto& params = member(request, "params");
    if (!exact_keys(request, {"jsonrpc", "id", "method", "params"}) ||
        text(member(request, "jsonrpc")) != "2.0" ||
        text(member(request, "method")) != "fs/write_text_file" ||
        !only_keys(params, {"_meta", "content", "path", "sessionId"}) || !valid_meta(params) ||
        text(member(params, "sessionId")) != session_id ||
        !bounded_nonempty(member(params, "path"), 32 * 1024) ||
        !bounded_string(member(params, "content"), k_max_record_bytes) ||
        !record_request_id(id, request_ids))
    {
        return {};
    }
    // OpenCode 1.18.33 proposes an ACP write even without client FS capability.
    // Refuse it truthfully; OpenCode retains ownership of its approved native edit.
    return json_line(object({
        {"jsonrpc", Json("2.0")}, {"id", id},
        {"error", object({{"code", Json(-32601)}, {"message", Json("Method not found")}})},
    }));
}

std::string permission_response(
    const Json& request, const std::string& session_id, const Json& announced, bool auto_approve, Json* permission_ids)
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
    // Native --auto replies once to pending asks. OpenCode enforces explicit
    // denies before emitting these requests; no configured rule is replaced.
    const bool approve = auto_approve || announced_task_tool(tool, announced);
    const std::string chosen_id   = approve ? "once" : "reject";
    const std::string chosen_kind = approve ? "allow_once" : "reject_once";
    bool offered = false;
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
        if (option_id == chosen_id && kind == chosen_kind) {
            offered = true;
        }
    }
    if (!offered || option_ids.empty() || option_ids.size() > 64 ||
        !record_request_id(id, permission_ids))
    {
        return {};
    }
    return json_line(object({
        {"jsonrpc", Json("2.0")}, {"id", id},
        {"result", object({{"outcome", object({
            {"outcome", Json("selected")}, {"optionId", Json(chosen_id)},
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
    const Json& attestations,
    const Json& task_tools,
    bool auto_approve,
    const std::optional<addon::Delegated_model_selection>& model_selection)
{
    auto state = object({
        {"stage", Json("await_initialize")}, {"pending_base64", Json("")},
        {"deferred_input_base64", Json("")}, {"deferred_input_is_request", Json(false)},
        {"deferred_input_has_permission_reply", Json(false)},
        {"prompt_base64", Json(addon::encode_base64(prompt))}, {"session_id", Json("")},
        {"working_directory", Json(working_directory)}, {"provider_version", Json(provider_version)},
        {"result_base64", Json("")}, {"result_message_id", Json("")},
        {"permission_ids", Json::array()}, {"mcp_servers", mcp_servers}, {"attestations", attestations},
        {"task_tools", task_tools}, {"announced", Json::array()}, {"auto_approve", Json(auto_approve)},
    });
    if (model_selection) {
        state.set("model_selection", addon::encode_delegated_model_selection(*model_selection));
    }
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
    auto parsed_state = addon::parse_exchange_protocol_state(inspection.protocol_state);
    // An in-flight inspection without an explicit mode keeps default refusal.
    if (parsed_state && parsed_state->is_object() && !parsed_state->find("auto_approve")) {
        parsed_state->set("auto_approve", Json(false));
    }
    // Older continuations did not distinguish permission replies from refusals.
    // Keep their deferred input causally blocking until it has been delivered.
    if (parsed_state && parsed_state->is_object() && !parsed_state->find("deferred_input_has_permission_reply")) {
        parsed_state->set("deferred_input_has_permission_reply",
            Json(!text(member(*parsed_state, "deferred_input_base64")).empty()));
    }
    if (!parsed_state ||
        !only_keys(*parsed_state, {"stage", "pending_base64", "prompt_base64", "session_id",
                                  "working_directory", "provider_version", "result_base64",
                                  "result_message_id", "permission_ids", "mcp_servers", "attestations",
                                  "deferred_input_base64", "deferred_input_is_request", "deferred_input_has_permission_reply",
                                  "task_tools", "announced", "auto_approve", "model_selection", "configuration_before"}))
    {
        return failure("acp_invalid_state");
    }
    Json state = std::move(*parsed_state);
    std::optional<addon::Delegated_model_selection> model_selection;
    if (const auto* selection = state.find("model_selection")) {
        addon::Contract_failure contract_failure;
        model_selection = addon::decode_delegated_model_selection(*selection, &contract_failure);
        if (!model_selection) {
            return failure("model_selection_unavailable");
        }
    }
    if (!member(state, "stage").is_string() || !member(state, "session_id").is_string() ||
        !member(state, "working_directory").is_string() ||
        !bounded_nonempty(member(state, "provider_version"), 64) ||
        !member(state, "result_message_id").is_string() || !member(state, "permission_ids").is_array() ||
        !member(state, "mcp_servers").is_array() || !member(state, "attestations").is_array() ||
        !member(state, "pending_base64").is_string() || !member(state, "prompt_base64").is_string() ||
        !member(state, "result_base64").is_string() ||
        !member(state, "deferred_input_base64").is_string() ||
        !member(state, "deferred_input_is_request").is_boolean() ||
        !member(state, "deferred_input_has_permission_reply").is_boolean() ||
        !member(state, "task_tools").is_array() || !member(state, "announced").is_array() ||
        !member(state, "auto_approve").is_boolean())
    {
        return failure("acp_invalid_state");
    }
    auto pending_bytes = addon::decode_base64(member(state, "pending_base64").as_string());
    auto prompt_bytes  = addon::decode_base64(member(state, "prompt_base64").as_string());
    auto result_bytes  = addon::decode_base64(member(state, "result_base64").as_string());
    auto deferred_input = addon::decode_base64(member(state, "deferred_input_base64").as_string());
    if (!pending_bytes || !prompt_bytes || !result_bytes || !deferred_input) {
        return failure("acp_invalid_state");
    }
    if (inspection.timed_out) return failure("acp_timed_out");
    if (inspection.final && inspection.exit_code != 0) return failure("acp_nonzero_exit");
    auto pending    = std::move(*pending_bytes);
    auto result     = std::move(*result_bytes);
    auto message_id = text(member(state, "result_message_id"));
    auto permissions = member(state, "permission_ids");
    pending.append(inspection.stdout_bytes);
    if (text(member(state, "stage")) == "await_exit" && !pending.empty()) return failure("acp_invalid_close");
    if (pending.size() > k_max_record_bytes && pending.find('\n') == std::string::npos) {
        return failure("acp_record_bound_exceeded");
    }
    // Preserve the causal boundary while a record preceding these writes is partial.
    std::string next_input = std::move(*deferred_input);
    bool next_input_contains_request     = member(state, "deferred_input_is_request").as_boolean();
    bool next_input_has_permission_reply = member(state, "deferred_input_has_permission_reply").as_boolean();
    std::string_view remaining = pending;
    for (;;) {
        const auto line_end = remaining.find('\n');
        if (line_end == std::string_view::npos) {
            break;
        }
        if (line_end == 0 || line_end > k_max_record_bytes) {
            return failure("acp_invalid_record");
        }
        const auto record = remaining.substr(0, line_end);
        remaining.remove_prefix(line_end + 1);
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
                const auto session_id = text(member(state, "session_id"));
                const bool unsupported_write = text(member(value, "method")) == "fs/write_text_file";
                const auto reply = unsupported_write
                    ? unsupported_write_response(value, session_id, &permissions)
                    : permission_response(value, session_id, member(state, "announced"),
                                          member(state, "auto_approve").as_boolean(), &permissions);
                if (reply.empty()) {
                    return failure("acp_unsupported_reverse_request");
                }
                if (reply.size() > addon::limits::k_max_structured_stream_bytes - next_input.size()) {
                    return failure("acp_permission_reply_bound_exceeded");
                }
                // Independent reverse requests may share a transport read. Their
                // replies do not authorize responses to an unsent session request.
                next_input += reply;
                next_input_has_permission_reply |= !unsupported_write;
                state.set("permission_ids", permissions);
                continue;
            }
            if (next_input_contains_request) {
                const auto type = text(member(member(member(value, "params"), "update"), "sessionUpdate"));
                if (type != "available_commands_update" &&
                    !(model_selection && type == "config_option_update" &&
                      one_of(stage, {"await_model", "await_effort", "await_prompt"})) &&
                    !(stage == "await_close" && one_of(type, {"usage_update", "session_info_update"})))
                {
                    return failure("acp_out_of_order_notification");
                }
            }
            auto session = text(member(state, "session_id"));
            if (!apply_update(value, stage, &session, &result, &message_id)) {
                return failure("acp_unauthenticated_notification");
            }
            const auto& update = member(member(value, "params"), "update");
            if (text(member(update, "sessionUpdate")) == "config_option_update" &&
                (!model_selection ||
                 (stage == "await_prompt" && !selection_confirmed(member(update, "configOptions"), *model_selection))))
            {
                return failure("model_selection_unavailable");
            }
            const auto& tools  = member(state, "task_tools").as_array();
            if (text(member(update, "sessionUpdate")) == "tool_call" && !child_projection(update) &&
                text(member(update, "kind")) == "other" &&
                (!update.find("name") || member(update, "name") == member(update, "title")) &&
                std::find(tools.begin(), tools.end(), member(update, "title")) != tools.end())
            {
                auto announced = member(state, "announced");
                if (announced.as_array().size() >= 256) {
                    return failure("acp_task_tool_call_bound_exceeded");
                }
                announced.push_back(object({{"toolCallId", member(update, "toolCallId")}, {"title", member(update, "title")}}));
                state.set("announced", std::move(announced));
            }
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
            state.set("session_id", Json(session));
            state.set("mcp_servers", Json::array());
            state.set("attestations", Json::array());
            const bool select_model = model_selection && model_selection->model;
            const bool select_effort = model_selection && model_selection->reasoning_effort;
            if (select_model || select_effort) {
                const auto previous = configuration_value(member(response_result, "configOptions"), select_model ? "model" : "effort");
                if (!previous) {
                    return failure(select_model ? "model_selection_unavailable" : "reasoning_effort_unavailable");
                }
                if (!select_model) {
                    // Setting effort alone must retain the native session's default model.
                    model_selection->model = configuration_value(member(response_result, "configOptions"), "model");
                    const addon::Delegated_model_selection_declaration selection_support{true, true};
                    addon::Contract_failure contract_failure;
                    if (!model_selection->model ||
                        !addon::validate_delegated_model_selection(*model_selection, &selection_support, &contract_failure))
                    {
                        return failure("reasoning_effort_unavailable");
                    }
                    state.set("model_selection", addon::encode_delegated_model_selection(*model_selection));
                }
                next_input = configure_input(state, select_model,
                    select_model ? *model_selection->model : *model_selection->reasoning_effort, previous);
            }
            else {
                next_input = prompt_input(state, *prompt_bytes);
            }
        }
        else
        if (stage == "await_model" || stage == "await_effort") {
            const bool model = stage == "await_model";
            const auto* previous = state.find("configuration_before");
            const bool retry = previous && previous->is_null();
            if (!model_selection || (model && !model_selection->model) ||
                (!model && !model_selection->reasoning_effort) ||
                !next_input.empty() || !response(value, (model ? 5 : 6) + (retry ? 2 : 0), &response_result) ||
                !only_keys(response_result, {"_meta", "configOptions"}) || !valid_meta(response_result))
            {
                return failure(model ? "model_selection_unavailable" : "reasoning_effort_unavailable");
            }
            const auto& options = member(response_result, "configOptions");
            if ((model && configuration_value(options, "model") != model_selection->model) ||
                (!model && !selection_confirmed(options, *model_selection)))
            {
                // OpenCode 2 can briefly echo the previous selection while applying its switch.
                // Repeat that same setting once and require a fresh, correlated confirmation.
                const auto current = configuration_value(options, model ? "model" : "effort");
                if (!text(member(state, "provider_version")).starts_with("2.") ||
                    !previous || !previous->is_string() || current != previous->as_string() ||
                    (!model && configuration_value(options, "model") != model_selection->model))
                {
                    return failure(model ? "model_selection_unavailable" : "reasoning_effort_unavailable");
                }
                next_input = configure_input(state, model,
                    model ? *model_selection->model : *model_selection->reasoning_effort, std::nullopt);
            }
            else
            if (model && model_selection->reasoning_effort) {
                const auto effort = configuration_value(options, "effort");
                if (!effort) {
                    return failure("reasoning_effort_unavailable");
                }
                next_input = configure_input(state, false, *model_selection->reasoning_effort, effort);
            }
            else {
                next_input = prompt_input(state, *prompt_bytes);
            }
        }
        else
        if (stage == "await_prompt") {
            if (next_input_contains_request || next_input_has_permission_reply ||
                !response(value, 3, &response_result) ||
                !prompt_response(response_result) || result.empty())
            {
                return failure("acp_invalid_prompt_completion");
            }
            state.set("stage", Json("await_close"));
            const auto close_request = json_line(object({
                {"jsonrpc", Json("2.0")}, {"id", Json(4)}, {"method", Json("session/close")},
                {"params", object({{"sessionId", member(state, "session_id")}})},
            }));
            if (close_request.size() > addon::limits::k_max_structured_stream_bytes - next_input.size()) {
                return failure("acp_permission_reply_bound_exceeded");
            }
            // Native writes are fire-and-forget: their refusals may share a read
            // with end_turn. Deliver those errors before the new close request.
            next_input += close_request;
        }
        else
        if (stage == "await_close") {
            if (!next_input.empty() || !response(value, 4, &response_result) ||
                !only_keys(response_result, {"_meta"}) || !valid_meta(response_result) || !remaining.empty())
            {
                return failure("acp_invalid_close");
            }
            state.set("stage", Json("await_exit"));
            break;
        }
        else {
            return failure("acp_out_of_order_output");
        }
        next_input_contains_request = true;
    }
    state.set("pending_base64", Json(addon::encode_base64(remaining)));
    state.set("result_base64", Json(addon::encode_base64(result)));
    state.set("result_message_id", Json(message_id));
    const bool defer_input = !remaining.empty() && !next_input.empty();
    state.set("deferred_input_base64", Json(defer_input ? addon::encode_base64(next_input) : ""));
    state.set("deferred_input_is_request", Json(defer_input && next_input_contains_request));
    state.set("deferred_input_has_permission_reply", Json(defer_input && next_input_has_permission_reply));
    if (text(member(state, "stage")) == "await_exit") {
        if (inspection.final) {
            addon::Structured_inspect_result complete;
            complete.complete     = true;
            complete.result_bytes = std::move(result);
            return complete;
        }
        auto waiting = partial(state, {});
        waiting.close_stdin = true;
        return waiting;
    }
    if (inspection.final) {
        return failure("acp_exit_before_close");
    }
    if (!next_input.empty() && !defer_input) {
        return partial(state, next_input);
    }
    return partial(state, {});
}

} // namespace opencode
