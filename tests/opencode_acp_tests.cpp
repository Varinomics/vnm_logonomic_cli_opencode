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
const std::string k_message =
    "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{\"sessionId\":\"ses_root\","
    "\"update\":{\"sessionUpdate\":\"agent_message_chunk\",\"messageId\":\"msg_result\","
    "\"content\":{\"type\":\"text\",\"text\":\"Finished\"}}}}\n";
const std::string k_permission =
    "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"session/request_permission\",\"params\":{"
    "\"sessionId\":\"ses_root\",\"toolCall\":{\"toolCallId\":\"call_read\"},\"options\":[{"
    "\"kind\":\"allow_once\",\"name\":\"Allow once\",\"optionId\":\"once\"}]}}\n";

std::string permission_request(const Json& id)
{
    auto request = parse(k_permission);
    request.set("id", id);
    return addon::serialize_json(request) + '\n';
}

void require_permission_reply(const std::string& bytes, const Json& id)
{
    const auto reply = parse(bytes);
    const auto* reply_id = reply.find("id");
    require(reply_id && addon::serialize_json(*reply_id) == addon::serialize_json(id),
        "permission reply must echo its request ID");
    const auto* result = reply.find("result");
    const auto* outcome = result ? result->find("outcome") : nullptr;
    require(outcome && addon::serialize_json(*outcome) ==
            addon::serialize_json(parse("{\"outcome\":\"selected\",\"optionId\":\"once\"}")),
        "permission reply must select the adopted allow-once option");
}

class Session
{
public:
    explicit Session(const fs::path& attestation)
    {
        const auto path = attestation.u8string();
        Json entry = parse("{\"server_id\":\"task_control\",\"nonce\":\"0123456789abcdef0123456789abcdef\"}");
        entry.set("path", Json(std::string(path.begin(), path.end())));
        m_state = opencode::make_acp_inspection(
            "Implement the requested change", ".", "1.2.3", Json::array(),
            Json(Json::Array{entry})).protocol_state;
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

void run(const fs::path& attestation)
{
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
        require(remainder.size() <= addon::k_max_structured_stream_bytes, "fixture stdout must fit the SDK bound");
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
        session.feed(k_initialize);
        const auto result = session.feed(k_new);
        require(result.failure_reason == "acp_task_servers_not_ready", "root response needs readiness update");
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
        session.feed(k_message);
        std::string replacement = k_message;
        replacement.replace(replacement.find("msg_result"), 10, "msg_final");
        replacement.replace(replacement.find("Finished"), 8, "Revised");
        session.feed(replacement);
        session.feed("{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}\n");
        const auto result = session.feed("{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{}}\n");
        require(result.result_bytes == "Revised", "a new message identity replaces accumulated result");
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
    {
        std::ofstream file(attestation, std::ios::binary | std::ios::trunc);
        file << "not-ready";
        file.close();
        Session session(attestation);
        session.feed(k_initialize);
        const auto result = session.feed(k_ready + k_new);
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
