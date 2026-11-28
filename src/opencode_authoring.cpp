#include "opencode_authoring.h"
#include "opencode_launcher.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace opencode {
namespace {

namespace sdk = logonomic::cli_addon;
namespace fs = std::filesystem;
using Json = sdk::Json_value;
using sdk::json::member;
using sdk::json::text;

constexpr char k_agent[] = "logonomic-workflow-author";

bool empty_array(const Json& value)
{
    return value.is_null() || (value.is_array() && value.as_array().empty());
}

bool declared_launcher_inputs(const sdk::Environment& env, std::string_view assignments)
{
    // A stored launcher can explicitly select an identity using its own
    // environment. Decline inputs outside this capability rather than run
    // the vendor with a different identity.
    const auto known = [](std::string_view name) {
        const auto lower = sdk::ascii_lowercase(name);
        constexpr std::string_view extra[] = {"opencode_config_content", "opencode_config", "opencode_config_dir",
            "opencode_test_home", "opencode_db", "xdg_cache_home", "xdg_state_home", "systemroot", "windir", "temp", "tmp"};
        return std::ranges::any_of(sdk::k_ambient_baseline_names, [&](const auto* item) {
            return sdk::ascii_lowercase(item) == lower;
        }) || std::ranges::find(extra, lower) != std::end(extra);
    };
    if (!std::ranges::all_of(env, [&](const auto& pair) { return known(pair.first); })) return false;
    std::istringstream lines{std::string(assignments)};
    for (std::string line; std::getline(lines, line);) {
        if (sdk::trim(line, sdk::Launcher_whitespace::ASCII).empty()) continue;
        const auto assignment = sdk::environment_assignment(line, sdk::Launcher_whitespace::ASCII);
        if (!assignment || !known(assignment->first)) return false;
    }
    return true;
}

bool absent(const fs::path& path)
{
    std::error_code error;
    return !fs::exists(path, error) && !error;
}

bool has_no_tool_modules(const fs::path& directory)
{
    if (absent(directory)) return true;
    std::error_code error;
    fs::directory_iterator entries(directory, error);
    const fs::directory_iterator end;
    for (; !error && entries != end; entries.increment(error)) {
        const auto extension = sdk::ascii_lowercase(entries->path().extension().string());
        if (extension == ".js" || extension == ".ts") return false;
    }
    return !error;
}

// Pure mode skips config plugins. OpenCode 1.18.33 still imports tool modules
// from every config directory and reads the first global AGENTS.md.
bool profile_files_admitted(const sdk::Environment& env)
{
    const auto path = [&](std::string_view name) {
        return fs::u8path(sdk::environment_value(env, name));
    };
#ifdef _WIN32
    const auto home = path("USERPROFILE");
#else
    const auto home = path("HOME");
#endif
    const auto xdg = path("XDG_CONFIG_HOME");
    const auto config = (xdg.empty() ? home / ".config" : xdg) / "opencode";
    if (!home.is_absolute() || !config.is_absolute() || !absent(config / "AGENTS.md")) return false;
    const auto override_home = path("OPENCODE_TEST_HOME");
    const auto override_config = path("OPENCODE_CONFIG_DIR");
    for (const auto& dir : {config, (override_home.empty() ? home : override_home) / ".opencode", override_config}) {
        if (!dir.empty() && (!dir.is_absolute() || !has_no_tool_modules(dir / "tool") ||
            !has_no_tool_modules(dir / "tools"))) return false;
    }
    return true;
}

bool bundled_model_libraries(const Json& value)
{
    if (value.is_array()) return std::ranges::all_of(value.as_array(), bundled_model_libraries);
    if (!value.is_object()) return true;
    for (const auto& entry : value.as_object()) {
        if (entry.name == "npm") {
            if (!entry.value.is_string()) return false;
            const auto& name = entry.value.as_string();
            if (name != "@ai-sdk/openai" && name != "@ai-sdk/openai-compatible" && name != "@ai-sdk/anthropic" &&
                name != "@ai-sdk/google" && name != "@ai-sdk/azure" && name != "@ai-sdk/groq" &&
                name != "@ai-sdk/mistral" && name != "@ai-sdk/xai" && name != "@openrouter/ai-sdk-provider") return false;
        }
        if (!bundled_model_libraries(entry.value)) return false;
    }
    return true;
}

bool admitted_config(const Json& config, const std::optional<std::string>& model)
{
    const auto& agent = member(member(config, "agent"), k_agent);
    const auto& permission = member(agent, "permission");
    const auto& options = member(agent, "options");
    const auto& references = member(config, "references");
    if (!references.is_null() && (!references.is_object() || !references.as_object().empty())) return false;
    if (!sdk::json::only_keys(agent, {"mode", "permission", "options", "model"}) ||
        text(agent, "mode") != "primary" || text(agent, "model") != model.value_or(std::string{}) ||
        !sdk::json::exact_keys(permission, {"*"}) || text(permission, "*") != "deny" ||
        (!options.is_null() && (!options.is_object() || !options.as_object().empty())) ||
        text(config, "share") != "disabled" || !empty_array(member(config, "instructions")) ||
        !bundled_model_libraries(member(config, "provider"))) return false;
    const auto& mcp = member(config, "mcp");
    if (mcp.is_null()) return true;
    return mcp.is_object() && std::ranges::all_of(mcp.as_object(), [](const auto& entry) {
        const auto& enabled = member(entry.value, "enabled");
        return enabled.is_boolean() && !enabled.as_boolean();
    });
}

bool denied_tools(const Json& agent)
{
    const auto& tools = member(agent, "tools");
    return text(agent, "name") == k_agent && text(agent, "mode") == "primary" &&
        tools.is_object() && !tools.as_object().empty() &&
        std::ranges::all_of(tools.as_object(), [](const auto& tool) {
            return tool.value.is_boolean() && !tool.value.as_boolean();
        });
}

std::optional<std::string> author_config(const sdk::Environment& env, const std::optional<std::string>& model)
{
    const auto raw = sdk::environment_value(env, "OPENCODE_CONFIG_CONTENT");
    auto config = raw.empty() ? std::optional(Json::object()) : sdk::json::parse(raw);
    if (!config || !config->is_object()) return {};
    auto agents = member(*config, "agent");
    if (agents.is_null()) agents = Json::object();
    if (!agents.is_object() || agents.find(k_agent)) return {};
    auto agent = sdk::json::object({{"mode", Json("primary")},
        {"permission", sdk::json::object({{"*", Json("deny")}})}});
    if (model) agent.set("model", Json(*model));
    agents.set(k_agent, std::move(agent));
    config->set("agent", std::move(agents));
    config->set("share", Json("disabled"));
    config->set("snapshot", Json(false));
    return sdk::serialize_json(*config);
}

void prepare(sdk::Invocation_context& call, const sdk::Structured_prepare_request& request)
{
    const auto& context = request.context;
    const auto reject = [&] { call.fail(sdk::Result_status::UNAVAILABLE, "opencode_authoring_unavailable"); };
    if (!context.launch_environment || context.account_context || context.profile_storage) return reject();
    std::error_code error;
    const auto working_directory = fs::u8path(context.working_directory);
    if (!working_directory.is_absolute() || !fs::is_directory(working_directory, error) ||
        error || !fs::is_empty(working_directory, error) || error) return reject();
    const auto started = std::chrono::steady_clock::now();
    const auto options = delegated_launcher_options(sdk::literal_argv(context.launcher.script_body));
    if (!options || options->auto_approve) return reject();
    const auto& frozen = context.launch_environment->effective_environment;
    auto env = sdk::environment_pairs(frozen);
    if (!declared_launcher_inputs(env, context.launcher.environment_text) || !profile_files_admitted(env)) return reject();
    auto launch = verified_launch(context.launcher, context.launcher_argv, frozen, context.working_directory,
        false, call, started, true);
    if (!launch || launch->version != "1.18.33" || launch->argv.size() != 1 || !launch->environment.empty()) return reject();
    const auto content = author_config(env, options->model);
    if (!content) return reject();
    sdk::Structured_prepare_result result;
    auto& process = result.process;
    process.environment = {{"OPENCODE_CONFIG_CONTENT", *content}, {"OPENCODE_PURE", "1"},
        {"OPENCODE_DISABLE_PROJECT_CONFIG", "1"}, {"OPENCODE_DISABLE_DEFAULT_PLUGINS", "1"},
        {"OPENCODE_DISABLE_CLAUDE_CODE", "1"}, {"OPENCODE_DISABLE_EXTERNAL_SKILLS", "1"},
        {"OPENCODE_DISABLE_LSP_DOWNLOAD", "1"}, {"OPENCODE_DISABLE_AUTOUPDATE", "1"},
        {"OPENCODE_DISABLE_AUTOCOMPACT", "1"}, {"PWD", context.working_directory}};
    for (const auto& [name, value] : env) {
        if (std::ranges::none_of(process.environment, [&](const auto& pair) {
            return sdk::ascii_lowercase(pair.first) == sdk::ascii_lowercase(name);
        })) process.environment_names.push_back(name);
    }
    for (const auto& [name, value] : process.environment) sdk::set_environment(env, name, value);
    const auto native_debug = [&](std::initializer_list<std::string> command) -> std::optional<Json> {
        auto argv = launch->argv;
        argv.insert(argv.end(), {"--pure", "debug"});
        argv.insert(argv.end(), command);
        const auto output = sdk::probe(argv, env, context.working_directory, remaining_probe_ms(call, started), call);
        return output ? sdk::json::parse(output->output) : std::nullopt;
    };
    const auto final = native_debug({"config"});
    if (!final || !admitted_config(*final, options->model)) return reject();
    // Only query the native tool registry after its startup inputs have been
    // verified. Without a --tool argument this command reports permissions.
    const auto resolved = native_debug({"agent", k_agent});
    if (!resolved || !denied_tools(*resolved)) return reject();
#ifdef _WIN32
    process.executable = "helpers/opencode_launch_bridge.exe";
#else
    process.executable = "helpers/opencode_launch_bridge";
#endif
    process.argv = std::move(launch->argv);
    process.argv.insert(process.argv.end(), {"--pure", "run", "--format", "json", "--agent", k_agent,
        "--title", "Workflow authoring", "--dir", context.working_directory});
    if (options->model) process.argv.insert(process.argv.end(), {"--model", *options->model});
    process.stdin_behavior = sdk::Stdin_behavior::PAYLOAD;
    process.stdin_payload = request.prompt;
    result.inspection.protocol_state = "opencode-authoring-1";
    sdk::Contract_failure failure;
    auto payload = sdk::encode_structured_prepare_result(result);
    if (!sdk::validate_structured_process_proposal(process, &failure) || !sdk::validate_session_exchange_payload(payload, &failure)) return reject();
    call.succeed(std::move(payload));
}

sdk::Structured_inspect_result inspect(const sdk::Structured_inspection& observation)
{
    sdk::Structured_inspect_result result;
    result.complete = true;
    result.failure_reason = "opencode_response_invalid";
    if (!observation.final || observation.timed_out || observation.exit_code != 0 ||
        observation.protocol_state != "opencode-authoring-1") return result;
    std::string session;
    std::string message;
    std::string answer;
    std::string_view stream = observation.stdout_bytes;
    bool started = false;
    bool stopped = false;
    while (!stream.empty()) {
        const auto end = stream.find('\n');
        const auto event = sdk::json::parse(stream.substr(0, end));
        if (!event || !event->is_object()) return result;
        stream = end == stream.npos ? std::string_view{} : stream.substr(end + 1);
        const auto type = text(*event, "type");
        if (type == "error") {
            const auto name = text(member(*event, "error"), "name");
            result.failure_reason = name == "ProviderAuthError" ? "authentication_required" : "opencode_response_error";
            return result;
        }
        const auto& part = member(*event, "part");
        const auto id = text(*event, "sessionID");
        const auto author = text(part, "messageID");
        if (id.empty() || author.empty() || text(part, "sessionID") != id ||
            (!session.empty() && (id != session || author != message)) || stopped) return result;
        session = id;
        message = author;
        if (type == "step_start" && text(part, "type") == "step-start" && !started) started = true;
        else if (type == "text" && text(part, "type") == "text" && started) answer += text(part, "text");
        else if (type == "step_finish" && text(part, "type") == "step-finish" && started && text(part, "reason") == "stop") stopped = true;
        else return result;
    }
    if (stopped && !answer.empty()) {
        result.failure_reason.reset();
        result.result_bytes = std::move(answer);
    }
    return result;
}

} // namespace

void handle_authoring(sdk::Invocation_context& call)
{
    const auto& request = call.request();
    sdk::Contract_failure failure;
    if (request.operation == "prepare") {
        if (const auto input = sdk::decode_authoring_prepare_request(request.payload, &failure)) return prepare(call, *input);
    }
    else if (request.operation == "inspect") {
        if (const auto input = sdk::decode_authoring_inspect_request(request.payload, &failure)) {
            auto payload = sdk::encode_structured_inspect_result(inspect(input->inspection));
            if (sdk::validate_session_exchange_payload(payload, &failure)) return call.succeed(std::move(payload));
        }
    }
    else return call.reject_unknown_operation();
    call.fail(failure.reason_code == sdk::reason::k_purpose_unavailable ? sdk::Result_status::UNAVAILABLE :
        sdk::Result_status::FAILED, failure.reason_code);
}

} // namespace opencode
