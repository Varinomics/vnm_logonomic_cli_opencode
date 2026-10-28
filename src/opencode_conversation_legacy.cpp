#include "opencode_conversation.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opencode {
namespace {

namespace json = addon::json;
using Json = addon::Json_value;
using Block = addon::Conversation_block;
using Kind = addon::Conversation_block_kind;

bool unavailable(addon::Contract_failure* failure, const char* detail)
{
    *failure = {"conversation_content_unavailable", detail};
    return false;
}

bool text_member(const Json& value, std::string_view key)
{
    return json::member(value, key).is_string();
}

bool optional_text_member(const Json& value, std::string_view key)
{
    const auto* member = value.find(key);
    return !member || member->is_string();
}

bool optional_flag(const Json& value, std::string_view key)
{
    const auto* member = value.find(key);
    return !member || member->is_boolean();
}

bool flag(const Json& value, std::string_view key)
{
    const auto& member = json::member(value, key);
    return member.is_boolean() && member.as_boolean();
}

void marker(std::vector<Block>& blocks, std::string text, std::string label = {}, bool compact = false)
{
    blocks.push_back({Kind::TEXT, std::move(text), std::move(label), compact});
}

bool data_url(std::string_view url)
{
    constexpr std::string_view prefix = "data:";
    return url.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), url.begin(),
        [](unsigned char left, unsigned char right) { return left == std::tolower(right); });
}

bool append_attachment(const Json& part, std::vector<Block>& blocks,
    bool compact, addon::Contract_failure* failure)
{
    if (json::text(part, "type") != "file" || !text_member(part, "mime") || !text_member(part, "url") ||
        !optional_text_member(part, "filename"))
    {
        return unavailable(failure, "An OpenCode attachment has an unsupported reference.");
    }
    const auto mime = json::text(part, "mime");
    const auto url  = json::text(part, "url");
    std::string reference = "Media type: " + mime + "\n";
    reference += url.empty() ? "[Attachment URL is empty]"
        : data_url(url) ? "Embedded " + mime + " attachment" : url;
    if (const auto filename = json::text(part, "filename"); !filename.empty()) {
        reference = filename + "\n" + reference;
    }
    if (const auto* source = part.find("source")) {
        const auto type = json::text(*source, "type");
        const auto key  = type == "resource" ? "uri" : "path";
        if ((type != "file" && type != "symbol" && type != "resource") || !text_member(*source, key)) {
            return unavailable(failure, "An OpenCode attachment has an unsupported source reference.");
        }
        const auto source_reference = json::text(*source, key);
        if (!source_reference.empty()) {
            reference += "\nSource: " +
                (data_url(source_reference) ? "Embedded attachment" : source_reference);
        }
        if (type == "symbol") {
            if (!text_member(*source, "name")) {
                return unavailable(failure, "An OpenCode attachment has an unsupported symbol reference.");
            }
            reference += "\nSymbol: " + json::text(*source, "name");
        }
    }
    blocks.push_back({Kind::ATTACHMENT, std::move(reference), {}, compact});
    return true;
}

bool append_error(const Json& error, std::vector<Block>& blocks,
    std::string label, bool compact, addon::Contract_failure* failure)
{
    const auto& data = json::member(error, "data");
    if (!text_member(error, "name") || !data.is_object() || !optional_text_member(data, "message")) {
        return unavailable(failure, "An OpenCode error has an unsupported format.");
    }
    std::string text = json::text(error, "name");
    if (const auto message = json::text(data, "message"); !message.empty()) {
        text += ": " + message;
    }
    marker(blocks, std::move(text), std::move(label), compact);
    return true;
}

bool append_user_summary(const Json& summary, std::vector<Block>& blocks, addon::Contract_failure* failure)
{
    const auto& diffs = json::member(summary, "diffs");
    if (!summary.is_object() || !optional_text_member(summary, "title") ||
        !optional_text_member(summary, "body") || !diffs.is_array())
    {
        return unavailable(failure, "An OpenCode user message has an unsupported summary.");
    }
    for (const auto& diff : diffs.as_array()) {
        const auto status = json::text(diff, "status");
        if (!diff.is_object() || !optional_text_member(diff, "file") || !optional_text_member(diff, "patch") ||
            !json::member(diff, "additions").is_number() || !json::member(diff, "deletions").is_number() ||
            (diff.find("status") && status != "added" && status != "deleted" && status != "modified"))
        {
            return unavailable(failure, "An OpenCode user summary has an unsupported change record.");
        }
    }
    // This is retained user-message change metadata, not an assistant's
    // compaction flag or replacement text for the user's original parts.
    marker(blocks, addon::serialize_json(summary), "User summary");
    return true;
}

bool append_tool(const Json& part, std::vector<Block>& blocks, addon::Contract_failure* failure)
{
    const auto& state = json::member(part, "state");
    const auto& input = json::member(state, "input");
    if (!text_member(part, "tool") || !text_member(part, "callID") ||
        !state.is_object() || !input.is_object() || !text_member(state, "status") ||
        !optional_text_member(state, "title"))
    {
        return unavailable(failure, "An OpenCode tool part has an unsupported call.");
    }
    const auto status = json::text(state, "status");
    const auto label  = json::text(part, "tool") + " " + json::text(part, "callID");
    if (status == "pending") {
        if (!text_member(state, "raw")) {
            return unavailable(failure, "An OpenCode pending tool call has no retained arguments.");
        }
        const auto arguments = json::object({{"input", input}, {"raw", json::member(state, "raw")}});
        blocks.push_back({Kind::TOOL_CALL, addon::serialize_json(arguments), label, false});
        marker(blocks, "[Tool call pending]", label);
        return true;
    }
    blocks.push_back({Kind::TOOL_CALL, addon::serialize_json(input), label, false});
    if (const auto title = json::text(state, "title"); !title.empty()) {
        marker(blocks, title, "Tool title");
    }
    if (status == "running") {
        marker(blocks, "[Tool call running]", label);
        return true;
    }
    if (status == "error") {
        if (!text_member(state, "error")) {
            return unavailable(failure, "An OpenCode failed tool call has no retained error.");
        }
        blocks.push_back({Kind::TOOL_RESULT, "[Tool error]\n" + json::text(state, "error"), label, false});
        return true;
    }
    if (status != "completed" || !text_member(state, "output")) {
        return unavailable(failure, "An OpenCode tool part has an unsupported result.");
    }
    blocks.push_back({Kind::TOOL_RESULT, json::text(state, "output"), label, false});
    if (const auto* attachments = state.find("attachments")) {
        if (!attachments->is_array()) {
            return unavailable(failure, "An OpenCode tool result has unsupported attachments.");
        }
        for (const auto& attachment : attachments->as_array()) {
            if (!append_attachment(attachment, blocks, false, failure)) {
                return false;
            }
        }
    }
    return true;
}

bool append_part(const Json& part, addon::Conversation_message& message,
    bool compact_text, addon::Contract_failure* failure)
{
    const auto type = json::text(part, "type");
    auto& blocks = message.content;
    if (type == "text" || type == "reasoning") {
        if (!text_member(part, "text") || !optional_flag(part, "synthetic") || !optional_flag(part, "ignored")) {
            return unavailable(failure, "An OpenCode text part has an unsupported format.");
        }
        const bool reasoning = type == "reasoning";
        blocks.push_back({Kind::TEXT, json::text(part, "text"), reasoning ? "Reasoning" : "",
            !reasoning && compact_text && !flag(part, "synthetic") && !flag(part, "ignored")});
        return true;
    }
    if (type == "tool") {
        return append_tool(part, blocks, failure);
    }
    if (type == "file") {
        return append_attachment(part, blocks, compact_text, failure);
    }
    if (type == "agent") {
        if (!text_member(part, "name")) {
            return unavailable(failure, "An OpenCode agent mention has no retained name.");
        }
        std::string mention = json::text(part, "name");
        if (const auto* source = part.find("source")) {
            if (!text_member(*source, "value")) {
                return unavailable(failure, "An OpenCode agent mention has an unsupported source.");
            }
            mention += "\n" + json::text(*source, "value");
        }
        marker(blocks, std::move(mention), "Agent", compact_text);
        return true;
    }
    if (type == "subtask") {
        if (!text_member(part, "prompt") || !text_member(part, "description") ||
            !text_member(part, "agent") || !optional_text_member(part, "command"))
        {
            return unavailable(failure, "An OpenCode subtask has an unsupported format.");
        }
        auto arguments = json::object({
            {"agent", json::member(part, "agent")},
            {"description", json::member(part, "description")},
            {"prompt", json::member(part, "prompt")},
        });
        if (const auto* command = part.find("command")) {
            arguments.set("command", *command);
        }
        if (const auto* model = part.find("model")) {
            if (!model->is_object() || !text_member(*model, "providerID") || !text_member(*model, "modelID")) {
                return unavailable(failure, "An OpenCode subtask has an unsupported model selection.");
            }
            arguments.set("model", *model);
        }
        blocks.push_back({Kind::TOOL_CALL, addon::serialize_json(arguments), "Subtask", false});
        return true;
    }
    if (type == "retry") {
        const auto& attempt = json::member(part, "attempt");
        if (!attempt.is_integer() || attempt.as_integer() < 0) {
            return unavailable(failure, "An OpenCode retry has an unsupported attempt.");
        }
        marker(blocks, "[Retry attempt " + std::to_string(attempt.as_integer()) + "]", "Retry");
        return append_error(json::member(part, "error"), blocks, "Retry error", false, failure);
    }
    if (type == "compaction") {
        if (!json::member(part, "auto").is_boolean()) {
            return unavailable(failure, "An OpenCode compaction part has an unsupported format.");
        }
        marker(blocks, flag(part, "auto")
            ? "[Automatic context compaction requested]" : "[Context compaction requested]", "Compaction");
        return true;
    }
    if (type == "step-start") {
        marker(blocks, "[Step started]", "Step");
        return true;
    }
    if (type == "step-finish") {
        if (!text_member(part, "reason")) {
            return unavailable(failure, "An OpenCode step has no retained finish reason.");
        }
        marker(blocks, "[Step finished: " + json::text(part, "reason") + "]", "Step");
        return true;
    }
    if (type == "snapshot") {
        if (!text_member(part, "snapshot")) {
            return unavailable(failure, "An OpenCode snapshot part has no retained reference.");
        }
        marker(blocks, json::text(part, "snapshot"), "Snapshot");
        return true;
    }
    if (type == "patch") {
        const auto& files = json::member(part, "files");
        if (!text_member(part, "hash") || !files.is_array() ||
            !std::all_of(files.as_array().begin(), files.as_array().end(),
                [](const auto& file) { return file.is_string(); }))
        {
            return unavailable(failure, "An OpenCode patch part has an unsupported reference.");
        }
        const auto reference = json::object({{"hash", json::member(part, "hash")}, {"files", files}});
        marker(blocks, addon::serialize_json(reference), "Patch");
        return true;
    }
    return unavailable(failure, "An OpenCode conversation part has an unsupported format.");
}

} // namespace

bool append_legacy_message(const Json& info, const std::vector<Json>& parts,
    addon::Conversation_writer& writer, addon::Contract_failure* failure)
{
    const auto role = json::text(info, "role");
    if (role != "user" && role != "assistant") {
        return unavailable(failure, "An OpenCode conversation message has an unsupported role.");
    }
    if (role == "assistant" && !optional_flag(info, "summary")) {
        return unavailable(failure, "An OpenCode assistant message has an unsupported summary.");
    }
    addon::Conversation_message message;
    message.role = role;
    const bool summary = role == "assistant" && flag(info, "summary");
    for (const auto& part : parts) {
        if (!append_part(part, message, !summary, failure)) {
            return false;
        }
    }
    if (role == "user") {
        if (const auto* changes = info.find("summary"); changes &&
            !append_user_summary(*changes, message.content, failure))
        {
            return false;
        }
    }
    if (summary) {
        for (auto& block : message.content) {
            if (block.kind == Kind::TEXT && block.label.empty()) {
                block.label = "Compacted conversation summary";
            }
        }
    }
    if (const auto* error = info.find("error")) {
        if (!append_error(*error, message.content, "Assistant error", true, failure)) {
            return false;
        }
    }
    if (const auto* structured = info.find("structured")) {
        marker(message.content, addon::serialize_json(*structured), "Structured output", !summary);
    }
    if (role == "assistant") {
        const auto& time = json::member(info, "time");
        const auto* completed = time.find("completed");
        if (!time.is_object() || (completed && !completed->is_number())) {
            return unavailable(failure, "An OpenCode assistant message has an unsupported completion state.");
        }
        if (!completed) {
            marker(message.content, "[Assistant response incomplete]", {}, true);
        }
    }
    if (message.content.empty()) {
        marker(message.content, "[Empty message]");
    }
    return writer.append(message, failure);
}

} // namespace opencode
