#include "opencode_preparation.h"
#include "opencode_acp.h"
#include "opencode_launcher.h"

#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/sha256.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace opencode {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

constexpr char k_instruction_name[] = "logonomic-title-instruction.md";
constexpr char k_plugin_asset[] = "assets/logonomic_opencode_plugin.mjs";
constexpr char k_changed_files_instruction[] =
    "When the user asks to bring up, open, show, or inspect files changed in "
    "this Session and the conversation already supplies repository-relative "
    "paths, call the `logonomic_show_changed_files` tool exactly once with "
    "exactly those paths in their existing order. Do not use Git, inspect or "
    "read files for discovery, call `vnmctl` or its help or discover commands, "
    "use a shell or Python, create a manifest, check path existence, loop per "
    "file, retry, or supply repository, Session, title, or authority. Use "
    "`logonomic_show_changes` only when the conversation supplies exact "
    "unified-diff hunks.";

bool environment_name_equal(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
#else
    return left == right;
#endif
}

bool blank(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
}

std::string uncomment_jsonc(std::string_view text)
{
    std::string output(text);
    bool quoted  = false;
    bool escaped = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (escaped) { escaped = false; }
            else
            if (c == '\\') { escaped = true; }
            else
            if (c == '"') { quoted = false; }
            continue;
        }
        if (c == '"') {
            quoted = true;
            continue;
        }
        if (c != '/' || i + 1 == text.size()) {
            continue;
        }
        if (text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\r' && text[i] != '\n') {
                output[i++] = ' ';
            }
            if (i < text.size()) {
                --i;
            }
        }
        else
        if (text[i + 1] == '*') {
            output[i++] = ' ';
            output[i++] = ' ';
            bool closed = false;
            for (; i < text.size(); ++i) {
                if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') {
                    output[i++] = ' ';
                    output[i]   = ' ';
                    closed = true;
                    break;
                }
                if (text[i] != '\r' && text[i] != '\n') {
                    output[i] = ' ';
                }
            }
            if (!closed) {
                throw std::runtime_error("OpenCode configuration contains an unterminated comment.");
            }
        }
    }
    quoted  = false;
    escaped = false;
    for (std::size_t i = 0; i < output.size(); ++i) {
        const char c = output[i];
        if (quoted) {
            if (escaped) { escaped = false; }
            else
            if (c == '\\') { escaped = true; }
            else
            if (c == '"') { quoted = false; }
        }
        else
        if (c == '"') {
            quoted = true;
        }
        else
        if (c == ',') {
            std::size_t next = i + 1;
            while (next < output.size() && std::isspace(static_cast<unsigned char>(output[next]))) {
                ++next;
            }
            if (next < output.size() && (output[next] == ']' || output[next] == '}')) {
                output[i] = ' ';
            }
        }
    }
    return output;
}

fs::path native_path(std::string_view value)
{
    return fs::path(std::u8string(value.begin(), value.end()));
}

std::string path_text(const fs::path& path)
{
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool is_link(const fs::path& path)
{
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return fs::is_symlink(fs::symlink_status(path));
#endif
}

void require_directory(const fs::path& path)
{
    if (is_link(path) || !fs::is_directory(path)) {
        throw std::runtime_error("OpenCode managed instruction directory is not owned storage.");
    }
}

std::string read_instruction(const fs::path& path, std::size_t expected_size)
{
    if (is_link(path) || !fs::is_regular_file(path) || fs::file_size(path) != expected_size) {
        throw std::runtime_error("OpenCode managed instruction generation is inconsistent.");
    }
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(expected_size, '\0');
    if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("OpenCode managed instruction could not be read.");
    }
    return bytes;
}

} // namespace

Inline_config inline_config(const addon::Json_value& environment)
{
    Inline_config config;
    const addon::Json_value* selected = nullptr;
    for (const auto& member : environment.as_object()) {
        if (environment_name_equal(member.name, config.environment_name)) {
            if (selected) {
                throw std::runtime_error("OpenCode configuration has multiple environment case variants.");
            }
            selected = &member.value;
            config.environment_name = member.name;
        }
    }
    if (!selected || blank(selected->as_string())) {
        return config;
    }
    const auto parsed = addon::parse_json(uncomment_jsonc(selected->as_string()), {}, nullptr);
    if (!parsed || !parsed->is_object()) {
        throw std::runtime_error("OpenCode configuration must be a JSON object.");
    }
    config.value = *parsed;
    if (const auto* instructions = config.value.find("instructions")) {
        if (!instructions->is_array() ||
            std::any_of(instructions->as_array().begin(), instructions->as_array().end(),
                [](const auto& value) { return !value.is_string(); }))
        {
            throw std::runtime_error("OpenCode instructions must be an array of strings.");
        }
    }
    if (const auto* plugins = config.value.find("plugin")) {
        if (!plugins->is_array()) {
            throw std::runtime_error("OpenCode plugin configuration must be an array.");
        }
        for (const auto& plugin : plugins->as_array()) {
            if (plugin.is_string() && !blank(plugin.as_string())) {
                continue;
            }
            if (plugin.is_array() && plugin.as_array().size() == 2 &&
                plugin.as_array()[0].is_string() && !blank(plugin.as_array()[0].as_string()) &&
                plugin.as_array()[1].is_object())
            {
                continue;
            }
            throw std::runtime_error("OpenCode plugin entries must be names or name/options pairs.");
        }
    }
    return config;
}

std::string materialize_instruction(std::string_view profile_root, std::string_view instruction)
{
    if (blank(instruction) || instruction.find('\0') != std::string_view::npos) {
        throw std::runtime_error("OpenCode title instruction must be nonempty text.");
    }
    std::string body(instruction);
    while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) {
        body.pop_back();
    }
    body += "\n\n";
    body += k_changed_files_instruction;
    const std::string digest = addon::to_hex(addon::Sha256::digest(body));
    const fs::path root = native_path(profile_root);
    const fs::path store = root / "instructions";
    const fs::path generation = store / digest;
    const fs::path file = generation / k_instruction_name;
    static std::mutex materialization_mutex;
    const std::lock_guard lock(materialization_mutex);
    require_directory(root);
    fs::create_directory(store);
    require_directory(store);
    if (fs::exists(generation)) {
        require_directory(generation);
        if (read_instruction(file, body.size()) != body) {
            throw std::runtime_error("OpenCode managed instruction bytes were changed.");
        }
        return path_text(file);
    }
    const fs::path staging = store / ("stage-" + addon::random_nonce_hex());
    fs::create_directory(staging);
#ifndef _WIN32
    fs::permissions(staging, fs::perms::owner_all, fs::perm_options::replace);
#endif
    try {
        {
            std::ofstream output(staging / k_instruction_name, std::ios::binary);
            output.write(body.data(), static_cast<std::streamsize>(body.size()));
            output.close();
            if (!output) {
                throw std::runtime_error("OpenCode managed instruction write failed.");
            }
        }
        std::error_code error;
        fs::rename(staging, generation, error);
        if (error) {
            require_directory(generation);
            if (read_instruction(file, body.size()) != body) {
                throw std::runtime_error("OpenCode managed instruction publication failed.");
            }
            fs::remove_all(staging);
        }
    }
    catch (...) {
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        throw;
    }
    return path_text(file);
}

namespace {

std::string interactive_config(addon::Json_value config, const std::string& instruction)
{
    auto instructions = config.find("instructions") ? *config.find("instructions") : addon::Json_value::array();
    instructions.push_back(addon::Json_value(instruction));
    config.set("instructions", std::move(instructions));
    const auto plugins = config.find("plugin") ? *config.find("plugin") : addon::Json_value::array();
    config.erase("plugin");
    std::string result = addon::serialize_json(config);
    result.pop_back();
    result += ",\"plugin\":[";
    for (const auto& plugin : plugins.as_array()) {
        result += addon::serialize_json(plugin) + ",";
    }
    // The host substitutes a complete JSON string, including Windows escaping.
    result += "{{asset_json:assets/logonomic_opencode_plugin.mjs}}]}";
    return result;
}

void prepare_interactive(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto launcher = recognize_launcher(request->launcher.script_body);
    if (!launcher || !request->profile_storage) {
        context.fail(addon::Result_status::UNAVAILABLE, "launcher_unavailable");
        return;
    }
    for (const auto& [name, value] : launcher->environment) {
        (void)value;
        if (environment_name_equal(name, "OPENCODE_CONFIG_CONTENT")) {
            context.fail(addon::Result_status::UNAVAILABLE, "inline_configuration_assignment");
            return;
        }
    }
    if (std::find(request->declared_assets.begin(), request->declared_assets.end(), k_plugin_asset) == request->declared_assets.end()) {
        context.fail(addon::Result_status::FAILED, "asset_undeclared");
        return;
    }
    auto config = inline_config(request->launch_environment->effective_environment);
    const auto argv = literal_launcher_argv(request->launcher.script_body);
    const auto launch = verified_launch(request->launcher, argv,
        request->launch_environment->effective_environment, request->working_directory, false, context);
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    const auto instruction = materialize_instruction(request->profile_storage->root, request->title_instruction);
    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    patch.set.emplace_back(config.environment_name, interactive_config(std::move(config.value), instruction));
    patch.set.emplace_back("OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED", launch ? "1" : "0");
    context.succeed(addon::encode_launch_patch(patch));
}

void prepare_delegated(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!request->control.connection) {
        context.fail(addon::Result_status::UNAVAILABLE, "task_server_connection_unavailable");
        return;
    }
    if (request->purpose != addon::Structured_purpose::IMPLEMENTATION) {
        context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable");
        return;
    }
    if (request->prompt.empty() || !addon::is_valid_utf8(request->prompt) ||
        !native_path(request->context.working_directory).is_absolute())
    {
        context.fail(addon::Result_status::FAILED, "invalid_implementation_request");
        return;
    }
    const auto launch = verified_launch(request->context.launcher, request->context.launcher_argv,
        request->context.launch_environment->effective_environment, request->context.working_directory, true, context);
    if (!launch) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "launcher_program_unavailable");
        return;
    }
    auto config = inline_config(request->context.launch_environment->effective_environment);
    if (const auto* permission = config.value.find("permission")) {
        if (!permission->is_object() &&
            !(permission->is_string() && (permission->as_string() == "allow" || permission->as_string() == "ask" || permission->as_string() == "deny")))
        {
            context.fail(addon::Result_status::FAILED, "invalid_permission_configuration");
            return;
        }
    }
    addon::Delegated_prepare_result result;
    result.profile_storage = request->context.profile_storage;
    auto servers = addon::Json_value::array();
    auto attestations = addon::Json_value::array();
    for (const auto& server : request->control.servers) {
        const auto nonce = addon::random_nonce_hex();
        const auto stem = "mcp/server-" + nonce;
        // The host accepts the receipt path only in its clean '/'-separated
        // form; a native join would insert '\' on Windows.
        const auto ready_file =
            (native_path(request->control.private_artifact_root) / (stem + ".ready")).generic_u8string();
        const std::string ready_path(reinterpret_cast<const char*>(ready_file.data()), ready_file.size());
        const auto& connection = *request->control.connection;
        addon::Json_value::Array arguments{
            addon::Json_value("--descriptor"), addon::Json_value(connection.descriptor_path),
            addon::Json_value("--server"), addon::Json_value(server.server_id),
            addon::Json_value("--attestation-path"), addon::Json_value(ready_path),
            addon::Json_value("--attestation-nonce"), addon::Json_value(nonce),
        };
        // Materialization creates the parent; the host exclusively reserves the ready file.
        result.private_artifacts.push_back({stem + ".connection", nonce});
        result.acknowledgment.server_ids.push_back(server.server_id);
        auto mcp = addon::Json_value::object();
        mcp.set("name", addon::Json_value("logonomic-task-" + request->control.task_id + "-" + server.server_id));
        mcp.set("command", addon::Json_value(connection.relay_program));
        mcp.set("args", addon::Json_value(std::move(arguments)));
        mcp.set("env", addon::Json_value::array());
        servers.push_back(std::move(mcp));
        auto attestation = addon::Json_value::object();
        attestation.set("server_id", addon::Json_value(server.server_id));
        attestation.set("path", addon::Json_value(ready_path));
        attestation.set("nonce", addon::Json_value(nonce));
        attestations.push_back(std::move(attestation));
    }
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    // The package bridge executes the verified selected chain without altering it.
#ifdef _WIN32
    result.process.executable = "helpers/opencode_launch_bridge.exe";
#else
    result.process.executable = "helpers/opencode_launch_bridge";
#endif
    result.process.argv = launch->argv;
    result.process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    result.process.stdin_payload = acp_initial_input();
    result.process.environment = launch->environment;
    result.process.environment.erase(std::remove_if(result.process.environment.begin(), result.process.environment.end(),
        [&config](const auto& pair) { return environment_name_equal(pair.first, config.environment_name) || environment_name_equal(pair.first, "OPENCODE_ENABLE_QUESTION_TOOL"); }),
        result.process.environment.end());
    result.process.environment.emplace_back(config.environment_name, addon::serialize_json(config.value));
    result.process.environment.emplace_back("OPENCODE_ENABLE_QUESTION_TOOL", "0");
    result.process.environment_names = {"PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "XDG_CONFIG_HOME", "XDG_DATA_HOME"};
    result.inspection = make_acp_inspection(request->prompt, request->context.working_directory, launch->version, servers, attestations);
    result.inspection.keep_stdin_open = true;
    auto payload = addon::encode_delegated_prepare_result(result, 2);
    if (!addon::validate_session_exchange_payload(payload, &failure)) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    context.succeed(std::move(payload));
}

} // namespace

void handle_preparation(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.operation != "prepare") {
        context.reject_unknown_operation();
    }
    else
    if (request.capability.capability_id == "launch-preparation") {
        prepare_interactive(context);
    }
    else
    if (request.capability.capability_id == "delegated-session") {
        prepare_delegated(context);
    }
    else
    if (request.capability.capability_id == "structured-session") {
        context.fail(addon::Result_status::UNAVAILABLE, "purpose_unavailable",
            "OpenCode implementation requires delegated task control.");
    }
    else {
        context.reject_unknown_operation();
    }
}

} // namespace opencode
