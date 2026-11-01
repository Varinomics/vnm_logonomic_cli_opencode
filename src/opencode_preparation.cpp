#include "opencode_preparation.h"
#include "opencode_acp.h"
#include "opencode_conversation.h"
#include "opencode_launcher.h"

#include "logonomic_cli_addon/ids.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/profile_storage.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/sha256.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace opencode {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

constexpr char k_instruction_name[] = "logonomic-title-instruction.md";
constexpr char k_plugin_asset[] = "assets/logonomic_opencode_plugin.mjs";
constexpr char k_tui_plugin_asset[] = "assets/opencode_tui/tui.mjs";
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

fs::path addon_package_root()
{
#ifdef _WIN32
    // Read the host's UTF-16 process environment directly, independent of the CRT code page.
    constexpr auto name = L"LOGONOMIC_ADDON_PACKAGE_ROOT";
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (length >= size) return {};
    value.resize(length);
    return fs::path(std::move(value));
#else
    const char* value = std::getenv("LOGONOMIC_ADDON_PACKAGE_ROOT");
    return value ? native_path(value) : fs::path{};
#endif
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

Inline_config inline_config(const addon::Json_value& environment, std::string_view environment_name)
{
    Inline_config config;
    config.environment_name = environment_name;
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
    if (const auto* plugins = config.value.find("plugins")) {
        if (!plugins->is_array()) {
            throw std::runtime_error("OpenCode plugins must be an array.");
        }
        for (const auto& plugin : plugins->as_array()) {
            if (plugin.is_string() && !blank(plugin.as_string())) continue;
            const auto* package = plugin.find("package");
            const auto* options = plugin.find("options");
            if (!package || !package->is_string() || blank(package->as_string()) ||
                (options && !options->is_object()))
            {
                throw std::runtime_error("OpenCode plugins must be names or package/options objects.");
            }
        }
    }
    return config;
}

std::string materialize_instruction(std::string_view profile_root, std::string_view instruction,
    bool changed_files_tools)
{
    if (blank(instruction) || instruction.find('\0') != std::string_view::npos) {
        throw std::runtime_error("OpenCode title instruction must be nonempty text.");
    }
    std::string body(instruction);
    while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) {
        body.pop_back();
    }
    if (changed_files_tools) {
        body += "\n\n";
        body += k_changed_files_instruction;
    }
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

// Consecutive opening braces in serialized JSON can only occur inside a string.
// Escape the second brace so the host leaves those literal characters alone.
std::string literal_config_json(const addon::Json_value& value)
{
    auto text = addon::serialize_json(value);
    if (text.find("{{") == std::string::npos) return text;

    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        if (c == '{' && result.ends_with('{')) result += "\\u007b";
        else result += c;
    }
    return result;
}

std::string interactive_config(addon::Json_value config, const std::string& instruction,
    const std::optional<addon::Session_wake_preparation>& wake = std::nullopt)
{
    auto instructions = config.find("instructions") ? *config.find("instructions") : addon::Json_value::array();
    instructions.push_back(addon::Json_value(instruction));
    config.set("instructions", std::move(instructions));
    const auto plugins = config.find("plugin") ? *config.find("plugin") : addon::Json_value::array();
    config.erase("plugin");
    std::string result = literal_config_json(config);
    result.pop_back();
    result += ",\"plugin\":[";
    for (const auto& plugin : plugins.as_array()) {
        result += literal_config_json(plugin) + ",";
    }
    // The host substitutes a complete JSON string, including Windows escaping.
    if (wake && wake->available) {
        auto options = addon::Json_value::object();
        options.set("session_wake", wake->binding);
        result += "[{{asset_json:assets/logonomic_opencode_plugin.mjs}}," + literal_config_json(options) + "]]}";
    }
    else {
        result += "{{asset_json:assets/logonomic_opencode_plugin.mjs}}]}";
    }
    return result;
}

addon::Session_wake_preparation prepare_wake(std::string_view profile_root, bool verified)
{
    addon::Session_wake_preparation result;
    if (!verified) {
        result.reason = "opencode_launcher_unverified";
        return result;
    }
    // The plugin creates this file exclusively with owner-only permissions.
    // Neither credentials nor a native endpoint are placed in the launch patch.
    const auto file = native_path(profile_root) / ("wake-" + addon::random_nonce_hex() + ".json");
    result.binding.set("registration_file", addon::Json_value(path_text(file)));
    result.available = true;
    return result;
}

bool configure_tui(
    addon::Launch_patch& patch,
    const addon::Json_value& environment,
    const std::vector<std::string>& declared_assets,
    std::string_view title_instruction,
    bool session_wake,
    addon::Invocation_context& context)
{
    if (std::find(declared_assets.begin(), declared_assets.end(), k_tui_plugin_asset) == declared_assets.end()) {
        context.fail(addon::Result_status::FAILED, "asset_undeclared");
        return false;
    }
    // TUI plugins are directory entrypoints. The host owns this immutable
    // package directory for the lifetime of the launched native process.
    const auto package_root = addon_package_root();
    if (!package_root.is_absolute()) {
        context.fail(addon::Result_status::UNAVAILABLE, "package_root_unavailable");
        return false;
    }
    const auto instruction = materialize_instruction(patch.profile_storage->root, title_instruction, false);
    auto server = inline_config(environment);
    auto instructions = server.value.find("instructions") ? *server.value.find("instructions") : addon::Json_value::array();
    instructions.push_back(addon::Json_value(instruction));
    server.value.set("instructions", std::move(instructions));
    auto plugins = server.value.find("plugins") ? *server.value.find("plugins") : addon::Json_value::array();
    plugins.push_back(addon::Json_value(path_text(package_root / "assets" / "opencode_tui")));
    server.value.set("plugins", std::move(plugins));
    if (session_wake) {
        patch.session_wake = prepare_wake(patch.profile_storage->root, true);
        patch.set.emplace_back("OPENCODE_LOGONOMIC_WAKE_REGISTRATION",
            patch.session_wake->binding.find("registration_file")->as_string());
    }
    patch.set.emplace_back(server.environment_name, literal_config_json(server.value));
    patch.set.emplace_back("OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED", "1");
    patch.disposition = addon::Launch_disposition{true, true, false};
    return true;
}

// Refuses, on `context`, a launcher that an interactive launch or resume
// cannot prepare: one that is not OpenCode, lacks profile storage or the
// managed plugin, or assigns the inline configuration the package merges into.
bool interactive_launcher_admitted(
    const addon::Launcher_request&         launcher,
    bool                                   profile_storage,
    const std::vector<std::string>&        declared_assets,
    addon::Invocation_context&             context,
    bool                                   initial_prompt = false)
{
    const auto command = recognize_launcher(launcher.script_body);
    if (!command || !profile_storage) {
        context.fail(addon::Result_status::UNAVAILABLE,
            initial_prompt ? addon::reason::k_initial_prompt_unavailable : "launcher_unavailable");
        return false;
    }
    for (const auto& [name, value] : command->environment) {
        (void)value;
        if (environment_name_equal(name, "OPENCODE_CONFIG_CONTENT")) {
            context.fail(addon::Result_status::UNAVAILABLE,
                initial_prompt ? addon::reason::k_initial_prompt_unavailable : "inline_configuration_assignment");
            return false;
        }
    }
    if (std::find(declared_assets.begin(), declared_assets.end(), k_plugin_asset) == declared_assets.end()) {
        context.fail(addon::Result_status::FAILED, "asset_undeclared");
        return false;
    }
    return true;
}

std::optional<std::vector<std::string>> prepare_initial_prompt(
    const addon::Prepare_request& request,
    const Verified_launch&        launch,
    addon::Invocation_context&    context)
{
    auto invocation = initial_prompt_invocation(
        request.launcher, request.launcher_argv, request.launch_environment->effective_environment,
        request.working_directory, launch, request.initial_prompt->text);
    if (!invocation) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
        return std::nullopt;
    }
    auto probe = launch.argv;
    probe.emplace_back("--help");
    auto environment = addon::environment_pairs(request.launch_environment->effective_environment);
    for (const auto& [name, value] : launch.environment) {
        addon::set_environment(environment, name, value);
    }
    const auto help = addon::probe(probe, environment, request.working_directory, 3000, context);
    static const std::regex prompt_option(R"((^|\n)[^\n]*[ \t]--prompt([ \t=<]|$))");
    if (!help || (!std::regex_search(help->output, prompt_option) &&
                 !std::regex_search(help->errors, prompt_option)))
    {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : addon::reason::k_initial_prompt_unavailable);
        return std::nullopt;
    }
    return invocation;
}

void bind_conversation_source(addon::Launch_patch& patch, const std::optional<std::string>& source)
{
    std::erase_if(patch.set, [](const auto& entry) {
        return environment_name_equal(entry.first, k_export_source_environment);
    });
    std::erase_if(patch.unset, [](const auto& name) {
        return environment_name_equal(name, k_export_source_environment);
    });
    if (source) {
        patch.set.emplace_back(k_export_source_environment, *source);
    }
    else {
        patch.unset.emplace_back(k_export_source_environment);
    }
}

void finish_interactive(
    addon::Launch_patch                    patch,
    const addon::Prepare_request&          request,
    std::optional<std::vector<std::string>> initial_invocation,
    const std::optional<std::string>&       conversation_source,
    addon::Invocation_context&             context)
{
    bind_conversation_source(patch, conversation_source);
    if (request.initial_prompt) {
        patch.argv = std::move(initial_invocation);
        patch.initial_prompt = addon::Initial_prompt_acknowledgment{
            addon::initial_prompt_text_digest_hex(request.initial_prompt->text)};
        addon::Contract_failure failure;
        if (!addon::validate_launch_patch(patch, request.declared_assets, request.declared_assets, &failure)) {
            context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
            return;
        }
    }
    context.succeed(addon::encode_launch_patch(patch, 3));
}

void prepare_interactive(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure, 3);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    if (!interactive_launcher_admitted(request->launcher, request->profile_storage.has_value(),
            request->declared_assets, context, request->initial_prompt.has_value()))
    {
        return;
    }
    auto config = inline_config(request->launch_environment->effective_environment);
    // The host's complete invocation, shell included, is what the verification
    // must match before the event relay may report this launch's lifecycle.
    const auto launch = verified_launch(request->launcher, request->launcher_argv,
        request->launch_environment->effective_environment, request->working_directory, false, context);
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    const auto conversation_source = prepare_conversation_source(request->launcher, request->launcher_argv,
        request->launch_environment->effective_environment, request->working_directory, launch ? &*launch : nullptr);
    std::optional<std::vector<std::string>> initial_invocation;
    if (request->initial_prompt) {
        if (!launch) {
            context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
            return;
        }
        initial_invocation = prepare_initial_prompt(*request, *launch, context);
        if (!initial_invocation) {
            return;
        }
    }
    if (launch && uses_tui_plugin(launch->version)) {
        addon::Launch_patch patch;
        patch.profile_storage = request->profile_storage;
        patch.argv = tui_invocation(request->launcher, request->launcher_argv,
            request->launch_environment->effective_environment, request->working_directory);
        if (!patch.argv) {
            patch.disposition = addon::Launch_disposition{false, false, false};
            if (request->session_wake) {
                patch.session_wake = addon::Session_wake_preparation{};
                patch.session_wake->reason = "opencode_tui_launcher_unavailable";
            }
        }
        else
        if (!configure_tui(patch, request->launch_environment->effective_environment, request->declared_assets,
                request->title_instruction, request->session_wake.has_value(), context))
        {
            return;
        }
        finish_interactive(std::move(patch), *request, std::move(initial_invocation), conversation_source, context);
        return;
    }
    const auto command = recognize_launcher(request->launcher.script_body);
    if (!launch && (addon::base_name(command->program, addon::Launcher_whitespace::ASCII) == "opencode2" ||
        std::any_of(command->prefix_arguments.begin(), command->prefix_arguments.end(),
            [](const auto& value) { return value == "@opencode/cli" || value.starts_with("@opencode/cli@"); })))
    {
        addon::Launch_patch patch;
        patch.profile_storage = request->profile_storage;
        patch.disposition = addon::Launch_disposition{false, false, false};
        if (request->session_wake) patch.session_wake = prepare_wake(request->profile_storage->root, false);
        finish_interactive(std::move(patch), *request, {}, conversation_source, context);
        return;
    }
    const auto instruction = materialize_instruction(request->profile_storage->root, request->title_instruction);
    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    if (request->session_wake) patch.session_wake = prepare_wake(request->profile_storage->root, launch.has_value());
    patch.set.emplace_back(config.environment_name, interactive_config(std::move(config.value), instruction, patch.session_wake));
    patch.set.emplace_back("OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED", launch ? "1" : "0");
    patch.disposition = addon::Launch_disposition{true, launch.has_value(), false};
    finish_interactive(std::move(patch), *request, std::move(initial_invocation), conversation_source, context);
}

// OpenCode's own session identifiers: `ses_` and ASCII letters and digits.
bool opencode_session_id(const addon::Json_value& value)
{
    if (!value.is_string()) {
        return false;
    }
    const auto& text = value.as_string();
    return text.size() > 4 && text.size() <= 256 && text.starts_with("ses_") &&
        std::all_of(text.begin() + 4, text.end(), [](unsigned char c) { return std::isalnum(c) != 0; });
}

void prepare_resume(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_resume_prepare_request(context.request().payload, &failure, 2);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& launch  = request->context;
    const auto& record  = request->resume_record;
    const auto* session = record.is_object() && record.as_object().size() == 1
        ? record.find("provider_session_id") : nullptr;
    if (!session || !opencode_session_id(*session)) {
        context.fail(addon::Result_status::FAILED, addon::reason::k_resume_record_invalid);
        return;
    }
    if (!interactive_launcher_admitted(launch.launcher, launch.profile_storage.has_value(),
            launch.declared_assets, context))
    {
        return;
    }
    auto config = inline_config(launch.launch_environment->effective_environment);
    // The resumed session confirms its identity through its lifecycle, so a
    // launch that cannot report lifecycle cannot resume.
    const auto verified = verified_launch(launch.launcher, launch.launcher_argv,
        launch.launch_environment->effective_environment, launch.working_directory, false, context);
    if (!verified) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "launcher_program_unavailable");
        return;
    }
    const auto conversation_source = prepare_conversation_source(launch.launcher, launch.launcher_argv,
        launch.launch_environment->effective_environment, launch.working_directory, &*verified);
    if (uses_tui_plugin(verified->version)) {
        const auto invocation = tui_invocation(launch.launcher, launch.launcher_argv,
            launch.launch_environment->effective_environment, launch.working_directory, session->as_string());
        if (!invocation) {
            context.fail(addon::Result_status::UNAVAILABLE, "resume_launcher_unavailable");
            return;
        }
        addon::Resume_prepare_result result;
        result.process.executable = addon::Process_executable::launcher_program();
        result.process.argv.assign(invocation->begin() + 1, invocation->end());
        result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
        result.patch.profile_storage = launch.profile_storage;
        if (!configure_tui(result.patch, launch.launch_environment->effective_environment, launch.declared_assets,
                request->title_instruction, launch.session_wake.has_value(), context))
        {
            return;
        }
        bind_conversation_source(result.patch, conversation_source);
        result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(record);
        context.succeed(addon::encode_resume_prepare_result(result, 2));
        return;
    }
    const auto arguments = resume_arguments(
        launch.launcher, launch.launcher_argv, launch.launch_environment->effective_environment,
        launch.working_directory, session->as_string());
    if (!arguments) {
        context.fail(addon::Result_status::UNAVAILABLE, "resume_launcher_unavailable");
        return;
    }
    const auto instruction = materialize_instruction(launch.profile_storage->root, request->title_instruction);
    addon::Resume_prepare_result result;
    result.process.executable     = addon::Process_executable::launcher_program();
    result.process.argv           = *arguments;
    result.process.stdin_behavior = addon::Stdin_behavior::INHERIT;
    result.patch.profile_storage  = launch.profile_storage;
    if (launch.session_wake) result.patch.session_wake = prepare_wake(launch.profile_storage->root, true);
    result.patch.set.emplace_back(config.environment_name, interactive_config(std::move(config.value), instruction, result.patch.session_wake));
    result.patch.set.emplace_back("OPENCODE_LOGONOMIC_EVENT_RELAY_ENABLED", "1");
    // The managed plugin reports this session as resumed once the resumed
    // OpenCode has it in its own store.
    result.patch.set.emplace_back("OPENCODE_LOGONOMIC_RESUME_SESSION", session->as_string());
    bind_conversation_source(result.patch, conversation_source);
    result.acknowledgment.record_digest_hex = addon::resume_record_digest_hex(record);
    context.succeed(addon::encode_resume_prepare_result(result, 2));
}

void prepare_delegated(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(context.request().payload, &failure);
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
    if (!delegated_auto_approval(addon::literal_argv(request->context.launcher.script_body))) {
        context.fail(addon::Result_status::UNAVAILABLE, "delegated_launcher_options_unavailable");
        return;
    }
    const auto launch = verified_launch(request->context.launcher, request->context.launcher_argv,
        request->context.launch_environment->effective_environment, request->context.working_directory, true, context);
    if (!launch) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::UNAVAILABLE,
            context.cancel_requested() ? "cancelled" : "launcher_program_unavailable");
        return;
    }
    if (uses_tui_plugin(launch->version)) {
        context.fail(addon::Result_status::UNAVAILABLE, "opencode_acp_generation_unavailable");
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
    auto task_tools = addon::Json_value::array();
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
        const auto name = "logonomic-task-" + request->control.task_id + "-" + server.server_id;
        // OpenCode names an MCP tool <server>_<tool>.
        for (const auto& action : server.actions) {
            task_tools.push_back(addon::Json_value(name + "_" + action));
        }
        mcp.set("name", addon::Json_value(name));
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
    result.process.environment.emplace_back(config.environment_name, literal_config_json(config.value));
    result.process.environment.emplace_back("OPENCODE_ENABLE_QUESTION_TOOL", "0");
    // OPENCODE_CONFIG and OPENCODE_CONFIG_DIR select the user's configuration,
    // and with it the permission rules the delegated run keeps.
    result.process.environment_names = {"PATH", "PATHEXT", "HOME", "USERPROFILE", "LOCALAPPDATA", "APPDATA",
        "XDG_CONFIG_HOME", "XDG_DATA_HOME", "OPENCODE_CONFIG", "OPENCODE_CONFIG_DIR"};
    std::erase_if(result.process.environment_names, [&result](const std::string& name) {
        return std::any_of(result.process.environment.begin(), result.process.environment.end(),
            [&name](const auto& entry) { return environment_name_equal(entry.first, name); });
    });
    result.inspection = make_acp_inspection(
        request->prompt, request->context.working_directory, launch->version, servers, attestations, task_tools,
        launch->auto_approve);
    result.inspection.keep_stdin_open = true;
    auto payload = addon::encode_delegated_prepare_result(result);
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
    if (request.capability.capability_id == "interactive-resume" &&
        request.operation == addon::k_prepare_resume_operation)
    {
        prepare_resume(context);
    }
    else
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
