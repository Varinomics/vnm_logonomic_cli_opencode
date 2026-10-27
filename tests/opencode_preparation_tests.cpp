#include "opencode_preparation.h"

#include <logonomic_cli_addon/host_simulator.h>
#include <logonomic_cli_addon/helper_ingress.h>
#include <logonomic_cli_addon/ids.h>
#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/profile_storage.h>
#include <logonomic_cli_addon/session_contracts.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string path_text(const fs::path& path)
{
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

fs::path native_path(std::string_view text)
{
    return fs::path(std::u8string(text.begin(), text.end()));
}

void config_tests()
{
    auto environment = addon::Json_value::object();
    environment.set("OPENCODE_CONFIG_CONTENT", addon::Json_value(
        "{/* retained */\"instructions\":[\"user.md\",],\"plugin\":[[\"user-plugin\",{\"url\":\"https://example.test/a//b\"}],],\"model\":\"user/model\",}"));
    const auto parsed = opencode::inline_config(environment);
    require(parsed.value.find("model")->as_string() == "user/model", "JSONC retains model");
    require(parsed.value.find("instructions")->as_array()[0].as_string() == "user.md", "JSONC retains instructions");
    require(parsed.value.find("plugin")->as_array()[0].as_array()[1].find("url")->as_string() == "https://example.test/a//b", "comment syntax in strings survives");
    for (const char* invalid : {"[]", "{/*", "{\"instructions\":false}", "{\"instructions\":[3]}", "{\"plugin\":[[\"name\",false]]}"}) {
        environment.set("OPENCODE_CONFIG_CONTENT", addon::Json_value(invalid));
        bool rejected = false;
        try {
            (void)opencode::inline_config(environment);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "malformed user configuration is rejected");
    }
#ifdef _WIN32
    environment.set("OPENCODE_CONFIG_CONTENT", addon::Json_value("{}"));
    environment.set("opencode_config_content", addon::Json_value("{}"));
    bool collision_rejected = false;
    try {
        (void)opencode::inline_config(environment);
    }
    catch (const std::runtime_error&) {
        collision_rejected = true;
    }
    require(collision_rejected, "Windows config case collisions are rejected");
#endif
}

void instruction_tests(const fs::path& root)
{
    const auto first = opencode::materialize_instruction(path_text(root), "Keep the session title useful.\n");
    const auto repeat = opencode::materialize_instruction(path_text(root), "Keep the session title useful.\n");
    const auto changed = opencode::materialize_instruction(path_text(root), "Use the repository task as the title.");
    require(first == repeat && first != changed, "instruction generations are immutable and content addressed");
    const auto instruction_path = native_path(first);
    std::ifstream stream(instruction_path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    require(bytes.find("logonomic_show_changed_files") != std::string::npos, "changed-file tool instruction retained");
    stream.close();
    std::ofstream altered(instruction_path, std::ios::binary | std::ios::trunc);
    altered << "changed";
    altered.close();
    bool corruption_rejected = false;
    try {
        (void)opencode::materialize_instruction(path_text(root), "Keep the session title useful.\n");
    }
    catch (const std::runtime_error&) {
        corruption_rejected = true;
    }
    require(corruption_rejected, "corrupted prior instruction is not silently overwritten");
}

addon::Host_outcome invoke(addon::Host_simulator& host, const addon::Capability_pair& capability,
    std::string operation, addon::Json_value payload)
{
    const auto id = host.invoke(capability, std::move(operation), std::move(payload), 15000);
    const auto result = host.wait_outcome(id, 18000);
    require(result.has_value(), "adapter invocation completed");
    return *result;
}

void adapter_tests(const fs::path& package, const fs::path& root)
{
    addon::Simulator_options options;
    options.package_directory = package;
    options.scratch_directory = root / "scratch";
    options.profile_storage_root = root / "profiles";
    addon::Host_simulator host(std::move(options), nullptr);
    addon::Contract_failure failure;
    require(host.start(&failure), "real package adapter starts");
    addon::Attention_classification_request attention;
    attention.base.event.name = "question.replied";
    attention.base.event.body.set("event", addon::Json_value("question.replied"));
    attention.base.event.body.set("session_id", addon::Json_value("native-session"));
    attention.base.event.body.set("turn_id", addon::Json_value("native-turn"));
    attention.base.event.body.set("question_id", addon::Json_value("que_answered"));
    attention.base.event.body.set("answer_nonempty", addon::Json_value(true));
    attention.base.transition_nonce = std::string(32, 'd');
    const auto classified = invoke(host, {addon::k_attention_kind_capability_id, addon::k_attention_kind_version},
        addon::k_classify_event_operation, addon::encode_attention_classification_request(attention));
    const auto attention_result = addon::decode_attention_classification_result(classified.payload, &failure);
    require(classified.status == addon::Host_status::SUCCESS && attention_result &&
            attention_result->attention_kind && attention_result->attention_kind->kind == "interactive_question" &&
            attention_result->base.observations.front().attention_request_id == "que_answered" &&
            attention_result->base.observations.front().provider_event_name == "question.replied",
        "actual packaged adapter admits the optional question companion without changing native identity");
    std::string error;
    const auto profile = addon::allocate_profile_storage_root(host.profile_storage_root(), "logonomic.cli.opencode", &error);
    require(profile.has_value(), "profile root allocated");
    addon::Prepare_request request;
    request.launcher = {"opencode", "direct", ""};
    request.working_directory = path_text(root);
    request.title_instruction = "Keep the title connected to the current task.";
    request.declared_assets = {"assets/logonomic_opencode_plugin.mjs"};
    request.profile_storage = addon::Profile_storage_context{1, *profile};
    request.launch_environment = addon::Launch_environment{};
#ifdef _WIN32
    request.launch_environment->platform_id = "windows-x86_64";
#else
    request.launch_environment->platform_id = addon::host_platform_id();
#endif
    request.launch_environment->effective_environment.set("PATH", addon::Json_value(path_text(root / "absent-bin")));
    request.launch_environment->effective_environment.set("OPENCODE_CONFIG_CONTENT", addon::Json_value("{\"model\":\"retained/model\",\"instructions\":[\"user.md\"]}"));
    request.launcher_argv = {"opencode"};
    request.session_wake = addon::Session_wake_offer{};
    const auto valid_environment = request.launch_environment->effective_environment;
    for (const char* invalid : {"[]", "{/*", "{\"instructions\":false}",
             "{\"instructions\":[3]}", "{\"plugin\":false}", "{\"plugin\":[[\"private-sentinel\",false]]}"})
    {
        request.launch_environment->effective_environment.set("OPENCODE_CONFIG_CONTENT", addon::Json_value(invalid));
        const auto rejected = invoke(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request, 3));
        require(rejected.status == addon::Host_status::FAILED && rejected.reason_code == "opencode_settings_invalid",
            "invalid inline settings retain their category through the adapter privacy boundary");
        require(rejected.message.find("private-sentinel") == std::string::npos,
            "invalid configuration contents remain private");
    }
    request.launch_environment->effective_environment = valid_environment;
    const auto discovered = invoke(host, {"launcher-discovery", 1}, "discover", addon::encode_discover_request(
        request.launch_environment->effective_environment, path_text(root), request.launch_environment->platform_id));
    const auto candidates = addon::decode_discover_result(discovered.payload, request.launch_environment->platform_id, &failure);
    require(discovered.status == addon::Host_status::SUCCESS && candidates && candidates->empty(),
        "real adapter discovery does not advertise an absent installation");
    const auto prepared = invoke(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request, 3));
    require(prepared.status == addon::Host_status::SUCCESS, "interactive prepare remains available without verified lifecycle");
    const auto patch = addon::decode_launch_patch(prepared.payload, &failure, 3);
    require(patch.has_value(), "prepared patch decodes");
    require(patch->session_wake && !patch->session_wake->available &&
            patch->session_wake->reason == "opencode_launcher_unverified",
        "an offered wake reports the unavailable launcher without inventing a live binding");
    require(patch->disposition && patch->disposition->prepared && !patch->disposition->lifecycle_events &&
            !patch->disposition->coordination_delivery,
        "an unverified launch is prepared without lifecycle or coordination");
    require(patch->profile_storage && patch->profile_storage->root == *profile, "exact profile echo survives real process");
    bool merged = false;
    bool disabled = false;
    for (const auto& [name, value] : patch->set) {
        if (name == "OPENCODE_CONFIG_CONTENT") {
            merged = value.find("retained/model") != std::string::npos && value.find("user.md") != std::string::npos &&
                value.find("{{asset_json:assets/logonomic_opencode_plugin.mjs}}") != std::string::npos;
        }
        if (name == "OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED") {
            disabled = value == "0";
        }
    }
    require(merged && disabled, "config is preserved while unverifiable lifecycle is explicitly disabled");
    request.launcher.script_body = "OPENCODE_CONFIG_CONTENT={} opencode";
    require(invoke(host, {"launch-preparation", 3}, "prepare", addon::encode_prepare_request(request, 3)).status == addon::Host_status::UNAVAILABLE,
        "script-owned config is not overwritten");

    request.launcher.script_body = "opencode";
    addon::Resume_prepare_request resume_request;
    resume_request.context.launcher           = request.launcher;
    resume_request.context.launcher_argv      = request.launcher_argv;
    resume_request.context.working_directory  = request.working_directory;
    resume_request.context.declared_assets    = request.declared_assets;
    resume_request.context.profile_storage    = request.profile_storage;
    resume_request.context.launch_environment = request.launch_environment;
    resume_request.title_instruction          = request.title_instruction;
    resume_request.resume_record.set("provider_session_id", addon::Json_value("ses_4f1c2a9e7b3d5f8a1c6e0b2d9f"));
    const auto unverified_resume = invoke(host, {"interactive-resume", 2}, "prepare_resume",
        addon::encode_resume_prepare_request(resume_request, 2));
    require(unverified_resume.status == addon::Host_status::UNAVAILABLE &&
            unverified_resume.reason_code == "launcher_program_unavailable",
        "a resume that could not confirm itself through lifecycle is unavailable");
    for (const char* record : {"{\"provider_session_id\":\"session-4f1c2a9e\"}",
             "{\"provider_session_id\":\"ses_4f1c2a9e\",\"title\":\"Resume the parser work\"}",
             "{\"provider_session_id\":17}"})
    {
        resume_request.resume_record = *addon::parse_json(record, {}, nullptr);
        const auto refused = invoke(host, {"interactive-resume", 2}, "prepare_resume",
            addon::encode_resume_prepare_request(resume_request, 2));
        require(refused.status == addon::Host_status::FAILED &&
                refused.reason_code == addon::reason::k_resume_record_invalid,
            "only an OpenCode session identity is resumable");
    }

    int exit_code = -1;
    require(host.shutdown(3000, &exit_code) && exit_code == 0, "adapter shuts down cleanly");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    const fs::path root = fs::temp_directory_path() / ("opencode-preparation-" + addon::random_nonce_hex());
    fs::create_directories(root);
    int result = 0;
    try {
        config_tests();
        instruction_tests(root);
        adapter_tests(native_path(argv[1]), root);
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    fs::remove_all(root);
    return result;
}
