#include "opencode_acp.h"

#include <logonomic_cli_addon/json.h>
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
