#include "opencode_acp.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = addon::Json_value;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Json parse(const std::string& bytes)
{
    auto result = addon::parse_json(bytes, {}, nullptr);
    require(result.has_value(), "fixture JSON must parse");
    return std::move(*result);
}

const std::string k_initialize =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":1,"
    "\"agentInfo\":{\"name\":\"OpenCode\",\"version\":\"1.2.3\"}}}\n";
const std::string k_ready =
    "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"ses_root\","
    "\"update\":{\"sessionUpdate\":\"available_commands_update\",\"availableCommands\":[]}}}\n";
const std::string k_new =
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"sessionId\":\"ses_root\"}}\n";
const std::string k_end_turn =
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}\n";
const std::string k_closed =
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{}}\n";
const std::string k_message =
    "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"ses_root\","
    "\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"messageId\":\"msg_result\","
    "\"content\":{\"type\":\"text\",\"text\":\"Finished\"}}}}\n";
// OpenCode 1.18.32 offers once, always and reject; the package answers a tool
// the launcher's posture did not approve with the request's own reject option.
const std::string k_permission =
    "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"session/request_permission\",\"params\":{"
    "\"sessionId\":\"ses_root\",\"toolCall\":{\"toolCallId\":\"call_read\"},\"options\":["
    "{\"kind\":\"allow_once\",\"name\":\"Allow once\",\"optionId\":\"once\"},"
    "{\"kind\":\"allow_always\",\"name\":\"Always allow\",\"optionId\":\"always\"},"
    "{\"kind\":\"reject_once\",\"name\":\"Reject\",\"optionId\":\"reject\"}]}}\n";
// The task's own Task tool, as the fixture's server names it.
const std::string k_task_tool = "logonomic-task-t-task_control_status";

std::string permission_request(const Json& id)
{
    auto request = parse(k_permission);
    request.set("id", id);
    return addon::serialize_json(request) + '\n';
}

void require_permission_reply(const std::string& bytes, const Json& id, const char* option = "reject")
{
    const auto reply = parse(bytes);
    const auto* reply_id = reply.find("id");
    require(reply_id && addon::serialize_json(*reply_id) == addon::serialize_json(id),
        "permission reply must echo its request ID");
    const auto* result = reply.find("result");
    const auto* outcome = result ? result->find("outcome") : nullptr;
    require(outcome && addon::serialize_json(*outcome) ==
            addon::serialize_json(parse(std::string("{\"outcome\":\"selected\",\"optionId\":\"") + option + "\"}")),
        "permission reply must select the request's own option the posture calls for");
}

class Session
{
public:
    explicit Session(const fs::path& attestation, bool auto_approve = false)
    {
        const auto path = attestation.u8string();
        Json entry = parse("{\"server_id\":\"task_control\",\"nonce\":\"0123456789abcdef0123456789abcdef\"}");
        entry.set("path", Json(std::string(path.begin(), path.end())));
        m_state = opencode::make_acp_inspection(
            "Implement the requested change", ".", "1.2.3", Json::array(),
            Json(Json::Array{entry}), Json(Json::Array{Json(k_task_tool)}), auto_approve).protocol_state;
    }

    void omit_auto_mode()
    {
        auto state = parse(m_state);
        state.erase("auto_approve");
        m_state = addon::serialize_json(state);
    }

    void omit_permission_reply_marker()
    {
        auto state = parse(m_state);
        state.erase("deferred_input_has_permission_reply");
        m_state = addon::serialize_json(state);
    }

    addon::Structured_inspect_result feed(const std::string& bytes, bool final = false,
                                         bool timed_out = false, int exit_code = 0)
    {
        addon::Structured_inspection input;
        input.protocol_state = m_state;
        input.stdout_bytes   = bytes;
        input.final          = final;
        input.timed_out      = timed_out;
        input.exit_code      = exit_code;
        auto result = opencode::inspect_acp(input);
        if (!result.complete) {
            m_state = result.protocol_state;
        }
        return result;
    }

    void reach_prompt()
    {
        const auto initialized = feed(k_initialize);
        require(!initialized.complete && initialized.stdin_bytes.has_value(), "initialize must request session/new");
        const auto ready = feed(k_ready + k_new);
        require(!ready.complete && ready.stdin_bytes.has_value(), "attested root must request prompt");
        require(ready.stdin_bytes->find("session/prompt") != std::string::npos, "prompt request method");
    }

private:
    std::string m_state;
};

std::string foreign_session(std::string record)
{
    record.replace(record.find("ses_root"), 8, "ses_foreign");
    return record;
}

void run(const fs::path& attestation)
{
    // Native OpenCode 1.18.33 asks for an ACP write after edit approval despite
    // empty client capabilities. A JSON-RPC refusal lets its own edit complete.
    // The client must neither claim a successful write nor gain filesystem access.
    auto write_request = parse(R"({"jsonrpc":"2.0","id":18,"method":"fs/write_text_file",)"
        R"("params":{"sessionId":"ses_root","path":"fixture","content":"proposed edit"}})");
    const auto proposed_file = attestation.parent_path() / "proposed.txt";
    write_request.find("params")->set("path", Json(proposed_file.generic_string()));
    const auto write_bytes = addon::serialize_json(write_request) + '\n';
    const auto unsupported = parse(
        R"({"jsonrpc":"2.0","id":18,"error":{"code":-32601,"message":"Method not found"}})");
    {
        const auto initialize = parse(opencode::acp_initial_input());
        require(initialize.find("params")->find("clientCapabilities")->as_object().empty(),
            "declining the unsolicited write must not advertise filesystem capability");
        Session automatic(attestation, true);
        automatic.reach_prompt();
        auto permission = parse(k_permission);
        permission.find("params")->find("toolCall")->set("kind", Json("edit"));
        const auto approved = automatic.feed(addon::serialize_json(permission) + '\n');
        require(approved.stdin_bytes.has_value(), "native edit ask must receive a reply");
        require_permission_reply(*approved.stdin_bytes, Json(17), "once");
        const auto refused = automatic.feed(write_bytes);
        require(!refused.complete && refused.stdin_bytes && parse(*refused.stdin_bytes) == unsupported,
            "the approved edit's client write must receive a truthful unsupported-method error");
        require(!fs::exists(proposed_file), "the inspector must not perform the proposed write");
        automatic.feed(k_message);
        const auto ended = automatic.feed(k_end_turn);
        require(!ended.complete && ended.stdin_bytes, "native edit completion must request close");
        const auto closed = automatic.feed(k_closed);
        require(closed.complete && closed.result_bytes == "Finished",
            "refusing the client write must preserve native prompt completion");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        std::string emitted;
        for (const char byte : write_bytes) {
            const auto result = session.feed(std::string(1, byte));
            require(!result.complete, "a fragmented client write must remain a protocol refusal");
            if (result.stdin_bytes) emitted += *result.stdin_bytes;
        }
        require(parse(emitted) == unsupported, "fragmented writes must retain one correlated refusal");
        require(session.feed(permission_request(Json(18.0))).failure_reason == "acp_unsupported_reverse_request",
            "filesystem and permission requests share duplicate-ID protection");
    }
    {
        Session automatic(attestation, true);
        automatic.reach_prompt();
        const auto approved = automatic.feed(k_permission);
        require(approved.stdin_bytes.has_value(), "edit approval must be delivered before its continuation");
        require_permission_reply(*approved.stdin_bytes, Json(17), "once");
        const auto completed = automatic.feed(write_bytes + k_message + k_end_turn);
        require(!completed.complete && completed.stdin_bytes && !completed.failure_reason,
            "a native fire-and-forget write and end_turn in one read must retain refusal and request close");
        const auto refusal_end = completed.stdin_bytes->find('\n');
        require(parse(completed.stdin_bytes->substr(0, refusal_end)) == unsupported &&
                parse(completed.stdin_bytes->substr(refusal_end + 1)).find("method")->as_string() == "session/close",
            "the combined continuation must send the write refusal before the close request");
        require(automatic.feed(k_closed).result_bytes == "Finished",
            "the coalesced native edit must close and deliver its result");
        const auto transcript = write_bytes + k_message + k_end_turn;
        for (std::size_t split = 1; split < transcript.size(); ++split) {
            Session fragmented(attestation, true);
            fragmented.reach_prompt();
            fragmented.feed(k_permission);
            const auto first  = fragmented.feed(transcript.substr(0, split));
            const auto second = fragmented.feed(transcript.substr(split));
            require(!first.complete && !second.complete &&
                    first.stdin_bytes.value_or("") + second.stdin_bytes.value_or("") == *completed.stdin_bytes &&
                    second.protocol_state == completed.protocol_state,
                "every split of write, message and end_turn must preserve replies and continuation");
            require(fragmented.feed(k_closed).result_bytes == "Finished",
                "a split native edit must close and deliver the same result");
        }
    }
    for (const auto& requests : {write_bytes + k_permission, k_permission + write_bytes}) {
        for (const bool legacy : {false, true}) {
            Session session(attestation, true);
            session.reach_prompt();
            const auto partial = session.feed(requests + k_message + k_end_turn.substr(0, 15));
            require(!partial.complete && !partial.stdin_bytes,
                "partial end_turn must withhold queued permission and filesystem replies");
            if (legacy) session.omit_permission_reply_marker();
            const auto result = session.feed(k_end_turn.substr(15));
            require(result.failure_reason == "acp_invalid_prompt_completion" && !result.stdin_bytes,
                "a deferred permission reply must still block completion beside a filesystem refusal");
        }
    }
    {
        Session session(attestation, true);
        session.feed(k_initialize);
        const auto early_write = session.feed(k_new + write_bytes);
        require(early_write.failure_reason == "acp_out_of_order_reverse_request" && !early_write.stdin_bytes,
            "an unsent prompt must still block a filesystem reverse request");
        Session closing(attestation, true);
        closing.reach_prompt();
        const auto early_close = closing.feed(write_bytes + k_message + k_end_turn + k_closed);
        require(early_close.failure_reason == "acp_invalid_close" && !early_close.stdin_bytes,
            "an unsent close must still block its response after coalesced write refusals");
    }
    {
        auto error = unsupported;
        error.set("id", Json(""));
        const auto overhead = addon::serialize_json(error).size() + 1;
        const auto id_size  = addon::limits::k_max_structured_stream_bytes / 5 - overhead - 1;
        std::string requests;
        for (int id = 0; id < 5; ++id) {
            auto request = write_request;
            request.set("id", Json(std::string(id_size - 1, 'w') + std::to_string(id)));
            requests += addon::serialize_json(request) + '\n';
        }
        Session session(attestation, true);
        session.reach_prompt();
        const auto prefix = session.feed(requests.substr(0, 128 * 1024));
        require(!prefix.complete && !prefix.stdin_bytes, "a partial large write request must wait");
        const auto tail = requests.substr(128 * 1024) + k_message + k_end_turn;
        require(tail.size() <= addon::limits::k_max_structured_stream_bytes, "fixture stdout must fit the SDK bound");
        const auto result = session.feed(tail);
        require(result.failure_reason == "acp_permission_reply_bound_exceeded" && !result.stdin_bytes,
            "appending close to bounded write refusals must not overflow the stdin continuation");
    }
    {
        Session session(attestation, true);
        session.reach_prompt();
        session.feed(permission_request(Json(18)));
        require(session.feed(write_bytes).failure_reason == "acp_unsupported_reverse_request",
            "a filesystem request cannot reuse a permission request ID");
    }
    for (const auto& invalid : {
             R"({"sessionId":"ses_foreign","path":"fixture","content":"edit"})",
             R"({"sessionId":"ses_root","path":"","content":"edit"})",
             R"({"sessionId":"ses_root","path":false,"content":"edit"})",
             R"({"sessionId":"ses_root","path":"fixture","content":false})",
             R"({"sessionId":"ses_root","path":"fixture","content":"edit","_meta":false})",
             R"({"sessionId":"ses_root","path":"fixture","content":"edit","extra":true})"})
    {
        Session session(attestation, true);
        session.reach_prompt();
        auto request = write_request;
        request.set("params", parse(invalid));
        const auto result = session.feed(k_permission + addon::serialize_json(request) + '\n');
        require(result.failure_reason == "acp_unsupported_reverse_request" && !result.stdin_bytes,
            "foreign or malformed writes must fail without partial permission replies");
    }
    for (const auto& request : [&] {
        auto oversized = write_request;
        oversized.find("params")->set("path", Json(std::string(32 * 1024 + 1, 'p')));
        auto other_method = write_request;
        other_method.set("method", Json("fs/read_text_file"));
        auto invalid_id = write_request;
        invalid_id.set("id", Json(false));
        return Json::Array{oversized, other_method, invalid_id};
    }())
    {
        Session session(attestation, true);
        session.reach_prompt();
        require(session.feed(addon::serialize_json(request) + '\n').failure_reason ==
            "acp_unsupported_reverse_request", "the refusal must remain bounded and method-specific");
    }
    {
        Session session(attestation, true);
        session.reach_prompt();
        for (int id = 1000; id < 1256; ++id) {
            auto request = write_request;
            request.set("id", Json(id));
            const auto result = session.feed(addon::serialize_json(request) + '\n');
            require(!result.complete && result.stdin_bytes, "256 filesystem request IDs fit the shared bound");
        }
        require(session.feed(k_permission).failure_reason == "acp_unsupported_reverse_request",
            "filesystem refusals must consume the existing session request bound");
    }
    // Native --auto (TUI SyncProvider) replies once to permission.asked.
    // Permission.ask rejects explicit deny rules before publishing that event.
    {
        Session automatic(attestation, true);
        automatic.reach_prompt();
        const auto first = automatic.feed(k_permission);
        require(!first.complete && first.stdin_bytes, "selected auto mode answers a native pending ask");
        require_permission_reply(*first.stdin_bytes, Json(17), "once");
        const auto next = automatic.feed(permission_request(Json(18)));
        require(!next.complete && next.stdin_bytes, "auto mode survives the serialized inspection continuation");
        require_permission_reply(*next.stdin_bytes, Json(18), "once");
        const auto foreign = automatic.feed(foreign_session(permission_request(Json(19))));
        require(foreign.failure_reason == "acp_unsupported_reverse_request" && !foreign.stdin_bytes,
            "auto mode cannot approve another session's request");
    }
    {
        Session automatic(attestation, true);
        automatic.reach_prompt();
        auto request = parse(k_permission);
        auto& options = request.find("params")->find("options")->as_array();
        options.erase(options.begin());
        const auto unoffered = automatic.feed(addon::serialize_json(request) + '\n');
        require(unoffered.failure_reason == "acp_unsupported_reverse_request" && !unoffered.stdin_bytes,
            "auto mode never substitutes a persistent grant when allow-once is absent");
    }
    {
        Session default_mode(attestation);
        default_mode.omit_auto_mode();
        default_mode.reach_prompt();
        const auto rejected = default_mode.feed(k_permission);
        require(!rejected.complete && rejected.stdin_bytes, "an unspecified mode keeps the original posture");
        require_permission_reply(*rejected.stdin_bytes, Json(17));
    }
    {
        auto state = parse(opencode::make_acp_inspection(
            "Task", ".", "1.18.25", Json::array(), Json::array(), Json::array()).protocol_state);
        state.set("auto_approve", Json("true"));
        addon::Structured_inspection malformed;
        malformed.protocol_state = addon::serialize_json(state);
        require(opencode::inspect_acp(malformed).failure_reason == "acp_invalid_state",
            "an auto-mode continuation requires a boolean policy value");
    }
    // ACP lets the agent announce the new session before or after it answers
    // session/new; OpenCode 1.18.32 announces after.
    for (const auto& root_output : {k_ready + k_new, k_new + k_ready}) {
        Session session(attestation);
        session.feed(k_initialize);
        auto result = session.feed(root_output);
        require(!result.complete && result.stdin_bytes &&
                result.stdin_bytes->find("session/prompt") != std::string::npos,
            "either announcement order must request the prompt");
        result = session.feed(k_message);
        require(!result.complete && !result.stdin_bytes, "message must wait for completion response");
        result = session.feed(k_end_turn);
        require(!result.complete && result.stdin_bytes &&
                result.stdin_bytes->find("session/close") != std::string::npos, "end turn must request close");
        result = session.feed(k_closed, true);
        require(result.complete && result.result_bytes == "Finished",
            "either announcement order must deliver the result");
    }
    {
        Session session(attestation);
        session.feed(k_initialize);
        const auto answered = session.feed(k_new);
        require(!answered.complete && answered.stdin_bytes && !answered.failure_reason,
            "an attested root response alone must request the prompt");
        const auto announced = session.feed(k_ready);
        require(!announced.complete && !announced.stdin_bytes && !announced.failure_reason,
            "an announcement after the prompt request is housekeeping");
    }
    for (std::size_t split = 1; split < k_ready.size(); ++split) {
        Session session(attestation);
        session.feed(k_initialize);
        const auto first = session.feed(k_new + k_ready.substr(0, split));
        require(!first.stdin_bytes && !first.failure_reason, "partial late announcement holds prompt");
        const auto second = session.feed(k_ready.substr(split));
        require(second.stdin_bytes.has_value() && !second.failure_reason,
            "late announcement releases deferred prompt");
    }
    {
        Session session(attestation);
        session.feed(k_initialize);
        const auto result = session.feed(foreign_session(k_ready) + k_new);
        require(result.failure_reason == "acp_unauthenticated_notification",
            "an announcement before the response must name the answered session");
    }
    {
        Session session(attestation);
        session.feed(k_initialize);
        const auto result = session.feed(k_new + foreign_session(k_ready));
        require(result.failure_reason == "acp_unauthenticated_notification",
            "an announcement after the response must name the answered session");
    }
    {
        Session session(attestation);
        session.feed(k_initialize);
        const auto result = session.feed(k_ready + "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n");
        require(result.failure_reason == "acp_invalid_root_session",
            "a root response without a session is not a readiness failure");
    }
    // Startup output must retain its pre-request meaning across every read split.
    for (const auto& early : {k_message, k_permission}) {
        for (std::size_t split = 1; split <= early.size(); ++split) {
            Session session(attestation);
            session.feed(k_initialize);
            const auto first = session.feed(k_ready + k_new + early.substr(0, split));
            require(!first.stdin_bytes, "pre-request output must withhold task input");
            if (first.failure_reason) continue;
            const auto empty = session.feed({});
            require(!empty.stdin_bytes && !empty.failure_reason, "empty read must retain pending input");
            const auto second = session.feed(early.substr(split));
            require(second.failure_reason.has_value() && !second.stdin_bytes,
                "pre-request model/permission tail must be rejected");
        }
    }
    for (std::size_t split = 1; split < k_ready.size(); ++split) {
        Session session(attestation);
        session.feed(k_initialize);
        const auto first = session.feed(k_ready + k_new + k_ready.substr(0, split));
        require(!first.stdin_bytes && !first.failure_reason, "partial housekeeping holds prompt");
        const auto second = session.feed(k_ready.substr(split));
        require(second.stdin_bytes.has_value() && !second.failure_reason,
            "valid housekeeping releases deferred prompt");
        const auto third = session.feed({});
        require(!third.stdin_bytes && !third.failure_reason, "deferred prompt is not replayed");
    }
    // The oracle is the native OpenCode ACP contract: causal requests, authenticated
    // root updates, nonce readiness, final-message replacement, and closed completion.
    {
        // JSON-RPC IDs correlate independent requests; transport read boundaries
        // cannot change their replies. This fixture does not assert vendor concurrency.
        const auto second = permission_request(Json("permission_write"));
        const auto transcript = k_permission + k_message + second;
        Session coalesced(attestation);
        coalesced.reach_prompt();
        const auto combined = coalesced.feed(transcript);
        require(!combined.complete && combined.stdin_bytes, "coalesced permissions must both receive replies");

        Session split(attestation);
        split.reach_prompt();
        const auto first_reply = split.feed(k_permission);
        require(!first_reply.complete && first_reply.stdin_bytes, "first independent permission reply");
        require_permission_reply(*first_reply.stdin_bytes, Json(17));
        const auto message = split.feed(k_message);
        require(!message.complete && !message.stdin_bytes, "interleaved update must not emit input");
        const auto second_reply = split.feed(second);
        require(!second_reply.complete && second_reply.stdin_bytes, "second independent permission reply");
        require_permission_reply(*second_reply.stdin_bytes, Json("permission_write"));
        require(*combined.stdin_bytes == *first_reply.stdin_bytes + *second_reply.stdin_bytes,
            "coalesced and separate permissions must emit identical ordered replies");
        require(combined.protocol_state == second_reply.protocol_state,
            "coalesced and separate permissions must retain identical state");

        Session byte_split(attestation);
        byte_split.reach_prompt();
        std::string emitted;
        addon::Structured_inspect_result byte_result;
        for (const char byte : transcript) {
            byte_result = byte_split.feed(std::string(1, byte));
            require(!byte_result.complete, "byte-split permission transcript must remain valid");
            if (byte_result.stdin_bytes) {
                emitted += *byte_result.stdin_bytes;
            }
        }
        require(emitted == *combined.stdin_bytes && byte_result.protocol_state == combined.protocol_state,
            "byte-split permissions must preserve ordered replies and state");
    }
    {
        Session session(attestation);
        session.feed(k_initialize);
        const auto result = session.feed(k_ready + k_new + k_permission);
        require(result.failure_reason == "acp_out_of_order_reverse_request",
            "permission request cannot precede delivery of the prompt request");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        const auto result = session.feed(k_permission + permission_request(Json(17.0)));
        require(result.failure_reason == "acp_unsupported_reverse_request" && !result.stdin_bytes,
            "coalesced duplicate request ID must fail without partial replies");
    }
    for (const auto& invalid : {
             "{\"sessionId\":\"ses_foreign\",\"toolCall\":{\"toolCallId\":\"call_read\"},"
             "\"options\":[{\"kind\":\"allow_once\",\"name\":\"Allow once\",\"optionId\":\"once\"}]}",
             "{\"sessionId\":\"ses_root\",\"toolCall\":{\"toolCallId\":\"call_read\"},\"options\":[]}",
             "{\"sessionId\":\"ses_root\",\"toolCall\":{},"
             "\"options\":[{\"kind\":\"allow_once\",\"name\":\"Allow once\",\"optionId\":\"once\"}]}"})
    {
        Session session(attestation);
        session.reach_prompt();
        auto request = parse(permission_request(Json(18)));
        request.set("params", parse(invalid));
        const auto result = session.feed(k_permission + addon::serialize_json(request) + '\n');
        require(result.failure_reason == "acp_unsupported_reverse_request" && !result.stdin_bytes,
            "foreign or malformed coalesced permission must fail without partial replies");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        const auto result = session.feed(k_message + k_permission +
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}\n");
        require(result.failure_reason == "acp_invalid_prompt_completion",
            "prompt completion cannot precede pending permission replies");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        const auto result = session.feed(k_message +
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}\n"
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{}}\n");
        require(result.failure_reason == "acp_invalid_close",
            "close response cannot precede delivery of the close request");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        std::string requests;
        for (int id = 1000; id < 1256; ++id) {
            requests += permission_request(Json(id));
        }
        const auto accepted = session.feed(requests);
        require(!accepted.complete && accepted.stdin_bytes, "256 unique permission IDs fit the session bound");
        const auto rejected = session.feed(permission_request(Json(1256)));
        require(rejected.failure_reason == "acp_unsupported_reverse_request",
            "batching must retain the 256-ID session bound");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        std::string requests;
        for (int id = 0; id < 6; ++id) {
            requests += permission_request(Json(std::string(180 * 1024, 'p') + std::to_string(id)));
        }
        // A carried partial record plus one legal stdout chunk can produce more
        // reply bytes than the SDK permits in one stdin continuation.
        const auto prefix = session.feed(requests.substr(0, 128 * 1024));
        require(!prefix.complete && !prefix.stdin_bytes, "partial permission record must wait");
        const auto remainder = requests.substr(128 * 1024);
        require(remainder.size() <= addon::limits::k_max_structured_stream_bytes, "fixture stdout must fit the SDK bound");
        const auto result = session.feed(remainder);
        require(result.failure_reason == "acp_permission_reply_bound_exceeded" && !result.stdin_bytes,
            "permission replies must not overflow the SDK stdin bound");
    }
    {
        Session session(attestation);
        auto result = session.feed(k_initialize.substr(0, 23));
        require(!result.complete && !result.stdin_bytes, "partial initialize must wait for a complete line");
        result = session.feed(k_initialize.substr(23));
        require(!result.complete && result.stdin_bytes.has_value(), "remaining initialize must advance");
        result = session.feed(k_ready + k_new);
        require(!result.complete && result.stdin_bytes.has_value(), "root readiness and response must advance");
        result = session.feed(k_permission);
        require(!result.complete && result.stdin_bytes &&
                result.stdin_bytes->find("selected") != std::string::npos, "allow-once permission response");
        result = session.feed(k_message);
        require(!result.complete && !result.stdin_bytes, "message must wait for completion response");
        result = session.feed("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}\n");
        require(!result.complete && result.stdin_bytes &&
                result.stdin_bytes->find("session/close") != std::string::npos, "end turn must request close");
        result = session.feed("{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{}}\n", true);
        require(result.complete && result.result_bytes == "Finished", "authenticated close must deliver result");
    }
    {
        Session session(attestation);
        const auto result = session.feed(k_initialize + k_new);
        require(result.failure_reason == "acp_invalid_root_session", "response cannot precede emitted request");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        session.feed(k_permission);
        std::string duplicate = k_permission;
        duplicate.replace(duplicate.find(":17,"), 4, ":17.0,");
        const auto result = session.feed(duplicate);
        require(result.failure_reason == "acp_unsupported_reverse_request", "numeric permission IDs compare by value");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        std::string foreign = k_message;
        foreign.replace(foreign.find("ses_root"), 8, "ses_foreign");
        const auto result = session.feed(foreign);
        require(result.failure_reason == "acp_unauthenticated_notification", "foreign session output must fail");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        std::string replacement = k_message;
        replacement.replace(replacement.find("msg_result"), 10, "msg_final");
        replacement.replace(replacement.find("Finished"), 8, "Revised");
        const auto split = replacement.size() / 2;
        const auto partial = session.feed(k_message + replacement + replacement.substr(0, split));
        require(!partial.complete && !partial.stdin_bytes && !partial.failure_reason,
            "coalesced chunks followed by a partial record must wait for the rest");
        const auto ended = session.feed(replacement.substr(split) + k_end_turn);
        require(ended.stdin_bytes && ended.stdin_bytes->find("session/close") != std::string::npos,
            "end_turn after a split message chunk must request close");
        const auto result = session.feed(k_closed);
        require(result.complete && result.result_bytes == "RevisedRevised",
            "a new message replaces earlier chunks and keeps its own chunks across reads");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        const auto result = session.feed(std::string(256 * 1024 + 1, 'x'));
        require(result.failure_reason == "acp_record_bound_exceeded", "unterminated record bound");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        const auto result = session.feed({}, true, true);
        require(result.failure_reason == "acp_timed_out", "timeout must remain failure");
    }
    {
        Session session(attestation);
        session.reach_prompt();
        session.feed(k_message);
        session.feed("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}\n");
        const auto result = session.feed("{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{}}\ntrailing");
        require(result.failure_reason == "acp_invalid_close", "trailing close output must fail");
    }
    // Oracle: the owner's rule that a delegated run keeps its launcher's
    // permission posture and the package approves only its own Task tools
    // (2026-09-28), and OpenCode 1.18.32's requests recorded against a
    // scripted endpoint: an MCP tool call is announced by a tool_call titled
    // <server>_<tool> with kind other, and its permission request repeats that
    // id, title and kind; a shell request is titled with the command text and
    // has kind execute.
    {
        const auto announce = [](const std::string& call, const std::string& title, const char* kind) {
            return "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"ses_root\","
                "\"update\":{\"sessionUpdate\":\"tool_call\",\"toolCallId\":\"" + call + "\",\"title\":\"" + title +
                "\",\"kind\":\"" + kind + "\",\"status\":\"pending\",\"locations\":[],\"rawInput\":{}}}}\n";
        };
        const auto request = [](const std::string& call, const std::string& title, const char* kind, bool reject) {
            auto value = parse(k_permission);
            auto tool = parse("{\"status\":\"pending\",\"locations\":[],\"rawInput\":{}}");
            tool.set("toolCallId", Json(call));
            tool.set("title", Json(title));
            tool.set("kind", Json(std::string(kind)));
            value.find("params")->set("toolCall", std::move(tool));
            if (!reject) {
                auto& options = value.find("params")->find("options")->as_array();
                options.pop_back();
            }
            return addon::serialize_json(value) + '\n';
        };
        Session approved(attestation);
        approved.reach_prompt();
        const auto task = approved.feed(announce("call_task", k_task_tool, "other") +
            request("call_task", k_task_tool, "other", true));
        require(!task.complete && task.stdin_bytes, "an announced Task tool call is answered");
        require_permission_reply(*task.stdin_bytes, Json(17), "once");
        for (const auto& [announced, requested, kind] : {
                std::tuple<std::string, std::string, const char*>{"bash", k_task_tool, "execute"},
                {"logonomic-task-t-task_control_other", "logonomic-task-t-task_control_other", "other"},
                {"", k_task_tool, "other"}})
        {
            Session session(attestation);
            session.reach_prompt();
            const auto prefix = announced.empty() ? std::string{} : announce("call_task", announced, "other");
            const auto answered = session.feed(prefix + request("call_task", requested, kind, true));
            require(!answered.complete && answered.stdin_bytes,
                "a request for anything but an announced Task tool is answered");
            require_permission_reply(*answered.stdin_bytes, Json(17), "reject");
        }
        Session unoffered(attestation);
        unoffered.reach_prompt();
        require(unoffered.feed(request("call_other", "echo probe", "execute", false)).failure_reason ==
            "acp_unsupported_reverse_request", "a request without its own reject option is not answered otherwise");
    }
    {
        std::ofstream file(attestation, std::ios::binary | std::ios::trunc);
        file << "not-ready";
    }
    for (const auto& root_output : {k_ready + k_new, k_new, k_new + k_ready}) {
        Session session(attestation);
        session.feed(k_initialize);
        const auto result = session.feed(root_output);
        require(result.failure_reason == "acp_task_servers_not_ready", "nonce file is mandatory readiness evidence");
    }
}

} // namespace

int main()
{
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = fs::temp_directory_path() / ("opencode-acp-tests-" + std::to_string(suffix));
    fs::create_directory(directory);
    const auto attestation = directory / "ready";
    std::ofstream(attestation, std::ios::binary) << "0123456789abcdef0123456789abcdef";
    int status = 0;
    try {
        run(attestation);
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    fs::remove(attestation);
    fs::remove(directory);
    return status;
}
