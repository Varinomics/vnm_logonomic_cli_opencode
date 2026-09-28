#include "opencode_launcher.h"
#include "opencode_preparation.h"
#include "opencode_acp.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

int failures = 0;

void check(bool condition, const char* name)
{
    if (!condition) { std::cerr << "FAIL " << name << '\n'; ++failures; }
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

} // namespace

int main(int argc, char** argv)
{
    // The test's own native image is copied into a stock installation. This
    // exercises actual probe startup/output/cancellation without an installed
    // vendor CLI or network access.
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        const char* mode = std::getenv("OPENCODE_FIXTURE_MODE");
        if (mode && std::string_view(mode) == "wait") std::this_thread::sleep_for(std::chrono::seconds(30));
        if (mode && std::string_view(mode) == "overflow") { std::cout << std::string(70000, 'a'); return 0; }
        if (mode && std::string_view(mode) == "failed") return 1;
        if (mode && std::string_view(mode) == "frozen" && std::getenv("TEMP")) return 1;
        const char* version = std::getenv("OPENCODE_FIXTURE_VERSION");
        std::cout << (version ? version : "1.18.25") << '\n';
        return 0;
    }
    for (const char* script : {
            "opencode --model openai/gpt-5", "npx -y opencode-ai@latest --version",
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
    check(opencode::literal_launcher_argv("\"C:/a b/opencode.exe\" --model test") ==
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
    discovery.platform_id = "linux-x86_64";
#endif
    discovery.ambient_environment.set("PATH", addon::Json_value(path_string(root)));
    const auto discovered = opencode::discover_launcher(discovery);
    check(discovered && discovered->script_body == "opencode" && discovered->runner_key ==
#ifdef _WIN32
        "powershell",
#else
        "direct",
#endif
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
        if (const auto* value = effective.find(name)) {
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
    prepared.invocation = {"connection", {"delegated-session", 2}, "prepare",
        addon::encode_delegated_prepare_request(delegated, 2)};
    opencode::handle_preparation(prepared);
    addon::Contract_failure failure;
    const auto result = prepared.payload
        ? addon::decode_delegated_prepare_result(*prepared.payload, &failure, 2) : std::nullopt;
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
    delegated.control.connection.reset();
    Context refused;
    refused.invocation = prepared.invocation;
    refused.invocation.payload = addon::encode_delegated_prepare_request(delegated, 2);
    opencode::handle_preparation(refused);
    check(!refused.payload && refused.reason == "task_server_connection_unavailable",
        "missing connection refuses delegation before native probing");
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
    launcher.script_body += " --model selected";
    check(!opencode::verified_launch(launcher, {program_text, "--model", "selected"}, effective,
        path_string(root), true, context), "delegation refuses changed stock argv");
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "fixture cleanup");
    return failures ? 1 : 0;
}
