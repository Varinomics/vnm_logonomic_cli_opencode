#include "opencode_launcher.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace opencode {
namespace {

using addon::lower;
using addon::set_environment;
using addon::regular_file;
using addon::resolve_program;
using addon::native_image;
using addon::path_to_utf8;

using addon::Environment;
using addon::Launcher_whitespace;
namespace fs = std::filesystem;

// Words PowerShell passes through unchanged: no quoting, variables, escapes,
// separators, redirections, groupings or comments.
bool plain_powershell_command(std::string_view script)
{
    return !addon::trim(script, Launcher_whitespace::ASCII).empty() && std::all_of(script.begin(), script.end(), [](unsigned char character) {
        return std::isalnum(character) || std::string_view(" \t-_./\\:=").find(static_cast<char>(character)) !=
            std::string_view::npos;
    });
}

bool package_flag(std::string_view value)
{
    return value == "-y" || value == "--yes" || value == "--quiet" ||
        value == "--silent" || value == "--no-install" || value == "--bun";
}

bool opencode_program(std::string_view program)
{
    const auto name = addon::base_name(program, Launcher_whitespace::ASCII);
    return name == "opencode" || name == "opencode2";
}

std::optional<Launcher_command> recognize(std::string_view script, int depth)
{
    if (depth == 0) return std::nullopt;
    Environment inherited;
    std::istringstream lines{std::string(script)};
    for (std::string line; std::getline(lines, line);) {
        line = addon::trim(line, Launcher_whitespace::ASCII);
        if (line.empty() || line.starts_with('#') || line.starts_with("//") ||
            lower(line).starts_with("rem ")) continue;
        for (std::string segment : addon::launcher_segments(line, Launcher_whitespace::ASCII)) {
            const std::string normalized = lower(segment);
            std::optional<std::pair<std::string, std::string>> standalone;
            if (normalized.starts_with("set ")) standalone = addon::environment_assignment(
                segment.substr(4), Launcher_whitespace::ASCII);
            else if (normalized.starts_with("export ")) standalone = addon::environment_assignment(
                segment.substr(7), Launcher_whitespace::ASCII);
            else if (normalized.starts_with("$env:")) standalone = addon::environment_assignment(
                segment.substr(5), Launcher_whitespace::ASCII);
            if (standalone) {
                inherited.push_back(*standalone);
                continue;
            }
            Environment environment = inherited;
            // Leading assignment values may contain quoted whitespace, unlike
            // the command-token grammar used for the remainder of the line.
            static const std::regex leading(R"re(^([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|'[^']*'|[^\s]*)\s*)re");
            std::smatch match;
            while (std::regex_search(segment, match, leading)) {
                environment.emplace_back(match[1].str(), addon::unquote(match[2].str(), Launcher_whitespace::ASCII));
                segment.erase(0, match.length());
            }
            const auto words = addon::launcher_tokens(segment);
            std::size_t index = 0;
            while (index < words.size()) {
                const auto value = addon::environment_assignment(words[index], Launcher_whitespace::ASCII);
                const auto prefix = lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                if (value) environment.push_back(*value);
                else if (prefix != "call" && prefix != "command" && prefix != "exec" && prefix != "&") break;
                ++index;
            }
            if (index == words.size()) continue;
            const std::string program = addon::unquote(words[index], Launcher_whitespace::ASCII);
            const std::string base = addon::base_name(program, Launcher_whitespace::ASCII);
            if (opencode_program(program)) return Launcher_command{program, {}, environment};
            if (base == "npx" || base == "bunx") {
                std::vector<std::string> arguments;
                for (++index; index < words.size(); ++index) {
                    const auto argument = addon::unquote(words[index], Launcher_whitespace::ASCII);
                    arguments.push_back(argument);
                    if (argument.starts_with('-')) {
                        if (!package_flag(argument)) break;
                    }
                    else {
                        const auto package = lower(argument);
                        if (package == "opencode-ai" || package.starts_with("opencode-ai@") ||
                            package == "@opencode/cli" || package.starts_with("@opencode/cli@")) {
                            return Launcher_command{program, arguments, environment};
                        }
                        break;
                    }
                }
            }
            else if (base == "cmd" || base == "powershell" || base == "pwsh" ||
                     base == "bash" || base == "sh")
            {
                for (++index; index + 1 < words.size(); ++index) {
                    const auto argument = lower(addon::unquote(words[index], Launcher_whitespace::ASCII));
                    if (argument != "/c" && argument != "-c" && argument != "-command") continue;
                    std::string tail;
                    for (++index; index < words.size(); ++index) {
                        if (!tail.empty()) tail += ' ';
                        tail += words[index];
                    }
                    auto command = recognize(addon::unquote(tail, Launcher_whitespace::ASCII), depth - 1);
                    if (command) {
                        command->environment.insert(command->environment.begin(), environment.begin(), environment.end());
                        return command;
                    }
                    break;
                }
            }
        }
    }
    return std::nullopt;
}

bool equal_name(std::string_view left, std::string_view right)
{
#ifdef _WIN32
    return lower(std::string(left)) == lower(std::string(right));
#else
    return left == right;
#endif
}

std::optional<fs::path> npm_shim_target(const fs::path& shim)
{
    // npm/read-cmd-shim treats these stock target statements as the Windows
    // equivalent of a symlink. Package-directory presence cannot select it.
    std::ifstream input(shim, std::ios::binary);
    std::string bytes(65537, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || input.gcount() > 65536) return std::nullopt;
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    static const std::regex batch(R"re("%(?:~dp0|dp0%)\\([^"]+)"\s+%\*)re");
    static const std::regex powershell(R"re("\$basedir/([^"]+)"\s+\$args)re");
    const auto& pattern = lower(shim.extension().string()) == ".cmd" ? batch : powershell;
    std::optional<fs::path> target;
    for (auto match = std::sregex_iterator(bytes.begin(), bytes.end(), pattern);
         match != std::sregex_iterator(); ++match)
    {
        std::string text = (*match)[1].str();
        std::replace(text.begin(), text.end(), '\\', '/');
        const auto path = addon::path_from_utf8(text).lexically_normal();
        if (target && *target != path) return std::nullopt;
        target = path;
    }
    return target;
}

#ifdef _WIN32
constexpr std::size_t k_cmd_line_characters   = 8191;
constexpr std::size_t k_cmd_program_allowance = 1024;

std::string powershell_quote(std::string_view text)
{
    std::string result = "'";
    for (char character : text) {
        result += character;
        if (character == '\'') result += '\'';
    }
    return result + '\'';
}

bool powershell_literal(std::string_view argument)
{
    for (const std::string_view quote : {"\"", "\xE2\x80\x98", "\xE2\x80\x99", "\xE2\x80\x9A", "\xE2\x80\x9B"}) {
        if (argument.find(quote) != std::string_view::npos) {
            return false;
        }
    }
    return !argument.ends_with('\\') || argument.find_first_of(" \t") == std::string_view::npos;
}

bool cmd_literal(std::string_view argument)
{
    const bool control = std::any_of(argument.begin(), argument.end(),
        [](unsigned char byte) { return byte < 0x20 || byte == 0x7F; });
    return !control && argument.find_first_of("\"%!") == std::string_view::npos &&
        (argument.find(' ') != std::string_view::npos || argument.find_first_of("&|<>^()") == std::string_view::npos);
}

std::size_t cmd_argument_characters(const std::vector<std::string>& command)
{
    std::size_t characters = 0;
    for (auto argument = command.begin() + 1; argument != command.end(); ++argument) {
        const auto kept = argument->find_last_not_of('\\');
        characters += 3 + argument->size() +
            (kept == std::string::npos ? argument->size() : argument->size() - kept - 1);
    }
    return characters;
}
#endif

} // namespace

std::optional<Launcher_command> recognize_launcher(std::string_view script)
{
    return recognize(script, 8);
}

std::optional<addon::Launcher_candidate> discover_launcher(const addon::Discover_request& request)
{
    Environment environment;
    for (const auto& member : request.ambient_environment.as_object()) {
        environment.emplace_back(member.name, member.value.as_string());
    }
    const auto program = resolve_program("opencode", environment, {}, Launcher_whitespace::ASCII) ? "opencode" : "opencode2";
    if (!resolve_program(program, environment, {}, Launcher_whitespace::ASCII)) return std::nullopt;
    return addon::Launcher_candidate{
        "interactive", "OpenCode", program,
        request.platform_id == "windows-x86_64" ? addon::k_runner_powershell : addon::k_runner_direct,
        "", true, addon::Json_value::object(),
    };
}

bool uses_tui_plugin(std::string_view version)
{
    const auto major = version.substr(0, version.find('.'));
    return addon::is_semantic_version(version) && major != "0" && major != "1";
}

std::optional<std::vector<std::string>> tui_invocation(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    std::string_view                resume_session)
{
    const auto literal = addon::literal_argv(launcher.script_body);
    if (literal.empty() || !opencode_program(literal.front())) return std::nullopt;
    const auto directory = addon::path_from_utf8(working_directory);
    const auto pwd = addon::environment_value(addon::environment_pairs(effective_environment), "PWD");
    const auto base = pwd.empty() ? directory : addon::path_from_utf8(pwd);
    bool project = false;
    bool standalone = false;
    for (std::size_t index = 1; index < literal.size(); ++index) {
        const std::string_view argument = literal[index];
        if (argument == "--standalone") { standalone = true; continue; }
        if (argument == "--auto" || argument == "--print-logs") continue;
        if (argument == "--continue" || argument == "-c") {
            if (!resume_session.empty()) return std::nullopt;
            continue;
        }
        const auto equals = argument.find('=');
        const auto name = argument.substr(0, equals);
        if (name == "--session" || name == "-s" || name == "--prompt" || name == "--log-level") {
            if (!resume_session.empty() && name != "--log-level") return std::nullopt;
            if (equals != std::string_view::npos) {
                if (equals + 1 == argument.size()) return std::nullopt;
            }
            else {
                if (++index == literal.size() || literal[index].starts_with('-')) return std::nullopt;
            }
            continue;
        }
        if (argument.starts_with('-') || project) return std::nullopt;
        const auto path = addon::path_from_utf8(argument);
        std::error_code selected_error;
        std::error_code directory_error;
        const auto selected = fs::weakly_canonical(path.is_absolute() ? path : base / path, selected_error);
        const auto expected = fs::weakly_canonical(directory, directory_error);
        if (selected_error || directory_error || selected != expected) return std::nullopt;
        project = true;
    }
    std::vector<std::string> addition;
    if (!standalone) addition.emplace_back("--standalone");
    if (!resume_session.empty()) addition.insert(addition.end(), {"--session", std::string(resume_session)});
    auto invocation = launcher_argv;
    if (launcher.runner_key == "direct" && invocation.size() == literal.size() &&
        std::equal(literal.begin() + 1, literal.end(), invocation.begin() + 1))
    {
        invocation.insert(invocation.end(), addition.begin(), addition.end());
        return invocation;
    }
    if (launcher.runner_key == "powershell" && plain_powershell_command(launcher.script_body) &&
        invocation.size() == 4 && addon::base_name(invocation[0], Launcher_whitespace::ASCII) == "powershell" &&
        invocation[1] == "-NoLogo" && invocation[2] == "-Command" && invocation[3] == launcher.script_body)
    {
        for (const auto& argument : addition) invocation[3] += " " + argument;
        return invocation;
    }
    return std::nullopt;
}

namespace {

std::optional<std::vector<std::string>> session_launcher_arguments(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    bool                            native_tui)
{
    const auto command = recognize_launcher(launcher.script_body);
    const auto literal = addon::literal_argv(launcher.script_body);
    if (!command || !command->prefix_arguments.empty() || !command->environment.empty() ||
        literal.empty() || !opencode_program(literal.front()) ||
        (!native_tui && addon::base_name(literal.front(), Launcher_whitespace::ASCII) != "opencode"))
    {
        return std::nullopt;
    }
    // The TUI options (1.18) that keep the session in this directory
    // and interface with plugins loaded, and a project argument naming this
    // directory. Enabled --continue, --session, --fork, --prompt and --pure pick
    // another session, add a turn or drop the plugin; anything unknown is refused
    // rather than guessed at.
    static const std::set<std::string_view> valued{
        "--model", "-m", "--agent", "--log-level", "--port", "--hostname", "--mdns-domain", "--replay-limit"};
    static const std::set<std::string_view> flags{
        "--auto", "--yolo", "--dangerously-skip-permissions", "--print-logs", "--mdns", "--mini"};
    static const std::set<std::string_view> negated{
        "--no-auto", "--no-yolo", "--no-dangerously-skip-permissions", "--no-print-logs", "--no-mdns", "--no-mini",
        "--no-replay", "--replay=false", "--no-continue", "--continue=false", "--no-fork", "--fork=false",
        "--no-pure", "--pure=false"};
    // OpenCode resolves a relative project argument against PWD when it is
    // set, and against its working directory otherwise.
    const auto session_directory = addon::path_from_utf8(working_directory);
    const auto pwd = addon::environment_value(addon::environment_pairs(effective_environment), "PWD");
    const auto project_base = pwd.empty() ? session_directory : addon::path_from_utf8(pwd);
    const auto same_directory = [&session_directory, &project_base](std::string_view project) {
        const auto path = addon::path_from_utf8(project);
        std::error_code project_error;
        std::error_code session_error;
        const auto resolved = fs::weakly_canonical(path.is_absolute() ? path : project_base / path, project_error);
        const auto expected = fs::weakly_canonical(session_directory, session_error);
        return !project_error && !session_error && resolved == expected;
    };
    bool project = false;
    for (std::size_t index = 1; index < literal.size(); ++index) {
        const std::string_view argument = literal[index];
        if (!argument.starts_with('-')) {
            if (project || !same_directory(argument)) {
                return std::nullopt;
            }
            project = true;
            continue;
        }
        if (flags.contains(argument) || negated.contains(argument)) {
            continue;
        }
        if (native_tui && argument == "--standalone") {
            continue;
        }
        const auto equals = argument.find('=');
        if (equals != std::string_view::npos) {
            const auto name  = argument.substr(0, equals);
            const auto value = argument.substr(equals + 1);
            if (!value.empty() && (valued.contains(name) || name == "--cors" ||
                (flags.contains(name) && (value == "true" || value == "false"))))
            {
                continue;
            }
            return std::nullopt;
        }
        // --cors takes every following word up to the next option.
        const bool list = argument == "--cors";
        if ((!list && !valued.contains(argument)) || index + 1 == literal.size() ||
            literal[index + 1].starts_with('-'))
        {
            return std::nullopt;
        }
        ++index;
        while (list && index + 1 < literal.size() && !literal[index + 1].starts_with('-')) {
            ++index;
        }
    }
    if (launcher.runner_key == "direct" && launcher_argv.size() == literal.size() &&
        std::equal(literal.begin() + 1, literal.end(), launcher_argv.begin() + 1))
    {
        return std::vector<std::string>(launcher_argv.begin() + 1, launcher_argv.end());
    }
    if (launcher.runner_key == "powershell" && plain_powershell_command(launcher.script_body) &&
        launcher_argv.size() == 4 && addon::base_name(launcher_argv[0], Launcher_whitespace::ASCII) == "powershell" &&
        launcher_argv[1] == "-NoLogo" && launcher_argv[2] == "-Command" && launcher_argv[3] == launcher.script_body)
    {
        return std::vector<std::string>{"-NoLogo", "-Command", launcher.script_body};
    }
    return std::nullopt;
}

} // namespace

bool local_interactive_launcher(const addon::Launcher_request& launcher,
    const std::vector<std::string>& launcher_argv, const addon::Json_value& effective_environment,
    std::string_view working_directory)
{
    return session_launcher_arguments(
        launcher, launcher_argv, effective_environment, working_directory, false).has_value();
}

std::optional<std::vector<std::string>> resume_arguments(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    std::string_view                session)
{
    auto arguments = session_launcher_arguments(
        launcher, launcher_argv, effective_environment, working_directory, false);
    if (!arguments) {
        return std::nullopt;
    }
    if (launcher.runner_key == "direct") {
        arguments->insert(arguments->end(), {"--session", std::string(session)});
    }
    else {
        arguments->back() += " --session " + std::string(session);
    }
    return arguments;
}

std::optional<std::vector<std::string>> initial_prompt_invocation(
    const addon::Launcher_request&  launcher,
    const std::vector<std::string>& launcher_argv,
    const addon::Json_value&        effective_environment,
    std::string_view                working_directory,
    const Verified_launch&         verified,
    std::string_view                prompt)
{
    // Native composers reserve normalized exit/command input, and yargs strips
    // matching outer quotes from an attached --prompt value.
    if (prompt.empty() || addon::trim(prompt) != prompt ||
        prompt.starts_with("\xEF\xBB\xBF") || prompt.ends_with("\xEF\xBB\xBF") ||
        ((prompt.front() == '\'' || prompt.front() == '"') && prompt.back() == prompt.front()) ||
        prompt.starts_with('/') ||
        prompt == "exit" || prompt == "quit" || prompt == ":q")
    {
        return std::nullopt;
    }
    const bool native_tui = uses_tui_plugin(verified.version);
    const auto arguments = session_launcher_arguments(
        launcher, launcher_argv, effective_environment, working_directory, native_tui);
    if (!arguments) {
        return std::nullopt;
    }
    std::optional<std::vector<std::string>> invocation;
    if (native_tui) {
        invocation = tui_invocation(launcher, launcher_argv, effective_environment, working_directory);
    }
    else {
        invocation = std::vector<std::string>{launcher_argv.front()};
        invocation->insert(invocation->end(), arguments->begin(), arguments->end());
    }
    if (!invocation) {
        return std::nullopt;
    }
    const std::string prompt_argument = "--prompt=" + std::string(prompt);
#ifdef _WIN32
    auto command = launcher.runner_key == "direct"
        ? *invocation : addon::literal_argv(invocation->back());
    const auto selected = resolve_program(command.front(), addon::environment_pairs(effective_environment),
        working_directory, Launcher_whitespace::ASCII);
    if (!selected || (launcher.runner_key == "direct" && !native_image(*selected))) {
        return std::nullopt;
    }
    const auto extension = lower(selected->extension().string());
    if (extension == ".cmd" || extension == ".bat") {
        command.emplace_back(prompt_argument);
        // A batch shim expands %* through cmd.exe's own parser and line bound.
        // UTF-8 byte counts bound its UTF-16 arguments, including quote escaping;
        // reserve the same executable-prefix allowance as the Claude integration.
        if (!std::all_of(command.begin() + 1, command.end(), cmd_literal) ||
            cmd_argument_characters(command) + k_cmd_program_allowance > k_cmd_line_characters)
        {
            return std::nullopt;
        }
    }
#endif
    if (launcher.runner_key == "direct") {
        invocation->emplace_back(prompt_argument);
        return invocation;
    }
#ifdef _WIN32
    // Preserve the admitted shell and shim: either can contribute the selected
    // launch environment. Only the new literal argument needs quoting.
    if (!powershell_literal(prompt_argument)) {
        return std::nullopt;
    }
    invocation->back() += " " + powershell_quote(prompt_argument);
    return invocation;
#else
    return std::nullopt;
#endif
}

bool version_is_at_least(std::string_view version, int major, int minor, int patch)
{
    if (!addon::is_semantic_version(version)) return false;
    std::array<std::string_view, 3> components;
    auto remaining = version.substr(0, version.find_first_of("-+"));
    for (auto& component : components) {
        const auto separator = remaining.find('.');
        component = remaining.substr(0, separator);
        if (separator != std::string_view::npos) remaining.remove_prefix(separator + 1);
    }
    const std::array minima{major, minor, patch};
    for (std::size_t index = 0; index < minima.size(); ++index) {
        const auto minimum = std::to_string(minima[index]);
        if (components[index].size() != minimum.size()) return components[index].size() > minimum.size();
        if (components[index] != minimum) return components[index] > minimum;
    }
    const auto suffix = version.find_first_of("-+");
    return suffix == std::string_view::npos || version[suffix] != '-';
}

std::string parse_version_output(std::string_view output, std::string_view errors)
{
    constexpr std::string_view labels[] = {"OpenCode CLI", "opencode"};
    const auto first  = addon::parse_version_stream(output, labels, Launcher_whitespace::ASCII);
    const auto second = addon::parse_version_stream(errors, labels, Launcher_whitespace::ASCII);
    if (!first || !second || (!first->empty() && !second->empty() && *first != *second)) return {};
    return first->empty() ? *second : *first;
}

std::optional<bool> delegated_auto_approval(const std::vector<std::string>& literal)
{
    if (literal.empty() || !opencode_program(literal.front())) {
        return std::nullopt;
    }
    if (literal.size() == 1) {
        return false;
    }
    if (literal.size() == 2 && literal[1] == "--auto") {
        return true;
    }
    return std::nullopt;
}

std::optional<Verified_launch> verified_launch(
    const addon::Launcher_request& launcher, const std::vector<std::string>& launcher_argv,
    const addon::Json_value& effective_environment, std::string_view working_directory,
    bool delegated, addon::Invocation_context& context)
{
    auto command = recognize_launcher(launcher.script_body);
    if (!command || !command->prefix_arguments.empty() || launcher_argv.empty()) return std::nullopt;
    const auto literal = addon::literal_argv(launcher.script_body);
    if (literal.empty() || !opencode_program(literal.front())) return std::nullopt;
    const auto auto_approve = delegated_auto_approval(literal);
    if (delegated && !auto_approve) return std::nullopt;
    if (launcher.runner_key == "direct") {
        if (launcher_argv.size() != literal.size() ||
            !std::equal(literal.begin() + 1, literal.end(), launcher_argv.begin() + 1)) return std::nullopt;
    }
    else if (launcher.runner_key != "powershell" || launcher_argv.size() != 4 ||
             addon::base_name(launcher_argv[0], Launcher_whitespace::ASCII) != "powershell" || launcher_argv[1] != "-NoLogo" ||
             launcher_argv[2] != "-Command" || launcher_argv[3] != launcher.script_body)
    {
        return std::nullopt;
    }
    // An interactive launch runs the script itself. Without quoting, expansion
    // or separators PowerShell reads it as exactly its literal words, the first
    // one naming the command it runs.
    if (!delegated && launcher.runner_key == "powershell" && !plain_powershell_command(launcher.script_body)) {
        return std::nullopt;
    }
    Environment environment;
    if (!effective_environment.is_object()) return std::nullopt;
    for (const auto& member : effective_environment.as_object()) {
        if (!member.value.is_string()) return std::nullopt;
        set_environment(environment, member.name, member.value.as_string());
    }
    // The host already expanded and applied launcher.environment_text. Only
    // script-owned assignments remain to overlay on that effective view.
    for (const auto& [name, value] : command->environment) set_environment(environment, name, value);
    const auto selected = resolve_program(
        command->program, environment, working_directory, Launcher_whitespace::ASCII);
    if (!selected) return std::nullopt;
    if (launcher.runner_key == "direct") {
        const auto requested = resolve_program(
            launcher_argv.front(), environment, working_directory, Launcher_whitespace::ASCII);
        if (!requested || !equal_name(path_to_utf8(*requested), path_to_utf8(*selected))) return std::nullopt;
    }
    auto executable = *selected;
    std::string package_version;
    std::vector<fs::path> files{*selected};
    const auto extension = lower(selected->extension().string());
    const auto command_name = addon::base_name(command->program, Launcher_whitespace::ASCII);
    std::optional<fs::path> node_script;
    if (extension == ".cmd" || extension == ".ps1" || extension == ".cjs") {
        const bool script_module = extension == ".cjs";
        auto root = selected->parent_path().parent_path();
        std::optional<fs::path> shim_target;
        if (!script_module) {
            shim_target = npm_shim_target(*selected);
            const auto classic = fs::path("node_modules/opencode-ai");
            const auto native = fs::path("node_modules/@opencode/cli");
            if (shim_target == classic / "bin/opencode.exe") {
                root = selected->parent_path() / classic;
            }
            else if (shim_target == native / "bin/opencode.exe" ||
                     shim_target == native / "bin" / (command_name + ".cjs")) {
                root = selected->parent_path() / native;
            }
            else return std::nullopt;
        }
        const auto manifest = regular_file(root / "package.json");
        if (!manifest) return std::nullopt;
        std::error_code error;
        if (fs::file_size(*manifest, error) > 1024 * 1024 || error) return std::nullopt;
        std::ifstream input(*manifest, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        addon::Json_parse_error parse_error;
        const auto package = addon::parse_json(bytes, {}, &parse_error);
        if (!package || !package->is_object()) return std::nullopt;
        const auto* name    = package->find("name");
        const auto* version = package->find("version");
        const auto* bin     = package->find("bin");
        if (bin && bin->is_object()) bin = bin->find(command_name);
        if (!name || !name->is_string() || (name->as_string() != "opencode-ai" && name->as_string() != "@opencode/cli") ||
            !version || !version->is_string() || !addon::is_semantic_version(version->as_string()) ||
            !bin || !bin->is_string()) return std::nullopt;
        std::string bin_path = bin->as_string();
        std::replace(bin_path.begin(), bin_path.end(), '\\', '/');
        const bool tui_package = name->as_string() == "@opencode/cli";
        const auto relative_bin = addon::path_from_utf8(bin_path).lexically_normal();
        // @opencode/cli postinstall replaces both command mappings with the
        // selected native image. An unmodified source launcher still uses Node.
        const bool source_launcher = tui_package && relative_bin == fs::path("bin") / (command_name + ".cjs");
        if ((!source_launcher && relative_bin != fs::path("bin/opencode.exe")) ||
            (script_module && !source_launcher)) {
            return std::nullopt;
        }
        const auto image = regular_file(root / addon::path_from_utf8(bin_path));
        if (!image || (shim_target && regular_file(selected->parent_path() / *shim_target) != image)) {
            return std::nullopt;
        }
        executable      = *image;
        package_version = version->as_string();
        files.push_back(*manifest);
        files.push_back(executable);
        if (source_launcher) {
            if (script_module && *image != *selected) return std::nullopt;
            const auto local_node = regular_file(selected->parent_path() / "node.exe");
            const auto node = local_node ? local_node : resolve_program(
                "node", environment, working_directory, Launcher_whitespace::ASCII);
            if (!node || !native_image(*node)) return std::nullopt;
            node_script = executable;
            executable = *node;
            files.push_back(executable);
        }
    }
    if (!native_image(executable)) return std::nullopt;
    std::vector<std::string> probe_argv{path_to_utf8(executable), "--version"};
    std::vector<std::string> launch_argv{path_to_utf8(executable)};
    if (node_script) {
        probe_argv.insert(probe_argv.begin() + 1, path_to_utf8(*node_script));
        launch_argv.emplace_back(path_to_utf8(*node_script));
    }
    int timeout_ms = 3000;
    // The stock Node launcher selects a platform package at runtime, so its
    // selected native image cannot participate in our file-identity cache.
    bool cacheable = !node_script;
    if (delegated) {
        launch_argv = {path_to_utf8(*selected), "acp", "--cwd", std::string(working_directory)};
        probe_argv  = {path_to_utf8(*selected), "--version"};
        if (launcher.runner_key == "powershell" || !native_image(*selected)) {
#ifdef _WIN32
            const auto shell = resolve_program(launcher.runner_key == "powershell"
                ? launcher_argv.front() : "powershell.exe", environment, working_directory, Launcher_whitespace::ASCII);
            if (!shell || !native_image(*shell)) return std::nullopt;
            const auto make_shell = [&](const std::vector<std::string>& arguments) {
                std::string script = "& " + powershell_quote(path_to_utf8(*selected));
                for (const auto& argument : arguments) script += " " + powershell_quote(argument);
                script += "; exit $LASTEXITCODE";
                return std::vector<std::string>{path_to_utf8(*shell), "-NoLogo", "-Command", script};
            };
            probe_argv  = make_shell({"--version"});
            launch_argv = make_shell({"acp", "--cwd", std::string(working_directory)});
            files.push_back(*shell);
            timeout_ms = 10000;
#else
            return std::nullopt;
#endif
        }
    }
    else if (launcher.runner_key == "powershell") {
#ifdef _WIN32
        // Probe the program the launch's own shell resolves and runs, which may
        // be a shim other than the installation the package located. PowerShell
        // resolves it by rules no file identity here tracks (.ps1 precedence,
        // profiles), so this evidence is never reused.
        const auto shell = resolve_program(launcher_argv.front(), environment, working_directory, Launcher_whitespace::ASCII);
        if (!shell || !native_image(*shell)) return std::nullopt;
        probe_argv = {path_to_utf8(*shell), "-NoLogo", "-Command", literal.front() + " --version; exit $LASTEXITCODE"};
        files.push_back(*shell);
        timeout_ms = 10000;
        cacheable  = false;
#else
        return std::nullopt;
#endif
    }
    // The proof belongs to these exact installation bytes. A replacement while
    // the probe runs cannot publish evidence about a different executable.
    std::vector<std::pair<std::uintmax_t, fs::file_time_type>> identities;
    for (const auto& file : files) {
        std::error_code error;
        const auto size = fs::file_size(file, error);
        if (error) return std::nullopt;
        const auto modified = fs::last_write_time(file, error);
        if (error) return std::nullopt;
        identities.emplace_back(size, modified);
    }
    // Keep only a digest of potentially private environment input in the
    // bounded cache. Installation metadata and the exact probe argv are part
    // of the identity; failed and cancelled probes are never cached.
    addon::Sha256 digest;
    const auto append = [&](std::string_view value) {
        digest.update(std::to_string(value.size()));
        digest.update(":");
        digest.update(value);
    };
    append(working_directory);
    for (const auto& argument : probe_argv) append(argument);
    for (const auto& [name, value] : environment) { append(name); append(value); }
    for (std::size_t index = 0; index < files.size(); ++index) {
        append(path_to_utf8(files[index]));
        append(std::to_string(identities[index].first));
        append(addon::file_time_ticks_text(identities[index].second));
    }
    const auto cache_key = addon::to_hex(digest.finish());
    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cache;
    if (cacheable) {
        std::lock_guard lock(cache_mutex);
        const auto cached = cache.find(cache_key);
        if (cached != cache.end() && !context.cancel_requested()) {
            if ((!package_version.empty() && cached->second != package_version) ||
                !version_is_at_least(cached->second, 1, 18, delegated ? 25 : 11)) return std::nullopt;
            return Verified_launch{std::move(launch_argv), cached->second, command->environment,
                delegated && auto_approve.value_or(false)};
        }
    }
    const auto output = addon::probe(probe_argv, environment, working_directory, timeout_ms, context);
    if (!output) return std::nullopt;
    for (std::size_t index = 0; index < files.size(); ++index) {
        std::error_code error;
        if (fs::file_size(files[index], error) != identities[index].first || error) return std::nullopt;
        if (fs::last_write_time(files[index], error) != identities[index].second || error) return std::nullopt;
    }
    const auto version = parse_version_output(output->output, output->errors);
    if ((!package_version.empty() && version != package_version) ||
        !version_is_at_least(version, 1, 18, delegated ? 25 : 11)) return std::nullopt;
    if (cacheable) {
        std::lock_guard lock(cache_mutex);
        if (cache.size() >= 32) cache.erase(cache.begin());
        cache[cache_key] = version;
    }
    return Verified_launch{std::move(launch_argv), version, command->environment,
        delegated && auto_approve.value_or(false)};
}

} // namespace opencode
