#include "opencode_launcher.h"
#include "opencode_launcher_approval.h"
#include "opencode_preparation.h"
#include "opencode_acp.h"

#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, const char* name)
{
    if (!condition) { std::cerr << "FAIL " << name << '\n'; ++failures; }
}

void launcher_approval_contract()
{
    const auto& rules = opencode::launcher_approval_rules();
    const auto yolo = addon::Launcher_approval_action::YOLOFY;
    const auto undo = addon::Launcher_approval_action::UNYOLOFY;
    const auto changed = addon::Launcher_approval_disposition::CHANGED;
    const auto windows = addon::transform_launcher_approval({
        {"opencode", "powershell", ""}, "windows-x86_64", yolo}, rules);
    check(windows.disposition == changed && windows.script_body == "opencode --auto",
        "discovered Windows OpenCode launcher supports Yolofy");
    const std::string options = R"( --agent build --prompt "keep --yolo and --auto as text")";
    const std::string configured = "opencode" + options;
    const auto comparison = addon::launcher_approval_comparison_key(
        {{configured, "direct", "PROFILE=work"}, "linux-x86_64"}, rules);
    for (const auto* alias : {"--yolo", "--dangerously-skip-permissions"}) {
        const std::string source = std::string("opencode ") + alias + options;
        const auto enabled = addon::transform_launcher_approval(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64", yolo}, rules);
        check(enabled.disposition == changed && enabled.script_body == "opencode --auto" + options,
            "OpenCode bypass aliases normalize while custom options survive");
        const auto repeated = addon::transform_launcher_approval(
            {{enabled.script_body, "direct", "PROFILE=work"}, "linux-x86_64", yolo}, rules);
        check(repeated.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
            repeated.script_body == enabled.script_body, "OpenCode alias normalization is idempotent");
        const auto disabled = addon::transform_launcher_approval(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64", undo}, rules);
        check(disabled.disposition == changed && disabled.script_body == configured,
            "Unyolofy removes the OpenCode bypass alias and preserves prompt text");
        const auto removed_again = addon::transform_launcher_approval(
            {{disabled.script_body, "direct", "PROFILE=work"}, "linux-x86_64", undo}, rules);
        check(removed_again.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
            removed_again.script_body == configured, "OpenCode alias removal is idempotent");
        const auto alias_comparison = addon::launcher_approval_comparison_key(
            {{source, "direct", "PROFILE=work"}, "linux-x86_64"}, rules);
        check(comparison.supported && alias_comparison.supported && comparison.key == alias_comparison.key,
            "OpenCode aliases retain the same rescan identity");
    }
    for (const auto* negation : {"--no-auto", "--no-yolo", "--no-dangerously-skip-permissions"}) {
        const std::string source = std::string("opencode ") + negation + options;
        const auto enabled = addon::transform_launcher_approval(
            {{source, "direct", ""}, "linux-x86_64", yolo}, rules);
        check(enabled.disposition == changed && enabled.script_body == "opencode --auto" + options,
            "Yolofy replaces the understood OpenCode bypass negation");
        const auto disabled = addon::transform_launcher_approval(
            {{source, "direct", ""}, "linux-x86_64", undo}, rules);
        check(disabled.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
            disabled.script_body == source, "Unyolofy preserves an explicit OpenCode bypass negation");
    }
    const auto run = addon::transform_launcher_approval({
        {R"(opencode run --model provider/model "explain --auto")", "direct", ""},
        "linux-x86_64", yolo}, rules);
    check(run.disposition == changed &&
        run.script_body == R"(opencode run --auto --model provider/model "explain --auto")",
        "OpenCode run receives auto mode while its model and prompt remain unchanged");
    const auto removed = addon::transform_launcher_approval({
        {run.script_body, "direct", ""}, "linux-x86_64", undo}, rules);
    check(removed.disposition == changed &&
        removed.script_body == R"(opencode run --model provider/model "explain --auto")",
        "Unyolofy preserves flag-like text in an OpenCode prompt");
    const std::string prompt = R"(opencode --prompt="--auto")";
    const auto literal = addon::transform_launcher_approval({
        {prompt, "direct", ""}, "linux-x86_64", undo}, rules);
    check(literal.disposition == addon::Launcher_approval_disposition::UNCHANGED &&
        literal.script_body == prompt, "OpenCode attached prompt option operands are not flags");
    // Oracle: the standalone yargs parser embedded in OpenCode 1.18.33, with
    // its TUI option declarations: --prompt --auto enables auto; --auto false disables it.
    for (const auto* script : {
            R"(opencode --prompt "--auto")", "opencode --auto false",
            "opencode --yolo true", "opencode --continue false",
            "opencode --continue --auto false", "opencode --session --auto session-id"})
    {
        for (const auto action : {yolo, undo}) {
            const auto result = addon::transform_launcher_approval({
                {script, "direct", ""}, "linux-x86_64", action}, rules);
            const auto unsupported_key = addon::launcher_approval_comparison_key({
                {script, "direct", ""}, "linux-x86_64"}, rules);
            check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
                result.script_body.empty() && !unsupported_key.supported,
                "ambiguous native OpenCode option operands are preserved as unsupported");
        }
    }
    for (const auto* script : {"opencode false", "opencode run false"}) {
        const auto result = addon::transform_launcher_approval({
            {script, "direct", ""}, "linux-x86_64", yolo}, rules);
        check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            result.script_body.empty(), "Yolofy cannot turn an OpenCode positional prompt into a boolean operand");
    }
    const auto delimited = addon::transform_launcher_approval({
        {"opencode -- false", "direct", ""}, "linux-x86_64", yolo}, rules);
    check(delimited.disposition == changed && delimited.script_body == "opencode --auto -- false",
        "an explicit delimiter preserves the OpenCode false prompt while enabling auto mode");
    // OpenCode's run builder declares --title as a string with an optional
    // value: removing the intervening approval flag would capture the prompt.
    const auto title_prompt = addon::transform_launcher_approval({
        {"opencode run --title --auto prompt", "direct", ""}, "linux-x86_64", undo}, rules);
    check(title_prompt.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
        title_prompt.script_body.empty(), "Unyolofy cannot turn an OpenCode prompt into a title");
    for (const auto action : {yolo, undo}) {
        const auto result = addon::transform_launcher_approval({
            {"opencode run --title --yolo prompt", "direct", ""}, "linux-x86_64", action}, rules);
        check(result.disposition == addon::Launcher_approval_disposition::UNSUPPORTED &&
            result.script_body.empty(), "replacing OpenCode approval aliases preserves title operand ownership");
    }
    const std::pair<const char*, const char*> title_cases[]{
        {"opencode run --title --auto -- prompt", "opencode run --title -- prompt"},
        {"opencode run --title=label --auto prompt", "opencode run --title=label prompt"},
        {"opencode run --title label --auto prompt", "opencode run --title label prompt"},
    };
    for (const auto& [before, after] : title_cases) {
        const auto result = addon::transform_launcher_approval({
            {before, "direct", ""}, "linux-x86_64", undo}, rules);
        check(result.disposition == changed && result.script_body == after,
            "delimiters and bound OpenCode title values keep their native operand ownership");
    }
    const auto management = addon::transform_launcher_approval({
        {"opencode auth login", "direct", ""}, "linux-x86_64", yolo}, rules);
    check(management.disposition == addon::Launcher_approval_disposition::UNSUPPORTED,
        "OpenCode account management remains unchanged");
}

class Context final : public addon::Invocation_context
{
public:
    bool cancelled = false;
    std::optional<std::chrono::steady_clock::time_point> cancellation_time;
    addon::Invoke_request invocation;
    std::optional<addon::Json_value> payload;
    std::string reason;
    const addon::Invoke_request& request() const override { return invocation; }
    bool cancel_requested() const override
    {
        return cancelled || (cancellation_time && std::chrono::steady_clock::now() >= *cancellation_time);
    }
    bool wait_for_cancel(int timeout_ms) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        return cancel_requested();
    }
    void report_progress(std::uint64_t, std::optional<std::uint64_t>, std::string) override {}
    void succeed(addon::Json_value value) override { payload = std::move(value); }
    void fail(addon::Result_status, std::string value, std::string) override { reason = std::move(value); }
    bool finished() const override { return false; }
};

std::string path_string(const fs::path& path)
{
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

const addon::Json_value* environment_member(const addon::Json_value& environment, std::string_view name)
{
#ifdef _WIN32
    const auto expected = addon::lower(std::string(name));
    for (const auto& member : environment.as_object()) {
        if (addon::lower(member.name) == expected) return &member.value;
    }
    return nullptr;
#else
    return environment.find(name);
#endif
}

void set_host_directory(const std::string& name, const fs::path& directory)
{
#ifdef _WIN32
    const std::wstring wide(name.begin(), name.end());
    check(SetEnvironmentVariableW(wide.c_str(), directory.empty() ? nullptr : directory.c_str()) != 0,
        "set host directory in the native environment");
#else
    if (directory.empty()) unsetenv(name.c_str());
    else setenv(name.c_str(), directory.c_str(), 1);
#endif
}

// Apply the same asset scanner and substitution as the host before OpenCode decodes the JSON.
std::optional<addon::Json_value> applied_config(const addon::Environment& environment)
{
    const auto text = addon::environment_value(environment, "OPENCODE_CONFIG_CONTENT");
    const auto tokens = addon::scan_asset_tokens(text, nullptr);
    if (!tokens) return std::nullopt;
    const auto expanded = addon::substitute_asset_tokens(text, *tokens,
        {{"assets/logonomic_opencode_plugin.mjs", "managed-plugin.mjs"}});
    return expanded ? addon::parse_json(*expanded, {}, nullptr) : std::nullopt;
}

} // namespace

int main(int argc, char** argv)
{
    // The test's own native image is copied into a stock installation. This
    // exercises actual probe startup/output/cancellation without an installed
    // vendor CLI or network access.
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        if (!std::getenv("OPENCODE_FIXTURE_NO_PROMPT")) {
            // Native OpenCode 1.18.33 writes successful --help output to stderr.
            auto& output = std::getenv("OPENCODE_FIXTURE_HELP_STDOUT") ? std::cout : std::cerr;
            output << "Options:\n  --prompt <text>  Initial interactive message\n";
        }
        return 0;
    }
    if (argc > 1 && std::any_of(argv + 1, argv + argc, [](const char* value) {
            return std::string_view(value).starts_with("--prompt="); }))
    {
        auto arguments = addon::Json_value::array();
        for (int index = 1; index < argc; ++index) {
            arguments.push_back(addon::Json_value(argv[index]));
        }
        auto observation = addon::Json_value::object();
        observation.set("argv", std::move(arguments));
        const char* setting = std::getenv("OPENCODE_FIXTURE_SHIM_SETTING");
        observation.set("shim_setting", addon::Json_value(setting ? setting : ""));
        std::cout << addon::serialize_json(observation) << '\n';
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
#ifndef _WIN32
        if (const char* custody = std::getenv("OPENCODE_FIXTURE_CUSTODY")) {
            if (custody != std::to_string(getpgrp()) + ":" + std::to_string(getsid(0))) return 8;
        }
#endif
        const char* mode = std::getenv("OPENCODE_FIXTURE_MODE");
        if (mode && std::string_view(mode) == "wait") std::this_thread::sleep_for(std::chrono::seconds(30));
        if (mode && std::string_view(mode) == "overflow") { std::cout << std::string(70000, 'a'); return 0; }
        if (mode && std::string_view(mode) == "failed") return 1;
        if (mode && std::string_view(mode) == "frozen" && std::getenv("TEMP")) return 1;
        if (mode && std::string_view(mode) == "installed-version") {
            std::ifstream version(addon::path_from_utf8(argv[0]).parent_path() / "fixture.version");
            std::cout << version.rdbuf();
            return version ? 0 : 1;
        }
        const char* version = std::getenv("OPENCODE_FIXTURE_VERSION");
        std::cout << (version ? version : "1.18.25") << '\n';
        return 0;
    }
    launcher_approval_contract();
    for (const char* script : {
            "opencode --model openai/gpt-5", "npx -y opencode-ai@latest --version",
            "opencode2 --standalone", "npx --yes @opencode/cli@2.1.4", "bunx @opencode/cli",
            "bunx --bun opencode-ai", "call opencode.cmd", "exec opencode",
            "\"C:/Program Files/OpenCode/opencode.exe\"", "pwsh -Command \"opencode\"",
            "# comment\nopencode", "echo preparing && opencode"})
    {
        check(opencode::recognize_launcher(script).has_value(), script);
    }
    for (const char* script : {
            "echo \"ask opencode later\"", "npx @opencode-ai/cli@latest",
            "npx opencode-ai-tools", "opencode-ai --version", "opencode-helper.cmd",
            "npx --package opencode-ai prettier", "npx -p opencode-ai prettier",
            "bunx --unknown opencode-ai", "rem opencode", "// opencode"})
    {
        check(!opencode::recognize_launcher(script), script);
    }
    const auto environment = opencode::recognize_launcher(
        "$env:FIRST='one'; export SECOND=two\nOPENCODE_CONFIG_CONTENT='{\"theme\": \"dark\"}' opencode");
    check(environment && environment->environment.size() == 3 &&
        environment->environment.back().first == "OPENCODE_CONFIG_CONTENT" &&
        environment->environment.back().second == "{\"theme\": \"dark\"}", "assignment preservation");
    const auto unattached = opencode::recognize_launcher("UNATTACHED=value; opencode");
    check(unattached && unattached->environment.empty(), "bare assignment stays unbound");
    for (const char* script : {
            "export EXTRA=one unsupported; opencode", "export -p; opencode",
            "export EXTRA='unterminated; opencode", "export EXTRA=$(command); opencode"})
    {
        check(!opencode::recognize_launcher(script), "unsupported export operands are not assignments");
    }
    const auto ordinary = opencode::recognize_launcher("exported EXTRA=one; opencode");
    check(ordinary && ordinary->environment.empty(), "export requires an exact command word");
    check(addon::literal_argv("\"C:/a b/opencode.exe\" --model test") ==
        std::vector<std::string>{"C:/a b/opencode.exe", "--model", "test"}, "literal host argv");

    check(opencode::version_is_at_least("1.18.25", 1, 18, 25), "minimum admitted");
    check(opencode::version_is_at_least("2.0.0-preview.1", 1, 18, 25), "new generation admitted");
    check(opencode::version_is_at_least("1.18.25+build.7", 1, 18, 25), "build metadata admitted");
    check(!opencode::version_is_at_least("1.18.25-rc.1", 1, 18, 25), "minimum prerelease refused");
    check(!opencode::version_is_at_least("1.018.25", 1, 18, 25), "non-semver refused");
    check(opencode::parse_version_output("OpenCode CLI version 1.18.25\n", "") == "1.18.25", "labelled output");
    check(opencode::parse_version_output("v1.18.25\n", "warning: startup\n") == "1.18.25", "bare version with warning");
    check(opencode::parse_version_output("opencode 1.18.25\n", "opencode 1.18.26\n").empty(), "contradictory streams");
    check(opencode::parse_version_output("opencode 1.18.25\nopencode 1.18.26\n", "").empty(), "contradictory labels");
    check(opencode::parse_version_output(std::string("1.18.25\0", 8), "").empty(), "NUL output refused");
    check(opencode::parse_version_output(std::string(65537, 'a'), "").empty(), "output bound");
    check(opencode::parse_version_output(std::string("\xff", 1), "1.18.25").empty(), "invalid UTF-8 stream");

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = fs::temp_directory_path() / ("opencode-launcher-" + std::to_string(unique));
    fs::create_directories(root);
    const auto empty_environment = addon::Json_value::object();
    const auto native_tui = opencode::tui_invocation({"opencode2 --auto", "direct", ""},
        {"/opt/opencode2", "--auto"}, empty_environment, path_string(root));
    check(native_tui == std::optional(std::vector<std::string>{"/opt/opencode2", "--auto", "--standalone"}),
        "native TUI preparation isolates the server without changing approval policy");
    const auto private_tui = opencode::tui_invocation({"opencode --standalone", "direct", ""},
        {"opencode", "--standalone"}, empty_environment, path_string(root));
    check(private_tui == std::optional(std::vector<std::string>{"opencode", "--standalone"}),
        "an explicitly private native TUI is not rewritten twice");
    const auto shell_tui = opencode::tui_invocation({"opencode --auto", "powershell", ""},
        {"powershell.exe", "-NoLogo", "-Command", "opencode --auto"}, empty_environment, path_string(root));
    check(shell_tui == std::optional(std::vector<std::string>{
            "powershell.exe", "-NoLogo", "-Command", "opencode --auto --standalone"}),
        "PowerShell keeps its invocation boundary while selecting the private native server");
    for (const auto* script : {
            "opencode run", "opencode serve", "opencode --server https://example.org", "opencode --help",
            "opencode --wizard", "opencode --model provider/model", "opencode .."})
    {
        check(!opencode::tui_invocation({script, "direct", ""}, addon::literal_argv(script),
            empty_environment, path_string(root)), "unsupported native interfaces stay unchanged");
    }
    check(!opencode::tui_invocation({"opencode; echo changed", "powershell", ""},
        {"powershell.exe", "-NoLogo", "-Command", "opencode; echo changed"}, empty_environment, path_string(root)),
        "native preparation never reconstructs a compound shell command");
#ifdef _WIN32
    const auto program = root / "opencode.exe";
#else
    const auto program = root / "opencode";
#endif
    fs::copy_file(fs::absolute(addon::path_from_utf8(argv[0])), program);
    addon::Discover_request discovery;
#ifdef _WIN32
    discovery.platform_id = "windows-x86_64";
#else
    discovery.platform_id = addon::host_platform_id();
#endif
    discovery.ambient_environment.set("PATH", addon::Json_value(path_string(root)));
    const auto discovered = opencode::discover_launcher(discovery);
    check(discovered && discovered->script_body == "opencode" && discovered->runner_key == "direct",
        "discover installed CLI with native runner");
    discovery.ambient_environment.set("PATH", addon::Json_value("relative-directory"));
    check(!opencode::discover_launcher(discovery), "discover does not resolve relative PATH without cwd");
    discovery.ambient_environment.set("PATH", addon::Json_value(""));
    discovery.default_home_fallback = path_string(root);
    check(!opencode::discover_launcher(discovery), "discover does not invent home search roots");
#ifndef _WIN32
    fs::permissions(program, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    discovery.ambient_environment.set("PATH", addon::Json_value(path_string(root)));
    check(!opencode::discover_launcher(discovery), "discover requires executable permission");
    fs::permissions(program, fs::perms::owner_all, fs::perm_options::replace);
#endif
    addon::Json_value effective = addon::Json_value::object();
    for (const auto& [name, value] : addon::current_process_environment()) effective.set(name, addon::Json_value(value));
    effective.set("OPENCODE_FIXTURE_VERSION", addon::Json_value("1.18.25"));
    const auto program_text = path_string(program);
    addon::Launcher_request launcher{"\"" + program_text + "\"", "direct", ""};
    Context context;
#ifndef _WIN32
    effective.set("OPENCODE_FIXTURE_CUSTODY",
        addon::Json_value(std::to_string(getpgrp()) + ":" + std::to_string(getsid(0))));
#endif
    const auto launch = opencode::verified_launch(launcher, {program_text}, effective, path_string(root), true, context);
    check(launch && launch->version == "1.18.25" && launch->argv.size() == 4 &&
        launch->argv[1] == "acp" && launch->argv[2] == "--cwd", "real native ACP probe");
    addon::Delegated_prepare_request delegated;
    delegated.context.launcher = launcher;
    delegated.context.launcher_argv = {program_text};
    delegated.context.working_directory = path_string(root);
    delegated.context.launch_environment = addon::Launch_environment{};
    delegated.context.launch_environment->platform_id = discovery.platform_id;
    auto& task_environment = delegated.context.launch_environment->effective_environment;
    for (const auto* name : {"PATH", "SystemRoot", "TEMP", "TMP", "OPENCODE_FIXTURE_VERSION"}) {
        if (const auto* value = environment_member(effective, name)) {
            task_environment.set(name, *value);
        }
    }
    delegated.prompt = "Implement the selected task.";
    delegated.control.task_id = "catalog-task";
    // The host supplies its paths in Qt's '/'-separated form on every platform.
    const auto host_path = [](const fs::path& path) {
        const auto bytes = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    delegated.control.private_artifact_root = host_path(root / "task private");
    delegated.control.connection = addon::Delegated_connection{
        host_path(root / "host relay"), host_path(root / "task private/.logonomic/task-servers.json")};
    delegated.control.servers.push_back({"control", program_text, {"task_status"},
        {{"VNM_CONTROL_TOKEN", "opencode-host-authority-canary"}}});
    Context prepared;
    prepared.invocation = {"connection", {"delegated-session", 3}, "prepare",
        addon::encode_delegated_prepare_request(delegated)};
    opencode::handle_preparation(prepared);
    addon::Contract_failure failure;
    const auto result = prepared.payload
        ? addon::decode_delegated_prepare_result(*prepared.payload, &failure) : std::nullopt;
    if (!result) {
        std::cerr << "Delegated preparation: " << prepared.reason << ' ' << failure.reason_code << '\n';
    }
    check(result && result->acknowledgment.connection_version == addon::k_delegated_connection_version,
        "actual preparation acknowledges relay configuration");
    if (result) {
        const auto state = addon::parse_json(result->inspection.protocol_state, {}, nullptr);
        const auto& server = state->find("mcp_servers")->as_array().front();
        const auto& attestation = state->find("attestations")->as_array().front();
        const auto& args = server.find("args")->as_array();
        check(server.find("command")->as_string() == delegated.control.connection->relay_program &&
            server.find("env")->as_array().empty() && args.size() == 8 &&
            args[0].as_string() == "--descriptor" && args[1].as_string() == delegated.control.connection->descriptor_path &&
            args[2].as_string() == "--server" && args[3].as_string() == "control" &&
            args[4].as_string() == "--attestation-path" && args[5] == *attestation.find("path") &&
            args[6].as_string() == "--attestation-nonce" && args[7] == *attestation.find("nonce"),
            "relay arguments preserve exact native readiness path and nonce");
        // The host refuses a receipt path that is not clean or contains '\'.
        check(attestation.find("path")->as_string() == delegated.control.private_artifact_root + "/mcp/server-" +
                attestation.find("nonce")->as_string() + ".ready",
            "readiness path keeps the host's separator form below the private root");
        check(result->private_artifacts.size() == 1 &&
            result->private_artifacts.front().relative_path.ends_with(".connection"),
            "ordinary artifact creates readiness parent without reserving ready file");
        // Oracle: OpenCode's configuration documentation - OPENCODE_CONFIG and
        // OPENCODE_CONFIG_DIR select the configuration whose permission rules a
        // delegated run keeps (the owner's rule, 2026-09-28).
        const auto& names = result->process.environment_names;
        check(std::find(names.begin(), names.end(), "OPENCODE_CONFIG") != names.end() &&
            std::find(names.begin(), names.end(), "OPENCODE_CONFIG_DIR") != names.end(),
            "the user's configuration selectors reach the delegated OpenCode");
        check(std::find(names.begin(), names.end(), "OPENCODE_CONFIG_CONTENT") == names.end(),
            "the package's own inline configuration is not also inherited");
        check(addon::validate_structured_process_proposal(result->process, &failure),
            "the host admits the delegated process proposal");
        check(!fs::exists(addon::path_from_utf8(attestation.find("path")->as_string())),
            "preparation leaves the ready file absent for host reservation");
        check(result->inspection.protocol_state.find("opencode-host-authority-canary") == std::string::npos &&
            addon::serialize_json(*prepared.payload).find("opencode-host-authority-canary") == std::string::npos &&
            result->private_artifacts.front().content.find("opencode-host-authority-canary") == std::string::npos,
            "host credentials cannot enter continuation or private artifacts");
        addon::Structured_inspection input;
        input.protocol_state = result->inspection.protocol_state;
        input.stdout_bytes = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":1,"
            "\"agentInfo\":{\"name\":\"OpenCode\",\"version\":\"1.18.25\"}}}\n";
        const auto next = opencode::inspect_acp(input);
        check(next.stdin_bytes && next.stdin_bytes->find("--attestation-path") != std::string::npos &&
            next.stdin_bytes->find("opencode-host-authority-canary") == std::string::npos,
            "actual native session request carries nonsecret relay configuration");
    }
    // These settings are data even when their spelling matches the host's asset grammar.
    const auto user_config = *addon::parse_json(
        R"({"permission":{"bash":"deny","edit":"ask"},"instructions":["{{file}}"],"plugins":[{"package":"existing-server-plugin","options":{"{{":"{{asset:assets/logonomic_opencode_plugin.mjs}}"}}]})", {}, nullptr);
    const auto inline_settings = addon::Json_value(addon::serialize_json(user_config));
    auto automatic = delegated;
    const auto yolofied = addon::transform_launcher_approval({
        automatic.context.launcher, discovery.platform_id, addon::Launcher_approval_action::YOLOFY},
        opencode::launcher_approval_rules());
    check(yolofied.disposition == addon::Launcher_approval_disposition::CHANGED,
        "the product's Yolofy transform adds the canonical auto option");
    automatic.context.launcher.script_body = yolofied.script_body;
    automatic.context.launcher_argv.push_back("--auto");
    automatic.context.launch_environment->effective_environment.set("OPENCODE_CONFIG_CONTENT",
        inline_settings);
    Context auto_prepared;
    auto_prepared.invocation = prepared.invocation;
    auto_prepared.invocation.payload = addon::encode_delegated_prepare_request(automatic);
    opencode::handle_preparation(auto_prepared);
    const auto auto_result = auto_prepared.payload
        ? addon::decode_delegated_prepare_result(*auto_prepared.payload, &failure) : std::nullopt;
    check(auto_result.has_value(), "Yolofy keeps a verified legacy ACP launcher delegatable");
    if (auto_result) {
        const auto state = addon::parse_json(auto_result->inspection.protocol_state, {}, nullptr);
        const auto* approve = state ? state->find("auto_approve") : nullptr;
        check(approve && approve->is_boolean() && approve->as_boolean(),
            "the selected auto mode reaches the delegated permission responder");
        check(applied_config(auto_result->process.environment) == user_config,
            "the delegated host expands no user settings and keeps explicit native denies and asks");
    }
    check(!opencode::verified_launch(automatic.context.launcher, {program_text},
        automatic.context.launch_environment->effective_environment, path_string(root), true, context),
        "auto mode is not inferred when the captured invocation omits the selected flag");
    auto absent = automatic;
    const auto absent_program = path_string(root / "absent" / program.filename());
    absent.context.launcher.script_body = "\"" + absent_program + "\" --auto";
    absent.context.launcher_argv = {absent_program, "--auto"};
    Context missing;
    missing.invocation = prepared.invocation;
    missing.invocation.payload = addon::encode_delegated_prepare_request(absent);
    opencode::handle_preparation(missing);
    check(!missing.payload && missing.reason == "launcher_program_unavailable",
        "an unavailable executable retains its own refusal");
    auto newer = automatic;
    newer.context.launch_environment->effective_environment.set("OPENCODE_FIXTURE_VERSION", addon::Json_value("2.1.4"));
    Context unsupported_generation;
    unsupported_generation.invocation = prepared.invocation;
    unsupported_generation.invocation.payload = addon::encode_delegated_prepare_request(newer);
    opencode::handle_preparation(unsupported_generation);
    check(!unsupported_generation.payload && unsupported_generation.reason == "opencode_acp_generation_unavailable",
        "the distinct native TUI generation keeps its existing delegated integration limit");
    for (const auto* option : {"--model=provider/model", "--auto=false", "--no-auto"}) {
        auto unsupported = delegated;
        unsupported.context.launcher.script_body += " " + std::string(option);
        unsupported.context.launcher_argv.push_back(option);
        Context rejected;
        rejected.invocation = prepared.invocation;
        rejected.invocation.payload = addon::encode_delegated_prepare_request(unsupported);
        opencode::handle_preparation(rejected);
        check(!rejected.payload && rejected.reason == "delegated_launcher_options_unavailable",
            "unsupported options are distinct from an unavailable executable");
    }
    delegated.control.connection.reset();
    Context refused;
    refused.invocation = prepared.invocation;
    refused.invocation.payload = addon::encode_delegated_prepare_request(delegated);
    opencode::handle_preparation(refused);
    check(!refused.payload && refused.reason == "task_server_connection_unavailable",
        "missing connection refuses delegation before native probing");

    fs::create_directories(root / "profile");
    auto launch_environment = addon::Json_value::object();
    for (const auto* name : {"SystemRoot", "PATHEXT", "TEMP", "TMP", "OPENCODE_FIXTURE_VERSION"}) {
        if (const auto* value = environment_member(effective, name)) {
            launch_environment.set(name, *value);
        }
    }
    addon::Prepare_request interactive;
    interactive.launcher          = launcher;
    interactive.launcher_argv     = {program_text};
    interactive.working_directory = path_string(root);
    interactive.title_instruction = "Keep the title connected to the current task.";
    interactive.declared_assets   = {"assets/logonomic_opencode_plugin.mjs"};
    interactive.profile_storage   = addon::Profile_storage_context{1, path_string(root / "profile")};
    interactive.launch_environment = addon::Launch_environment{};
    interactive.launch_environment->platform_id = discovery.platform_id;
    interactive.launch_environment->effective_environment = launch_environment;
    auto classic_settings = user_config;
    const auto user_plugin = *addon::parse_json(
        R"(["classic-plugin",{"literal":"{{asset_json:assets/logonomic_opencode_plugin.mjs}}"}])", {}, nullptr);
    classic_settings.set("plugin", addon::Json_value(addon::Json_value::Array{user_plugin}));
    interactive.launch_environment->effective_environment.set("OPENCODE_CONFIG_CONTENT",
        addon::Json_value(addon::serialize_json(classic_settings)));
    // Script-owned inline configuration would replace the managed merge when
    // the shell runs. Every export operand must reach the admission guard.
    for (const char* prefix : {
            "export\tOPENCODE_CONFIG_CONTENT='{}'",
            "export EXTRA=one OPENCODE_CONFIG_CONTENT='{}'",
            "export \tEXTRA='two words'\tOPENCODE_CONFIG_CONTENT='{}'",
            "export EXTRA=two\\ words OPENCODE_CONFIG_CONTENT='{}'",
            "export EXTRA=two' words' OPENCODE_CONFIG_CONTENT=''"})
    {
        auto assigned = interactive;
        assigned.launcher.runner_key = addon::k_runner_bash;
        assigned.launcher.script_body = std::string(prefix) + "; " + launcher.script_body;
        assigned.launcher_argv = {"bash", "-lc", assigned.launcher.script_body};
        Context rejected;
        rejected.invocation = {"interactive", {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(assigned, 3)};
        opencode::handle_preparation(rejected);
        check(!rejected.payload && rejected.reason == "inline_configuration_assignment",
            "each script export of inline configuration keeps its specific refusal");
    }
    const auto prepare_interactive = [&interactive]() {
        Context prepared_interactive;
        prepared_interactive.invocation = {"interactive", {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(interactive, 3)};
        opencode::handle_preparation(prepared_interactive);
        if (!prepared_interactive.payload) {
            std::cerr << "Interactive preparation: " << prepared_interactive.reason << '\n';
        }
        addon::Contract_failure patch_failure;
        return prepared_interactive.payload
            ? addon::decode_launch_patch(*prepared_interactive.payload, &patch_failure, 3) : std::nullopt;
    };
    // Whether a prepared launch reports its lifecycle, with the relay switch
    // and the disposition in agreement; nullopt for anything else.
    const auto lifecycle_reported = [](const std::optional<addon::Launch_patch>& patch) -> std::optional<bool> {
        if (!patch || !patch->disposition || !patch->disposition->prepared ||
            patch->disposition->coordination_delivery)
        {
            return std::nullopt;
        }
        std::string relay;
        for (const auto& [name, value] : patch->set) {
            if (name == "OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED") relay = value;
        }
        if (relay != (patch->disposition->lifecycle_events ? "1" : "0")) return std::nullopt;
        return patch->disposition->lifecycle_events;
    };
    const auto classic_config_ok = [&](const addon::Launch_patch& patch) {
        const auto config = applied_config(patch.set);
        if (!config || !config->find("instructions") || !config->find("plugin")) return false;
        const auto& instructions = config->find("instructions")->as_array();
        const auto& plugins = config->find("plugin")->as_array();
        return !instructions.empty() && instructions.front().as_string() == "{{file}}" &&
            plugins.size() == 2 && plugins.front() == user_plugin && plugins.back().as_string() == "managed-plugin.mjs";
    };
    const auto direct = prepare_interactive();
    check(lifecycle_reported(direct) == true, "verified direct launch reports its lifecycle");
    check(direct && classic_config_ok(*direct), "classic launch expands only its own plugin asset");

    // Oracle: interactive-initial-prompt@1 and the two native TUI home routes,
    // which submit --prompt once after model selection is ready.
    const std::string initial_text = "Read the continuation brief at " +
        path_string(root / "continuation's brief.md") + " and explain the working state; retain $context & paths.";
    const addon::Initial_prompt_request first_message{initial_text, path_string(root / "continuation's brief.md")};
    const auto initial_message_delivered = [
            &interactive,
            &initial_text
        ](
            const std::optional<addon::Launch_patch>& patch,
            std::string_view expected_shim_setting = {}
        )
        {
            addon::Contract_failure acknowledgment_failure;
            if (!patch || !addon::validate_initial_prompt_acknowledgment(
                    interactive.initial_prompt, *patch, &acknowledgment_failure))
            {
                return false;
            }
            Context execution;
            const auto environment = addon::environment_pairs(interactive.launch_environment->effective_environment);
            auto invocation = *patch->argv;
            // The terminal backend resolves the image in its final environment;
            // SDK probes pass their first argument directly to CreateProcessW.
            const auto image = addon::resolve_program(invocation.front(), environment, interactive.working_directory);
            if (!image) {
                return false;
            }
            invocation.front() = addon::path_to_utf8(*image);
            const auto received = addon::probe(
                invocation, environment, interactive.working_directory, 5000, execution);
            const auto observation = received ? addon::parse_json(received->output, {}, nullptr) : std::nullopt;
            if (!observation) {
                return false;
            }
            const auto* arguments = observation->find("argv");
            const auto* setting   = observation->find("shim_setting");
            if (!arguments || !arguments->is_array() || !setting || !setting->is_string() ||
                setting->as_string() != expected_shim_setting)
            {
                return false;
            }
            std::size_t messages = 0;
            for (const auto& argument : arguments->as_array()) {
                if (argument.as_string() == "--prompt=" + initial_text) {
                    ++messages;
                }
            }
            return messages == 1;
        };
    interactive.initial_prompt = first_message;
    const auto initial_patch = prepare_interactive();
    check(initial_message_delivered(initial_patch) && lifecycle_reported(initial_patch) == true,
        "native stderr help permits delivery of the exact first message once");
    interactive.launch_environment->effective_environment.set("OPENCODE_FIXTURE_HELP_STDOUT", addon::Json_value("1"));
    const auto stdout_initial = prepare_interactive();
    check(initial_message_delivered(stdout_initial) && lifecycle_reported(stdout_initial) == true,
        "native stdout help remains compatible with delivery of the exact first message once");
    interactive.launch_environment->effective_environment.erase("OPENCODE_FIXTURE_HELP_STDOUT");
    interactive.launcher.script_body += " --model provider/model --auto";
    interactive.launcher_argv.insert(interactive.launcher_argv.end(), {"--model", "provider/model", "--auto"});
    const auto configured_initial = prepare_interactive();
    check(initial_message_delivered(configured_initial) && configured_initial->argv->size() == 5 &&
        (*configured_initial->argv)[1] == "--model" && (*configured_initial->argv)[2] == "provider/model" &&
        (*configured_initial->argv)[3] == "--auto",
        "first-message preparation keeps the selected model and approval option");
    interactive.launcher = launcher;
    interactive.launcher_argv = {program_text};
    for (const char* selection : {"--continue", "--session=ses_previous", "--prompt=earlier", "--pure", "run"}) {
        interactive.launcher.script_body = launcher.script_body + " " + selection;
        interactive.launcher_argv = {program_text, selection};
        Context rejected;
        rejected.invocation = {"initial", {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(interactive, 3)};
        opencode::handle_preparation(rejected);
        check(!rejected.payload && rejected.reason == addon::reason::k_initial_prompt_unavailable,
            "an existing conversation, prompt or alternate interface cannot receive the handoff");
    }
    interactive.launcher = launcher;
    interactive.launcher_argv = {program_text};
    for (const std::string text : {"'quoted'", "\"quoted\"", "'", "\"", "\xC2\xA0" "exit",
             "quit" "\xE2\x80\xA8", "\xEF\xBB\xBF" ":q", "exit" "\xEF\xBB\xBF"})
    {
        interactive.initial_prompt->text = text;
        Context rejected;
        rejected.invocation = {"initial", {"launch-preparation", 3}, "prepare",
            addon::encode_prepare_request(interactive, 3)};
        opencode::handle_preparation(rejected);
        check(!rejected.payload && rejected.reason == addon::reason::k_initial_prompt_unavailable,
            "native quote stripping and Unicode-normalized exit input cannot be acknowledged as messages");
    }
    interactive.initial_prompt = first_message;
    interactive.launch_environment->effective_environment.set("OPENCODE_FIXTURE_NO_PROMPT", addon::Json_value("1"));
    Context unsupported_prompt;
    unsupported_prompt.invocation = {"initial", {"launch-preparation", 3}, "prepare",
        addon::encode_prepare_request(interactive, 3)};
    opencode::handle_preparation(unsupported_prompt);
    check(!unsupported_prompt.payload && unsupported_prompt.reason == addon::reason::k_initial_prompt_unavailable,
        "a CLI without the native prompt option refuses the requested capability");
    interactive.launch_environment->effective_environment.erase("OPENCODE_FIXTURE_NO_PROMPT");
    interactive.initial_prompt.reset();

    // A resume relaunches the same verified launcher with the session selector
    // and names the session the managed plugin confirms.
    const std::string resumed_session = "ses_4f1c2a9e7b3d5f8a1c6e0b2d9f";
    addon::Resume_prepare_request resume;
    resume.context.launcher           = interactive.launcher;
    resume.context.launcher_argv      = interactive.launcher_argv;
    resume.context.working_directory  = interactive.working_directory;
    resume.context.declared_assets    = interactive.declared_assets;
    resume.context.profile_storage    = interactive.profile_storage;
    resume.context.launch_environment = interactive.launch_environment;
    resume.title_instruction          = interactive.title_instruction;
    resume.resume_record.set("provider_session_id", addon::Json_value(resumed_session));
    const auto prepare_resume = [&resume](std::string* reason) -> std::optional<addon::Resume_prepare_result> {
        Context prepared_resume;
        prepared_resume.invocation = {"resume", {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(resume, 2)};
        opencode::handle_preparation(prepared_resume);
        if (reason) *reason = prepared_resume.reason;
        addon::Contract_failure resume_failure;
        return prepared_resume.payload
            ? addon::decode_resume_prepare_result(*prepared_resume.payload, &resume_failure, 2) : std::nullopt;
    };
    // The resumed launch as the host validates it: the launcher program with
    // `arguments`, the managed configuration, the relay and the session to confirm.
    const auto resumes_with = [&](const std::vector<std::string>& arguments) {
        std::string reason;
        const auto result = prepare_resume(&reason);
        if (!result) {
            std::cerr << "Resume preparation: " << reason << '\n';
            return false;
        }
        std::string relay;
        std::string session;
        std::string config;
        for (const auto& [name, value] : result->patch.set) {
            if (name == "OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED") relay   = value;
            if (name == "OPENCODE_LOGONOMIC_RESUME_SESSION")      session = value;
            if (name == "OPENCODE_CONFIG_CONTENT")                config  = value;
        }
        addon::Contract_failure validation;
        return result->process.executable.kind == addon::Process_executable::Kind::LAUNCHER_PROGRAM &&
            result->process.argv == arguments && result->process.stdin_behavior == addon::Stdin_behavior::INHERIT &&
            addon::validate_resume_process_proposal(result->process, &validation) &&
            addon::validate_launch_patch(result->patch, resume.context.declared_assets,
                resume.context.declared_assets, &validation) &&
            addon::validate_resume_acknowledgment(resume.resume_record, result->acknowledgment, &validation) &&
            result->patch.profile_storage && result->patch.profile_storage->root == resume.context.profile_storage->root &&
            relay == "1" && session == resumed_session &&
            config.find("{{asset_json:assets/logonomic_opencode_plugin.mjs}}") != std::string::npos;
    };
    check(resumes_with({"--session", resumed_session}), "a direct launcher resumes the session");
    const auto direct_resume = prepare_resume(nullptr);
    check(direct_resume && classic_config_ok(direct_resume->patch), "classic resume expands only its own plugin asset");
    resume.context.launcher.script_body = interactive.launcher.script_body + " --model provider/model --auto";
    resume.context.launcher_argv = {program_text, "--model", "provider/model", "--auto"};
    check(resumes_with({"--model", "provider/model", "--auto", "--session", resumed_session}),
        "a resume keeps the launcher's own options");
    // Networking options, negated flags and a project argument naming this
    // directory leave the session where it is.
    for (const auto& options : {
             std::vector<std::string>{"--no-mdns"},
             std::vector<std::string>{"--cors", "http://localhost:3000", "--port", "4096"},
             std::vector<std::string>{"--mini", "--replay=false"},
             std::vector<std::string>{"--continue=false", "--no-fork", "--pure=false"},
             std::vector<std::string>{"."}})
    {
        resume.context.launcher.script_body = interactive.launcher.script_body;
        resume.context.launcher_argv = {program_text};
        for (const auto& option : options) {
            resume.context.launcher.script_body += " " + option;
            resume.context.launcher_argv.push_back(option);
        }
        auto expected = options;
        expected.insert(expected.end(), {"--session", resumed_session});
        check(resumes_with(expected), options.front().c_str());
    }
    // Options that pick another session or directory, add a turn, or drop the
    // managed plugin, cannot resume this session.
    for (const char* option : {"--continue", "--pure", "--prompt", "--session", "workspace-subdirectory", ".."}) {
        resume.context.launcher.script_body = interactive.launcher.script_body + " " + option;
        resume.context.launcher_argv = {program_text, option};
        std::string reason;
        check(!prepare_resume(&reason) && reason == "resume_launcher_unavailable", option);
    }
    // OpenCode resolves a relative project against PWD when it is set.
    resume.context.launcher.script_body = interactive.launcher.script_body + " .";
    resume.context.launcher_argv = {program_text, "."};
    auto& resume_environment = resume.context.launch_environment->effective_environment;
    resume_environment.set("PWD", addon::Json_value(path_string(root / "profile")));
    std::string pwd_reason;
    check(!prepare_resume(&pwd_reason) && pwd_reason == "resume_launcher_unavailable",
        "a project resolved against another PWD cannot resume");
    resume_environment.set("PWD", addon::Json_value(path_string(root)));
    check(resumes_with({".", "--session", resumed_session}), "a project resolved against the session's PWD");
    resume_environment.erase("PWD");

#ifdef _WIN32
    // The host runs a PowerShell launcher as `powershell -NoLogo -Command <script>`.
    // The version evidence must come from what that shell itself runs.
    const auto system_root = environment_member(effective, "SystemRoot");
    const auto powershell_directory = (system_root ? system_root->as_string() : std::string("C:\\Windows")) +
        "\\System32\\WindowsPowerShell\\v1.0";
    auto shell_environment = launch_environment;
    shell_environment.set("PATH", addon::Json_value(path_string(root) + ";" + powershell_directory));
    const addon::Launcher_request shell_launcher{"opencode", "powershell", ""};
    const std::vector<std::string> shell_argv{"powershell.exe", "-NoLogo", "-Command", "opencode"};
    check(opencode::verified_launch(shell_launcher, shell_argv, shell_environment, path_string(root), false, context)
            .has_value(),
        "interactive PowerShell launcher of a verified program");
    check(!opencode::verified_launch(shell_launcher, {"powershell.exe", "-NoLogo", "-Command", "opencode --pure"},
            shell_environment, path_string(root), false, context),
        "interactive PowerShell launcher must run the recognized script");
    const std::string quoted_script = "\"" + program_text + "\"";
    check(!opencode::verified_launch({quoted_script, "powershell", ""},
            {"powershell.exe", "-NoLogo", "-Command", quoted_script}, shell_environment, path_string(root), false, context),
        "PowerShell evaluates a quoted path as a string, not a launch");
    // An npm installation of OpenCode 1.18.25 whose selected shim runs
    // the native fixture with `shim_version`, exercising shell-owned evidence.
    const auto npm_script = [](const char* version) {
        return std::string("@echo off\r\nset OPENCODE_FIXTURE_VERSION=") + version +
            "\r\nset OPENCODE_FIXTURE_SHIM_SETTING=preserved-profile\r\n"
            "\"%~dp0\\node_modules\\opencode-ai\\bin\\opencode.exe\" %*\r\n";
    };
    const auto npm_installation = [&](const char* name, const char* shim_version) {
        const auto directory = root / name;
        fs::create_directories(directory / "node_modules" / "opencode-ai" / "bin");
        fs::copy_file(program, directory / "node_modules" / "opencode-ai" / "bin" / "opencode.exe");
        std::ofstream(directory / "node_modules" / "opencode-ai" / "package.json", std::ios::binary)
            << R"({"name":"opencode-ai","version":"1.18.25","bin":{"opencode":"bin/opencode.exe"}})";
        std::ofstream(directory / "opencode.cmd", std::ios::binary) << npm_script(shim_version);
        auto environment = shell_environment;
        environment.set("PATH", addon::Json_value(path_string(directory) + ";" + powershell_directory));
        return environment;
    };
    check(!opencode::verified_launch(shell_launcher, shell_argv, npm_installation("shim", "1.18.10"),
            path_string(root), false, context),
        "the version evidence is what the PowerShell launch runs");
    // What PowerShell runs can change without changing anything the package
    // can identify; here the shim keeps its size and time.
    const auto cached_environment = npm_installation("cached", "1.18.25");
    check(opencode::verified_launch(shell_launcher, shell_argv, cached_environment, path_string(root), false, context)
            .has_value(),
        "a PowerShell shim that runs the located installation");
    const auto cached_shim = root / "cached" / "opencode.cmd";
    const auto written = fs::last_write_time(cached_shim);
    std::ofstream(cached_shim, std::ios::binary | std::ios::trunc) << npm_script("1.18.10");
    fs::last_write_time(cached_shim, written);
    check(!opencode::verified_launch(shell_launcher, shell_argv, cached_environment, path_string(root), false, context),
        "PowerShell evidence is probed again for every launch");
    interactive.launcher      = shell_launcher;
    interactive.launcher_argv = shell_argv;
    interactive.launch_environment->effective_environment = shell_environment;
    check(lifecycle_reported(prepare_interactive()) == true, "verified PowerShell launch reports its lifecycle");
    interactive.initial_prompt = first_message;
    check(initial_message_delivered(prepare_interactive()),
        "PowerShell preserves the exact prompt through native argument binding");
    interactive.launch_environment->effective_environment = npm_installation("initial", "1.18.25");
    check(initial_message_delivered(prepare_interactive(), "preserved-profile"),
        "initial-message preparation preserves the selected npm shim's environment effects");
    interactive.initial_prompt->text = "Explain the 20% completed work.";
    check(!prepare_interactive(), "a batch shim cannot acknowledge percent expansion as verbatim text");
    const auto initial_shim = root / "initial" / "opencode.cmd";
    interactive.launcher = {"\"" + path_string(initial_shim) + "\"", "direct", ""};
    interactive.launcher_argv = {path_string(initial_shim)};
    interactive.initial_prompt = first_message;
    check(!prepare_interactive(), "a direct Windows batch file is not silently replaced by its native target");
    interactive.launcher = shell_launcher;
    interactive.launcher_argv = shell_argv;
    interactive.launch_environment->effective_environment = shell_environment;
    interactive.initial_prompt->text = "Explain the \"quoted\" brief.";
    check(!prepare_interactive(), "PowerShell cannot acknowledge text whose native quote binding changes it");
    interactive.initial_prompt.reset();
    interactive.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", "opencode --pure"};
    check(lifecycle_reported(prepare_interactive()) == false,
        "an invocation other than the recognized launcher keeps lifecycle off");
    resume.context.launcher      = shell_launcher;
    resume.context.launcher_argv = shell_argv;
    resume.context.launch_environment->effective_environment = shell_environment;
    check(resumes_with({"-NoLogo", "-Command", "opencode --session " + resumed_session}),
        "a PowerShell launcher resumes the session through its script");
    resume.context.launcher.script_body = "opencode --no-continue --fork=false --no-pure --mini --replay=false .";
    resume.context.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", resume.context.launcher.script_body};
    check(resumes_with({"-NoLogo", "-Command", resume.context.launcher.script_body + " --session " + resumed_session}),
        "a PowerShell launcher keeps its options and project");
#endif
    effective.set("OPENCODE_FIXTURE_VERSION", addon::Json_value("1.18.24"));
    check(!opencode::verified_launch(launcher, {program_text}, effective, path_string(root), true, context),
        "changed effective environment invalidates version evidence");
    context.cancelled = true;
    check(!opencode::verified_launch(launcher, {program_text}, effective, path_string(root), false, context),
        "cancelled request does not probe");
    context.cancelled = false;
    effective.set("OPENCODE_FIXTURE_VERSION", addon::Json_value("1.18.25"));
    for (const char* mode : {"overflow", "failed"}) {
        effective.set("OPENCODE_FIXTURE_MODE", addon::Json_value(mode));
        check(!opencode::verified_launch(launcher, {program_text}, effective, path_string(root), false, context), mode);
    }
    effective.set("OPENCODE_FIXTURE_MODE", addon::Json_value("wait"));
    context.cancellation_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    const auto cancellation_started = std::chrono::steady_clock::now();
    check(!opencode::verified_launch(launcher, {program_text}, effective, path_string(root), false, context),
        "active probe cancellation");
    check(std::chrono::steady_clock::now() - cancellation_started < std::chrono::seconds(2),
        "cancelled probe reaped promptly");
    context.cancellation_time.reset();
    effective.erase("OPENCODE_FIXTURE_MODE");
    const char* ambient_temp = std::getenv("TEMP");
    const std::optional<std::string> saved_temp = ambient_temp
        ? std::optional<std::string>(ambient_temp) : std::nullopt;
#ifdef _WIN32
    _putenv_s("TEMP", "adapter-ambient-must-not-reach-probe");
#else
    setenv("TEMP", "adapter-ambient-must-not-reach-probe", 1);
#endif
    std::erase_if(effective.as_object(), [](const addon::Json_member& member) {
        std::string name = member.name;
        for (char& character : name) if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
        return name == "TEMP";
    });
    effective.set("OPENCODE_FIXTURE_MODE", addon::Json_value("frozen"));
    check(opencode::verified_launch(launcher, {program_text}, effective, path_string(root), false, context).has_value(),
        "probe never fills frozen environment omissions from adapter ambient");
#ifdef _WIN32
    _putenv_s("TEMP", saved_temp ? saved_temp->c_str() : "");
#else
    if (saved_temp) setenv("TEMP", saved_temp->c_str(), 1);
    else unsetenv("TEMP");
#endif
    effective.erase("OPENCODE_FIXTURE_MODE");
    // Oracle: OpenCode 2.1.4's public CLI, plugin directory and synthetic inbox
    // contracts. Its server and TUI configuration arrays have different folds.
    interactive.launcher = launcher;
    interactive.launcher_argv = {program_text};
    interactive.launch_environment->effective_environment = launch_environment;
    interactive.launch_environment->effective_environment.set("OPENCODE_FIXTURE_VERSION", addon::Json_value("2.1.4"));
    interactive.launch_environment->effective_environment.set("OPENCODE_CONFIG_CONTENT", inline_settings);
    interactive.launch_environment->effective_environment.set("OPENCODE_CLI_CONFIG_CONTENT", addon::Json_value(
        R"({"plugins":["existing-tui-plugin"],"theme":"user-theme"})"));
    interactive.declared_assets.push_back("assets/opencode_tui/tui.mjs");
    interactive.session_wake = addon::Session_wake_offer{};
    const fs::path generation = root / u8"generation-\u00e9-\u6771";
    fs::create_directory(generation);
    const auto scratch = root / u8"scratch-{{file}}-\u6771";
    fs::create_directory(scratch);
    const auto host_environment = addon::current_process_environment();
    const auto saved_package_root = addon::environment_value(host_environment, addon::k_env_package_root);
    const auto saved_scratch = addon::environment_value(host_environment, addon::k_env_scratch_dir);
    set_host_directory(addon::k_env_package_root, generation);
    for (const auto& unavailable : {fs::path{}, fs::path("relative-scratch")}) {
        set_host_directory(addon::k_env_scratch_dir, unavailable);
        const auto without = prepare_interactive();
        check(without && lifecycle_reported(without) == true && without->session_wake &&
            !without->session_wake->available && without->session_wake->reason == "opencode_scratch_unavailable" &&
            std::find(without->unset.begin(), without->unset.end(), "OPENCODE_LOGONOMIC_WAKE_REGISTRATION") != without->unset.end(),
            "missing or relative scratch disables only wake and clears an inherited binding");
    }
    set_host_directory(addon::k_env_scratch_dir, scratch);
    const auto tui_config_ok = [&](const addon::Launch_patch& patch) {
        if (!patch.session_wake || !patch.session_wake->available) return false;
        const auto* registration = patch.session_wake->binding.find("registration_file");
        const auto emitted = addon::parse_json(addon::environment_value(patch.set, "OPENCODE_LOGONOMIC_WAKE_REGISTRATION"), {}, nullptr);
        if (!registration || emitted != *registration ||
            addon::path_from_utf8(registration->as_string()).parent_path() != scratch ||
            !addon::validate_launch_patch(patch, interactive.declared_assets, interactive.declared_assets, nullptr)) return false;
        const auto config = applied_config(patch.set);
        if (!config || !config->find("instructions") || !config->find("plugins")) return false;
        const auto& plugins = config->find("plugins")->as_array();
        const auto& instructions = config->find("instructions")->as_array();
        return plugins.size() == 2 && plugins.front() == user_config.find("plugins")->as_array().front() &&
            plugins.back().as_string() == path_string(generation / "assets" / "opencode_tui") &&
            !instructions.empty() && instructions.front().as_string() == "{{file}}" && !config->find("plugin");
    };
    const auto tui_patch = prepare_interactive();
    check(tui_patch && tui_patch->argv == std::optional(std::vector<std::string>{program_text, "--standalone"}) &&
        tui_patch->session_wake && tui_patch->session_wake->available && lifecycle_reported(tui_patch) == true,
        "verified OpenCode 2 prepares live TUI wake in a private native server");
    interactive.initial_prompt = first_message;
    const auto native_initial = prepare_interactive();
    check(initial_message_delivered(native_initial) && native_initial->argv->size() == 3 &&
        (*native_initial->argv)[1] == "--standalone" && native_initial->session_wake &&
        native_initial->session_wake->available,
        "OpenCode 2 receives the exact first message in its private interactive server");
    interactive.initial_prompt.reset();
    if (tui_patch) {
        check(std::none_of(tui_patch->set.begin(), tui_patch->set.end(),
                [](const auto& value) { return value.first == "OPENCODE_CLI_CONFIG_CONTENT"; }),
            "native TUI discovery preserves the user's cli.json and inline plugin list");
        check(tui_config_ok(*tui_patch), "native launch keeps literal settings and the exact managed plugin directory");
        const auto config = applied_config(tui_patch->set);
        if (config) {
            std::ifstream instruction(addon::path_from_utf8(config->find("instructions")->as_array().back().as_string()));
            const std::string text{std::istreambuf_iterator<char>(instruction), {}};
            check(text == interactive.title_instruction, "native TUI instructions do not advertise unadapted tools");
        }
    }
    resume.context.launcher = interactive.launcher;
    resume.context.launcher_argv = interactive.launcher_argv;
    resume.context.launch_environment = interactive.launch_environment;
    resume.context.declared_assets = interactive.declared_assets;
    resume.context.session_wake = addon::Session_wake_offer{};
    const auto native_resume = prepare_resume(nullptr);
    check(native_resume && native_resume->process.argv ==
        std::vector<std::string>{"--standalone", "--session", resumed_session} &&
        native_resume->patch.session_wake && native_resume->patch.session_wake->available,
        "native TUI resume owns a fresh private bridge and the exact stored session");
    check(native_resume && tui_config_ok(native_resume->patch),
        "native resume keeps literal settings and the exact managed plugin directory");
    interactive.launcher.script_body += " --server https://example.org";
    interactive.launcher_argv.insert(interactive.launcher_argv.end(), {"--server", "https://example.org"});
    const auto unmanaged = prepare_interactive();
    check(unmanaged && unmanaged->set.empty() && !unmanaged->argv && unmanaged->disposition &&
        !unmanaged->disposition->prepared && unmanaged->session_wake && !unmanaged->session_wake->available,
        "an attached native server stays an unchanged ordinary launch without wake authority");
    const auto npm_root = root / "npm";
    const auto npm_package = npm_root / "node_modules" / "@opencode" / "cli";
    fs::create_directories(npm_package / "bin");
    fs::copy_file(program, npm_package / "bin" / "opencode.exe");
    std::ofstream(npm_package / "package.json", std::ios::binary) <<
        R"({"name":"@opencode/cli","version":"2.1.4","bin":{"opencode":"./bin/opencode.exe","opencode2":"./bin/opencode.exe"}})";
    const auto npm_shim = npm_root / "opencode.cmd";
    std::ofstream(npm_shim, std::ios::binary) <<
        "@echo off\r\n\"%~dp0\\node_modules\\@opencode\\cli\\bin\\opencode.exe\" %*\r\n";
#ifndef _WIN32
    fs::permissions(npm_shim, fs::perms::owner_all, fs::perm_options::replace);
#endif
    auto npm_environment = launch_environment;
    npm_environment.set("OPENCODE_FIXTURE_VERSION", addon::Json_value("2.1.4"));
    const auto npm_launch = opencode::verified_launch({"\"" + path_string(npm_shim) + "\"", "direct", ""},
        {path_string(npm_shim)}, npm_environment, path_string(root), false, context);
    check(npm_launch && npm_launch->version == "2.1.4",
        "published native @opencode/cli bin metadata is recognized through its stock npm shim");
    // The selected npm link, not another installed package, owns the version.
    const auto classic_package = npm_root / "node_modules" / "opencode-ai";
    fs::create_directories(classic_package / "bin");
    fs::copy_file(program, classic_package / "bin" / "opencode.exe");
    std::ofstream(classic_package / "package.json", std::ios::binary) <<
        R"({"name":"opencode-ai","version":"1.18.25","bin":{"opencode":"./bin/opencode.exe"}})";
    std::ofstream(classic_package / "bin" / "fixture.version") << "1.18.25\n";
    std::ofstream(npm_package / "bin" / "fixture.version") << "2.1.4\n";
    npm_environment.set("OPENCODE_FIXTURE_MODE", addon::Json_value("installed-version"));
    const auto mixed_launch = opencode::verified_launch({"\"" + path_string(npm_shim) + "\"", "direct", ""},
        {path_string(npm_shim)}, npm_environment, path_string(root), false, context);
    if (mixed_launch && mixed_launch->version != "2.1.4") {
        std::cerr << "Mixed installation selected native shim but probed " << mixed_launch->version << '\n';
    }
    check(mixed_launch && mixed_launch->version == "2.1.4" &&
        fs::equivalent(addon::path_from_utf8(mixed_launch->argv.front()), npm_package / "bin" / "opencode.exe"),
        "a native npm shim retains its selected package when both OpenCode families are installed");
    interactive.launcher = {"\"" + path_string(npm_shim) + "\"", "direct", ""};
    interactive.launcher_argv = {path_string(npm_shim)};
    interactive.launch_environment->effective_environment = npm_environment;
    const auto mixed_patch = prepare_interactive();
    check(mixed_patch && mixed_patch->argv == std::optional(std::vector<std::string>{path_string(npm_shim), "--standalone"}) &&
        mixed_patch->session_wake && mixed_patch->session_wake->available,
        "mixed installed families prepare the selected native TUI with its own server");
#ifdef _WIN32
    const auto native_initial_shim = npm_root / "opencode2.cmd";
    std::ofstream(native_initial_shim, std::ios::binary) <<
        "@echo off\r\nset OPENCODE_FIXTURE_SHIM_SETTING=preserved-tui\r\n"
        "\"%~dp0\\node_modules\\@opencode\\cli\\bin\\opencode.exe\" %*\r\n";
    interactive.launcher = {"opencode2 --auto", "powershell", ""};
    interactive.launcher_argv = {"powershell.exe", "-NoLogo", "-Command", "opencode2 --auto"};
    interactive.launch_environment->effective_environment = npm_environment;
    interactive.launch_environment->effective_environment.set("PATH",
        addon::Json_value(path_string(npm_root) + ";" + powershell_directory));
    interactive.initial_prompt = first_message;
    const auto shim_initial = prepare_interactive();
    check(initial_message_delivered(shim_initial, "preserved-tui") && shim_initial->session_wake &&
        shim_initial->session_wake->available,
        "OpenCode 2 keeps its npm shim's environment and private server when delivering the first message");
    interactive.initial_prompt.reset();
#endif
    const auto ps_shim = npm_root / "opencode2.ps1";
    std::ofstream(ps_shim, std::ios::binary) <<
        "$basedir=Split-Path $MyInvocation.MyCommand.Definition -Parent\n"
        "& \"$basedir/node_modules/@opencode/cli/bin/opencode.exe\" $args\n";
#ifndef _WIN32
    fs::permissions(ps_shim, fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto ps_launch = opencode::verified_launch({"\"" + path_string(ps_shim) + "\"", "direct", ""},
        {path_string(ps_shim)}, npm_environment, path_string(root), false, context);
    check(ps_launch && ps_launch->version == "2.1.4",
        "the opencode2 PowerShell npm link resolves its published shared native bin");
    std::ofstream(npm_shim, std::ios::binary | std::ios::trunc) <<
        "@echo off\r\n\"%~dp0\\node_modules\\opencode-ai\\bin\\opencode.exe\" %*\r\n";
    const auto classic_launch = opencode::verified_launch({"\"" + path_string(npm_shim) + "\"", "direct", ""},
        {path_string(npm_shim)}, npm_environment, path_string(root), false, context);
    check(classic_launch && classic_launch->version == "1.18.25",
        "the selected classic npm link remains classic with native OpenCode also installed");
    std::ofstream(npm_shim, std::ios::binary | std::ios::app) <<
        "\"%~dp0\\node_modules\\@opencode\\cli\\bin\\opencode.exe\" %*\r\n";
    check(!opencode::verified_launch({"\"" + path_string(npm_shim) + "\"", "direct", ""},
        {path_string(npm_shim)}, npm_environment, path_string(root), false, context),
        "a wrapper with multiple different native targets cannot establish launch identity");
    set_host_directory(addon::k_env_package_root, addon::path_from_utf8(saved_package_root));
    set_host_directory(addon::k_env_scratch_dir, addon::path_from_utf8(saved_scratch));
    launcher.script_body += " --model selected";
    check(!opencode::verified_launch(launcher, {program_text, "--model", "selected"}, effective,
        path_string(root), true, context), "delegation refuses changed stock argv");
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "fixture cleanup");
    return failures ? 1 : 0;
}
