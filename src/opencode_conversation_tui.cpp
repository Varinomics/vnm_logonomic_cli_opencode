#include "opencode_conversation.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace opencode {
namespace {

namespace json = addon::json;
using Json = addon::Json_value;
using Message = addon::Conversation_message;
using Kind = addon::Conversation_block_kind;
using Phase = addon::Conversation_message_phase;

bool text(Message& message, const Json& value, std::string_view label = {}, bool compact = true,
    Kind kind = Kind::TEXT)
{
    if (!value.is_string()) {
        return false;
    }
    message.content.push_back({kind, value.as_string(), std::string(label), compact});
    return true;
}

void marker(Message& message, std::string value, bool compact = false)
{
    message.content.push_back({Kind::TEXT, std::move(value), {}, compact});
}

bool optional_text(Message& message, const Json& value, std::string_view name,
    std::string_view label, bool compact = true)
{
    const auto* field = value.find(name);
    return !field || text(message, *field, label, compact);
}

bool data_uri(std::string_view uri)
{
    return uri.size() >= 5 && (uri[0] == 'd' || uri[0] == 'D') &&
        (uri[1] == 'a' || uri[1] == 'A') && (uri[2] == 't' || uri[2] == 'T') &&
        (uri[3] == 'a' || uri[3] == 'A') && uri[4] == ':';
}

bool attachment(Message& message, const Json& file, bool prompt)
{
    const auto& mime = json::member(file, "mime");
    const auto* name = file.find("name");
    if (!mime.is_string() || (name && !name->is_string())) {
        return false;
    }
    std::string_view reference;
    if (prompt) {
        const auto& source = json::member(file, "source");
        const auto type = json::text(source, "type");
        if (!json::member(file, "data").is_string()) {
            return false;
        }
        if (type == "inline") {
            reference = "[Embedded attachment; binary data not copied]";
        }
        else
        if (type == "uri" && json::member(source, "uri").is_string()) {
            reference = json::member(source, "uri").as_string();
        }
        else {
            return false;
        }
    }
    else {
        if (!json::member(file, "uri").is_string()) {
            return false;
        }
        reference = json::member(file, "uri").as_string();
    }
    if (data_uri(reference)) {
        reference = "[Embedded attachment; binary data not copied]";
    }
    std::string description = mime.as_string();
    if (name) {
        description += "\n" + name->as_string();
    }
    description += '\n';
    description += reference;
    message.content.push_back({Kind::ATTACHMENT, std::move(description), {}, prompt});
    return !prompt || optional_text(message, file, "description", "Attachment description");
}

bool error(Message& message, const Json& value, std::string_view label, bool compact)
{
    const auto& type = json::member(value, "type");
    const auto& body = json::member(value, "message");
    if (!type.is_string() || !body.is_string()) {
        return false;
    }
    std::string detail = type.as_string() + ": " + body.as_string();
    if (const auto* status = value.find("status")) {
        if (!status->is_integer()) {
            return false;
        }
        detail += "\nHTTP status: " + addon::serialize_json(*status);
    }
    if (const auto* response = value.find("response")) {
        if (!json::member(*response, "body").is_string()) {
            return false;
        }
        detail += "\nResponse:\n" + json::text(*response, "body");
    }
    if (const auto* retry = value.find("retryAfterMs")) {
        if (!retry->is_integer() || retry->as_integer() < 0) {
            return false;
        }
        detail += "\nRetry after (ms): " + addon::serialize_json(*retry);
    }
    message.content.push_back({Kind::TEXT, std::move(detail), std::string(label), compact});
    return true;
}

bool user(Message& message, const Json& row)
{
    message.role = "user";
    if (!text(message, json::member(row, "text"))) {
        return false;
    }
    if (const auto* files = row.find("files")) {
        if (!files->is_array()) {
            return false;
        }
        for (const auto& file : files->as_array()) {
            if (!attachment(message, file, true)) {
                return false;
            }
        }
    }
    for (const auto* field : {"agents", "skills"}) {
        const auto* values = row.find(field);
        if (!values) {
            continue;
        }
        if (!values->is_array()) {
            return false;
        }
        const bool skill = std::string_view(field) == "skills";
        for (const auto& value : values->as_array()) {
            if (!text(message, json::member(value, "name"), skill ? "Skill" : "Agent mention") ||
                (skill && !optional_text(message, value, "text", "Skill content")))
            {
                return false;
            }
        }
    }
    return true;
}

bool tool_content(Message& message, const Json& values, const std::string& label)
{
    if (!values.is_array() || values.as_array().empty()) {
        return false;
    }
    for (const auto& value : values.as_array()) {
        const auto type = json::text(value, "type");
        if (type == "text") {
            if (!text(message, json::member(value, "text"), label, false, Kind::TOOL_RESULT)) {
                return false;
            }
        }
        else
        if (type == "file") {
            if (!attachment(message, value, false)) {
                return false;
            }
        }
        else {
            return false;
        }
    }
    return true;
}

bool tool(Message& message, const Json& value)
{
    const auto& name = json::member(value, "name");
    const auto& id   = json::member(value, "id");
    if (!name.is_string() || !id.is_string()) {
        return false;
    }
    const std::string label = addon::serialize_json(name) + " [" + addon::serialize_json(id) + "]";
    const auto& state = json::member(value, "state");
    const auto& input = json::member(state, "input");
    const auto status = json::text(state, "status");
    if (status == "streaming") {
        if (!text(message, input, label, false, Kind::TOOL_CALL)) {
            return false;
        }
        marker(message, "[Tool call is still streaming.]");
        return true;
    }
    if (!input.is_object()) {
        return false;
    }
    message.content.push_back({Kind::TOOL_CALL, addon::serialize_json(input), label, false});
    if (status == "running") {
        marker(message, "[Tool is still running.]");
        return true;
    }
    if (status == "completed") {
        return tool_content(message, json::member(state, "content"), label);
    }
    if (status == "error") {
        if (!error(message, json::member(state, "error"), "Tool error", false)) {
            return false;
        }
        const auto* content = state.find("content");
        return !content || tool_content(message, *content, label);
    }
    return false;
}

bool assistant(Message& message, const Json& row)
{
    message.role = "assistant";
    const auto& content = json::member(row, "content");
    if (!content.is_array()) {
        return false;
    }
    const auto* finish_value = row.find("finish");
    const auto& time = json::member(row, "time");
    const auto* completed = time.find("completed");
    if ((finish_value && !finish_value->is_string()) ||
        !json::member(time, "created").is_number() || (completed && !completed->is_number()))
    {
        return false;
    }
    const auto finish = finish_value ? finish_value->as_string() : std::string{};
    if (finish == "stop") {
        message.phase = Phase::FINAL;
    }
    else
    if (finish == "tool-calls") {
        message.phase = Phase::COMMENTARY;
    }
    for (const auto& value : content.as_array()) {
        const auto type = json::text(value, "type");
        if (type == "text" || type == "reasoning") {
            const bool reasoning = type == "reasoning";
            if (!text(message, json::member(value, "text"), reasoning ? "Reasoning" : "",
                    !reasoning && message.phase != Phase::COMMENTARY))
            {
                return false;
            }
        }
        else
        if (type == "tool") {
            if (!tool(message, value)) {
                return false;
            }
        }
        else {
            return false;
        }
    }
    if (!completed) {
        marker(message, "[Assistant response is incomplete.]");
    }
    if (!finish.empty() && finish != "stop" && finish != "tool-calls") {
        marker(message, "[Assistant finish reason: " + finish + ".]", true);
    }
    if (!optional_text(message, row, "rawFinish", "Native finish reason")) {
        return false;
    }
    if (const auto* failure = row.find("error")) {
        if (!error(message, *failure, "Assistant error", true)) {
            return false;
        }
    }
    if (const auto* retry = row.find("retry")) {
        const auto& attempt = json::member(*retry, "attempt");
        if (!attempt.is_integer() || attempt.as_integer() < 1 ||
            !error(message, json::member(*retry, "error"), "Retry error", false))
        {
            return false;
        }
        marker(message, "[Assistant retry attempt " + addon::serialize_json(attempt) + ".]");
    }
    if (message.content.empty()) {
        marker(message, "[Assistant response contains no text.]");
    }
    return true;
}

bool shell(Message& message, const Json& row)
{
    message.role = "shell";
    if (!text(message, json::member(row, "command"), "Shell command", false, Kind::TOOL_CALL)) {
        return false;
    }
    const auto status = json::text(row, "status");
    if (status != "running" && status != "exited" && status != "timeout" && status != "killed") {
        return false;
    }
    if (const auto* output = row.find("output")) {
        const auto& cursor    = json::member(*output, "cursor");
        const auto& size      = json::member(*output, "size");
        const auto& truncated = json::member(*output, "truncated");
        if (!cursor.is_integer() || cursor.as_integer() < 0 ||
            !size.is_integer() || size.as_integer() < 0 || !truncated.is_boolean() ||
            !text(message, json::member(*output, "output"), "Shell output", false, Kind::TOOL_RESULT))
        {
            return false;
        }
        if (truncated.as_boolean() || cursor.as_integer() < size.as_integer()) {
            marker(message, "[Native shell output is partial or truncated.]");
        }
    }
    if (const auto* exit = row.find("exit")) {
        if (!exit->is_number()) {
            return false;
        }
        marker(message, "[Shell exit status: " + addon::serialize_json(*exit) + ".]");
    }
    marker(message, status == "running" ? "[Shell is still running.]" : "[Shell status: " + status + ".]");
    return true;
}

bool compaction(Message& message, const Json& row)
{
    message.role = "compaction";
    if (!text(message, json::member(row, "reason"), "Compaction reason", false)) {
        return false;
    }
    const auto status = json::text(row, "status");
    if (status == "running" || status == "completed") {
        marker(message, status == "running" ? "[Compaction is incomplete.]" : "[Compaction completed.]");
        return text(message, json::member(row, "summary"), "Compaction summary", false);
    }
    if (status == "failed") {
        marker(message, "[Compaction failed.]");
        return error(message, json::member(row, "error"), "Compaction error", false);
    }
    return false;
}

bool model_reference(Message& message, const Json& model, std::string_view label)
{
    const auto& id       = json::member(model, "id");
    const auto& provider = json::member(model, "providerID");
    if (!id.is_string() || !provider.is_string()) {
        return false;
    }
    message.content.push_back({Kind::TEXT, provider.as_string() + "/" + id.as_string(), std::string(label), false});
    return optional_text(message, model, "variant", "Model variant", false);
}

bool location_reference(Message& message, const Json& location, std::string_view label)
{
    return text(message, json::member(location, "directory"), label, false) &&
        optional_text(message, location, "workspaceID", "Workspace", false);
}

bool system_message(Message& message, const Json& row, std::string_view type)
{
    message.role = std::string(type);
    if (type == "system" || type == "synthetic" || type == "skill") {
        return text(message, json::member(row, "text"), {}, false) &&
            optional_text(message, row, type == "skill" ? "name" : "description",
                type == "skill" ? "Skill" : "Description", false);
    }
    if (type == "agent-switched") {
        return text(message, json::member(row, "agent"), "Agent", false) &&
            optional_text(message, row, "previous", "Previous agent", false);
    }
    if (type == "model-switched") {
        if (!model_reference(message, json::member(row, "model"), "Model")) {
            return false;
        }
        const auto* previous = row.find("previous");
        return !previous || model_reference(message, *previous, "Previous model");
    }
    if (type == "location-switched") {
        if (!location_reference(message, json::member(row, "location"), "Directory")) {
            return false;
        }
        const auto* previous = row.find("previous");
        return !previous || location_reference(message, *previous, "Previous directory");
    }
    if (type == "idle") {
        const auto outcome = json::text(row, "outcome");
        if (outcome != "succeeded" && outcome != "failed" && outcome != "interrupted") {
            return false;
        }
        marker(message, "[Session idle: " + outcome + ".]");
        const auto* failure = row.find("error");
        return !failure || error(message, *failure, "Session error", false);
    }
    return false;
}

} // namespace

bool append_tui_message(
    const Json&                 row,
    addon::Conversation_writer& writer,
    addon::Contract_failure*    failure)
{
    const auto type = json::text(row, "type");
    Message message;
    const bool rendered = type == "user" ? user(message, row)
        : type == "assistant" ? assistant(message, row)
        : type == "shell" ? shell(message, row)
        : type == "compaction" ? compaction(message, row)
        : system_message(message, row, type);
    if (!rendered) {
        *failure = {"conversation_content_unavailable", "An OpenCode message contains unsupported content."};
        return false;
    }
    return writer.append(message, failure);
}

} // namespace opencode
